# Interop suite

C, Python and C# on either side of a channel, each in its own process. One
agent program per language speaks the protocol below through its binding and
nothing else; a pytest driver (`test_interop.py`) runs every writer against
every reader.

| Agent | Program |
|---|---|
| C | `tests/interop_helper` from the build directory |
| Python | `agent_py.py`, run with the installed `ps_msgr` wheel |
| C# | `agent_cs/`, published with Native AOT |

`check.sh [build-dir]` (default `build/release`) runs the suite in the build
container after the release preset: it builds the C agent, installs the
wheel into a scratch directory, publishes the C# agent to
`<build-dir>/interop/agent_cs/`, runs pytest, then `ruff` and
`dotnet format`.

```
docker/run.sh cmake --workflow --preset release
docker/run.sh interop/check.sh
```

## Payload

Every value is a `motor_status`, the struct of `spec/c-api.md`, mirrored in
each agent:

| Field | Type | Offset |
|---|---|---|
| `sequence` | `uint64_t` | 0 |
| `speed_rpm` | `float` | 8 |
| `current_a` | `float` | 12 |
| `temperature_c` | `float` | 16 |

24 bytes: 4 bytes of zero padding follow `temperature_c`. The payload type is
`0x00010001` (schema 1, version 1).

The agents derive the value for sequence `n` from `n` alone, so that a
reader can check any value it gets:

```
speed_rpm     = (n mod 65536) / 2
current_a     = (n mod 1024) / 64
temperature_c = (n mod 2048) / 8 - 40
```

Each result is exact in binary32, so the three languages produce the same
bytes.

## Protocol

An agent runs one command from its arguments and exits, or, without
arguments, runs one command per line of stdin until EOF (a *session*). A
session keeps one reader handle per `DIR NAME` open until it ends, so
`attached` and the reader's generation history carry across commands.
`write` is not available in a session. Arguments contain no whitespace.
Numbers are decimal, or hexadecimal with `0x`.

Each result is one JSON object on one line of stdout, flushed at once.

| Command | Results |
|---|---|
| `payload-layout` | `{"sizeof":24,"offsets":{"sequence":0,"speed_rpm":8,"current_a":12,"temperature_c":16},"payload_type":65537}` |
| `write DIR NAME [OPTION...] SEQUENCE...` | `{"opened":true}`, then `{"generation":G,"sequence":N}` per publish |
| `read DIR NAME` | a *read result* |
| `read DIR NAME --for MS` | read results, then `{"reads":R,"busy":B}` |
| `wait DIR NAME LAST_GENERATION TIMEOUT_MS` | `{"changed":true}`, or `{"changed":false}` on timeout |
| `alive DIR NAME` | `{"alive":true\|false}` |

- `write` opens a writer on channel `NAME` in directory `DIR` and publishes
  the value of each `SEQUENCE` in order. Options:
  - `--capacity N` (default 24) and `--payload-type T` (default
    `0x00010001`)
  - `--recreate` and `--no-notify`: `PSMSGR_STATE_RECREATE` and
    `PSMSGR_STATE_NO_NOTIFY`
  - `--rate HZ`: publish value *i* at *i*/`HZ` seconds after the open,
    instead of as fast as possible
  - `--hold`: after the last publish, keep the writer open until stdin
    reaches EOF

  Without `--hold` the writer closes after the last publish.
- A read result is `{"nodata":true}`, or

  ```
  {"generation":G,"length":L,"timestamp_ns":T,"attached":A,"payload_type":P,"value":V,"hex":"..."}
  ```

  from one read and a `describe` right after it. `value` is the decoded
  struct (`{"sequence":N,"speed_rpm":F,"current_a":F,"temperature_c":F}`)
  when `length` is 24, else `null`. `hex` is the payload, in lowercase hex.
  The C agent reads into a 4 KiB buffer.
- `read --for MS` reads in a loop for `MS` milliseconds. It prints each
  result that differs from the one it printed before, never a `BUSY`
  result, and ends with the number of reads and of `BUSY` results.
- `wait` passes `TIMEOUT_MS` to the binding; a negative value waits
  indefinitely.
- A call that fails prints `{"error":CODE,"exception":E}`, where `CODE` is
  the `PSMSGR_E_*` value and `E` the binding's mapping: `null` for C, the
  exception class name for Python and C# (for C#, `CODE` is its
  `PsMsgrError`).

Exit status: 0, or 1 if the command (outside a session) printed an error, or
2 on a usage error (with a message on stderr).

## Scenarios

`test_interop.py` runs each scenario with every writer and reader language,
the same one included (9 pairs), unless noted. Every test uses its own
temporary directory as the channel directory.

1. **Basic** (`test_basic`): three values; the reader gets the last one
   bit-exact, with its generation, length, payload type and a timestamp from
   the publish.
2. **Lazy attach** (`test_lazy_attach`): the reader starts first and gets
   `nodata`; once the writer publishes, it gets the value with `attached`
   exactly once.
3. **Wait/notify** (`test_wait_notify`): a reader blocked in `wait` is woken
   by a new writer's publish. `wait` on a `NO_NOTIFY` channel gives `NOTSUP`.
4. **Recreate** (`test_recreate`): a writer reopens the channel with another
   capacity and payload type and `RECREATE`; the attached reader reattaches
   (`attached`), sees the new payload type, and the generation continues.
5. **Writer death** (`test_writer_death`): after a `SIGKILL`, the reader
   reports the writer dead and keeps the last value. A writer in the next
   language reuses the file: the generation continues and the reader stays
   attached.
6. **Continuous** (`test_continuous`, per writer language): 2000 values at
   1 kHz, with a polling (`read --for`) and a waiting reader (a session
   alternating `wait` and `read`) in each language at once, so each run
   covers three pairs. Every value decodes consistently, matches the
   writer's generation for its sequence, and generations never decrease per
   reader. `BUSY` is counted, not failed. The waiting readers end on the
   writer's last generation.
7. **Errors** (`test_errors`, per language): a second writer while another
   language's writer holds the channel gets `WRITER_EXISTS`, and a different
   capacity without `RECREATE` gets `MISMATCH`, each in the binding's
   mapping.

`test_payload_layout` checks that the three agents agree on the struct's
size, offsets and payload type.
