#!/usr/bin/env bash
# Fetch the vendored SQLite port into firmware/components/sqlite3.
#
# Not committed: the amalgamation is 7.4MB, and §15.1's R1 kill switch may
# delete this dependency outright. Pinned to a commit so a fetch months from
# now builds the same binary that was benchmarked.
set -euo pipefail

COMMIT=bf5a8617a9c1933b525f9484ed7e328a1d8bd02f     # 2026-06-25
REPO=https://github.com/nopnop2002/esp32-idf-sqlite3.git
DEST="$(cd "$(dirname "$0")/.." && pwd)/firmware/components/sqlite3"

if [ -d "$DEST" ]; then echo "already present: $DEST"; exit 0; fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
git clone --quiet "$REPO" "$TMP/src"
git -C "$TMP/src" checkout --quiet "$COMMIT"
mkdir -p "$(dirname "$DEST")"
cp -R "$TMP/src/components/esp32-idf-sqlite3" "$DEST"

echo "sqlite3 -> $DEST  (SQLite $(grep -m1 -o '"3\.[0-9.]*"' "$DEST/sqlite3.c" | tr -d '"'), pinned $COMMIT)"
