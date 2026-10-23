# flint — build script (cargo-style targets, zero dependencies)
CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -I include
SRC       = src/flint_core.c src/flint_asm.c src/flint_cli.c
TESTSRC   = src/flint_core.c src/flint_asm.c

BIN       = build/flint
TESTS     = build/test_core build/test_asm build/test_heap

all: $(BIN)

$(BIN): $(SRC) src/main.c src/flint_dis.c include/flint.h include/flint_cli.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) src/flint_dis.c src/main.c -o $@

build/test_core: tests/test_core.c src/flint_core.c include/flint.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(TESTSRC) tests/test_core.c -o $@

build/test_asm: tests/test_asm.c src/flint_core.c src/flint_asm.c include/flint.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(TESTSRC) tests/test_asm.c -o $@

build/test_heap: tests/test_heap.c src/flint_core.c src/flint_asm.c include/flint.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(TESTSRC) tests/test_heap.c -o $@

test: $(TESTS) $(BIN)
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

clean:
	rm -rf build

.PHONY: all test clean
