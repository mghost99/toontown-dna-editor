#!/bin/bash
# Builds (if needed) and starts the native DNA Editor.  Usage: run.sh [DNA_FILE] [--size WxH]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/build/app/dna-editor"
if [ ! -x "$BIN" ] || [ -n "$(find "$HERE/src" "$HERE/app" -newer "$BIN" -name '*.[ch]*' 2>/dev/null | head -1)" ]; then
  cmake -S "$HERE" -B "$HERE/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
  ninja -C "$HERE/build" >/dev/null
fi
cd "$HERE"
exec "$BIN" "$@"
