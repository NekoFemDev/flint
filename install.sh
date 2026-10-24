#!/bin/sh
# install.sh — simple FLINT installer.
# Builds the `build/flint` binary and installs it to $PREFIX/bin/flint.
#
# Usage:
#   ./install.sh                      # PREFIX=/usr (or Termux prefix)
#   PREFIX=/usr/local ./install.sh
#   DESTDIR=/tmp/pkg PREFIX=/usr ./install.sh

set -eu

REPO_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
cd "$REPO_DIR"

# Default prefix is /usr, but on Termux use the real Termux prefix.
PREFIX="${PREFIX:-/usr}"
if [ "$PREFIX" = "/usr" ] && [ -d /data/data/com.termux/files/usr ]; then
  PREFIX="/data/data/com.termux/files/usr"
fi

DESTDIR="${DESTDIR:-}"
BINDIR="${DESTDIR}${PREFIX}/bin"

echo "[install] repository: $REPO_DIR"
echo "[install] prefix:     $PREFIX"
echo "[install] bindir:     $BINDIR"

echo "[install] building flint..."
make

if [ ! -x build/flint ]; then
  echo "[install] error: build/flint not found" >&2
  exit 1
fi

mkdir -p "$BINDIR"
install -m 0755 build/flint "$BINDIR/flint"

echo "[install] installed: $BINDIR/flint"
"$BINDIR/flint" --version

echo "[install] done. Run: flint --version"
