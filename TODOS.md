# FLINT Automatic Memory — TODOs

Status: **implemented (v0.3.0-pre.2)**. Auto mode now has real automatic memory:
a conservative, non-moving mark-sweep collector plus a first-fit free-list
allocator (`src/flint_gc.c`), wired into `alloc`/`free`/`gc`, the `.fbc`
image path, and the standalone ELF stub. The `0.3.0-pre.2` patch adds a
stack/heap-frontier guard for `push`/`call` (see below). Manual mode is
otherwise unchanged.

## Evidence (v0.3.0)

- A base64 encoder that allocates a per-chunk scratch block with
  `alloc` inside `enter`/`leave` (`examples/b64-mem`): 1 MiB passes in
  `flint run`, `.fbc`, and standalone ELF; output == `base64 -w0`.
  - `--gc-stats` on 1 MiB random: 68 collections, live ≤ ~28 KiB
    (bounded) while 2.45 MB was handed out and 2.43 MB reclaimed
    (~36 KB allocated between collections ≈ the 32 KiB threshold).
  - 16 MiB run: 1092 collections, live still ~28 KiB — bounded, no RSS
    growth pattern.
- The same program in **manual** mode fails exactly like the pre-GC
  evidence: 10th chunk, 36 KiB of output, rc=251
  (`address out of bounds`).
- `sh tests/gc.sh`: 8 inputs (0/1/2/3 bytes, 16 KiB, 1 MiB random /
  zeros / 0xFF) × 4 execution paths (project, single-file `--memory
  auto`, `.fbc`, isolated ELF) all diff clean against `base64 -w0`,
  plus stress/stats/env checks.
- `build/test_gc`: 82 checks — header layout, split/coalesce, roots
  (registers/stack/data/host/pins), cycles, escaping objects, interior
  pointers, mark-stack overflow slow path, bounded allocation loops,
  stress mode, manual-mode no-ops, and the stack/heap frontier guard.
- **v0.3.0-pre.2 regression:** stack growth is checked against `heap_ptr`.
  A program that allocated most of memory and then pushed 100 words used
  to cross the frontier; in auto mode the collector then reported
  `managed heap corrupt (block header damaged)`, while manual mode
  silently overwrote its bump heap. `test_gc` (auto) and `test_core`
  (manual) now require `FLINT_ERR_STACK` before the first crossing and
  verify that the heap frontier/header is untouched.
- An arena-style mark/restore on `enter`/`leave` is **not** the design.
  In auto mode `enter N` only zeroes the fresh frame (fewer stale
  conservative roots); it reclaims nothing.

## What “real automatic memory” must mean

- [x] Memory is reclaimed because an object is **unreachable**, not because a
  frame ended.
- [x] The programmer does not have to call `free`.
- [x] An object stays valid while any root can reach it.
- [x] Cycles are collectable.
- [x] It works in `flint run`, `.fbc`, and standalone ELF.
- [x] Manual mode remains predictable and unchanged.

## The unavoidable design decision: FLINT is typeless

The VM sees 64-bit words. It cannot tell a pointer from an integer. That
forces one of these models:

| Model | How it works | Escaping objects | Cycles | Effort |
|---|---|---|---|---|
| Frame arena | `enter` marks heap, `leave` restores | No | N/A | Small |
| Reference counting | Header refcount, retain/release | Yes | Leaks | Medium |
| Conservative mark-sweep | Scan roots and heap words conservatively, non-moving | Yes | Yes | Medium/large |
| Precise tracing GC | Stack maps/type tags, exact roots | Yes | Yes | Large |
| Moving/generational GC | Copy/compact, update all roots | Yes | Yes | Very large |

For FLINT’s raw-pointer, typeless design, the shortest path to **real**
automatic memory is (and what v0.3.0 implements):

**Conservative, non-moving mark-sweep GC + a real free-list allocator.**

Why non-moving: FLINT exposes raw addresses. A moving collector would have
to rewrite every pointer in registers, stack, globals, and heap objects.
With conservative roots, you cannot safely distinguish real pointers from
integers that look like pointers, so moving is unsafe without a major
language change.

