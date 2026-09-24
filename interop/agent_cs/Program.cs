// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using PsMsgr;

namespace PsMsgr.InteropAgent;

[StructLayout(LayoutKind.Sequential)]
internal struct MotorStatus
{
    public ulong Sequence;
    public float SpeedRpm;
    public float CurrentA;
    public float TemperatureC;
}

/// <summary>The C# agent of the interop suite (interop/README.md).</summary>
internal static unsafe class Program
{
    private const uint MotorStatusV1 = 0x0001_0001;

    private const string Usage = """
        usage: PsMsgr.InteropAgent payload-layout
               PsMsgr.InteropAgent write DIR NAME [--capacity N] [--payload-type T] [--recreate]
                                         [--no-notify] [--rate HZ] [--hold] SEQUENCE...
               PsMsgr.InteropAgent read DIR NAME [--for MS]
               PsMsgr.InteropAgent wait DIR NAME LAST_GENERATION TIMEOUT_MS
               PsMsgr.InteropAgent alive DIR NAME
               PsMsgr.InteropAgent    (the commands above but write, one per line of stdin)
        """;

    private sealed class UsageException : Exception;

    public static int Main(string[] args)
    {
        try
        {
            return args.Length > 0 ? Command(args, null) : Session();
        }
        catch (Exception e) when (e is UsageException or FormatException or OverflowException)
        {
            Console.Error.WriteLine(Usage);
            return 2;
        }
    }

    private static string Invariant(FormattableString s) => FormattableString.Invariant(s);

    private static string Json(bool b) => b ? "true" : "false";

    private static string Json(float f) => f.ToString("R", CultureInfo.InvariantCulture);

    private static void Emit(string line) => Console.Out.Write(line + "\n");

    private static string Error(PsMsgrException e) =>
        Invariant($$"""{"error":{{(int)e.Code}},"exception":"{{e.GetType().Name}}"}""");

    private static ulong ParseU64(string s) =>
        s.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
            ? ulong.Parse(s.AsSpan(2), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture)
            : ulong.Parse(s, NumberStyles.None, CultureInfo.InvariantCulture);

    private static uint ParseU32(string s) => checked((uint)ParseU64(s));

    private static MotorStatus MotorStatusOf(ulong n) => new()
    {
        Sequence = n,
        SpeedRpm = (n % 65536) / 2f,
        CurrentA = (n % 1024) / 64f,
        TemperatureC = (n % 2048) / 8f - 40f,
    };

    private static int PayloadLayout()
    {
        MotorStatus s;
        byte* p = (byte*)&s;
        Emit(Invariant($$"""
            {"sizeof":{{sizeof(MotorStatus)}},"offsets":{"sequence":{{(byte*)&s.Sequence - p}},"speed_rpm":{{(byte*)&s.SpeedRpm - p}},"current_a":{{(byte*)&s.CurrentA - p}},"temperature_c":{{(byte*)&s.TemperatureC - p}}},"payload_type":{{MotorStatusV1}}}
            """));
        return 0;
    }

    private static int Write(string[] args)
    {
        var options = new StateOptions((uint)sizeof(MotorStatus)) { PayloadType = MotorStatusV1, Directory = args[0] };
        double rate = 0;
        bool hold = false;
        int i = 2;
        for (; i < args.Length && args[i].StartsWith("--", StringComparison.Ordinal); i++)
        {
            switch (args[i])
            {
                case "--recreate": options.Recreate = true; break;
                case "--no-notify": options.Notify = false; break;
                case "--hold": hold = true; break;
                case "--capacity" when i + 1 < args.Length:
                    options = new StateOptions(ParseU32(args[++i]))
                    {
                        PayloadType = options.PayloadType,
                        Recreate = options.Recreate,
                        Notify = options.Notify,
                        Directory = options.Directory,
                    };
                    break;
                case "--payload-type" when i + 1 < args.Length: options.PayloadType = ParseU32(args[++i]); break;
                case "--rate" when i + 1 < args.Length:
                    rate = double.Parse(args[++i], CultureInfo.InvariantCulture);
                    break;
                default: throw new UsageException();
            }
        }
        ulong[] sequences = args[i..].Select(ParseU64).ToArray();

        StateWriter w;
        try
        {
            w = StateWriter.Open(args[1], options);
        }
        catch (PsMsgrException e)
        {
            Emit(Error(e));
            return 1;
        }
        using (w)
        {
            Emit("""{"opened":true}""");
            var clock = Stopwatch.StartNew();
            for (int k = 0; k < sequences.Length; k++)
            {
                if (rate > 0)
                {
                    TimeSpan due = TimeSpan.FromSeconds(k / rate) - clock.Elapsed;
                    if (due > TimeSpan.Zero)
                        Thread.Sleep(due);
                }
                MotorStatus s = MotorStatusOf(sequences[k]);
                uint generation;
                try
                {
                    generation = w.Publish(in s);
                }
                catch (PsMsgrException e)
                {
                    Emit(Error(e));
                    return 1;
                }
                Emit(Invariant($$"""{"generation":{{generation}},"sequence":{{s.Sequence}}}"""));
            }
            if (hold)
                Console.In.ReadToEnd();
        }
        return 0;
    }

