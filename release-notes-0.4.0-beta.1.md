# FLINT 0.4.0-beta.1

The first beta of the 0.4.0 line — and FLINT can now build **shared
libraries**. `flint build --so` fuses a prebuilt host shared object with
the program's images and its exported symbols into one
`target/lib<name>.so` — any process can `dlopen()` it and call into the
bytecode by name. No external tool runs during the build: like `--elf`,
it is pure byte fusion of a stub the Makefile compiled once, so the
zero-dependency, single-artifact story holds for libraries too.

## Added

### `flint build --so`

The fused file is the same self-extracting pattern as the standalone
ELF, with one difference: the dynamic loader maps a library's segments
and ignores everything past them, so the fused file stays a valid `.so`:

```
[ build/stub.so ][ code ][ mem ][ exported symbols ][ trailer ]
```

At load time the stub finds its own file through `dladdr` (falling back
to `/proc/self/maps` when the library was found via a search path like
`LD_LIBRARY_PATH`), decodes the trailer and validates it: magic `FLSO`,
version, sizes, symbol records and an FNV-1a checksum over code, mem and
symbols. A truncated or post-processed file fails to load with
`flint_so_error()` set — no crash, no silent fallback.

The host API (`include/flint_so.h`) covers both styles of use:

```c
flint_so_run();              /* fresh VM, run main, r0 at HLT (or -FLINT_ERR_*) */
flint_so_run_args(argc, argv);  /* ... with sys 12..14 fed */

flint_vm *vm = flint_so_vm_new();          /* isolated instance */
flint_word out;
flint_so_call(vm, "s_double", &in, 1, &out);   /* call an export by name */
flint_so_vm_free(vm);
```

- Instances copy the payload's mem image and install the CLI syscall
  table (stdio + POSIX files); the shared code image is read-only, so
  `N` instances coexist and the whole flint core API (`flint_sys_set`,
  `flint_step`, the collector calls, ...) works on them.
- `flint_so_call` runs an exported label through a two-instruction
  trampoline (`call r15` + `exit`) appended past the payload code —
  up to 4 arguments travel in `r0..r3`, the result comes back from
  `r0`, and the VM core needed no call-in API. A trapping call is
  abandoned with the stack/frame pointers restored, so the VM stays
  usable for the next call.
- Both memory models work: a fused `auto` program runs on its
  conservative collector.
- `FLINT_GC_STRESS=1` / `FLINT_GC_STATS=1` are honored like in the CLI
  and the `--elf` stub.

### `export`

`export name` publishes a global code label in the program's symbol
table — the hook `--so` builds on:

```
export s_double
s_double:
    add r0, r0, r0
    return
```

The directive emits no bytecode, forward references resolve after the
fixup pass, and the error paths are strict: locals, data labels,
undefined names and duplicates are rejected at the directive's line.
Names cap at 59 characters. `.fbc` images and standalone ELFs ignore
the table; embedders reach it as `flint_prog.syms` / `nsyms`.

## Verification

- Full `make test` on the aarch64 Termux source tree: core + asm + heap
  + gc + 12/12 examples + the lib/ suites + `gc.sh` + the new `.so`
  host — all green.
- `tests/test_so.c` drives a fused `tests/so_demo.flt`: payload load
  validation, `flint_so_run` (argc 0) and `flint_so_run_args` (argc 3),
  call-by-name before and after `main`, unknown symbol / bad nargs /
  NULL vm, trap + recovery (`s_div0` → `ETRAP`, then a normal call),
  two isolated instances, and a bare-name `LD_LIBRARY_PATH` load (which
  exercises the `/proc/self/maps` fallback on glibc CI).
- `test_asm` pins the `export` surface: symbol table content, the
  trampoline call pattern (77 through `call r15`), forward references,
  and the rejection paths with their reported lines.
- Manual checks on Termux/bionic: absolute-path and relative-path loads,
  `-o` override, an `auto`-mode project fused and run on the collector,
  checksum corruption rejected (`checksum mismatch`), and a `strip`ped
  artifact rejected cleanly (`bad magic`).
- `release.yml` builds and runs `make test` on its Ubuntu runner before
  packaging; the archive now carries `build/stub.so`.

## Prebuilt binaries

Each archive contains `flint`, `build/stub.elf`, `build/stub.so` and
the standard `lib/` sources, so `include "stdlib.flt"`,
`flint build --elf` and `flint build --so` all work from the extracted
directory (keep the layout: both stubs are looked up at `build/...`
relative to the working directory):

| File | Built by | Notes |
|------|----------|-------|
| `flint-v0.4.0-beta.1-x86_64-linux.tar.gz` | GitHub Actions (`release.yml`) | Ubuntu runner, `make test` green before packaging |
| `flint-v0.4.0-beta.1-x86_64-linux.tar.gz.sha256` | same | checksum, bare filename (`sha256sum -c` from anywhere) |
| `flint-v0.4.0-beta.1-aarch64-termux.tar.gz` | Termux build (bionic, aarch64) | for Android/Termux |
| `flint-v0.4.0-beta.1-aarch64-termux.tar.gz.sha256` | same | checksum |

## Installation

```bash
git clone https://github.com/NekoFemDev/flint.git
cd flint
make
make test
```

or grab an archive above.

## Notes

- Treat the fused `.so` as the final artifact: `strip` (and similar
  tools) rewrite the file and drop the appended payload — the loader
  then reports `not a fused Flint library (bad magic)`.
- Build for the target architecture, like `--elf`; a cross-arch library
  needs the matching cross toolchain for the stub.
- Load with `RTLD_LOCAL` (the default): the library exports the whole
  core API, and under `RTLD_GLOBAL` those symbols would interpose across
  the host and other fused libraries. Separate libraries carry separate
  payload state, so several can coexist in one process.
