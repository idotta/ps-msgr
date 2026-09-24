#!/bin/sh
# examples/check.sh's run checks, on the board: the armhf-release C examples,
# the Python examples on the installed wheel, the C# examples published with
# Native AOT for linux-arm, and the C examples built on the board against the
# installed libpsmsgr-dev (standalone). Library: the installed libpsmsgr1.
set -eu
B=$HOME/bbb-bindings
here=$B/repo/examples
tmp=$(mktemp -d /dev/shm/examples.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
export PATH=$B/venv/bin:$PATH
unset PSMSGR_LIBRARY

mkdir "$tmp/c"
for p in motor_writer motor_reader; do
    gcc -O2 -Wall -Wextra -Werror -I"$here" -o "$tmp/c/$p" "$here/c/$p.c" -lpsmsgr
done
echo "standalone C built on the board: gcc -O2 -Wall -Wextra -Werror -I examples <src> -lpsmsgr (libpsmsgr-dev; no cmake or pkg-config on the image)"

writer_c=$B/build/examples/c/motor_writer
reader_c=$B/build/examples/c/motor_reader
writer_standalone=$tmp/c/motor_writer
reader_standalone=$tmp/c/motor_reader
writer_py=$here/python/motor_writer.py
reader_py=$here/python/motor_reader.py
writer_cs=$B/cs/MotorWriter/MotorWriter
reader_cs=$B/cs/MotorReader/MotorReader
build=$B/build

fail() {
    echo "check.sh: $*" >&2
    for f in "$d"/*.out "$d"/*.err; do
        [ -f "$f" ] && { echo "--- $f" >&2; cat "$f" >&2; }
    done
    exit 1
}

# pair WRITER READER: the reader starts first and must print 5 of the writer's
# 10 values, exactly as the writer printed them, in order.
pair() {
    eval "writer=\$writer_$1 reader=\$reader_$2"
    d=$tmp/pair-$1-$2
    mkdir "$d"
    timeout 20 "$reader" --dir "$d" --count 5 --timeout 100 >"$d/reader.out" 2>"$d/reader.err" &
    pid=$!
    i=0
    until grep -q '^waiting for the writer$' "$d/reader.out"; do
        i=$((i + 1))
        [ $i -le 200 ] || fail "$2 reader did not start"
        sleep 0.05
    done
    timeout 20 "$writer" --dir "$d" --count 10 --rate 20 >"$d/writer.out" 2>"$d/writer.err" \
        || fail "$1 writer failed"
    wait $pid || fail "$1 -> $2: reader failed"
    grep '^seq=' "$d/reader.out" >"$d/values"
    [ "$(wc -l <"$d/values")" -eq 5 ] || fail "$1 -> $2: expected 5 values"
    ! grep -vxF -f "$d/writer.out" "$d/values" >/dev/null || fail "$1 -> $2: values differ"
    sort -cu -t= -k2,2n "$d/values" 2>/dev/null || fail "$1 -> $2: values out of order"
    echo "$1 -> $2: ok"
}

for w in c py cs; do
    for r in c py cs; do
        pair $w $r
    done
done
pair standalone standalone

# A reader started after the writer is gone gets its last value, then reports it gone.
d=$tmp/gone
mkdir "$d"
"$writer_c" --dir "$d" --count 3 >"$d/writer.out"
for r in c py cs; do
    eval "reader=\$reader_$r"
    rc=0
    timeout 1 "$reader" --dir "$d" --timeout 100 >"$d/$r.out" 2>&1 || rc=$?
    [ $rc -eq 124 ] || fail "$r reader: exit status $rc"
    [ "$(head -n 1 "$d/$r.out")" = "$(tail -n 1 "$d/writer.out")" ] || fail "$r reader: wrong last value"
    grep -q '^writer gone: last value [0-9.]* s old$' "$d/$r.out" || fail "$r reader: no 'writer gone'"
done
echo "writer gone: ok"

# A writer that is alive but not publishing is stale.
d=$tmp/stale
mkdir "$d"
"$writer_c" --dir "$d" --count 2 --rate 0.25 >"$d/writer.out" &
pid=$!
sleep 0.2
for r in c py cs; do
    eval "reader=\$reader_$r"
    rc=0
    timeout 1 "$reader" --dir "$d" --timeout 100 >"$d/$r.out" 2>&1 || rc=$?
    [ $rc -eq 124 ] || fail "$r reader: exit status $rc"
    grep -q '^stale: last value [0-9.]* s old$' "$d/$r.out" || fail "$r reader: no 'stale'"
done
wait $pid
echo "stale: ok"

# A channel with another payload type is refused; the example writers replace it.
d=$tmp/mismatch
mkdir "$d"
"$build/tests/interop_helper" write "$d" motor --payload-type 0x00020001 1 >/dev/null
for r in c py cs; do
    eval "reader=\$reader_$r"
    rc=0
    timeout 5 "$reader" --dir "$d" >"$d/$r.out" 2>"$d/$r.err" || rc=$?
    [ $rc -eq 1 ] || fail "$r reader: exit status $rc"
    grep -q 'motor: payload type 0x00020001, expected 0x00010001 (MOTOR_STATUS_V1)$' "$d/$r.err" \
        || fail "$r reader: no payload type error"
done
"$writer_py" --dir "$d" --count 1 >"$d/writer.out"
timeout 5 "$reader_cs" --dir "$d" --count 1 >"$d/cs.out"
cmp -s "$d/writer.out" "$d/cs.out" || fail "recreated channel: wrong value"
echo "payload type mismatch: ok"
