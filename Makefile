# flint — build script (cargo-style targets, zero dependencies)
CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -I include
SRC       = src/flint_core.c src/flint_asm.c src/flint_cli.c src/flint_img.c
TESTSRC   = src/flint_core.c src/flint_asm.c

BIN       = build/flint
STUB      = build/stub.elf
TESTS     = build/test_core build/test_asm build/test_heap

all: $(BIN) $(STUB)

$(BIN): $(SRC) src/main.c src/flint_dis.c include/flint.h include/flint_cli.h include/flint_img.h include/flint_stub.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) src/flint_dis.c src/main.c -o $@

# `flint build --elf` fuses this stub with the program's images into one
# standalone executable — see include/flint_stub.h for the layout.
#
# flint_cli.c's front-end references the assembler, image and disassembler
# TUs, so the stub link carries those as well: dead weight at run time
# (the stub only installs the syscall table and runs the payload), kept
# for a portable link line — no --gc-sections tricks.
#
# Static linking is the point of the stub: with -static the fused binary
# carries its own libc and runs on any glibc/musl/bionic host. That needs
# a static libc, so the probe below treats an unresolved `libc.a` as "no
# static support". Termux's bionic toolchain is the known case — there
# -static dies with "cannot open crtbegin_static.o" and "unable to find
# library -lc", so the fallback links libc.so dynamically and keeps only
# libgcc static (-static-libgcc). Every Android device already ships
# bionic libc, so the stub still runs as-is. Force a choice with:
#   make STUB_LDFLAGS=-static            (host with a static libc)
#   make STUB_LDFLAGS=-static-libgcc     (dynamic libc, e.g. Termux)
# Either way the stub bundles the VM core + CLI host, so the fused binary
# never needs flint, sources or a .fbc file at run time.
STUB_LDFLAGS ?= $(if $(filter libc.a,$(shell $(CC) -print-file-name=libc.a 2>/dev/null)),-static-libgcc,-static)

$(STUB): src/stub.c src/flint_core.c src/flint_asm.c src/flint_cli.c \
         src/flint_img.c src/flint_dis.c \
         include/flint.h include/flint_cli.h include/flint_img.h include/flint_stub.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(STUB_LDFLAGS) -o $@ \
	  src/stub.c src/flint_core.c src/flint_asm.c src/flint_cli.c \
	  src/flint_img.c src/flint_dis.c

build/test_core: tests/test_core.c src/flint_core.c include/flint.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(TESTSRC) tests/test_core.c -o $@

build/test_asm: tests/test_asm.c src/flint_core.c src/flint_asm.c include/flint.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(TESTSRC) tests/test_asm.c -o $@

build/test_heap: tests/test_heap.c src/flint_core.c src/flint_asm.c include/flint.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(TESTSRC) tests/test_heap.c -o $@

test: $(TESTS) $(BIN) $(STUB)
	./build/test_core
	./build/test_asm
	./build/test_heap
	@./$(BIN) tests/hello.flt  | grep -q "Hello, FLINT!"   \
	  && ./$(BIN) tests/sum.flt   | grep -q 55            \
	  && ./$(BIN) tests/fact.flt  | grep -q 720           \
	  && ./$(BIN) tests/off.flt   | grep -q 119           \
	  && ./$(BIN) tests/fio.flt   | grep -q 42            \
	  && ./$(BIN) tests/regs.flt  | grep -q "r0 = 15"      \
	  && ./$(BIN) tests/regs.flt  | grep -q "r15 = 0"     \
	  && ./$(BIN) tests/libdemo.flt | grep -q 63          \
	  && ./$(BIN) tests/libdemo.flt | grep -q 9           \
	  && ./$(BIN) tests/heap.flt  | grep -q "heap ok"       \
	  && ./$(BIN) tests/heap.flt  | grep -q "mode: manual"  \
	  && ./$(BIN) tests/mem.flt   | grep -q "flint"
	@echo "examples: 9/9 programs run"
	@./$(BIN) build --elf tests/hello.flt >/dev/null \
	  && ./target/hello | grep -q "Hello, FLINT!" \
	  && ./$(BIN) build --elf tests/heap.flt >/dev/null \
	  && ./target/heap | grep -q "heap ok" \
	  && ./target/heap | grep -q "mode: manual" \
	  && echo "stub: hello + heap standalone ELFs run"

bench: $(BIN)
	@sh tests/bench.sh

clean:
	rm -rf build target

.PHONY: all test bench clean
