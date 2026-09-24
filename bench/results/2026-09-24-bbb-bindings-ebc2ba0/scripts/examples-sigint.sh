#!/bin/bash
# Each example reader against a writer in another language, then Ctrl-C
# (SIGINT) to the reader and to a running writer; prints output and exit status.
set -m  # job control: background jobs keep the default SIGINT disposition, as from a terminal
B=$HOME/bbb-bindings
export PATH=$B/venv/bin:$PATH
d=$(mktemp -d /dev/shm/sigint.XXXXXX)
trap 'rm -rf "$d"' EXIT
set -- "c $B/build/examples/c/motor_reader" "py $B/repo/examples/python/motor_reader.py" "cs $B/cs/MotorReader/MotorReader"
writers="cs:$B/cs/MotorWriter/MotorWriter c:$B/build/examples/c/motor_writer py:$B/repo/examples/python/motor_writer.py"
for r in "$@"; do
    lang=${r%% *}; reader=${r#* }
    w=$(echo $writers | cut -d' ' -f1); writers="$(echo $writers | cut -d' ' -f2-) $w"
    wl=${w%%:*}; writer=${w#*:}
    echo "=== $wl writer (--count 3) -> $lang reader, then SIGINT to the reader"
    $reader --dir "$d" > "$d/r.out" 2>&1 &
    pid=$!
    sleep 1.5
    $writer --dir "$d" --count 3 > "$d/w.out" 2>&1; echo "writer exit status: $?, printed $(grep -c "^seq=" "$d/w.out") values"
    sleep 1
    kill -INT $pid; wait $pid; echo "reader exit status after SIGINT: $?"
    cat "$d/r.out"
done
for w in "c $B/build/examples/c/motor_writer" "py $B/repo/examples/python/motor_writer.py" "cs $B/cs/MotorWriter/MotorWriter"; do
    lang=${w%% *}; writer=${w#* }
    $writer --dir "$d" > "$d/w.out" 2>&1 &
    pid=$!
    sleep 2
    kill -INT $pid; wait $pid; echo "=== $lang writer, SIGINT after 2 s: exit status $?, $(grep -c '^seq=' "$d/w.out") values, last: $(tail -n 1 "$d/w.out")"
done
