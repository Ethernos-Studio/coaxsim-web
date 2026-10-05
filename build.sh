#!/usr/bin/env bash
set -euo pipefail
mkdir -p web
emcc -O3 -msimd128 -fno-exceptions -fno-rtti \
  src/coax.cpp -o web/coax.js \
  -s EXPORTED_FUNCTIONS='["_coax_create","_coax_destroy","_coax_process","_coax_width","_coax_height","_coax_jit","_coax_spl","_coax_tx","_coax_rx","_malloc","_free"]' \
  -s EXPORTED_RUNTIME_METHODS='["HEAPU8","HEAPF32"]' \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s MODULARIZE=1 -s EXPORT_NAME=CoaxModule
echo "OK: web/coax.js + web/coax.wasm"