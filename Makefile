CXX ?= c++
CPPFLAGS += $(shell pkg-config --cflags fftw3f)
CXXFLAGS += -O3 -std=c++17 -pthread -Wall -Wextra
LDLIBS += $(shell pkg-config --libs fftw3f)
# A local profile is generated from recordings by tools/profile-decoder.py.
# PROFILE= disables its use; no profile is required for an ordinary build.
PROFILE ?= $(wildcard build/decoder.profdata)
PROFILE_FLAGS = $(if $(PROFILE),-fprofile-instr-use=$(PROFILE))
.DEFAULT_GOAL := all
ifeq ($(shell uname -s),Darwin)
CPPFLAGS += -DW1700K_METAL
METAL_OBJECT := build/metal-channelizer.o
LDLIBS += -framework Metal -framework Foundation -framework Accelerate
VECTOR_LIBS := -framework Accelerate
build/metal-source.h: channelizer.metal
	@mkdir -p build
	python3 -c 'from pathlib import Path; Path("$@").write_text("static const char *metal_source = R\"MSL(\n" + Path("$<").read_text() + ")MSL\";\n")'
build/metal-channelizer.o: metal-channelizer.mm metal-channelizer.h channelizer.h build/metal-source.h
	/usr/bin/clang++ $(CPPFLAGS) $(CXXFLAGS) -fobjc-arc -c $< -o $@
build/test-metal: test-metal.cpp metal-channelizer.h channelizer.h $(METAL_OBJECT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) test-metal.cpp $(METAL_OBJECT) -o $@ $(LDLIBS)
check-metal: build/test-metal
	./build/test-metal
endif
all: build/w1700k-ble-wide receiver

.PHONY: receiver
receiver:
	$(MAKE) -C receiver
DECODER_SOURCES = main.cpp capture-clock.h waterfall.h $(METAL_OBJECT) metal-channelizer.h include/iq4.h include/iq8.h channelizer.h decoder.h soft-demod.h preamble.h streaming-demod.h streaming-vector.h packet-noise.h pipeline.h decode-pool.h include/w1700k_stream.h
build/w1700k-ble-wide: $(DECODER_SOURCES) $(PROFILE) Makefile
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(PROFILE_FLAGS) main.cpp $(METAL_OBJECT) -o $@ $(LDLIBS)
build/w1700k-ble-wide-instrumented: $(DECODER_SOURCES) Makefile
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fprofile-instr-generate main.cpp $(METAL_OBJECT) -o $@ $(LDLIBS)
clean:
	rm -f build/w1700k-ble-wide build/metal-channelizer.o build/metal-source.h build/test-metal
.PHONY: all clean check-metal

build/test-demod: test-demod.cpp demod-fixture.h decoder.h soft-demod.h preamble.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@
check-demod: build/test-demod
	./build/test-demod
.PHONY: check-demod

build/test-streaming: test-streaming.cpp demod-fixture.h streaming-demod.h streaming-vector.h packet-noise.h decoder.h soft-demod.h preamble.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DSTREAMING_AUDIT $< -o $@
check-streaming: build/test-streaming
	./build/test-streaming
.PHONY: check-streaming

build/test-sensitivity: test-sensitivity.cpp demod-fixture.h streaming-demod.h streaming-vector.h packet-noise.h decoder.h soft-demod.h preamble.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DSTREAMING_AUDIT $< -o $@
check-sensitivity: build/test-sensitivity
	./build/test-sensitivity
.PHONY: check-sensitivity

build/test-decode-pool: test-decode-pool.cpp decode-pool.h demod-fixture.h streaming-demod.h streaming-vector.h packet-noise.h decoder.h soft-demod.h preamble.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -pthread $< -o $@
check-decode-pool: build/test-decode-pool
	./build/test-decode-pool
.PHONY: check-decode-pool

build/test-waterfall: test-waterfall.cpp waterfall.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@ $(VECTOR_LIBS)
check-waterfall: build/test-waterfall
	./build/test-waterfall
.PHONY: check-waterfall

build/test-packet-noise: test-packet-noise.cpp packet-noise.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@
build/test-near-far: test-near-far.cpp demod-fixture.h streaming-demod.h streaming-vector.h packet-noise.h decoder.h soft-demod.h preamble.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DSTREAMING_AUDIT $< -o $@
check-packet-noise: build/test-packet-noise build/test-near-far
	./build/test-packet-noise
	./build/test-near-far
.PHONY: check-packet-noise

build/test-reply-sync: test-reply-sync.cpp demod-fixture.h streaming-demod.h streaming-vector.h packet-noise.h decoder.h soft-demod.h preamble.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DSTREAMING_AUDIT $< -o $@
check-reply-sync: build/test-reply-sync
	./build/test-reply-sync
.PHONY: check-reply-sync

build/test-streaming-vector: test-streaming-vector.cpp demod-fixture.h streaming-demod.h streaming-vector.h packet-noise.h decoder.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DSTREAMING_AUDIT $< -o $@
check-streaming-vector: build/test-streaming-vector
	./build/test-streaming-vector
.PHONY: check-streaming-vector

PYTHON ?= python3
CHECKS = check-demod check-streaming check-sensitivity check-decode-pool check-waterfall check-packet-noise check-reply-sync check-streaming-vector
ifeq ($(shell uname -s),Darwin)
CHECKS += check-metal
endif
.PHONY: check
check: all $(CHECKS)
	$(PYTHON) -m unittest discover -s tests -v
	@for test in tests/test_*.cjs; do node "$$test" || exit $$?; done
