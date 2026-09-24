// SPDX-License-Identifier: Apache-2.0
// Publishes a changing motor status on the "motor" channel.
//
//   MotorWriter [--dir DIR] [--rate HZ] [--count N]
using System.Globalization;
using PsMsgr;
using PsMsgr.Examples;

string? dir = null; // null: $PSMSGR_DIR, else /dev/shm
double rate = 10;
ulong count = 0; // 0: until Ctrl-C
try
{
    for (int i = 0; i < args.Length; i += 2)
    {
        switch (args[i])
        {
            case "--dir": dir = args[i + 1]; break;
            case "--rate": rate = double.Parse(args[i + 1], CultureInfo.InvariantCulture); break;
            case "--count": count = ulong.Parse(args[i + 1], CultureInfo.InvariantCulture); break;
            default: throw new FormatException();
        }
    }
    if (!(rate > 0))
        throw new FormatException();
}
catch (Exception e) when (e is FormatException or OverflowException or IndexOutOfRangeException)
{
    Console.Error.WriteLine("usage: MotorWriter [--dir DIR] [--rate HZ] [--count N]");
    return 2;
}

using var cts = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true; // exit through the using blocks below, which close the writer
    cts.Cancel();
};

try
{
    // This program owns the channel: Recreate replaces one left behind with another capacity or payload
    // type (e.g. by an older version) instead of failing with Mismatch. Attached readers move to the new file.
    var options = new StateOptions(MotorStatus.Size) { PayloadType = MotorStatus.V1, Recreate = true, Directory = dir };
    using StateWriter writer = StateWriter.Open(MotorStatus.Channel, options);
    using var timer = new PeriodicTimer(TimeSpan.FromSeconds(1 / rate));
    for (ulong seq = 1; ; seq++)
    {
        var status = new MotorStatus
        {
            Sequence = seq,
            SpeedRpm = 1500 + seq % 100 * 5,
            CurrentA = 2 + seq % 8 * 0.25f,
            TemperatureC = 40 + seq % 40 * 0.5f,
        };
        writer.Publish(status); // copies the struct's bytes: no serializer, no allocation
        Console.WriteLine(status);
        if (seq == count)
            break;
        await timer.WaitForNextTickAsync(cts.Token);
    }
    // Disposing the writer releases it: the channel and its last value stay, and readers see
    // IsWriterAlive turn false.
}
catch (OperationCanceledException)
{
}
catch (PsMsgrException e)
{
    Console.Error.WriteLine($"MotorWriter: {e.Message}");
    return 1;
}
return 0;
