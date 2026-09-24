// SPDX-License-Identifier: Apache-2.0
using System;

namespace PsMsgr;

/// <summary>How <see cref="StateWriter.Open"/> creates a channel, or which geometry it
/// expects of an existing one.</summary>
public sealed class StateOptions
{
    /// <summary>Options for a channel of up to <paramref name="capacity"/> payload bytes
    /// (0 to 16 MiB; 0 is a heartbeat channel).</summary>
    public StateOptions(uint capacity) { Capacity = capacity; }

    /// <summary>Maximum payload size in bytes.</summary>
    public uint Capacity { get; }

    /// <summary>Slots in the channel, 2 to 16.</summary>
    public uint SlotCount { get; set; } = Native.StateDefaultSlots;

    /// <summary>Application tag that readers check with <see cref="StateReader.Describe"/>.</summary>
    public uint PayloadType { get; set; }

    /// <summary>File mode of the channel files; the default is 0644.</summary>
    public uint Mode { get; set; } = 0x1A4;

    /// <summary>Replace an existing channel of another geometry instead of failing with
    /// <see cref="PsMsgrError.Mismatch"/>.</summary>
    public bool Recreate { get; set; }

    /// <summary>Wake waiting readers on each publish. Without it, publishing makes no
    /// syscall and <see cref="StateReader.Wait"/> fails with <see cref="PsMsgrError.NotSup"/>.</summary>
    public bool Notify { get; set; } = true;

    /// <summary>Directory of the channel files; null: <c>$PSMSGR_DIR</c>, else <c>/dev/shm</c>.</summary>
    public string? Directory { get; set; }
}

/// <summary>Result of a read or peek.</summary>
/// <param name="Generation">Change token, never 0.</param>
/// <param name="Length">Payload length in bytes.</param>
/// <param name="TimestampNs"><c>CLOCK_MONOTONIC</c> at publish (<see cref="Clock.NowNs"/>).</param>
/// <param name="Attached">First result since the reader (re)attached to a channel file:
/// re-check <see cref="ChannelDesc.PayloadType"/> before trusting the bytes.</param>
public readonly record struct StateInfo(uint Generation, uint Length, ulong TimestampNs, bool Attached)
{
    /// <summary>Time since the value was published.</summary>
    public TimeSpan Age => TimeSpan.FromTicks((long)(Clock.NowNs() - TimestampNs) / 100);
}

/// <summary>Constant properties of an attached channel.</summary>
/// <param name="Capacity">Maximum payload size in bytes.</param>
/// <param name="SlotCount">Slots in the channel.</param>
/// <param name="PayloadType">The writer's application tag.</param>
/// <param name="Notify">Whether <see cref="StateReader.Wait"/> is supported.</param>
public readonly record struct ChannelDesc(uint Capacity, uint SlotCount, uint PayloadType, bool Notify);

/// <summary>The library's clock.</summary>
public static class Clock
{
    /// <summary><c>CLOCK_MONOTONIC</c> in nanoseconds: the clock of
    /// <see cref="StateInfo.TimestampNs"/>.</summary>
    public static ulong NowNs() => Native.psmsgr_now_ns();
}

/// <summary>Channel management.</summary>
public static class Channel
{
    /// <summary>Retires and deletes a channel; readers see it disappear. False if it
    /// does not exist. Fails with <see cref="PsMsgrError.WriterExists"/> while a writer holds it.</summary>
    public static unsafe bool Unlink(string name, string? directory = null)
    {
        byte[] n = Strings.Name(name);
        byte[]? d = Strings.Directory(directory);
        int rc;
        fixed (byte* pn = n, pd = d)
            rc = Native.psmsgr_state_unlink(pn, pd);
        if (rc == Native.Ok)
            return true;
        if (rc == (int)PsMsgrError.NoData)
            return false;
        throw PsMsgrException.FromResult(rc, name);
    }
}

internal static class Strings
{
    public static byte[] Name(string name)
    {
        if (name is null)
            throw new ArgumentNullException(nameof(name));
        if (name.IndexOf('\0') >= 0)
            throw new ArgumentException("embedded null character in name", nameof(name));
        return Native.Utf8z(name);
    }

    public static byte[]? Directory(string? directory)
    {
        if (directory is null)
            return null;
        if (directory.IndexOf('\0') >= 0)
            throw new ArgumentException("embedded null character in directory", nameof(directory));
        return Native.Utf8z(directory);
    }
}
