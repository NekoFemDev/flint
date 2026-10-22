# flint — build script (cargo-style targets, zero dependencies)
CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -I include
SRC       = src/flint_core.c src/flint_asm.c src/flint_cli.c
TESTSRC   = src/flint_core.c src/flint_asm.c

BIN       = build/flint
TESTS     = build/test_core build/test_asm
TESTFILES = tests/hello.flt tests/sum.flt tests/fact.flt tests/off.flt

all: $(BIN)

$(BIN): $(SRC) include/flint.h include/flint_cli.h src/flint_dis.c
	$(CC) $(CFLAGS) $(SRC) src/flint_dis.c src/main.c -o $@

build/test_core: tests/test_core.c src/flint_core.c include/flint.h | build
	$(CC) $(CFLAGS) $(TESTSRC) tests/test_core.c -o $@

build/test_asm: tests/test_asm.c src/flint_core.c src/flint_asm.c include/flint.h | build
	$(CC) $(CFLAGS) $(TESTSRC) tests/test_asm.c -o $@

test: $(TESTS) $(BIN)
	./build/test_core
	./build/test_asm
	@./$(BIN) tests/hello.flt | grep -q "Hello, FLINT!"    \
	  && ./$(BIN) tests/sum.flt  | grep -q 55             \
	  && ./$(BIN) tests/fact.flt | grep -q 720            \
	  && ./$(BIN) tests/off.flt  | grep -q 119            \
	  && ./$(BIN) tests/fio.flt  | grep -q 42             \
	  && ./$(BIN) tests/regs.flt | grep -q "r0 = 7"        \
	  && ./$(BIN) tests/regs.flt | grep -q "r7 = 0"
	@echo "examples: 6/6 programs run"

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all test clean
