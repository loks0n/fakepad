#!/bin/bash
# EXPERIMENTAL: run a CrossOver bottle's program with fakepad injected, so Windows
# apps under CrossOver (e.g. Windows Steam / games) see the controller as an Xbox
# One pad. Requires that CrossOver's Wine controller layer (winebus) uses the SDL
# libusb HIDAPI backend — otherwise the fake device won't be visible. Untested.
#
# Usage: crossover-fakepad.command "/path/inside/bottle/Program.exe"
set -euo pipefail
PREFIX="${FAKEPAD_PREFIX:-$HOME/.local/share/fakepad}"
BOTTLE="${CX_BOTTLE:-Steam}"
CX="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"
WINE="$CX/bin/wine"
[ -x "$WINE" ] || { echo "CrossOver not found at $CX"; exit 1; }
[ -f "$PREFIX/fakepad.dylib" ] || { echo "run 'make install' first"; exit 1; }
[ -f "$PREFIX/libusb-real.dylib" ] || cp "$(pkg-config --variable=libdir libusb-1.0)/libusb-1.0.0.dylib" "$PREFIX/libusb-real.dylib"
export CX_BOTTLE DYLD_INSERT_LIBRARIES="$PREFIX/fakepad.dylib"
exec "$CX/bin/wineloader" "$@"
