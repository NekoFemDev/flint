#!/bin/sh
# tests/gc.sh — automatic-memory integration bench (conservative mark-sweep).
#
# Runs the chunked base64 example — it allocates a fresh input block and
# output block per chunk and never frees — through every execution path
# and diffs each output against `base64 -w0`:
#
#   1. flint run <project>            (memory = "auto" from Flint.toml)
#   2. flint run --memory auto <src>  (single-file model override)
#   3. flint run <image>.fbc          (the image carries mem_mode = auto)
#   4. standalone ELF from an isolated directory — no sources, no stub,
#      no lib, no repo cwd (runs out of $TMPDIR or /tmp)
#
# Extra checks: --gc-stress output equality, --gc-stats sanity
# (collections ran; live bytes stay bounded), FLINT_GC_STRESS=1 and
# FLINT_GC_STATS=1 environment variables (interpreter + ELF).
#
# Inputs: 0/1/2/3 bytes, 16 KiB random, 1 MiB random / zeros / 0xFF.
set -u

BIN=./build/flint
PROJ=examples/b64-mem
SRC=$PROJ/src/main.flt
ABS=$(pwd)
TMP=$ABS/build/gc
mkdir -p "$TMP"

fails=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; fails=$((fails + 1)); }

command -v base64 >/dev/null 2>&1 || { echo "gc: base64 not found, skip"; exit 0; }

# ---- inputs + expected outputs (the reference is base64 of the same file) ----

: > "$TMP/in_0"
printf 'a'   > "$TMP/in_1"
printf 'ab'  > "$TMP/in_2"
printf 'abc' > "$TMP/in_3"
dd if=/dev/urandom of="$TMP/in_16k"  bs=1024 count=16   2>/dev/null
dd if=/dev/urandom of="$TMP/in_1m_r" bs=1024 count=1024 2>/dev/null
dd if=/dev/zero    of="$TMP/in_1m_z" bs=1024 count=1024 2>/dev/null
dd if=/dev/zero    of="$TMP/in_1m_f" bs=1024 count=1024 2>/dev/null
tr '\0' '\377' < "$TMP/in_1m_f" > "$TMP/in_1m_f.tmp" && mv "$TMP/in_1m_f.tmp" "$TMP/in_1m_f"

# ---- build the image + standalone ELF once ----

"$BIN" build "$PROJ" -o "$TMP/b64mem.fbc" >/dev/null \
    || { fail "flint build $PROJ (.fbc)"; exit 1; }
"$BIN" build --elf "$PROJ" -o "$TMP/b64mem.elf" >/dev/null \
    || { fail "flint build --elf $PROJ"; exit 1; }

ISO="${TMPDIR:-/tmp}/flint-gc-$$"
mkdir -p "$ISO" 2>/dev/null || { ISO="$TMP/iso"; mkdir -p "$ISO"; }
cp "$TMP/b64mem.elf" "$ISO/b64mem" || { fail "copy ELF to $ISO"; exit 1; }

# ---- every input through all four paths ----

inputs="in_0 in_1 in_2 in_3 in_16k in_1m_r in_1m_z in_1m_f"
ninputs=0

for name in $inputs; do
    f="$TMP/$name"
    base64 -w0 < "$f" > "$f.exp"
    ninputs=$((ninputs + 1))

    "$BIN" run "$PROJ" < "$f" > "$TMP/o1" 2>"$TMP/e1"
    "$BIN" run --memory auto "$SRC" < "$f" > "$TMP/o2" 2>"$TMP/e2"
    "$BIN" run "$TMP/b64mem.fbc" < "$f" > "$TMP/o3" 2>"$TMP/e3"
    ( cd "$ISO" && ./b64mem < "$f" > "$TMP/o4" ) 2>"$TMP/e4"

    n=0
    for o in "$TMP/o1" "$TMP/o2" "$TMP/o3" "$TMP/o4"; do
        cmp -s "$f.exp" "$o" && n=$((n + 1))
    done
    if [ "$n" -eq 4 ]; then
        pass "$name: project/file/.fbc/ELF all match base64 -w0"
    else
        fail "$name: only $n/4 paths match"
    fi
done

# ---- stress mode: same output, collector forced on every allocation ----

"$BIN" run --gc-stress "$PROJ" < "$TMP/in_1m_z" > "$TMP/o" 2>/dev/null
cmp -s "$TMP/in_1m_z.exp" "$TMP/o" \
    && pass "gc-stress: 1 MiB zeros unchanged" || fail "gc-stress output"

# ---- stats: collections happened, live bytes bounded ----

"$BIN" run --gc-stats "$PROJ" < "$TMP/in_1m_r" > "$TMP/o" 2>"$TMP/stats"
cmp -s "$TMP/in_1m_r.exp" "$TMP/o" || fail "gc-stats run output"
colls=$(sed -n 's/.*collections=\([0-9][0-9]*\).*/\1/p' "$TMP/stats")
live=$(sed -n 's/.*live=\([0-9][0-9]*\).*/\1/p' "$TMP/stats")
if [ -n "$colls" ] && [ "$colls" -ge 1 ] 2>/dev/null; then
    pass "gc-stats: collections=$colls"
else
    fail "gc-stats: no collections line ($(cat "$TMP/stats"))"
fi
if [ -n "$live" ] && [ "$live" -le 40960 ] 2>/dev/null; then
    pass "gc-stats: live=$live bytes stays bounded (< 40 KiB)"
else
    fail "gc-stats: live=$live not bounded"
fi

# ---- environment variables (interpreter and standalone ELF) ----

FLINT_GC_STRESS=1 "$BIN" run --gc-stats "$PROJ" < "$TMP/in_16k" > "$TMP/o" 2>"$TMP/envstats"
if cmp -s "$TMP/in_16k.exp" "$TMP/o" && grep -q stress "$TMP/envstats"; then
    pass "FLINT_GC_STRESS=1: output unchanged, stats report stress"
else
    fail "FLINT_GC_STRESS=1"
fi

( cd "$ISO" && FLINT_GC_STATS=1 ./b64mem < "$TMP/in_16k" > "$TMP/o" 2>"$TMP/elfstats" )
if cmp -s "$TMP/in_16k.exp" "$TMP/o" && grep -q "stub: gc:" "$TMP/elfstats"; then
    pass "ELF + FLINT_GC_STATS=1: output unchanged, collector reported"
else
    fail "ELF + FLINT_GC_STATS=1"
fi

rm -rf "$ISO"

# ---- summary ----

if [ "$fails" -eq 0 ]; then
    echo "gc: integration bench ok ($ninputs inputs x 4 paths + stress/stats/env)"
    exit 0
fi
echo "gc: $fails case(s) failed"
exit 1