# fakepad

Make third-party **GIP** ("Xbox One / Series") wired controllers — like PowerA
"Enhanced Wired Controller" pads — work with **Steam on macOS**, where they
otherwise connect and disconnect several times a second and are unusable.

It works by shimming the `libusb` that Steam's bundled SDL loads: the shim hides
the real controller, presents a synthetic **wired Xbox 360 controller** that
SDL's rock-solid 360 driver binds to, and feeds that fake device live input read
from the real pad over its native GIP protocol. Rumble is forwarded back to the
real motors.

No kernel extension, no DriverKit entitlement, no SIP changes, and **no
modification of any file inside Steam's app bundle** (so Steam's integrity check
can't revert it).

## Why third-party Xbox pads break on macOS

- These controllers speak Microsoft's **GIP** protocol over a *vendor-specific*
  USB interface (class `0xFF`). They expose **no HID interface**, so macOS's HID
  stack and Apple's GameController framework never see them (Apple's driver is
  gated to Microsoft's vendor ID).
- Steam reaches them through SDL's `libusb` HIDAPI backend, but its handshake
  fails to complete the GIP metadata negotiation for these pads. It opens the
  device, its first read fails, it closes and reopens — forever. That is the
  "Controller Disconnected" spam.
- The controller hardware is fine. Driven with the correct GIP handshake it
  streams input perfectly (see `tools/gip-probe.c`).

## How it works

```
real PowerA pad ──(GIP handshake, our code)──▶ internal reader thread
                                                     │  translates to
                                                     ▼
Steam ◀─ SDL xbox360 driver ◀─ fake libusb device ◀─ 20-byte X360 report
                                (this shim)
```

1. `DYLD_INSERT_LIBRARIES` loads the shim into `steam_osx` (Steam has no hardened
   runtime / library validation, so this is allowed).
2. The shim interposes `dlopen` so SDL's `dlopen("libusb-1.0.0.dylib")` resolves
   to the shim's own libusb implementation.
3. `libusb_get_device_list` returns **one** synthetic device: a wired Xbox 360
   controller (`045E:028E`, interface class `FF`/sub `5D`/proto `01`). The real
   PowerA is hidden from Steam.
4. A single background thread opens the real pad through a genuine libusb
   (`libusb-real.dylib`), runs the GIP handshake (metadata request → fragment
   ACKs → device-state/LED/security/initial-reports), and continuously reads
   GIP input reports, translating each into a 20-byte 360 report.
5. SDL's async reads on the fake device return the latest 360 report; SDL's
   outgoing rumble packets are translated to a GIP `DIRECT_MOTOR` command and
   sent to the real pad.

The thread is started lazily on first libusb use so that only the one Steam
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
Steam as an "Xbox 360 Controller".

## Diagnostics

`tools/gip-probe.c` is a standalone program that opens the pad over raw USB, runs
the full GIP handshake, and prints decoded button/stick input — proof the
hardware works and a reference for the protocol.

```sh
make gip-probe && ./gip-probe   # close Steam first; only one process can hold the device
```

## Status & limitations

- Presents as a **wired Xbox 360 controller**: all standard buttons, both
  analog sticks + clicks, triggers, D-pad, and rumble work. In-game prompts show
  360-style glyphs, and the Series **Share** button has no 360 equivalent.
- Tested with a PowerA "Xbox Series X Wired Controller" (`20D6:2062`) on Apple
  Silicon macOS. Other GIP pads likely work; the button/axis map may need tweaks.
- This is a reverse-engineering / interoperability project. Not affiliated with
  Valve, Microsoft, or PowerA.

## The real fix

The underlying bug is in Steam/SDL's macOS GIP handshake for third-party pads.
This shim is a workaround; the proper fix is upstream in SDL's
`SDL_hidapi_gip.c`. Protocol details captured here may help that effort.
