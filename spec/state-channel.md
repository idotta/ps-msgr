# State channel — format and protocol

Status: **draft**. Format version **1.0**.

A *state channel* publishes the latest value of an opaque payload from one
writer to any number of readers on the same host. Readers always get the most
recent complete value, or nothing. They never get a torn value, and they are
never guaranteed to see every value.

## 1. Model

- **One writer per channel**, enforced system-wide (section 4). A process MAY
  hold writers for different channels.
- **Any number of readers.** Readers map the channel read-only and cannot
  corrupt it.
- The writer rotates through `slot_count` ≥ 2 slots, and each slot is
  protected by its own seqlock. A publish always writes into a slot that is
  not the current `latest`. A reader therefore has to retry only if the
  writer publishes `slot_count - 1` more times while that reader is copying
  one payload.
- Each publish carries a **generation** (u32 change token) and a
  **timestamp** (`CLOCK_MONOTONIC`, ns). Readers can get both without
  copying the payload (`peek`), which is the intended way to decide whether
  and how often to poll.

## 2. Files

Both files live in a directory `dir`:

- An explicit API argument, if given; otherwise
- the environment variable `PSMSGR_DIR`, if set and non-empty; otherwise
- `/dev/shm`.

- `dir` MUST be on tmpfs for the performance properties to hold. The library
  does not check this.
- The library never creates `dir`. A missing `dir` is `PSMSGR_E_SYS`
  (`ENOENT`) for a writer; a reader simply stays unattached. Creating the
  directory is a deployment job: systemd `RuntimeDirectory=` or
  `tmpfiles.d`.
- `PSMSGR_DIR` is read once, when a handle is opened.
- Every `open` uses `O_NOFOLLOW`, and a symlink in place of a channel file is
  an error.
- All writers of a channel MUST run as the same user. `/dev/shm` is
  sticky: only a file's owner can rename over it or unlink it. In addition,
  `fs.protected_regular` makes `O_CREAT` fail on another user's file there.

| File | Purpose |
|---|---|
| `psmsgr.<name>.state` | Channel data, mapped by the writer and readers. |
| `psmsgr.<name>.lock` | Writer lock (section 4). Never replaced or truncated, so its identity is stable. |
| `psmsgr.<name>.state.tmp.XXXXXX` | Transient file, only while the writer creates or recreates the data file. |

`<name>` MUST match `[A-Za-z0-9_-][A-Za-z0-9_.-]{0,63}`: 1 to 64
characters, and not starting with `.`.

All multi-byte fields are **little-endian, native alignment**. The library
MUST refuse to build on big-endian targets.

## 3. Data file layout

```
+----------------------+  offset 0
| channel header       |  header_size bytes (128 in v1.0)
+----------------------+  header_size
| slot 0               |  slot_stride bytes
+----------------------+
| ...                  |
+----------------------+
| slot slot_count-1    |
+----------------------+  file_size = header_size + slot_count * slot_stride
```

### 3.1 Channel header (128 bytes)

| Off | Type | Field | Access | Description |
|---:|---|---|---|---|
| 0 | u32 | `magic` | const | `0x534D5350` (bytes `"PSMS"`) |
| 4 | u16 | `version_major` | const | `1` |
| 6 | u16 | `version_minor` | const | `0` |
| 8 | u32 | `header_size` | const | `128` |
| 12 | u32 | `slot_header_size` | const | `32` |
| 16 | u32 | `slot_count` | const | 2 … 16 |
| 20 | u32 | `slot_stride` | const | `align_up(slot_header_size + capacity, 64)` |
| 24 | u32 | `capacity` | const | Max payload bytes, 0 … 16 MiB. 0 means a heartbeat channel (generation and timestamp only). |
| 28 | u32 | `payload_type` | const | Application-defined tag; 0 means unspecified. |
| 32 | u32 | `config_flags` | const | bit 0 `NO_NOTIFY`: the writer never wakes waiters. Other bits: 0. |
| 36 | u32 | `state` | atomic | bit 0 `RETIRED`: this file was replaced or unlinked, so readers must reattach. Other bits: 0. |
| 40 | u32 | `latest` | atomic | Index of the most recently published slot, or `0xFFFFFFFF` if nothing has been published. |
| 44 | u32 | `notify` | atomic | Futex word, incremented after every publish and on retire. |
| 48 | u32 | `writer_pid` | plain | PID of the last writer to open the channel. Diagnostic only. |
| 52 | u32 | reserved | — | 0 |
| 56 | u64 | `created_realtime_ns` | const | `CLOCK_REALTIME` when the file was created. Diagnostic only. |
| 64 | — | reserved | — | 64 bytes, all 0 |

