#!/usr/bin/env bash
# compile-qmd.sh — hash a plain-name .qml-diff into a device-ready .qmd.
#
# Input:  a plain-identifier qmldiff source, e.g. src/freeColour.qml-diff.
#         The file uses real QML names (root.pen.toolColor, colorComponent,
#         WritingTool.qml, etc.). Author it this way.
#
# Output: build/<name>.qmd — the same diff with every identifier rewritten
#         to its hashed form ([[u64]] / ~&u64&~) against reference/hashtab.
#         This is what qt-resource-rebuilder loads on the device.
#
# Hashtab:  reference/hashtab  (firmware 3.26.0.68, pulled 2026-04-24).
# Binary:   QMLDIFF env var, else ~/src/qmldiff/target/release/qmldiff,
#           else whatever `qmldiff` resolves to on PATH.
#
# Usage:   bin/compile-qmd.sh src/freeColour.qml-diff
#          → writes build/freeColour.qmd
#
# qmldiff's `hash-diffs` rewrites its input in place, so this script copies
# the source into build/ first and hashes the copy. The original stays clean.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <plain-name .qml-diff source>" >&2
    exit 2
fi

SRC=$1
if [[ ! -f $SRC ]]; then
    echo "error: source not found: $SRC" >&2
    exit 1
fi

REPO_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
HASHTAB=$REPO_ROOT/reference/hashtab
BUILD_DIR=$REPO_ROOT/build

if [[ ! -f $HASHTAB ]]; then
    echo "error: hashtab not found at $HASHTAB" >&2
    exit 1
fi

QMLDIFF=${QMLDIFF:-}
if [[ -z $QMLDIFF ]]; then
    if [[ -x $HOME/src/qmldiff/target/release/qmldiff ]]; then
        QMLDIFF=$HOME/src/qmldiff/target/release/qmldiff
    elif command -v qmldiff >/dev/null 2>&1; then
        QMLDIFF=$(command -v qmldiff)
    else
        echo "error: qmldiff binary not found. Set QMLDIFF=/path/to/qmldiff or" >&2
        echo "       build it (see reference/qmldiff-workflow.md)." >&2
        exit 1
    fi
fi

base=$(basename -- "$SRC")
name=${base%.qml-diff}
if [[ $name == "$base" ]]; then
    name=${base%.qmd}
fi
OUT=$BUILD_DIR/$name.qmd

mkdir -p "$BUILD_DIR"
cp -- "$SRC" "$OUT"
"$QMLDIFF" hash-diffs "$HASHTAB" "$OUT"

echo "compiled: $SRC -> $OUT"
