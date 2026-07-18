#!/bin/bash
# Launch Windows Steam under CrossOver with the GIP controller bridged via XInput.
set -euo pipefail
HELPER="${FAKEPAD_HELPER:-$HOME/.local/share/fakepad/fakepad-helper}"
CX="${CX_ROOT:-$HOME/Applications/CrossOver 2.app/Contents/SharedSupport/CrossOver}"
[ -d "$CX" ] || CX="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"
BOTTLE="${CX_BOTTLE:-Steam}"
[ -x "$HELPER" ] || { echo "helper missing — run 'make install' in the repo"; exit 1; }
pkill -x steam_osx 2>/dev/null || true   # free the pad from native Steam
"$HELPER" >/tmp/fakepad-helper.log 2>&1 &
HPID=$!; trap 'kill $HPID 2>/dev/null' EXIT
echo "helper running (pid $HPID); launching Windows Steam in bottle '$BOTTLE'…"
WINEPREFIX="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE" \
  "$CX/CrossOver-Hosted Application/wine" --bottle "$BOTTLE" --cx-app "C:\\Program Files (x86)\\Steam\\steam.exe"