Why mark-sweep: it handles cycles. Reference counting alone does not.

Why a free-list: the current bump allocator cannot reuse holes. Marking
garbage without reusable free blocks does not solve exhaustion.

## What already exists

- [x] `memory = "manual" | "auto"` in `Flint.toml` — now a real allocator
      switch (`.fbc` images and ELF stubs carry it)
- [x] `--memory` flag in `flint new`; `flint run --memory auto|manual`
- [x] `OP_ALLOC` / `OP_FREE`
- [x] `enter` / `leave` frames
- [x] `heap_start`, `heap_ptr`, `sp`, `fp`
- [x] stack/heap frontier guard for `push`/`pushi`/`call`/`enter`
      (`0.3.0-pre.2`; traps `FLINT_ERR_STACK` at `heap_ptr`)
- [x] `.fbc` image path
- [x] ELF stub path
- [x] basic heap tests and examples
- [x] managed object header (16 bytes: size, flags, reserved `next`, magic)
- [x] first-fit free list over the block sequence (split / coalesce / shrink)
- [x] root scanning (registers, stack, data, host roots, pins)
- [x] mark worklist + overflow slow path
- [x] sweep, cycles, escaping objects
- [x] GC triggers, safepoints, `FLINT_ERR_OOM`, stats, stress mode
- [x] `gc` opcode; `flint_gc_*` host API
- [x] tests: `tests/test_gc.c` + `tests/gc.sh` (+ `test_heap.c` split
      manual/auto expectations)

## What is missing

- [ ] Precise (non-conservative) root information — stack maps / tags
- [ ] Interior-pointer page map / block index (the containment walk is
      correct but O(blocks) per miss; a cache covers the hot case)
- [ ] Pause-time stats (zero-libc core has no clock; hosts can time
      `flint_gc_collect` themselves)
- [ ] Frame-pointer-chain walking to skip dead stack regions (full stack
      scan is the current conservative baseline)
- [ ] Everything else in Phase 7 (advanced work, after the MVP)

## Implementation notes (decisions taken while building the MVP)

- **Candidates = containment.** A word retains a block when it lands
  anywhere inside the payload of an *allocated* block (interior pointers
  count; block-start-only was the MVP alternative — containment is the
  "better" option from Phase 0 and costs one walk).
- **Zeroing everywhere it helps.** Payloads (alloc), fresh frames
  (`enter N`, auto mode), and the whole scratch region at init
  (`flint_heap_set` → `flint_gc_init`). Fewer stale conservative roots,
  deterministic reads.
- **`next` is reserved.** Free blocks are found by walking the block
  sequence in address order (first-fit semantics; coalescing during
  sweep); no separate chain to keep consistent.
- **Mark stack 1024.** Overflow is correct via repeated full rescans of
  marked blocks (`overflow_rescans` stat counts the slow passes).
- **`free` opcode stays a no-op in both modes**; the collector owns
  lifetime in auto. `flint_gc_free` is the validated explicit release
  for hosts (invalid/interior/double free → `FLINT_ERR_BADMEM`).
- **Known conservative limits** (documented in `flint.h`/README):
  false retention (integers/pcs that look like addresses — bounded by
  root-word count); pointers stored at unaligned addresses are missed
  (aligned-word scanning); writing outside a block can corrupt headers
  (traps `FLINT_ERR_BADMEM` at the next collector walk).

## Roadmap

### Phase 0 — Lock the semantics

- [x] Decide: **conservative non-moving mark-sweep** for the first real
      implementation.
- [x] Define a root precisely: a word that may point into the managed heap
      (any offset inside an allocated payload).
- [x] Define roots:
  - [x] registers `r0..r15`
  - [x] active stack range `[sp, mem_size)`
  - [x] data/global range `[0, heap_start)`
  - [x] host-registered roots/pins
- [x] Define object layout:
  - [x] header before payload (16 bytes)
  - [x] payload size
  - [x] mark/free flags
  - [x] alignment (blocks 16-aligned base, payloads 8-aligned, sizes 8-multiples)
  - [x] magic/validation field (`"FLGM"`)
