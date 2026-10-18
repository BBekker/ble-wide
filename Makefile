.DEFAULT_GOAL := all
CXX ?= c++
PYTHON ?= python3
include mk/platform.mk
CPPFLAGS += -Isrc -Iinclude -Ibuild $(FFTW_CFLAGS)
CXXFLAGS += -O3 -std=c++17 -pthread -Wall -Wextra
LDLIBS += $(FFTW_LIBS)

# Optional local optimization profile; PROFILE= disables it.
PROFILE ?= $(wildcard build/decoder.profdata)
PROFILE_FLAGS = $(if $(PROFILE),-fprofile-instr-use=$(PROFILE))
HEADERS := $(wildcard src/decoder/*.h src/channelizer/*.h src/pipeline/*.h include/*.h)
TEST_NAMES := decoder demod streaming sensitivity decode-pool waterfall packet-noise near-far reply-sync streaming-vector

ifeq ($(shell uname -s),Darwin)
CPPFLAGS += -DW1700K_METAL
METAL_OBJECT := build/metal-channelizer.o
LDLIBS += -framework Metal -framework Foundation -framework Accelerate
VECTOR_LIBS := -framework Accelerate
TEST_NAMES += metal

build/metal-source.h: src/channelizer/channelizer.metal | build
	$(PYTHON) -c 'from pathlib import Path; Path("$@").write_text("static const char *metal_source = R\"MSL(\n" + Path("$<").read_text() + ")MSL\";\n")'
build/metal-channelizer.o: src/channelizer/metal-channelizer.mm $(HEADERS) build/metal-source.h
	/usr/bin/clang++ $(CPPFLAGS) $(CXXFLAGS) -fobjc-arc -c $< -o $@
build/test-metal: tests/cpp/test-metal.cpp $(HEADERS) $(METAL_OBJECT) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(METAL_OBJECT) -o $@ $(LDLIBS)
endif

.PHONY: all receiver clean check check-cpp check-python check-web check-usb $(addprefix check-,$(TEST_NAMES))
all: build/w1700k-ble-wide receiver
receiver:
	$(MAKE) -C receiver
build:
	mkdir -p $@

build/w1700k-ble-wide: src/main.cpp $(HEADERS) $(METAL_OBJECT) $(PROFILE) Makefile | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(PROFILE_FLAGS) $< $(METAL_OBJECT) -o $@ $(LDLIBS)
build/w1700k-ble-wide-instrumented: src/main.cpp $(HEADERS) $(METAL_OBJECT) Makefile | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fprofile-instr-generate $< $(METAL_OBJECT) -o $@ $(LDLIBS)

build/test-%: tests/cpp/test-%.cpp tests/cpp/demod-fixture.h $(HEADERS) Makefile | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DSTREAMING_AUDIT $< -o $@ $(VECTOR_LIBS)
$(addprefix check-,$(TEST_NAMES)): check-%: build/test-%
	./build/test-$*
# Keep the existing noise-check target's near/far coverage.
check-packet-noise: check-near-far
check-cpp: $(addprefix check-,$(TEST_NAMES))
check-python: all
	$(PYTHON) -m unittest discover -s tests/python -v
check-web:
	@for test in tests/web/test_*.cjs; do node "$$test" || exit $$?; done
check-usb:
	$(MAKE) -C receiver check-usb check-usb-init
check: check-cpp check-python check-web check-usb

# Preserve local recordings, profiles, and diagnostics when cleaning binaries.
clean:
	rm -f build/w1700k-ble-wide build/w1700k-ble-wide-instrumented build/metal-channelizer.o build/metal-source.h $(addprefix build/test-,$(TEST_NAMES))
	$(MAKE) -C receiver clean
