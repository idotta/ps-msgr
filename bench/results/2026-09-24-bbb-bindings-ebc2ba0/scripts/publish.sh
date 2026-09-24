#!/bin/sh
set -eux
export NUGET_PACKAGES=/src/build/nuget
o=/src/build/bbb/out
P="-c Release -r linux-arm -p:ObjCopyName=arm-linux-gnueabihf-objcopy"
dotnet --info | head -20
dotnet publish /src/bindings/csharp/PsMsgr.AotSmoke $P -o $o/aot-smoke
dotnet publish /src/build/bbb/aot-static $P -o $o/aot-smoke-static
dotnet publish /src/interop/agent_cs $P -o $o/agent_cs
dotnet publish /src/examples/csharp/MotorWriter $P -o $o/MotorWriter
dotnet publish /src/examples/csharp/MotorReader $P -o $o/MotorReader
dotnet publish /src/build/bbb/cs-percall $P -o $o/percall
dotnet publish /src/build/bbb/cs-percall-static $P -o $o/percall-static
# xUnit suite: JIT, self-contained, so the board needs no runtime install
dotnet publish /src/bindings/csharp/PsMsgr.Tests -c Release -r linux-arm --self-contained true -o $o/tests
# Python wheel
rm -rf /tmp/w && mkdir -p /tmp/w && cp -R /src/bindings/python/pyproject.toml /src/bindings/python/README.md /src/bindings/python/src /tmp/w/
python3 -m pip wheel --quiet --no-deps --no-build-isolation --wheel-dir $o/wheel /tmp/w
