# fakepad

Make third-party **GIP** ("Xbox One / Series") wired controllers — like PowerA
"Enhanced Wired Controller" pads — work with **Steam on macOS**, where they
otherwise connect and disconnect several times a second and are unusable.

It works by shimming the `libusb` that Steam's bundled SDL loads: the shim hides
the real controller, presents a synthetic **Xbox One S controller**, and
transparently relays the raw GIP protocol between SDL's own Xbox One driver and
the real pad. Because SDL drives the native protocol end-to-end, every button
(including **Share**), rumble, triggers, and Xbox One button glyphs work.

No kernel extension, no DriverKit entitlement, no SIP changes, and **no
modification of any file inside Steam's app bundle** (so Steam's integrity check
can't revert it).

## Why third-party Xbox pads break on macOS

- These controllers speak Microsoft's **GIP** protocol over a *vendor-specific*
  USB interface (class `0xFF`, subclass `0x47`, protocol `0xD0`). They expose
  **no HID interface**, so macOS's HID stack and Apple's GameController framework
  never see them (Apple's driver is gated to Microsoft's vendor ID).
- Steam reaches them through SDL's `libusb` HIDAPI backend. But SDL selects its
  controller driver by **VID/PID**, and it doesn't recognise a third-party pad's
  VID/PID as an Xbox controller. So it never engages the Xbox One/GIP driver and
  falls back to a generic HID path that immediately stalls — it opens the device,
  the first read fails, it closes and reopens, forever. That is the
  "Controller Disconnected" spam.
- The controller hardware is fine. Driven with the correct GIP handshake it
  streams input perfectly (see `tools/gip-probe.c`). The bug is **driver
  selection**, not the handshake.

## The macOS wrinkle that makes this possible

SDL on macOS deliberately refuses to run its Xbox One/GIP driver for *wired*
controllers — it defers them to Apple's GameController framework (which won't
touch third-party pads). But the guard in the legacy `SDL_hidapi_xboxone.c`
driver only fires for devices whose path starts with `"DevSrvsID"` — i.e. those
enumerated via the native IOHIDManager backend:

```c
#if defined(SDL_PLATFORM_MACOS) && defined(SDL_JOYSTICK_MFI)
    if (SDL_GetHintBoolean(SDL_HINT_JOYSTICK_MFI, true) &&
        !SDL_IsJoystickBluetoothXboxOne(vendor_id, product_id) &&
        (device && SDL_strncmp(device->path, "DevSrvsID", 9) == 0)) {
        return false;   // defer to GCController
    }
#endif
```

A device offered through the **libusb** backend gets a USB-style path, so the
guard does **not** fire. If we also present the GIP interface signature
(`0xFF`/`0x47`/`0xD0`) and a VID/PID SDL maps to the `XBOXONE` type, SDL's Xbox
One driver binds it and runs the real GIP handshake over libusb.

## How it works

```
real PowerA pad ──▶ reader thread ──▶ ring buffer ──▶ SDL IN transfers
       ▲                (raw GIP bytes, both ways)          │
       └────────────── OUT transfers ◀── SDL Xbox One driver ┘
                         (this shim relays; SDL negotiates)
```

1. `DYLD_INSERT_LIBRARIES` loads the shim into `steam_osx` (Steam has no hardened
   runtime / library validation, so this is allowed).
2. The shim interposes `dlopen` so SDL's `dlopen("libusb-1.0.0.dylib")` resolves
   to the shim's own libusb implementation.
3. `libusb_get_device_list` returns **one** synthetic device: an Xbox One S
   controller (`045E:02EA`, interface `FF`/`47`/`D0`). The real pad is hidden
   from Steam, so SDL's driver selection lands on the Xbox One driver.
4. A single background thread opens the real pad through a genuine libusb
   (`libusb-real.dylib`) and relays its raw GIP byte stream:
   - real pad IN (`0x81`) → ring buffer → SDL's async IN transfers
   - SDL's OUT transfers (`0x01`) → written straight to the real pad
5. SDL's Xbox One driver performs the entire GIP handshake and input/rumble
   protocol directly with the real pad through this pipe. There is no protocol
   translation, so nothing to get wrong.

The reader thread starts lazily on first libusb use, so only the one Steam
process that actually drives the controller ever touches the hardware (avoids
multi-process contention from `DYLD_INSERT_LIBRARIES` propagating to children).

## Build & install

Requires the Xcode command-line tools and `libusb` (`brew install libusb`).

```sh
make
make install          # copies fakepad.dylib + a real libusb into ~/.local/share/fakepad
```

## Run

```sh
./steam-fakepad.command
```

Launch Steam via this script instead of the dock icon. Plug the controller
**directly into the Mac** (USB hubs can brown-out the whole chain). It appears in
Steam as an "Xbox One S Controller".

Other GIP pads: override the target with `FAKEPAD_VID` / `FAKEPAD_PID`
(hex, e.g. `FAKEPAD_VID=20d6 FAKEPAD_PID=2062`). Set `FAKEPAD_DEBUG=1` to log to
`/tmp/fakepad.log`.

## Diagnostics

`tools/gip-probe.c` is a standalone program that opens the pad over raw USB, runs
the full GIP handshake, and prints decoded button/stick input — proof the
hardware works and a reference for the protocol.

```sh
make gip-probe && ./gip-probe   # close Steam first; only one process can hold the device
```

## Status & limitations

- Presents as a wired **Xbox One S controller**: all buttons (incl. Share),
  both analog sticks + clicks, triggers, D-pad, rumble, and Xbox One glyphs work
  natively — SDL speaks the real GIP protocol to the pad through the relay.
- Tested with a PowerA "Xbox Series X Wired Controller" (`20D6:2062`) on Apple
  Silicon macOS. Other GIP pads likely work via `FAKEPAD_VID`/`FAKEPAD_PID`.
- This is a reverse-engineering / interoperability project. Not affiliated with
  Valve, Microsoft, or PowerA.

## The real fix

The underlying bug is that SDL doesn't recognise third-party GIP pads' VID/PIDs
as Xbox controllers on macOS, and its wired-Xbox handling defers to a framework
that ignores them. This shim is a workaround; the proper fix is upstream in SDL.
