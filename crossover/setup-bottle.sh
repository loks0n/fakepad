#!/bin/bash
# Install the fakepad xinput shim into a CrossOver bottle (writable; no app changes).
set -euo pipefail
BOTTLE_NAME="${1:-Steam}"
CX="${CX_ROOT:-$HOME/Applications/CrossOver 2.app/Contents/SharedSupport/CrossOver}"
[ -d "$CX" ] || CX="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"
BOTTLE="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE_NAME"
HERE="$(cd "$(dirname "$0")" && pwd)"
WINE="$CX/CrossOver-Hosted Application/wine"
[ -d "$BOTTLE" ] || { echo "bottle '$BOTTLE_NAME' not found"; exit 1; }
for d in xinput1_1 xinput1_2 xinput1_3 xinput1_4 xinput9_1_0; do
  cp "$HERE/xinput1_4.dll" "$BOTTLE/drive_c/windows/system32/$d.dll"
  WINEPREFIX="$BOTTLE" "$WINE" --bottle "$BOTTLE_NAME" --cx-app reg.exe add \
    "HKCU\\Software\\Wine\\DllOverrides" /v "$d" /d native /f >/dev/null 2>&1
done
echo "Installed xinput shim into bottle '$BOTTLE_NAME'."