- [x] Define `alloc` in auto mode:
  - [x] returns a payload pointer after the header
  - [x] zeroes the payload (chosen: zeroing over documenting false retention)
- [x] Define `free` in auto mode:
  - [x] recommended: no-op; GC owns lifetime (opcode keeps this)
  - [x] optional: explicit release with double-free/invalid-free traps
        (`flint_gc_free`, host API)
- [x] Define interior-pointer policy:
  - [x] MVP: only block-start pointers count as roots
  - [x] better: page map or block lookup so `buf+offset` also marks the
        block — implemented as a containment walk with a one-entry cache
- [x] Define GC safepoints:
  - [x] allocation failure
  - [x] allocation threshold (default 32 KiB of payload bytes)
  - [x] explicit `gc` opcode or host `flint_gc_collect()`
  - [x] not in the middle of an instruction with unrooted C temporaries
        (collection only runs at the top of `alloc` / the `gc` opcode)
  - [x] not during a host syscall (collector never runs there; hosts pin
        payloads they retain)
- [x] Define OOM behavior and a new error code: `FLINT_ERR_OOM` = 10.
- [x] Version bump: **0.3.0-pre.2** (auto mode is a breaking semantic change;
      pre.2 fixes the stack/heap collision).
- [x] Document that auto mode is a breaking semantic change.

### Phase 1 — Managed heap and allocator

- [x] Add a managed heap layout inside the existing `mem`:
  - [x] `heap_begin` = first managed block (heap_start rounded up to 16)
  - [x] `heap_ptr` = bump frontier
  - [x] stack still grows down
  - [x] bump still checks `heap_ptr + header + payload <= sp`
- [x] Add `flint_gc_header`: size / flags / `next` (reserved) / magic
- [x] Add free-list management (over the block sequence):
  - [x] first-fit
  - [x] split large blocks
  - [x] coalesce adjacent free blocks
  - [x] shrink `heap_ptr` if the top block is free
- [x] Implement `flint_gc_alloc(vm, size)`:
  - [x] try free list
  - [x] try bump
  - [x] trigger GC and retry
  - [x] trap OOM if still no space
- [x] Implement `flint_gc_free(vm, ptr)` for explicit free
- [x] Add stats: bytes allocated / live / free / collections / reclaimed /
      overflow rescans (pause time: no clock in the core — see “missing”)
- [x] Add poisoning of freed payloads in stress mode (0xDE)

### Phase 2 — Roots

- [x] Scan all 16 registers conservatively.
- [x] Scan the active stack range.
- [ ] Optionally walk the `fp` chain to reduce scanning of stale slots
      (deferred; full scan is the baseline)
- [x] Scan the data/global range.
- [x] Zero the stack at init and zero new frames to reduce false roots.
- [x] Validate candidate pointers:
  - [x] inside heap bounds
  - [x] inside an allocated block payload (header magic/size are validated
        by the structural walk; alignment is not required of candidates —
        byte-walked pointers are often interior)
  - [x] not in code or stack
- [x] Handle interior pointers: containment lookup marks the block
- [x] Add host root API: `flint_gc_root` / `flint_gc_unroot` /
      `flint_gc_pin` / `flint_gc_unpin`
- [x] Document that host code must not retain raw heap pointers without
      pinning.

### Phase 3 — Collector

- [x] Add a mark bitmap or header mark bit (header flag bit).
- [x] Implement an explicit mark worklist:
  - [x] no recursive marking; avoid C stack overflow
  - [x] fixed-size mark stack (1024 blocks, lives in `flint_vm`)
  - [x] handle mark-stack overflow explicitly (full-rescan until fixpoint)
- [x] Mark from roots: registers / stack / data / host roots
- [x] Scan marked object payloads conservatively (aligned words; mark the
      containing block on containment)
- [x] Sweep:
  - [x] walk from `heap_begin` to `heap_ptr`
  - [x] unmarked allocated blocks go to the free pool
  - [x] clear marks
  - [x] coalesce adjacent free blocks
  - [x] update stats (recount anchors incremental bookkeeping)
