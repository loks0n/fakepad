#!/bin/bash
# Launch Steam with the fakepad libusb shim injected, so a PowerA (or other
# third-party GIP) Xbox controller works over USB on macOS.
set -euo pipefail

PREFIX="${FAKEPAD_PREFIX:-$HOME/.local/share/fakepad}"
SHIM="$PREFIX/fakepad.dylib"
REAL="$PREFIX/libusb-real.dylib"
STEAM="$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steam_osx"

[ -f "$SHIM" ] || { echo "shim not found at $SHIM — run 'make install' first"; exit 1; }

# Ensure a real libusb sits next to the shim for it to forward hardware I/O to.
if [ ! -f "$REAL" ]; then
  for cand in \
    "$(pkg-config --variable=libdir libusb-1.0 2>/dev/null)/libusb-1.0.0.dylib" \
    "$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/libusb-1.0.0.dylib"; do
    if [ -f "$cand" ]; then cp "$cand" "$REAL"; codesign -f -s - "$REAL"; break; fi
  done
fi
[ -f "$REAL" ] || { echo "no real libusb found; install libusb (brew install libusb)"; exit 1; }
[ -x "$STEAM" ] || { echo "Steam not found at $STEAM"; exit 1; }

pkill -x steam_osx 2>/dev/null || true
sleep 1
echo "Launching Steam with fakepad…"
exec env DYLD_INSERT_LIBRARIES="$SHIM" "$STEAM"
