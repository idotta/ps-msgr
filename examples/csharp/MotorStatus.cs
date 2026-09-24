// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace PsMsgr.Examples;

/// <summary>
/// The payload of the "motor" channel: mirrors examples/motor_status.h, the definition. Sequential layout
/// with the same fields in the same order gives the C struct's natural alignment: 24 bytes, padded after
/// TemperatureC.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct MotorStatus
{
    public const string Channel = "motor";
    public const uint V1 = 0x0001_0001; // payload_type: schema 1, version 1

    public static readonly uint Size = (uint)Unsafe.SizeOf<MotorStatus>();

    public ulong Sequence;
    public float SpeedRpm;
    public float CurrentA;
    public float TemperatureC;

    public override readonly string ToString() => string.Create(
        CultureInfo.InvariantCulture,
        $"seq={Sequence} speed={SpeedRpm:F1} rpm current={CurrentA:F2} A temperature={TemperatureC:F1} C");
}
