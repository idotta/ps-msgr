// SPDX-License-Identifier: Apache-2.0
// Prints the motor status published on the "motor" channel, one line per value, and says so when the
// writer stalls or goes away.
//
//   MotorReader [--dir DIR] [--count N] [--timeout MS]
using System.Globalization;
using PsMsgr;
using PsMsgr.Examples;

string? dir = null; // null: $PSMSGR_DIR, else /dev/shm
ulong count = 0; // 0: until Ctrl-C
uint timeoutMs = 300; // 3 periods of a writer at 10 Hz
try
{
    for (int i = 0; i < args.Length; i += 2)
    {
        switch (args[i])
        {
            case "--dir": dir = args[i + 1]; break;
            case "--count": count = ulong.Parse(args[i + 1], CultureInfo.InvariantCulture); break;
            case "--timeout": timeoutMs = uint.Parse(args[i + 1], CultureInfo.InvariantCulture); break;
            default: throw new FormatException();
        }
    }
    if (timeoutMs == 0)
        throw new FormatException();
}
catch (Exception e) when (e is FormatException or OverflowException or IndexOutOfRangeException)
{
    Console.Error.WriteLine("usage: MotorReader [--dir DIR] [--count N] [--timeout MS]");
    return 2;
}

using var cts = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true; // exit through the using blocks below, which close the reader
    cts.Cancel();
};

try
{
    // Succeeds whether or not the writer has started: the reader attaches to the channel file once it exists.
    using StateReader reader = StateReader.Open(MotorStatus.Channel, dir);
    uint seen = 0; // generation of the last value printed; 0: none
    for (ulong n = 0; count == 0 || n < count;)
    {
        if (!reader.Wait(seen, TimeSpan.FromMilliseconds(timeoutMs), cts.Token))
        {
            // No new value in time. TryPeek gives the age of the last one without copying it;
            // IsWriterAlive tells a stalled writer from a dead one.
            if (!reader.TryPeek(out StateInfo last))
            {
                Console.WriteLine("waiting for the writer");
                continue;
            }
            CheckType(reader, last);
            string state = reader.IsWriterAlive ? "stale" : "writer gone";
            Console.WriteLine(string.Create(CultureInfo.InvariantCulture,
                $"{state}: last value {last.Age.TotalSeconds:F1} s old"));
            continue;
        }

        // TryRead<T> copies the value straight into the struct: no torn values, no allocation. A payload
        // of another size throws Mismatch.
        MotorStatus status;
        StateInfo info;
        try
        {
            if (!reader.TryRead(out status, out info))
                continue;
        }
        catch (PsMsgrException e) when (e.Code == PsMsgrError.Busy) // transient: the writer kept overwriting
        {
            continue;
        }
        CheckType(reader, info);
        seen = info.Generation;
        Console.WriteLine(status);
        n++;
    }
}
catch (OperationCanceledException)
{
}
catch (Exception e) when (e is PsMsgrException or InvalidDataException)
{
    Console.Error.WriteLine($"MotorReader: {e.Message}");
    return 1;
}
return 0;

// Attached marks the first result from a newly attached channel file: at the first value, and again whenever
// the writer recreated the channel. The bytes are only a MotorStatus if the payload type says so.
static void CheckType(StateReader reader, StateInfo info)
{
    if (info.Attached && reader.Describe() is { } desc && desc.PayloadType != MotorStatus.V1)
    {
        throw new InvalidDataException(string.Create(CultureInfo.InvariantCulture,
            $"{MotorStatus.Channel}: payload type 0x{desc.PayloadType:x8}, expected 0x{MotorStatus.V1:x8} (MOTOR_STATUS_V1)"));
    }
}
