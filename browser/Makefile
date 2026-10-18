EMXX ?= em++
CXX ?= c++
WASM_FLAGS = -O3 -std=c++17 -msimd128 -ffp-contract=fast -sMODULARIZE=1 -sEXPORT_ES6=1 \
             -sENVIRONMENT=web,worker,node -sINITIAL_MEMORY=67108864 \
             -sEXPORTED_RUNTIME_METHODS=UTF8ToString -sFILESYSTEM=0 -sASSERTIONS=0

all: build/ble.mjs build/sample-relay
build/ble.mjs: wasm.cpp ../channelizer.h ../fft80.h ../decoder.h
	@mkdir -p build
	$(EMXX) $(WASM_FLAGS) wasm.cpp -o $@
build/sample-relay: sample-relay.cpp ../include/w1700k_stream.h
	@mkdir -p build
	$(CXX) -O3 -std=c++17 -Wall -Wextra sample-relay.cpp -o $@
test: all
	npm test
.PHONY: all test
