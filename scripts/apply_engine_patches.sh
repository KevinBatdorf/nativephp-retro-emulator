#!/usr/bin/env bash
# Apply patches/<submodule>-*.patch into their engine submodules.
# Every build script runs this first — a fresh clone otherwise compiles
# unpatched engines (the SameBoy zero-mean audio fix lives only here).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

for p in "$REPO_ROOT"/patches/*.patch; do
    [ -e "$p" ] || continue
    name="$(basename "$p")"
    sub="${name%%-*}"

    if [ ! -d "$REPO_ROOT/$sub" ]; then
        echo "error: $name names no submodule directory '$sub'" >&2
        exit 1
    fi

    if git -C "$REPO_ROOT/$sub" apply --check "$p" 2>/dev/null; then
        git -C "$REPO_ROOT/$sub" apply "$p"
        echo "applied $name"
    elif git -C "$REPO_ROOT/$sub" apply --reverse --check "$p" 2>/dev/null; then
        echo "already applied: $name"
    else
        echo "error: $name neither applies nor is applied — submodule drifted from the patch" >&2
        exit 1
    fi
done
