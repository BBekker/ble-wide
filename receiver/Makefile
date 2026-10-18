# SPDX-License-Identifier: MIT
# w1700k-stream: a C++17 compiler and FFTW3 (single precision). Tune for the build host with
# CXXFLAGS="-O3 -march=native".
CXX ?= c++
include ../mk/platform.mk
CXXFLAGS ?= -O3
WARNINGS ?= -Wall -Wextra -Werror
PREFIX ?= /usr/local
override CXXFLAGS += -std=c++17 -pthread -I../include -Iusb $(FFTW_CFLAGS) $(WARNINGS)
LDLIBS += -pthread $(FFTW_LIBS) $(RT_LIBS)

SOURCES := $(wildcard src/*.cpp)
OBJECTS := $(patsubst src/%.cpp,build/%.o,$(SOURCES))

.PHONY: all clean install
all: build/w1700k-stream

build/w1700k-stream: $(OBJECTS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

build/%.o: src/%.cpp $(wildcard src/*.h) $(wildcard usb/*.h) ../include/w1700k_stream.h $(wildcard ../include/*.h) | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build:
	mkdir -p $@

install: build/w1700k-stream
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/include
	install -m 755 $< $(DESTDIR)$(PREFIX)/bin/w1700k-stream
	install -m 644 ../include/w1700k_stream.h $(DESTDIR)$(PREFIX)/include/w1700k_stream.h

clean:
	rm -f build/*.o build/w1700k-stream build/test-usb build/test-init
	rm -rf build/BLEUSBHelper.app

.PHONY: check-usb check-usb-init usb-helper
build/test-init: ../tests/cpp/usb/test-init.cpp usb/rtl8157-init.h usb/rtl8157-data.h | build
	$(CXX) $(CXXFLAGS) $< -o $@
check-usb-init: build/test-init
	./build/test-init
build/test-usb: ../tests/cpp/usb/test-usb.cpp usb/frames.h usb/ring.h | build
	$(CXX) $(CXXFLAGS) $< -o $@
check-usb: build/test-usb
	./build/test-usb
ifeq ($(shell uname -s),Darwin)
usb-helper: build/BLEUSBHelper.app/Contents/MacOS/rtl8157-helper
build/BLEUSBHelper.app/Contents/MacOS/rtl8157-helper: usb/helper.mm usb/power.h usb/ring.h usb/rtl8157-init.h usb/rtl8157-data.h usb/Info.plist
	mkdir -p build/BLEUSBHelper.app/Contents/MacOS
	cp usb/Info.plist build/BLEUSBHelper.app/Contents/Info.plist
	/usr/bin/clang++ -std=c++17 -O3 -Wall -Wextra -Werror -fobjc-arc $< -o $@ -framework Foundation -framework IOKit -framework IOUSBHost
	/usr/bin/codesign --force --sign - build/BLEUSBHelper.app
else
usb-helper:
	@echo 'The direct USB helper currently requires macOS.' >&2; exit 1
endif
