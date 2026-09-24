// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Text;

namespace PsMsgr.Tests;

/// <summary>C, Python and C# on either side of a channel, in separate processes.</summary>
public sealed class InteropTests : ChannelTest
{
    private static byte[] B(string s) => Encoding.UTF8.GetBytes(s);

    [Fact]
    public void CWriterCSharpReader()
    {
        using var r = OpenReader();
        using (var c = ChildWriter.C(Dir, 16, 0, "one", "two"))
        {
            Assert.True(r.Wait(0, TimeSpan.FromSeconds(10)));
            Assert.Equal(B("two"), r.Read(out StateInfo info));
            Assert.Equal((c.Generations[1], true), (info.Generation, info.Attached));
            Assert.Equal(GenAfter(c.Generations[0], 1), c.Generations[1]);
            Assert.True(r.IsWriterAlive);
            Assert.Equal(new ChannelDesc(16, 3, 0, true), r.Describe());
            Assert.Equal(PsMsgrError.WriterExists, Assert.Throws<PsMsgrException>(() => OpenWriter(16)).Code);
            c.Stop();
        }
        Assert.False(r.IsWriterAlive);
        Assert.Equal(B("two"), r.Read(out _));
    }

    [Fact]
    public void CWriterRecreatesUnderCSharpReader()
    {
        using var r = OpenReader();
        uint gen;
        using (var w = OpenWriter(8))
            gen = w.Publish(B("csharp"));
        Assert.Equal(B("csharp"), r.Read(out _));
        using var c = ChildWriter.C(Dir, 32, Native.StateRecreate, new string('c', 20));
        Assert.Equal([GenAfter(gen, 1)], c.Generations);
        Assert.Equal(B(new string('c', 20)), r.Read(out StateInfo info));
        Assert.Equal((GenAfter(gen, 1), true), (info.Generation, info.Attached));
        Assert.Equal(32u, r.Describe()!.Value.Capacity);
        c.Stop();
    }

    private const string PythonReader = """
        import sys
        from ps_msgr import StateReader
        with StateReader(sys.argv[1], directory=sys.argv[2]) as r:
            snap = r.read()
            desc = r.describe()
            print(snap.generation, snap.data.hex(), desc.payload_type, r.writer_alive())
        """;

    private const string PythonWriter = """
        import struct, sys
        from ps_msgr import StateWriter
        with StateWriter(sys.argv[1], 24, payload_type=0x10001, directory=sys.argv[2]) as w:
            print(w.publish(struct.pack("<Qfff4x", 7, 1500.0, 2.5, 41.0)), flush=True)
            sys.stdin.read()
        """;

    [Fact]
    public void CSharpWriterPythonReader()
    {
        Tools.BuildFile("tests/interop_helper"); // skips without a build tree, like the Python tests
        var status = new MotorStatus { Sequence = 7, SpeedRpm = 1500f, CurrentA = 2.5f, TemperatureC = 41f };
        using var w = OpenWriter(24, payloadType: 0x10001);
        uint gen = w.Publish(status);
        string hex = Convert.ToHexStringLower(System.Runtime.InteropServices.MemoryMarshal.AsBytes(new[] { status }.AsSpan()));
        Assert.Equal($"{gen} {hex} 65537 True\n", Tools.Run("python3", "-c", PythonReader, Chan, Dir));
    }

    [Fact]
    public void PythonWriterCSharpReader()
    {
        Tools.BuildFile("tests/interop_helper");
        using var r = OpenReader();
        using var python = new ChildWriter("python3", "-c", PythonWriter, Chan, Dir);
        uint gen = uint.Parse(python.ReadLine());
        Assert.True(r.TryRead(out MotorStatus status, out StateInfo info));
        Assert.Equal(new MotorStatus { Sequence = 7, SpeedRpm = 1500f, CurrentA = 2.5f, TemperatureC = 41f }, status);
        Assert.Equal((gen, true), (info.Generation, info.Attached));
        Assert.Equal(new ChannelDesc(24, 3, 0x10001, true), r.Describe());
        python.Stop();
    }

    [Fact]
    public void CSharpWriterCReader()
    {
        string dumpTool = Tools.BuildFile("tools/psmsgr-dump");
        string Dump() => Tools.Run(dumpTool, Chan, "--dir", Dir, "--hex");

        using (var w = OpenWriter(16, payloadType: 0x42))
        {
            uint gen = w.Publish(B("hello, dump"));
            string output = Dump();
            Assert.Contains("capacity      16\n", output);
            Assert.Contains("payload_type  0x00000042 (66)\n", output);
            Assert.Contains("writer        alive\n", output);
            Assert.Contains($"payload       generation {gen}, 11 bytes\n", output);
            Assert.Contains(" 68 65 6c 6c 6f 2c 20 64  75 6d 70 ", output);
        }
        Assert.Contains("writer        not running\n", Dump());
    }
}