- [x] Handle cycles correctly.
- [x] Keep the collector non-moving.
- [x] Add `flint_gc_collect(vm)` public API.
- [x] Add stress mode: collect on every allocation; output must remain
      correct (verified by `tests/gc.sh` and `test_gc`).

### Phase 4 — VM integration

- [x] Branch `OP_ALLOC` on `mem_mode`:
  - [x] manual: current bump behavior
  - [x] auto: `flint_gc_alloc`
- [x] Branch `OP_FREE`:
  - [x] manual: current no-op
  - [x] auto: no-op (validated operand; collector owns lifetime)
- [x] Trigger GC from `OP_ALLOC` when needed.
- [x] Ensure GC never runs with unrooted interpreter temporaries.
- [x] Ensure GC does not run during `sys` calls unless buffers are pinned.
- [x] Update `flint_init` and `flint_heap_set` to initialize GC state.
- [x] Add `flint_gc_stats` and `flint_gc_set_threshold`.
- [x] Add `flint_gc_set_stress`.
- [x] Rebuild `build/stub.elf`; standalone ELF embeds the collector.
- [x] Keep `.fbc` mode field; no image format change (version 1 header
      already carried mem_mode).
- [x] Add CLI flags/env:
  - [x] `--gc-stats`
  - [x] `--gc-stress`
  - [x] `FLINT_GC_STRESS=1` (also `FLINT_GC_STATS=1`)
- [x] Bump version and document the change.

### Phase 5 — Language and ergonomics

- [x] `alloc` in auto mode is the managed allocation primitive.
- [x] `alloc` returns a managed payload; `free` is optional.
- [x] Add a `gc` opcode (and host `flint_gc_collect`) for explicit collection.
- [x] Add an example that allocates in a loop without `free`
      (`examples/b64-mem`, run by `tests/gc.sh`).
- [x] Add an example where an object escapes a frame and survives
      (`tests/test_gc.c`, `S_escape`).
- [x] Add an example with a cycle that is collected (`tests/test_gc.c`,
      `S_gccycle`).
- [x] Document that `enter`/`leave` are not required for GC correctness.
- [x] Document that frame-scoped arenas are a different, weaker model.
- [x] If you want Python-like object creation, that is a higher-level
      language feature, not just a GC: objects, types, constructors,
      exceptions, closures, etc. (out of scope, unchanged)

### Phase 6 — Tests and acceptance

Unit tests (`tests/test_gc.c`):

- [x] header creation and validation
- [x] free-list split/coalesce
- [x] allocation reuse
- [x] OOM after GC
- [x] double free / invalid free (explicit free traps)
- [x] mark/sweep of a simple graph
- [x] unreachable object collected
- [x] reachable object retained
- [x] cycle with no roots collected
- [x] object reachable only from another heap object retained
- [x] object reachable only from a register retained
- [x] object reachable only from the stack retained
- [x] object reachable from data/global retained
- [x] escaping object survives `leave`
- [x] interior pointer policy test
- [x] false-pointer retention documented and bounded
- [x] mark-stack overflow
- [x] stress mode
- [x] stats correctness

Integration tests (`tests/gc.sh`):

- [x] base64 with per-chunk `alloc` inside `enter`/`leave`
  - [x] 0, 1, 2, 3 bytes
  - [x] 16 KiB
  - [x] 1 MiB random
  - [x] 1 MiB zeros
  - [x] 1 MiB `0xFF`
  - [x] compare with `base64 -w0`
- [x] same in `flint run`, `.fbc`, and standalone ELF
- [x] run ELF from an isolated directory (`$TMPDIR`/`/tmp`) without
      sources, stub, or lib
- [x] manual mode still behaves exactly as before
- [x] `make test` all pass
- [x] examples still run
- [x] no unbounded RSS growth over a long allocation loop (live bounded;
      16 MiB spot-check)
- [x] GC stress does not change output
- [x] deterministic results

Acceptance criteria for “real automatic”:

