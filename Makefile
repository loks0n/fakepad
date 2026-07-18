# fakepad — build the libusb shim (universal x86_64 + arm64)
CC      = clang
CFLAGS  = -O2 -Wall -Wno-pointer-bool-conversion
ARCHS   = -arch x86_64 -arch arm64
LIBUSB_CFLAGS = $(shell pkg-config --cflags libusb-1.0)

PREFIX ?= $(HOME)/.local/share/fakepad

all: fakepad.dylib gip-probe

fakepad.dylib: src/fakepad.c
	$(CC) $(ARCHS) $(CFLAGS) $(LIBUSB_CFLAGS) -dynamiclib -lpthread \
	  -install_name @rpath/fakepad.dylib -o $@ $<
	codesign -f -s - $@

gip-probe: tools/gip-probe.c
	$(CC) $(CFLAGS) $(LIBUSB_CFLAGS) $(shell pkg-config --libs libusb-1.0) -o $@ $<

# Install the shim + a real libusb copy into PREFIX for the launcher to use.
install: fakepad.dylib
	mkdir -p "$(PREFIX)"
	rm -f "$(PREFIX)/fakepad.dylib" "$(PREFIX)/libusb-real.dylib"
	cp fakepad.dylib "$(PREFIX)/"
	cp "$(shell pkg-config --variable=libdir libusb-1.0)/libusb-1.0.0.dylib" "$(PREFIX)/libusb-real.dylib"
	codesign -f -s - "$(PREFIX)/libusb-real.dylib"
	@echo "Installed to $(PREFIX)"

clean:
	rm -f fakepad.dylib gip-probe

.PHONY: all install clean app install-app crossover

APPNAME = Steam (fakepad).app
APPDIR  = build/$(APPNAME)

# Build a dock-able launcher app (regular-launch experience without touching Steam).
app: install
	rm -rf "$(APPDIR)"
	mkdir -p "$(APPDIR)/Contents/MacOS" "$(APPDIR)/Contents/Resources"
	cp app/Info.plist "$(APPDIR)/Contents/Info.plist"
	cp app/launch "$(APPDIR)/Contents/MacOS/launch"
	clang $(ARCHS) -O2 -o "$(APPDIR)/Contents/MacOS/ptylaunch" app/ptylaunch.c
	codesign -f -s - "$(APPDIR)/Contents/MacOS/ptylaunch"
	chmod +x "$(APPDIR)/Contents/MacOS/launch"
	-cp "/Applications/Steam.app/Contents/Resources/steam_osx.icns" "$(APPDIR)/Contents/Resources/AppIcon.icns" 2>/dev/null || \
	 cp "/Applications/Steam.app/Contents/Resources/"*.icns "$(APPDIR)/Contents/Resources/AppIcon.icns" 2>/dev/null || true
	codesign -f -s - "$(APPDIR)"
	@echo "Built '$(APPDIR)' — drag it to the Dock, or run: make install-app"

install-app: app
	rm -rf "/Applications/$(APPNAME)"
	cp -R "$(APPDIR)" "/Applications/"
	@echo "Installed to /Applications/$(APPNAME)"

# ---- CrossOver (Windows Steam under Wine) ----
MINGW ?= x86_64-w64-mingw32-gcc

crossover/xinput1_4.dll: crossover/xinput_shim.c crossover/xinput.def
	$(MINGW) -shared -O2 -o $@ crossover/xinput_shim.c crossover/xinput.def -lkernel32

crossover/fakepad-helper: crossover/helper.c
	$(CC) -O2 -o $@ crossover/helper.c $(LIBUSB_CFLAGS) $(shell pkg-config --libs libusb-1.0)

crossover: crossover/xinput1_4.dll crossover/fakepad-helper
	mkdir -p "$(PREFIX)"
	cp crossover/fakepad-helper "$(PREFIX)/"
	@echo "Built. Install into a bottle:  ./crossover/setup-bottle.sh <BottleName>"
	@echo "Then launch:  ./crossover-steam.command"