*const* fields are written before the file becomes visible (section 5.2) and
never change afterwards, so readers validate them once when they attach.

Rules within format major version 1:

- `header_size` is 128 and `slot_header_size` is 32. Both are fixed.
- A new minor version may only give meaning to reserved bytes or reserved
  flag bits, and every such addition MUST be safe for older readers to
  ignore.

### 3.2 Slot (at `header_size + i * slot_stride`)

| Off | Type | Field | Description |
|---:|---|---|---|
| 0 | u32 | `seq` | Seqlock counter: odd = write in progress, even = stable. Atomic. |
| 4 | u32 | `generation` | Generation of the payload in this slot. |
| 8 | u64 | `timestamp_ns` | `CLOCK_MONOTONIC` when the writer committed the payload. |
| 16 | u32 | `length` | Payload length, ≤ `capacity`. |
| 20 | — | reserved | 12 bytes, 0 |
| 32 | u8[] | `data` | Payload bytes. |

The payload is 32-byte aligned within the mapping, and the mapping is
page-aligned. So a writer using the zero-copy API can store `double`s and
other aligned types directly. This matters on ARMv7, where VFP loads and
stores fault on unaligned addresses.

Why these choices:

- All atomics are **32-bit**. ARMv7-A has no single-copy-atomic plain 64-bit
  load, and 32-bit atomics are lock-free everywhere.
- The 64-bit `timestamp_ns` is protected by the slot's seqlock and doesn't
  need to be atomic itself.
- There is no cache-line padding between the hot fields: the primary target
  is single-core. The layout is still correct on SMP, where the padding would
  only have been a performance tweak.

## 4. Writer exclusivity

A writer MUST hold an **open-file-description lock** (`F_OFD_SETLK`,
`F_WRLCK`, whole file) on `psmsgr.<name>.lock` for as long as its handle is
open.

