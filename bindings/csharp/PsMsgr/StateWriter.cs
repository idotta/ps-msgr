// SPDX-License-Identifier: Apache-2.0
using System;

namespace PsMsgr;

/// <summary>
/// The single writer of a channel: opens or creates it and takes the writer lock.
/// Like the C handle, not thread-safe.
/// </summary>
public sealed unsafe class StateWriter : IDisposable
{
    private readonly WriterHandle _handle;
    private readonly string _name;
    private readonly uint _capacity;
    // The open WriteScope, 0 if none. The scope is a readonly struct that only holds its
    // id, so copies of it (including the read-only variable of a using) stay coherent.
    private ulong _scope;
    private ulong _lastScope;

    private StateWriter(WriterHandle handle, string name, uint capacity)
    {
        _handle = handle;
        _name = name;
        _capacity = capacity;
    }

    /// <summary>Opens or creates the channel <paramref name="name"/> and takes the writer
    /// lock. Fails with <see cref="PsMsgrError.WriterExists"/> while another writer holds it,
    /// and with <see cref="PsMsgrError.Mismatch"/> if it exists with another geometry.</summary>
    public static StateWriter Open(string name, StateOptions options)
    {
        if (options is null)
            throw new ArgumentNullException(nameof(options));
        byte[] n = Strings.Name(name);
        byte[]? d = Strings.Directory(options.Directory);
        NativeOptions o;
        Native.psmsgr_state_options_init_sized(&o, (uint)sizeof(NativeOptions));
        o.Capacity = options.Capacity;
        o.SlotCount = options.SlotCount;
        o.PayloadType = options.PayloadType;
        o.Mode = options.Mode;
        o.Flags = (options.Recreate ? Native.StateRecreate : 0) | (options.Notify ? 0 : Native.StateNoNotify);
        var handle = new WriterHandle();
        IntPtr w;
        int rc;
        fixed (byte* pn = n, pd = d)
        {
            o.Dir = pd;
            rc = Native.psmsgr_state_writer_open(pn, &o, &w);
        }
        if (rc != Native.Ok)
        {
            var e = PsMsgrException.FromResult(rc, name);
            handle.Dispose();
            throw e;
        }
        handle.Set(w);
        return new StateWriter(handle, name, Native.psmsgr_state_writer_capacity(w));
    }

    /// <summary>Maximum payload size in bytes.</summary>
    public uint Capacity
    {
        get
        {
            Handle();
            return _capacity;
        }
    }

    /// <summary>Copies and publishes a value; returns its generation. Fails with
    /// <see cref="PsMsgrError.TooBig"/> if it exceeds <see cref="Capacity"/>.</summary>
    public uint Publish(ReadOnlySpan<byte> data)
    {
        IntPtr h = Handle();
        uint gen;
        int rc;
        fixed (byte* p = data)
            rc = Native.psmsgr_state_publish(h, p, (uint)data.Length, &gen);
        GC.KeepAlive(this);
        if (rc != Native.Ok)
            throw PsMsgrException.FromResult(rc, _name);
        return gen;
    }

    /// <summary>Publishes the <c>sizeof(T)</c> raw bytes of <paramref name="value"/>;
    /// returns its generation. Declare <typeparamref name="T"/> with a
    /// <c>[StructLayout]</c> that matches the readers' definition.</summary>
    public uint Publish<T>(in T value) where T : unmanaged
    {
        IntPtr h = Handle();
        uint gen;
        int rc;
        fixed (T* p = &value)
            rc = Native.psmsgr_state_publish(h, p, (uint)sizeof(T), &gen);
        GC.KeepAlive(this);
        if (rc != Native.Ok)
            throw PsMsgrException.FromResult(rc, _name);
        return gen;
    }

    /// <summary>Starts a zero-copy publish: write the value into
    /// <see cref="WriteScope.Buffer"/>, then <see cref="WriteScope.Commit"/>. Disposing the
    /// scope without a commit aborts it, and readers never see the partial value.</summary>
    public WriteScope Begin()
    {
        IntPtr h = Handle();
        void* buf;
        int rc = Native.psmsgr_state_begin(h, &buf);
        GC.KeepAlive(this);
        if (rc != Native.Ok)
            throw PsMsgrException.FromResult(rc, _name);
        _scope = ++_lastScope;
        return new WriteScope(this, _scope, new Span<byte>(buf, (int)_capacity));
    }

    internal void CheckScope(ulong scope)
    {
        Handle();
        if (scope == 0 || scope != _scope)
            throw new InvalidOperationException("the write scope has ended");
    }

    internal uint Commit(ulong scope, int length)
    {
        CheckScope(scope);
        if (length < 0)
            throw new ArgumentOutOfRangeException(nameof(length), length, "must not be negative");
        uint gen;
        int rc = Native.psmsgr_state_commit(_handle.DangerousGetHandle(), (uint)length, &gen);
        GC.KeepAlive(this);
        if (rc != Native.Ok)
            throw PsMsgrException.FromResult(rc, _name);
        _scope = 0;
        return gen;
    }

    internal void Abort(ulong scope)
    {
        // Closing the writer has aborted the begin already.
        if (_handle.IsClosed || scope == 0 || scope != _scope)
            return;
        _scope = 0;
        int rc = Native.psmsgr_state_abort(_handle.DangerousGetHandle());
        GC.KeepAlive(this);
        if (rc != Native.Ok)
            throw PsMsgrException.FromResult(rc, _name);
    }

    /// <summary>Releases the writer lock; the channel and its last value remain.
    /// Aborts an open <see cref="WriteScope"/>.</summary>
    public void Dispose() => _handle.Dispose();

    private IntPtr Handle()
    {
        if (_handle.IsClosed)
            throw new ObjectDisposedException(nameof(StateWriter));
        return _handle.DangerousGetHandle();
    }
}

/// <summary>
/// A zero-copy publish in progress (<see cref="StateWriter.Begin"/>). Exactly one
/// <see cref="Commit"/> or <see cref="Dispose"/> (which aborts) ends it; use it with
/// <c>using</c>. <see cref="Buffer"/> is valid until then, and while the writer is open.
/// </summary>
public readonly ref struct WriteScope
{
    private readonly StateWriter? _writer;
    private readonly ulong _id;
    private readonly Span<byte> _buffer;

    internal WriteScope(StateWriter writer, ulong id, Span<byte> buffer)
    {
        _writer = writer;
        _id = id;
        _buffer = buffer;
    }

    /// <summary>The slot to write the value into: <see cref="StateWriter.Capacity"/> bytes,
    /// 32-byte aligned.</summary>
    public Span<byte> Buffer
    {
        get
        {
            Writer().CheckScope(_id);
            return _buffer;
        }
    }

    /// <summary>Publishes the first <paramref name="length"/> bytes of <see cref="Buffer"/>;
    /// returns the generation. On <see cref="PsMsgrError.TooBig"/> the scope stays open.</summary>
    public uint Commit(int length) => Writer().Commit(_id, length);

    /// <summary>Aborts the publish unless it was committed: readers keep the last value.</summary>
    public void Dispose() => _writer?.Abort(_id);

    private StateWriter Writer() => _writer ?? throw new InvalidOperationException("the write scope has ended");
}
