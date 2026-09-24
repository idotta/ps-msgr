// SPDX-License-Identifier: Apache-2.0
using System;
using System.Threading;

namespace PsMsgr;

/// <summary>
/// A reader of a channel. Opening always succeeds for a valid name: the reader attaches
/// when the channel appears, and follows it when it is recreated. Like the C handle, not
/// thread-safe.
/// </summary>
public sealed unsafe class StateReader : IDisposable
{
    // A TOOSMALL after resizing to the described capacity means the channel was
    // replaced in between; more than a few in a row is not going to happen.
    private const int ReadAttempts = 4;
    private const int CancelSliceMs = 100;

    private readonly ReaderHandle _handle;
    private readonly string _name;
    // Receive buffer for Read(), sized to the channel's capacity.
    private byte[]? _buf;
    // An attach that TryPeek or TryRead(Span) reported: Read re-checks the capacity.
    private bool _recheck;
    // An attach consumed by a result that threw: reported by the next result instead.
    private bool _pending;

    private StateReader(ReaderHandle handle, string name)
    {
        _handle = handle;
        _name = name;
    }

    /// <summary>Opens a reader of the channel <paramref name="name"/> in
    /// <paramref name="directory"/> (null: <c>$PSMSGR_DIR</c>, else <c>/dev/shm</c>).</summary>
    public static StateReader Open(string name, string? directory = null)
    {
        byte[] n = Strings.Name(name);
        byte[]? d = Strings.Directory(directory);
        var handle = new ReaderHandle();
        IntPtr r;
        int rc;
        fixed (byte* pn = n, pd = d)
            rc = Native.psmsgr_state_reader_open(pn, pd, &r);
        if (rc != Native.Ok)
        {
            var e = PsMsgrException.FromResult(rc, name);
            handle.Dispose();
            throw e;
        }
        handle.Set(r);
        return new StateReader(handle, name);
    }

    /// <summary>Copies the latest value to the start of <paramref name="destination"/>
    /// without allocating. False if there is no value; fails with
    /// <see cref="PsMsgrError.TooSmall"/> if <paramref name="destination"/> is too small.</summary>
    public bool TryRead(Span<byte> destination, out StateInfo info)
    {
        IntPtr h = Handle();
        NativeInfo ni;
        int rc;
        fixed (byte* p = destination)
            rc = Native.psmsgr_state_read(h, p, (uint)destination.Length, &ni);
        GC.KeepAlive(this);
        if (rc == Native.Ok)
        {
            bool attached = TakeAttached(ni);
            _recheck |= attached;
            info = new StateInfo(ni.Generation, ni.Length, ni.TimestampNs, attached);
            return true;
        }
        info = default;
        if (rc == (int)PsMsgrError.NoData)
            return false;
        if (rc == (int)PsMsgrError.TooSmall)
        {
            _pending |= (ni.Flags & Native.InfoAttached) != 0;
            _recheck |= _pending;
            throw PsMsgrException.FromResult(
                rc, _name, $"payload of {ni.Length} bytes does not fit in a {destination.Length}-byte buffer");
        }
        throw PsMsgrException.FromResult(rc, _name);
    }

    /// <summary>Reads the latest value as the raw bytes of a <typeparamref name="T"/>.
    /// False if there is no value; fails with <see cref="PsMsgrError.Mismatch"/> if its
    /// length is not <c>sizeof(T)</c>.</summary>
    public bool TryRead<T>(out T value, out StateInfo info) where T : unmanaged
    {
        IntPtr h = Handle();
        T v = default;
        NativeInfo ni;
        int rc = Native.psmsgr_state_read(h, &v, (uint)sizeof(T), &ni);
        GC.KeepAlive(this);
        if (rc == Native.Ok && ni.Length == sizeof(T))
        {
            bool attached = TakeAttached(ni);
            _recheck |= attached;
            value = v;
            info = new StateInfo(ni.Generation, ni.Length, ni.TimestampNs, attached);
            return true;
        }
        value = default;
        info = default;
        if (rc == (int)PsMsgrError.NoData)
            return false;
        if (rc == Native.Ok || rc == (int)PsMsgrError.TooSmall)
        {
            _pending |= (ni.Flags & Native.InfoAttached) != 0;
            _recheck |= _pending;
            throw new PsMsgrException(
                PsMsgrError.Mismatch,
                $"payload of {ni.Length} bytes does not match a {sizeof(T)}-byte type",
                channelName: _name);
        }
        throw PsMsgrException.FromResult(rc, _name);
    }

    /// <summary>A copy of the latest value, or null if there is none. Reads into a buffer
    /// the reader owns, so that the copy is the only allocation.</summary>
    public byte[]? Read(out StateInfo info)
    {
        IntPtr h = Handle();
        NativeInfo ni;
        bool attached = false;
        bool resized = false;
        for (int i = 0; i < ReadAttempts; i++)
        {
            byte[]? buf = _buf;
            int rc;
            fixed (byte* p = buf)
                rc = Native.psmsgr_state_read(h, p, (uint)(buf?.Length ?? 0), &ni);
            GC.KeepAlive(this);
            if (rc == Native.Ok)
            {
                attached |= TakeAttached(ni);
                byte[] data = ni.Length == 0 ? Array.Empty<byte>() : buf.AsSpan(0, (int)ni.Length).ToArray();
                if ((attached && !resized) || _recheck)
                    Fit(h, 0);
                info = new StateInfo(ni.Generation, ni.Length, ni.TimestampNs, attached);
                return data;
            }
            if (rc == (int)PsMsgrError.NoData)
            {
                _pending |= attached;
                info = default;
                return null;
            }
            if (rc != (int)PsMsgrError.TooSmall)
            {
                _pending |= attached;
                throw PsMsgrException.FromResult(rc, _name);
            }
            attached |= TakeAttached(ni);
            Fit(h, ni.Length);
            resized = true;
        }
        _pending |= attached;
        throw new PsMsgrException(PsMsgrError.Busy, channelName: _name);
    }

