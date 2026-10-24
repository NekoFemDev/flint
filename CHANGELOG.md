# Changelog

## 0.3.0-pre.2 — 2026-10-24

### Fixed

- **Stack/heap collision.** `push` and `call` grow the stack through the
  shared `stk_push()` helper, which only checked `sp >= 8`. A program that
  allocated most of memory could drive the stack below `heap_ptr` and
  overwrite the heap. In auto mode this corrupted a managed block header,
  so the next collector walk reported `managed heap corrupt (block header
  damaged)` instead of the real cause. Manual mode had the same gap and
  could silently overwrite its bump arena.
  `stk_push()` now rejects any push whose new `sp` would be below
  `heap_ptr`, trapping `FLINT_ERR_STACK` (−6). This covers `push`, `pushi`,
  the return address of `call`, and `enter` (which already pre-checked its
  full frame).
- Added regression tests for both allocators: `tests/test_gc.c` (auto) and
  `tests/test_core.c` (manual) allocate most of the memory image, push past
  the frontier, and require `FLINT_ERR_STACK` with the heap frontier and
  block header left intact.
- Fixed a stale `flint_init()` comment that still described `mem_mode` as
  informational; `flint_heap_set()` selects the real model.

### Documentation

- `README.md`: documented the stack/heap frontier guard and the pre.2 fix;
  updated the version and stack model description.
- `TODOS.md`: updated status, evidence, and the regression description.
- Added this changelog.

### Compatibility

- No API or bytecode changes. Manual-mode programs that previously pushed
  past `heap_ptr` now trap instead of silently corrupting the bump arena.

## 0.3.0-pre.1 — 2026-10-23

- Automatic memory: conservative, non-moving mark-sweep collector, first-fit
  free-list allocator, managed block headers, `gc` opcode, GC stats/stress
  flags, host root/pin API, and `examples/b64-mem`.
- See the GitHub release notes for the full pre.1 description.
