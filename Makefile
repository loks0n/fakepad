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
	cp fakepad.dylib "$(PREFIX)/"
	cp "$(shell pkg-config --variable=libdir libusb-1.0)/libusb-1.0.0.dylib" "$(PREFIX)/libusb-real.dylib"
	codesign -f -s - "$(PREFIX)/libusb-real.dylib"
	@echo "Installed to $(PREFIX)"

clean:
	rm -f fakepad.dylib gip-probe

.PHONY: all install clean