- [x] An auto program can allocate in a loop without `free` and finish in
      bounded memory.
- [x] An object stored in a global or returned from a frame survives
      collection.
- [x] An unreachable cycle is reclaimed.
- [x] A pointer remains valid while reachable, regardless of frame exit.
- [x] Base64 1 MiB passes in `flint run`, `.fbc`, and ELF.
- [x] Manual mode is unchanged.
- [x] `make test` and stress mode pass.

### Phase 7 — Advanced work, after the MVP

- [ ] Precise stack maps emitted by the assembler.
- [ ] Frame descriptors describing which slots are pointers.
- [ ] Tagged values or boxed objects.
- [ ] Interior-pointer page map / block index (beyond the one-entry cache).
- [ ] Moving or compacting GC with handles/forwarding.
- [ ] Generational GC with write barriers on `store`/`storx`.
- [ ] Incremental GC and card tables.
- [ ] Finalizers and weak references.
- [ ] Thread-aware GC and atomics.
- [ ] Heap growth/shrink policy.
- [ ] Size classes, allocation fast paths, TLAB-like areas.
- [ ] Escape analysis and stack allocation.
- [ ] Heap visualizer and GC tracing.

## Shortest path to a real MVP

1. [x] Choose conservative, non-moving mark-sweep.
2. [x] Add object headers and a free-list allocator.
3. [x] Add conservative root scanning for registers, stack, and data.
4. [x] Add mark and sweep, including cycles.
5. [x] Trigger GC on allocation failure/threshold and add
   `flint_gc_collect()`.
6. [x] Wire `mem_mode == auto` to the GC allocator and rebuild the ELF stub.
7. [x] Add escaping-object, cycle, base64, `.fbc`, ELF, and stress tests.
8. [x] Update README and version.

Do not call it done until the base64 test passes **and** an escaping object
survives **and** an unreachable cycle is collected. Base64 alone can pass
with a simple arena, so it is not sufficient proof of real automatic
memory.

Status: all three hold. The base64 test passes in all three execution
paths (not merely with an arena — `S_escape` and `S_gccycle` in
`test_gc.c` exercise the real reachability semantics), and manual mode
is differential-tested against the old bump behavior.

## What not to do

- Do not just mark/restore on `enter`/`leave` and call it automatic memory.
  That is an arena. — **Followed:** `enter` only zeroes frames in auto
  mode; reclamation is reachability-based.
- Do not implement a moving GC before you have handles, tags, or precise
  roots. — **Followed:** non-moving.
- Do not mark recursively; use an explicit worklist. — **Followed.**
- Do not trigger GC in the middle of an instruction with unrooted
  temporaries. — **Followed:** collection only at the top of `alloc` and
  in the `gc` opcode.
- Do not let `free` be the only reclamation mechanism; forgotten frees and
  cycles will leak. — **Followed:** the collector reclaims automatically;
  `free` is a no-op in the language.
- Do not scan code as data. — **Followed:** the code image is separate
  from `mem`.
- Do not assume a word is a pointer without validating range, alignment,
  and header. — **Followed, with one deliberate change:** alignment is
  not required of candidates (byte-walked interior pointers are valid);
  range + allocated-block containment + validated headers do the
  filtering.

## Bottom line

FLINT 0.3.0-pre.2 has a real automatic memory manager: managed object
metadata, a first-fit free-list allocator with split/coalesce/shrink,
conservative root discovery, an explicit mark worklist with an overflow
slow path, sweep with cycle collection, safepoints, stats, stress mode,
a `gc` instruction, a host API, integration through `flint run` /
`.fbc` / ELF, and an acceptance test that pits the collector against
`base64 -w0` on 8 inputs × 4 execution paths. The pre.2 patch closes the
stack/heap collision: `push`/`call`/`enter` cannot cross `heap_ptr`.
Manual mode is otherwise untouched.

The known limits are conservative-GC limits, documented in the README:
false retention by address-looking words, missed unaligned pointer
storage, and header corruption traps if a program writes outside its
blocks. Phase 7 lists the precise-tracing work that would lift them.