    /// <summary>Generation, length and timestamp of the latest value, without copying it
    /// and without syscalls while attached. False if there is no value.</summary>
    public bool TryPeek(out StateInfo info)
    {
        IntPtr h = Handle();
        NativeInfo ni;
        int rc = Native.psmsgr_state_peek(h, &ni);
        GC.KeepAlive(this);
        if (rc == Native.Ok)
        {
            bool attached = TakeAttached(ni);
            _recheck |= attached;
            info = new StateInfo(ni.Generation, ni.Length, ni.TimestampNs, attached);
            return true;
        }
        info = default;
        if (rc == (int)PsMsgrError.NoData)
            return false;
        throw PsMsgrException.FromResult(rc, _name);
    }

    /// <summary>
    /// Blocks until the generation differs from <paramref name="lastGeneration"/> (0: until
    /// there is any value). False on timeout. <see cref="Timeout.InfiniteTimeSpan"/> waits
    /// indefinitely, <see cref="TimeSpan.Zero"/> polls once. With a cancelable
    /// <paramref name="cancellationToken"/>, the token is checked at least every 100 ms.
    /// Fails with <see cref="PsMsgrError.NotSup"/> on a channel without notification.
    /// </summary>
    public bool Wait(uint lastGeneration, TimeSpan timeout, CancellationToken cancellationToken = default)
    {
        IntPtr h = Handle();
        bool infinite = timeout == Timeout.InfiniteTimeSpan;
        if (!infinite && timeout < TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(timeout), timeout, "must be non-negative or Timeout.InfiniteTimeSpan");
        bool cancelable = cancellationToken.CanBeCanceled;
        ulong deadline = 0;
        if (!infinite)
        {
            // Saturates: a timeout beyond 2^64 ns (585 years) is infinite in effect.
            ulong ns = (ulong)timeout.Ticks * 100;
            if (ns / 100 != (ulong)timeout.Ticks)
                ns = ulong.MaxValue;
            ulong now = Native.psmsgr_now_ns();
            deadline = ulong.MaxValue - now < ns ? ulong.MaxValue : now + ns;
        }
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            int ms;
            if (infinite)
            {
                ms = cancelable ? CancelSliceMs : -1;
            }
            else
            {
                ulong now = Native.psmsgr_now_ns();
                // Rounded up, so that a positive remainder never becomes a poll.
                ulong remaining = now >= deadline ? 0 : (deadline - now + 999_999) / 1_000_000;
                ms = (int)Math.Min(remaining, (ulong)(cancelable ? CancelSliceMs : int.MaxValue));
            }
            int rc = Native.psmsgr_state_wait(h, lastGeneration, ms);
            GC.KeepAlive(this);
            if (rc == Native.Ok)
                return true;
            if (rc == (int)PsMsgrError.Timeout)
            {
                if (!infinite && (ms == 0 || Native.psmsgr_now_ns() >= deadline))
                    return false;
            }
            // INTR: the runtime signals threads too (e.g. to suspend them for a GC), so
            // it is expected; retry with the remaining time.
            else if (rc != (int)PsMsgrError.Intr)
            {
                throw PsMsgrException.FromResult(rc, _name);
            }
        }
    }

    /// <summary>Whether a writer holds the channel now. Makes syscalls; also reattaches
    /// if the channel file was replaced.</summary>
    public bool IsWriterAlive
    {
        get
        {
            int rc = Native.psmsgr_state_writer_alive(Handle());
            GC.KeepAlive(this);
            if (rc >= 0)
                return rc == 1;
            throw PsMsgrException.FromResult(rc, _name);
        }
    }

    /// <summary>The attached channel's constant properties; null if not attached.</summary>
    public ChannelDesc? Describe()
    {
        IntPtr h = Handle();
        NativeDesc d;
        int rc = Native.psmsgr_state_describe_sized(h, &d, (uint)sizeof(NativeDesc));
        GC.KeepAlive(this);
        if (rc == Native.Ok)
            return new ChannelDesc(d.Capacity, d.SlotCount, d.PayloadType, (d.Flags & Native.StateNoNotify) == 0);
        if (rc == (int)PsMsgrError.NoData)
            return null;
        throw PsMsgrException.FromResult(rc, _name);
    }

    /// <summary>Closes the reader.</summary>
    public void Dispose()
    {
        _handle.Dispose();
        _buf = null;
    }

    internal int BufferLength => _buf?.Length ?? 0;

    private bool TakeAttached(in NativeInfo ni)
    {
        bool attached = _pending || (ni.Flags & Native.InfoAttached) != 0;
        _pending = false;
        return attached;
    }

    // Resizes the receive buffer to the channel's capacity.
    private void Fit(IntPtr h, uint need)
    {
        _recheck = false;
        NativeDesc d;
        int rc = Native.psmsgr_state_describe_sized(h, &d, (uint)sizeof(NativeDesc));
        GC.KeepAlive(this);
        uint size;
        if (rc == Native.Ok)
            size = Math.Max(d.Capacity, need);
        else if (rc == (int)PsMsgrError.NoData)
            size = need;
        else
            throw PsMsgrException.FromResult(rc, _name);
        if (size != (_buf?.Length ?? 0))
            _buf = size == 0 ? null : new byte[size];
    }

    private IntPtr Handle()
    {
        if (_handle.IsClosed)
            throw new ObjectDisposedException(nameof(StateReader));
        return _handle.DangerousGetHandle();
    }
}