    private static string ReadResult(StateReader r, out bool busy)
    {
        busy = false;
        byte[]? data;
        StateInfo info;
        ChannelDesc desc;
        try
        {
            data = r.Read(out info);
            if (data is null)
                return """{"nodata":true}""";
            desc = r.Describe()!.Value;
        }
        catch (PsMsgrException e)
        {
            busy = e.Code == PsMsgrError.Busy;
            return Error(e);
        }
        string value = "null";
        if (data.Length == sizeof(MotorStatus))
        {
            MotorStatus s = MemoryMarshal.Read<MotorStatus>(data);
            value = Invariant($$"""
                {"sequence":{{s.Sequence}},"speed_rpm":{{Json(s.SpeedRpm)}},"current_a":{{Json(s.CurrentA)}},"temperature_c":{{Json(s.TemperatureC)}}}
                """);
        }
        return Invariant($$"""
            {"generation":{{info.Generation}},"length":{{info.Length}},"timestamp_ns":{{info.TimestampNs}},"attached":{{Json(info.Attached)}},"payload_type":{{desc.PayloadType}},"value":{{value}},"hex":"{{Convert.ToHexStringLower(data)}}"}
            """);
    }

    private static int Read(StateReader r, ReadOnlySpan<string> args)
    {
        string result;
        bool busy;
        if (args.Length == 0)
        {
            result = ReadResult(r, out busy);
            Emit(result);
            return result.StartsWith("""{"error":""", StringComparison.Ordinal) ? 1 : 0;
        }
        if (args.Length != 2 || args[0] != "--for")
            throw new UsageException();
        var clock = Stopwatch.StartNew();
        TimeSpan duration = TimeSpan.FromMilliseconds(ParseU64(args[1]));
        long reads = 0, busyCount = 0;
        string? last = null;
        do
        {
            reads++;
            result = ReadResult(r, out busy);
            if (busy)
            {
                busyCount++;
            }
            else if (result != last)
            {
                Emit(result);
                last = result;
            }
        } while (clock.Elapsed < duration);
        Emit(Invariant($$"""{"reads":{{reads}},"busy":{{busyCount}}}"""));
        return 0;
    }

    private static int Wait(StateReader r, ReadOnlySpan<string> args)
    {
        if (args.Length != 2)
            throw new UsageException();
        uint lastGeneration = ParseU32(args[0]);
        int timeoutMs = int.Parse(args[1], CultureInfo.InvariantCulture);
        try
        {
            bool changed = r.Wait(lastGeneration,
                timeoutMs < 0 ? Timeout.InfiniteTimeSpan : TimeSpan.FromMilliseconds(timeoutMs));
            Emit(Invariant($$"""{"changed":{{Json(changed)}}}"""));
            return 0;
        }
        catch (PsMsgrException e)
        {
            Emit(Error(e));
            return 1;
        }
    }

    private static int Alive(StateReader r, ReadOnlySpan<string> args)
    {
        if (args.Length != 0)
            throw new UsageException();
        try
        {
            Emit(Invariant($$"""{"alive":{{Json(r.IsWriterAlive)}}}"""));
            return 0;
        }
        catch (PsMsgrException e)
        {
            Emit(Error(e));
            return 1;
        }
    }

    private static int Command(string[] args, Dictionary<(string, string), StateReader>? readers)
    {
        string cmd = args[0];
        if (cmd == "payload-layout" && args.Length == 1)
            return PayloadLayout();
        if (cmd == "write" && readers is null && args.Length >= 3)
            return Write(args[1..]);
        if (cmd is not ("read" or "wait" or "alive") || args.Length < 3)
            throw new UsageException();
        (string dir, string name) = (args[1], args[2]);
        StateReader? r = null;
        if (readers is not null && !readers.TryGetValue((dir, name), out r))
            readers[(dir, name)] = r = StateReader.Open(name, dir);
        StateReader reader = r ?? StateReader.Open(name, dir);
        try
        {
            ReadOnlySpan<string> rest = args.AsSpan(3);
            return cmd switch
            {
                "read" => Read(reader, rest),
                "wait" => Wait(reader, rest),
                _ => Alive(reader, rest),
            };
        }
        finally
        {
            if (readers is null)
                reader.Dispose();
        }
    }

    private static int Session()
    {
        var readers = new Dictionary<(string, string), StateReader>();
        try
        {
            while (Console.In.ReadLine() is string line)
            {
                string[] args = line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
                if (args.Length > 0)
                    Command(args, readers);
            }
        }
        finally
        {
            foreach (StateReader r in readers.Values)
                r.Dispose();
        }
        return 0;
    }
}