- OFD locks rather than `flock`/POSIX locks: they conflict between two
  handles in the *same* process (POSIX locks don't), and the kernel releases
  them when the process dies, so a crashed writer never leaves the channel
  locked.
- If the lock is already held, opening a writer fails with
  `PSMSGR_E_WRITER_EXISTS`.
- **Identity check.** After acquiring the lock, the writer MUST verify that
  the path still names the inode it locked (`fstat(fd)` against
  `stat(path)`, comparing `st_dev` and `st_ino`). If they differ, it closes
  the descriptor and starts over. Without this check, `unlink` (section 7)
  could leave two writers: B opens the old lock file, A unlinks it and
  releases, B then locks the orphaned inode while C creates and locks a new
  one.
- **Writer liveness** from a reader: `F_OFD_GETLK` with `F_WRLCK` on a
  read-only descriptor of the lock file. It reports a conflicting lock
  without acquiring one, so checking liveness can never make a starting
  writer fail. This costs a syscall, so it is not on the hot path.
- The lock file is created with the channel `mode` and never deleted by the
  library, except by `unlink` (section 7).

## 5. Writer protocol

### 5.1 Open

1. Validate the name and options.
2. Open or create the lock file (`O_RDWR|O_CREAT|O_CLOEXEC`) and take the OFD
   write lock (section 4).
3. Delete any leftover `psmsgr.<name>.state.tmp.*` files. Holding the lock
   guarantees none of them is in use.
4. Open the data file `O_RDWR|O_CLOEXEC`.
   - **Missing** → *create* (5.2).
   - **Present** → map it and validate it, then take the first case that
     applies:
     - **Invalid** (it fails the reader validation in 6.1, e.g. bad magic,
       an unknown major version, or out-of-range fields) → *create* (5.2)
       if `RECREATE` is requested, else fail with `PSMSGR_E_FORMAT`.
     - **Different format version** (same major, different minor) →
       *create* (5.2), automatically. Attached readers follow through the
       retire, so upgrading the library needs no flag and no reboot.
     - **Different geometry**: `capacity`, `slot_count`, `payload_type` or
       `config_flags` differ from the request → *create* (5.2) if
       `RECREATE` is requested, else fail with `PSMSGR_E_MISMATCH`. A
       different geometry is an application decision, so it is never
       replaced silently.
     - Otherwise, **compatible** → *reuse* (5.3).
5. Set `writer_pid`.

### 5.2 Create

1. `mkostemp` a `psmsgr.<name>.state.tmp.XXXXXX` file in `dir`, then
   `fchmod(mode)`. Using `fchmod` makes the mode independent of the umask.
2. `posix_fallocate(0, file_size)`. **This is mandatory.** tmpfs files are
   sparse, and a store to an unbacked page on a full tmpfs raises `SIGBUS`
   in *every* process that maps it. `fallocate` turns that into an
   `ENOSPC` error at open time instead.
3. Map it, fill in the header (`latest = 0xFFFFFFFF`, `notify = 0`,
   `state = 0`, all slots zeroed), then `rename(2)` it over
   `psmsgr.<name>.state`. Readers therefore only ever open fully
   initialized files.
4. If an old data file was replaced and it has at least 128 bytes and a
   valid magic: set `RETIRED` in the old header's `state` (release),
   increment its `notify`, `FUTEX_WAKE` all waiters on it, then unmap it.
5. If the old file was readable and compatible in magic and major version,
   carry its generation over (5.3). Otherwise start at 1.

### 5.3 Reuse

- Any slot left with an odd `seq` by a writer that crashed mid-publish is
  reset to `seq + 1` (even). Such a slot is never `latest`, because `latest`
  is only updated after the slot is committed.
- The writer's next generation is `slots[latest].generation + 1`, or 1 if
  `latest == 0xFFFFFFFF`.

Readers that are already attached keep working through a writer restart: the
file and its mapping stay the same.

### 5.4 Publish

In C11 atomics, where `W` is the writer's private state:

```c
if (len > capacity) return PSMSGR_E_TOOBIG;
uint32_t i = (W.latest == NONE) ? 0 : (W.latest + 1) % slot_count;
slot *s = &slots[i];

uint32_t q = atomic_load_explicit(&s->seq, relaxed);        // even
atomic_store_explicit(&s->seq, q + 1, relaxed);             // odd: writing
atomic_thread_fence(memory_order_release);

s->generation   = W.gen;
s->length       = len;
memcpy(s->data, data, len);
s->timestamp_ns = clock_gettime_ns(CLOCK_MONOTONIC);      // commit time

atomic_store_explicit(&s->seq, q + 2, release);             // even: stable
atomic_store_explicit(&hdr->latest, i, release);
W.latest = i;

if (!(config_flags & NO_NOTIFY)) {
    atomic_fetch_add_explicit(&hdr->notify, 1, release);
    futex(&hdr->notify, FUTEX_WAKE, INT_MAX);               // shared futex, not _PRIVATE
}
W.gen = (W.gen == UINT32_MAX) ? 1 : W.gen + 1;             // 0 is never a valid generation
```

- The zero-copy variant (`begin` / `commit` / `abort`) splits this at the
  `memcpy`:
  - `begin` performs everything up to the release fence and returns
    `s->data`.
  - `commit(len)` writes the slot header fields and does the rest.
  - `abort` stores `q + 2` without updating `latest`.
- While a `begin` is open, other readers are unaffected: the slot being
  written is never `latest`.
- `publish` with `len == 0` is valid. On a `capacity == 0` channel it is the
  only valid length, which makes a heartbeat channel.
- The payload copy is a data race in the C11 sense, as in any seqlock. It is
  sound in practice because the fences are full compiler barriers and emit
  `dmb ish` on ARMv7. The implementation MUST keep the copy strictly between
  the fences, e.g. an out-of-line copy routine, and MUST annotate the copy
  for ThreadSanitizer.

### 5.5 Close

An open `begin` is aborted first. The writer then releases the lock and
unmaps the file. It does **not** delete the data file: readers keep the last value (and can see from `peek` and
`writer_alive` that it is aging), and the next writer reuses the file.

## 6. Reader protocol

### 6.1 Attach (lazy)

Opening a reader always succeeds when the name is valid, whether or not the
channel exists. This makes process start-up order irrelevant, which matters
for systemd units that start in parallel. An unattached reader tries to
attach on every `read` / `peek` / `describe` and while it waits.

To attach:

1. `open(O_RDONLY|O_CLOEXEC)`, then `fstat`. If the file is missing, return
   `PSMSGR_E_NODATA` and stay unattached.
2. Check `st_size >= 128`, then map the header and validate it. The checks
   are:
   - `magic`
   - `version_major == 1` (any minor)
   - `header_size == 128` and `slot_header_size == 32`
   - `2 <= slot_count <= 16`
   - `capacity <= 16 MiB`
   - `slot_stride == align_up(32 + capacity, 64)`
   - `file_size`, computed in 64-bit arithmetic, `<= st_size`

   Anything wrong → `PSMSGR_E_FORMAT`, and stay unattached. A reader never
   trusts a header field it hasn't range-checked.
3. `mmap(file_size, PROT_READ, MAP_SHARED | MAP_POPULATE)`. `MAP_POPULATE`
   pre-faults the pages, so the first read isn't slowed by page faults.
   Then remember `st_dev` and `st_ino`, and **close the descriptor**: an
   attached reader holds no file descriptors.
4. Cache the const header fields. The first `read` / `peek` result from a
   newly attached file carries `PSMSGR_INFO_ATTACHED` in `info.flags`, which
   tells the caller to re-check `describe()` (capacity, `payload_type`).

### 6.2 Retire check

Before every `read` / `peek`: if `atomic_load(&hdr->state, acquire) &
RETIRED`, unmap and attach again. This costs one load while attached, and
needs no syscalls until a retire actually happens.

**Orphaned files.** `RETIRED` covers replacements made by the library. It
does not cover a data file deleted behind the library's back, e.g. by `rm`
or by systemd `RemoveIPC`. In that case the reader keeps a mapping of a
dead file while a new writer creates a new one, and nothing in shared
memory tells the reader. So readers also do an **identity check**: `stat`
the path, compare `st_dev`/`st_ino` with the mapped file, and reattach if
they differ (or go unattached if the path is gone). The check costs a
syscall and runs only:

- in every `writer_alive` call, which is what a poller calls anyway when it
  sees data aging; and
- in `wait`, at least once per second of waiting (6.6).

`read` and `peek` never do it.

### 6.3 Read

```c
for (int attempt = 0; attempt < READ_RETRIES; ++attempt) {   // READ_RETRIES = 64
    uint32_t i = atomic_load_explicit(&hdr->latest, acquire);
    if (i == NONE) return PSMSGR_E_NODATA;
    if (i >= slot_count) return PSMSGR_E_FORMAT;
    slot *s = &slots[i];

    uint32_t q1 = atomic_load_explicit(&s->seq, acquire);
    if (!(q1 & 1)) {
        uint32_t gen = s->generation, len = s->length;
        uint64_t ts  = s->timestamp_ns;
        bool fits    = len <= size;
        if (len <= capacity && fits) memcpy(buf, s->data, len);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&s->seq, relaxed) == q1) {
            if (len > capacity) return PSMSGR_E_FORMAT;
            *info = (info){gen, len, ts, attached_flag()};   // PSMSGR_INFO_ATTACHED once per attach
            return fits ? PSMSGR_OK : PSMSGR_E_TOOSMALL;     // info->length valid either way
        }
    }
    if (attempt >= 3) sched_yield();   // single core: let a preempted writer finish
}
return PSMSGR_E_BUSY;
```

Every attempt re-reads `latest`, so after a retry the reader gets the
*newest* value, not the one it started with.

### 6.4 Peek

Same as the read loop, but it copies no payload and returns
`{generation, length, timestamp_ns}` of the latest value. It makes no
syscalls while attached. This is the primitive meant for controlling polling:

- **Change detection:** compare `generation` with the last one consumed.
- **Freshness:** `now_ns() - timestamp_ns` is the age of the latest value.
  `now_ns()` MUST use `CLOCK_MONOTONIC`, and the C API provides
  `psmsgr_now_ns()` so that bindings use the same clock.
- **Adaptive polling:** a reader that knows the writer's nominal period can
  sleep until `timestamp_ns + period` instead of polling blindly. For a
  stale or dead writer, use `writer_alive` (section 4).

The file's `mtime` is **not** used and is not meaningful: stores through a
shared mapping don't reliably update it, and reading it would take a
`stat()` syscall.

### 6.5 Generation semantics

- The generation is a **change token**. Consumers SHOULD compare it only for
  equality with the last generation they saw, never for ordering (it wraps,
  skipping 0).
- It is monotonic for the lifetime of a data file, and it is carried across
  writer restarts and, when possible, across recreates (5.2).
- 0 is never a valid generation, so callers can use 0 to mean "never seen".

### 6.6 Wait

`wait(last_gen, timeout)` blocks until the latest generation differs from
`last_gen` or the timeout expires. A retire or an orphaned file is handled
inside the loop by reattaching; it is not a return condition:

```c
for (;;) {
    uint32_t n = atomic_load_explicit(&hdr->notify, acquire);  // BEFORE the check
    if (retired) { reattach; continue; }
    if (peek().generation != last_gen) return PSMSGR_OK;       // NODATA counts as "unchanged"
    if (remaining <= 0) return PSMSGR_E_TIMEOUT;
    r = futex(&hdr->notify, FUTEX_WAIT, n, min(remaining, 1 s)); // shared futex
    if (r == -1 && errno == EINTR) return PSMSGR_E_INTR;
    if (r == -1 && errno == ETIMEDOUT) identity_check();         // orphan detection, 6.2
}
```

- Loading `notify` before the generation check prevents lost wake-ups: if a
  publish lands in between, `notify != n`, and `FUTEX_WAIT` returns
  `EAGAIN` immediately.
- There is no waiter count, which keeps reader mappings read-only
  (`FUTEX_WAIT` works on read-only shared mappings). The price is that the
  writer makes one `FUTEX_WAKE` syscall per publish, even when nobody is
  waiting. Channels published at high rates that are only ever polled SHOULD
  be created with `NO_NOTIFY`. On those channels, `wait` returns
  `PSMSGR_E_NOTSUP`.
- An unattached reader waits by retrying the attach every 10 ms until it
  attaches or the timeout expires.
- `timeout`: < 0 means infinite, 0 means check once. Timeouts are measured on
  `CLOCK_MONOTONIC`.

## 7. Unlink

`unlink(name)` requires the writer lock, including the identity check from
section 4, so it fails with `PSMSGR_E_WRITER_EXISTS` while a writer is
active. It then:

1. sets `RETIRED` in the header of the current data file,
2. increments its `notify` and wakes waiters,
3. unlinks the data file and the lock file.

Readers go back to unattached and return `PSMSGR_E_NODATA`.

Reboots clear tmpfs, so there is normally no need to unlink.

## 8. Operational notes (non-normative)

- `/dev/shm` defaults to 50% of RAM (about 250 MiB on the BeagleBone Black).
  `file_size` is roughly `slot_count × capacity`, so size channels
  accordingly.
- **systemd `RemoveIPC=`** (default `yes` in `logind.conf` on Debian)
  deletes POSIX shared memory owned by a *regular* user when that user's
  last session ends. Run channel writers as system users (UID < 1000), or
  set `PSMSGR_DIR` to a `RuntimeDirectory=` with
  `RuntimeDirectoryPreserve=yes`.
- Access control is ordinary file permissions. Use `mode` (default `0644`)
  plus a shared group, for example `0640`.
- **`/dev/shm` is world-writable.** Any local user can create
  `psmsgr.<name>.*` first: that blocks the real writer, and it can also feed
  forged data to readers. On a multi-user system, point `PSMSGR_DIR` at a
  dedicated directory that only the application group can write to, e.g.
  `/run/psmsgr` with mode `2770` created via `RuntimeDirectory=`. On a
  single-purpose BeagleBone, `/dev/shm` is fine.
- Another process can still `ftruncate` a channel file it has write access
  to, and readers then get `SIGBUS`. File permissions are the only
  protection; the library does not guard against this.
- **Timestamps on the AM335x.** The Cortex-A8 has no ARM generic timer, so
  `clock_gettime(CLOCK_MONOTONIC)` probably can't use the vDSO and becomes a
  real syscall. Publishing and `psmsgr_now_ns()` therefore likely cost one
  syscall on the BeagleBone Black; `peek` and `read` don't. `bench/` must
  measure this on the target.
- A reader keeps the last published value after the writer dies. Use
  `timestamp_ns` age and/or `writer_alive` to detect this.
