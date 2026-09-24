// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;

[assembly: SupportedOSPlatform("linux")]

namespace PsMsgr.Tests;

/// <summary>A test that gets its own channel directory.</summary>
public abstract class ChannelTest : IDisposable
{
    protected const string Chan = "chan";

    protected ChannelTest()
    {
        Dir = Directory.CreateTempSubdirectory("psmsgr-cs-").FullName;
    }

    protected string Dir { get; }

    protected string DataPath(string name = Chan) => Path.Combine(Dir, $"psmsgr.{name}.state");

    protected string LockPath(string name = Chan) => Path.Combine(Dir, $"psmsgr.{name}.lock");

    protected StateWriter OpenWriter(uint capacity, uint slotCount = 3, bool recreate = false, bool notify = true, uint payloadType = 0, string name = Chan)
        => StateWriter.Open(name, new StateOptions(capacity)
        {
            SlotCount = slotCount,
            Recreate = recreate,
            Notify = notify,
            PayloadType = payloadType,
            Directory = Dir,
        });

    protected StateReader OpenReader(string name = Chan) => StateReader.Open(name, Dir);

    public virtual void Dispose()
    {
        Directory.Delete(Dir, recursive: true);
        GC.SuppressFinalize(this);
    }

    /// <summary>The generation n publishes after gen: a new channel starts at a random one,
    /// and 0 is skipped on wrap.</summary>
    protected static uint GenAfter(uint gen, int n)
    {
        for (int i = 0; i < n; i++)
            gen = gen == uint.MaxValue ? 1 : gen + 1;
        return gen;
    }

    protected static ulong Inode(string path) => ulong.Parse(Tools.Run("stat", "-c", "%i", path).Trim());
}

internal static class Tools
{
    public static string Run(string file, params string[] args)
    {
        var psi = new ProcessStartInfo(file) { RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (string a in args)
            psi.ArgumentList.Add(a);
        using var p = Process.Start(psi)!;
        string stdout = p.StandardOutput.ReadToEnd();
        string stderr = p.StandardError.ReadToEnd();
        p.WaitForExit();
        Assert.True(p.ExitCode == 0, $"{file} exited with {p.ExitCode}: {stderr}");
        return stdout;
    }


    /// <summary>A file the preset built next to <c>$PSMSGR_LIBRARY</c>.</summary>
    public static string BuildFile(string relative)
    {
        string? library = Environment.GetEnvironmentVariable("PSMSGR_LIBRARY");
        if (string.IsNullOrEmpty(library))
            Assert.Skip("PSMSGR_LIBRARY is not set, so there is no build tree with the C helpers");
        string path = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(library))!, relative);
        Assert.True(System.IO.File.Exists(path), $"{path} is missing: build the preset that built {library}");
        return path;
    }
}

/// <summary>A child process that holds a writer until stopped: tests/interop_helper, or
/// Python with ps_msgr.</summary>
public sealed class ChildWriter : IDisposable
{
    private readonly Process _proc;

    public ChildWriter(string file, params string[] args)
    {
        var psi = new ProcessStartInfo(file)
        {
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
        };
        foreach (string a in args)
            psi.ArgumentList.Add(a);
        _proc = Process.Start(psi)!;
    }

    public static ChildWriter C(string dir, uint capacity, uint flags, params string[] payloads)
    {
        var w = new ChildWriter(Tools.BuildFile("tests/interop_helper"), ["write", dir, "chan", capacity.ToString(), flags.ToString(), .. payloads]);
        foreach (string _ in payloads)
            w.Generations.Add(uint.Parse(w.ReadLine()));
        return w;
    }

    public List<uint> Generations { get; } = [];

    public string ReadLine() => _proc.StandardOutput.ReadLine() ?? throw new InvalidOperationException("child exited");

    public void Kill()
    {
        _proc.Kill();
        _proc.WaitForExit();
    }

    /// <summary>Closes the child's stdin and checks that it exits cleanly.</summary>
    public void Stop()
    {
        _proc.StandardInput.Close();
        _proc.WaitForExit();
        Assert.Equal(0, _proc.ExitCode);
    }

    public void Dispose()
    {
        if (!_proc.HasExited)
            Kill();
        _proc.Dispose();
    }
}

/// <summary>Shaped like the C <c>struct motor_status</c> of the documentation.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct MotorStatus
{
    public ulong Sequence;
    public float SpeedRpm;
    public float CurrentA;
    public float TemperatureC;
}

/// <summary>SIGUSR2 with a handler installed without SA_RESTART, so that it interrupts a
/// blocking futex wait with EINTR.</summary>
internal static class InterruptingSignal
{
    private const int SigUsr2 = 12;

    static InterruptingSignal()
    {
        // Any function that ignores its argument works as the handler; getpid does. It
        // stays installed: resetting it could kill the process with a signal still pending.
        IntPtr handler = dlsym(IntPtr.Zero, "getpid");
        Assert.NotEqual(IntPtr.Zero, handler);
        signal(SigUsr2, handler);
        Assert.Equal(0, siginterrupt(SigUsr2, 1));
    }

    public static int CurrentThreadId() => gettid();

    public static void Send(int tid) => Assert.Equal(0, tgkill(getpid(), tid, SigUsr2));

    [DllImport("libc.so.6")]
    private static extern IntPtr dlsym(IntPtr handle, string symbol);

    [DllImport("libc.so.6")]
    private static extern IntPtr signal(int signum, IntPtr handler);

    [DllImport("libc.so.6")]
    private static extern int siginterrupt(int sig, int flag);

    [DllImport("libc.so.6")]
    private static extern int gettid();

    [DllImport("libc.so.6")]
    private static extern int getpid();

    [DllImport("libc.so.6")]
    private static extern int tgkill(int tgid, int tid, int sig);
}
