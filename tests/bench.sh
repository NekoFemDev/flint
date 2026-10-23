#!/bin/sh
# bench.sh — `flint build` + speed comparison on the bench examples:
#   project dir  vs  single source file  vs  built .fbc image
#
# usage: sh tests/bench.sh [reps]      (run from the repo root)
set -e

BIN=${FLINT:-./build/flint}
REPS=${1:-3}

if [ ! -x "$BIN" ]; then
    echo "bench: $BIN not found — run make first" >&2
    exit 1
fi

# build the images from the project form (includes resolved at build time)
$BIN build examples/bench_sum
$BIN build examples/bench_fact

# correctness gate: project, file and image must produce identical output
for p in bench_sum bench_fact; do
    a=$($BIN run "examples/$p" | cksum)
    b=$($BIN run "examples/$p.flt" | cksum)
    c=$($BIN run "build/$p.fbc" | cksum)
    if [ "$a" != "$b" ] || [ "$b" != "$c" ]; then
        echo "bench: OUTPUT MISMATCH for $p" >&2
        exit 1
    fi
done

# best-of-REPS wall time, milliseconds
best_of() {
    best=999999999
    i=0
    while [ "$i" -lt "$REPS" ]; do
        t0=$(date +%s%N)
        "$@" >/dev/null
        t1=$(date +%s%N)
        ms=$(( (t1 - t0) / 1000000 ))
        [ "$ms" -lt "$best" ] && best=$ms
        i=$((i + 1))
    done
    echo "$best"
}

printf '%-12s %9s %9s %9s\n' "program" "project" "file" "image"
for p in bench_sum bench_fact; do
    a=$(best_of "$BIN" run "examples/$p")
    b=$(best_of "$BIN" run "examples/$p.flt")
    c=$(best_of "$BIN" run "build/$p.fbc")
    printf '%-12s %8sms %8sms %8sms\n' "$p" "$a" "$b" "$c"
done
