// SPDX-License-Identifier: Apache-2.0
using System.IO;
using System.Runtime.InteropServices;

namespace PsMsgr;

/// <summary>The library's <c>PSMSGR_E_*</c> result codes.</summary>
public enum PsMsgrError
{
    /// <summary>Bad argument or channel name.</summary>
    Inval = -1,
    /// <summary>A system call failed; see <see cref="PsMsgrException.Errno"/>.</summary>
    Sys = -2,
    /// <summary>Channel absent or nothing published yet. The binding reports it as
    /// <c>false</c> or <c>null</c>, never as an exception.</summary>
    NoData = -3,
    /// <summary>The destination buffer is too small for the payload.</summary>
    TooSmall = -4,
    /// <summary>The payload is larger than the channel's capacity.</summary>
    TooBig = -5,
    /// <summary>The writer kept overwriting the slot being read. Transient: retry.</summary>
    Busy = -6,
    /// <summary>A wait timed out.</summary>
    Timeout = -7,
    /// <summary>A wait was interrupted by a signal.</summary>
    Intr = -8,
    /// <summary>Another writer holds the channel.</summary>
    WriterExists = -9,
    /// <summary>The existing channel has another geometry; open with
    /// <see cref="StateOptions.Recreate"/> to replace it.</summary>
    Mismatch = -10,
    /// <summary>The channel file is not a valid channel of a supported format.</summary>
    Format = -11,
    /// <summary>Not supported, e.g. a wait on a channel without notification.</summary>
    NotSup = -12,
    /// <summary>The call is not valid now, e.g. a publish while a write scope is open.</summary>
    State = -13,
}

/// <summary>A failed library call.</summary>
public class PsMsgrException : IOException
{
    /// <summary>Creates the exception for a result code. The message defaults to the
    /// system's text for <paramref name="errno"/>, else the library's text for the code.</summary>
    public PsMsgrException(PsMsgrError code, string? message = null, int errno = 0, string? channelName = null)
        : base(Format(code, message, errno, channelName))
    {
        Code = code;
        Errno = errno;
        ChannelName = channelName;
    }

    /// <summary>The <c>PSMSGR_E_*</c> result.</summary>
    public PsMsgrError Code { get; }

    /// <summary>For <see cref="PsMsgrError.Sys"/>, the <c>errno</c> of the failing system call; else 0.</summary>
    public int Errno { get; }

    /// <summary>The channel name, where there is one.</summary>
    public string? ChannelName { get; }

    // Right after the failing call, before any other P/Invoke: reads errno.
    internal static PsMsgrException FromResult(int rc, string? channelName, string? message = null)
    {
        int errno = rc == (int)PsMsgrError.Sys ? Marshal.GetLastWin32Error() : 0;
        return new PsMsgrException((PsMsgrError)rc, message, errno, channelName);
    }

    private static unsafe string Format(PsMsgrError code, string? message, int errno, string? channelName)
    {
        message ??= errno != 0
            ? Native.FromUtf8z(Native.strerror(errno))
            : Native.FromUtf8z(Native.psmsgr_strerror((int)code));
        if (errno != 0)
            message = $"{message} (errno {errno})";
        return channelName is null ? message : $"{message}: '{channelName}'";
    }
}
