// Per-call cost of the C# binding, in the psmsgr-bench CSV format (see percall.py).
using System.Buffers.Binary;
using System.Diagnostics;
using System.Globalization;
using PsMsgr;

CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
int iterations = 1000, batch = 100;
double seconds = 5, rate = 500;
for (int i = 0; i < args.Length; i++)
{
    switch (args[i])
    {
        case "--iterations": iterations = int.Parse(args[++i]); break;
        case "--batch": batch = int.Parse(args[++i]); break;
        case "--seconds": seconds = double.Parse(args[++i]); break;
        case "--rate": rate = double.Parse(args[++i]); break;
        case "--writer": Writer(args[++i], args[++i], rate, seconds); return;
    }
}

string dir = Directory.CreateTempSubdirectory("psmsgr-csbench-").FullName;
if (Directory.Exists("/dev/shm"))
{
    Directory.Delete(dir);
    dir = Path.Combine("/dev/shm", "psmsgr-csbench-" + Environment.ProcessId);
    Directory.CreateDirectory(dir);
}
try
{
    Console.WriteLine($"# dotnet: {Environment.Version} aot={!System.Runtime.CompilerServices.RuntimeFeature.IsDynamicCodeSupported}");
    Console.WriteLine($"# settings: iterations {iterations}, warm-up 1000, batch {batch}; wake-up {seconds} s at {rate} Hz");
    Console.WriteLine("test,size_bytes,n,min_ns,median_ns,p99_ns,max_ns,missed");
    Row("timer overhead", "", Timed(() => { }, iterations, 1, 0), 1);
    Row("Clock.NowNs", "", Timed(() => Clock.NowNs(), iterations, batch), batch);
    foreach (int size in new[] { 16, 256, 4096 })
    {
        byte[] payload = new byte[size];
        for (int i = 0; i < size; i++) payload[i] = (byte)i;
        using (var w = StateWriter.Open($"nn{size}", new StateOptions((uint)size) { Notify = false, Directory = dir }))
            Row("Publish NO_NOTIFY", size.ToString(), Timed(() => w.Publish(payload), iterations, batch), batch);
        using (var w = StateWriter.Open($"p{size}", new StateOptions((uint)size) { Directory = dir }))
        using (var r = StateReader.Open($"p{size}", dir))
        {
            Row("Publish", size.ToString(), Timed(() => w.Publish(payload), iterations, batch), batch);
            byte[] buf = new byte[size];
            Row("Read", size.ToString(), Timed(() => r.Read(out _), iterations, batch), batch);
            Row("TryRead(Span)", size.ToString(), Timed(() => r.TryRead(buf, out _), iterations, batch), batch);
            Row("TryPeek", size.ToString(), Timed(() => r.TryPeek(out _), iterations, batch), batch);
        }
    }

    var psi = new ProcessStartInfo(Environment.ProcessPath!) { RedirectStandardOutput = true };
    foreach (var a in new[] { "--rate", rate.ToString(), "--seconds", seconds.ToString(), "--writer", dir, "wake" })
        psi.ArgumentList.Add(a);
    using var p = Process.Start(psi)!;
    if (p.StandardOutput.ReadLine() != "ready") throw new InvalidOperationException("writer failed");
    var samples = new List<long>();
    long missed = 0;
    byte[] wb = new byte[16];
    using (var r = StateReader.Open("wake", dir))
    {
        r.TryRead(wb, out var info);
        uint seen = info.Generation;
        long end = Stopwatch.GetTimestamp() + (long)(seconds * Stopwatch.Frequency);
        while (Stopwatch.GetTimestamp() < end)
        {
            if (!r.Wait(seen, TimeSpan.FromMilliseconds(100))) continue;
            try { r.TryRead(wb, out info); }
            catch (PsMsgrException e) when (e.Code == PsMsgrError.Busy) { continue; }
            ulong t = Clock.NowNs();
            samples.Add((long)(t - BinaryPrimitives.ReadUInt64LittleEndian(wb)));
            missed += (uint)(info.Generation - seen - 1);
            seen = info.Generation;
        }
    }
    p.WaitForExit();
    Row("Wait wake-up", "16", samples, 1, missed.ToString());
}
finally
{
    Directory.Delete(dir, true);
}

static List<long> Timed(Action fn, int iterations, int batch, int warmup = 1000)
{
    for (int i = 0; i < warmup; i++) fn();
    var outp = new List<long>(iterations);
    double nsPerTick = 1e9 / Stopwatch.Frequency;
    for (int i = 0; i < iterations; i++)
    {
        long t0 = Stopwatch.GetTimestamp();
        for (int j = 0; j < batch; j++) fn();
        outp.Add((long)((Stopwatch.GetTimestamp() - t0) * nsPerTick));
    }
    return outp;
}

static void Row(string test, string size, List<long> samples, int batch, string missed = "")
{
    var v = samples.Select(s => (double)s / batch).Order().ToArray();
    int n = v.Length;
    double p99 = v[Math.Min(n - 1, (99 * n + 99) / 100 - 1)];
    Console.WriteLine($"\"{test}\",{size},{n},{v[0]:F0},{v[n / 2]:F0},{p99:F0},{v[^1]:F0},{missed}");
}

static void Writer(string dir, string name, double rate, double seconds)
{
    using var w = StateWriter.Open(name, new StateOptions(16) { Directory = dir });
    byte[] buf = new byte[16];
    BinaryPrimitives.WriteUInt64LittleEndian(buf, Clock.NowNs());
    w.Publish(buf);
    Console.WriteLine("ready");
    Console.Out.Flush();
    var sw = Stopwatch.StartNew();
    for (int i = 1; i / rate <= seconds; i++)
    {
        double delay = i / rate - sw.Elapsed.TotalSeconds;
        if (delay > 0) Thread.Sleep(TimeSpan.FromSeconds(delay));
        BinaryPrimitives.WriteUInt64LittleEndian(buf, Clock.NowNs());
        w.Publish(buf);
    }
}
