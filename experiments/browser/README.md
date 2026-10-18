# Browser receiver experiment

This is an incomplete alternative receiver that explores running the channelizer and decoder in WebAssembly. It is separate from the supported Airspace viewer in `web/` and is not part of the default build or test suite.

`wasm.cpp` uses the shared native sources in `src/`; `sample-relay.cpp` reads the sample-ring ABI in `include/`. `relay.mjs` is the local sample transport. Building requires Emscripten and the dependencies in `package.json`. The prototype does not currently include a complete frontend or its referenced `test.mjs` harness; `npm test` is not a supported validation command.

Use the root README and `python3 -m airspace` for the maintained receiver/viewer.
