#!/bin/sh
# Runs the cross-built Go tests on the board. Run from the directory that
# holds psmsgr.test, psmsgr-static.test and build/tests/interop_helper.
set -u
export TMPDIR=/dev/shm PSMSGR_BUILD_DIR="$PWD/build"
out=${1:-results}
mkdir -p "$out"

hog() { sh -c 'while :; do :; done' >/dev/null 2>&1 & echo $!; }

for i in 1 2 3; do
    ./psmsgr.test -test.count=1 -test.v > "$out/go-test-$i.log" 2>&1
    echo "dynamic run $i: $?"
done
for i in 1 2 3; do
    ./psmsgr-static.test -test.count=1 -test.v > "$out/go-test-static-$i.log" 2>&1
    echo "static run $i: $?"
done
for i in 1 2 3; do
    h=$(hog)
    ./psmsgr.test -test.count=1 -test.v > "$out/go-test-hog-$i.log" 2>&1
    rc=$?
    kill "$h"
    echo "hog run $i: $rc"
done
# The timing-sensitive tests, repeated.
./psmsgr.test -test.run 'Wait|Close|Signal|Intr' -test.count=30 > "$out/go-test-wait-x30.log" 2>&1
echo "wait x30: $?"
h=$(hog)
./psmsgr.test -test.run 'Wait|Close|Signal|Intr' -test.count=10 > "$out/go-test-wait-hog-x10.log" 2>&1
rc=$?
kill "$h"
echo "wait x10 with hog: $rc"

./psmsgr.test -test.run '^$' -test.bench . -test.benchmem -test.count=5 > "$out/go-bench.txt" 2>&1
echo "bench: $?"
./psmsgr-static.test -test.run '^$' -test.bench . -test.benchmem -test.count=5 > "$out/go-bench-static.txt" 2>&1
echo "bench static: $?"
