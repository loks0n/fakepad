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

# Install a LaunchAgent so the native helper is always running — then launching
# Steam from the CrossOver UI (or its dock icon) works with no extra step.
PLIST="$HOME/Library/LaunchAgents/io.loks0n.fakepad.helper.plist"
HELPER="$HOME/.local/share/fakepad/fakepad-helper"
cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>io.loks0n.fakepad.helper</string>
  <key>ProgramArguments</key><array><string>$HELPER</string></array>
  <key>RunAtLoad</key><true/><key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>/tmp/fakepad-helper.log</string>
  <key>StandardErrorPath</key><string>/tmp/fakepad-helper.log</string>
</dict></plist>
PL
launchctl bootout "gui/$(id -u)/io.loks0n.fakepad.helper" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST"
echo "Helper LaunchAgent installed & started — the CrossOver Steam icon now works."
echo "To use native macOS Steam instead:  launchctl bootout gui/$(id -u)/io.loks0n.fakepad.helper"
