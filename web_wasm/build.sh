#!/bin/bash
# Build the WASM dither module.
#
# Output: ../main/web/dither.wasm  (embedded into the firmware at build time)
#
# Dependencies — installed at this path on the dev machine:
#   /opt/homebrew/Cellar/emscripten/5.0.7  (brew install emscripten)
#   /opt/homebrew/opt/python@3.14          (emscripten requires Python ≥ 3.10)

set -e
cd "$(dirname "$0")"

EMSDK=/opt/homebrew/Cellar/emscripten/5.0.7
PYTHON=/opt/homebrew/opt/python@3.14/bin/python3.14
# Internal emcc spawn uses $EMSDK_PYTHON first, then `python3` from PATH.
# Set both so every nested invocation resolves to Python 3.14.
export EMSDK_PYTHON="$PYTHON"
export PATH="$(dirname "$PYTHON"):$PATH"
EMCC="$PYTHON $EMSDK/libexec/emcc.py"

MAIN=../main
SHIM=include_shim
OUT=../main/web/dither.wasm

# Standalone WASM (no JS runtime): we explicitly export the dither entry
# points, palette bridge, malloc, and free.  -sSTANDALONE_WASM produces
# a single .wasm with no glue JS; the browser instantiates it directly.

$EMCC \
    -O3 \
    -DNDEBUG \
    '-DESP_LOGI(tag,fmt,...)=((void)0)' \
    '-DESP_LOGE(tag,fmt,...)=((void)0)' \
    '-DESP_LOGW(tag,fmt,...)=((void)0)' \
    -I"$MAIN" \
    -I"$SHIM" \
    -sSTANDALONE_WASM=1 \
    -sEXPORTED_FUNCTIONS=_wasm_init,_wasm_set_palette_lab,_wasm_dither,_wasm_dither_15x,_wasm_dither_e6,_wasm_dither_e6_15x,_wasm_flat_fill,_wasm_flat_fill_15x,_wasm_rgb_to_lab,_malloc,_free \
    -sALLOW_MEMORY_GROWTH=1 \
    -sINITIAL_MEMORY=4194304 \
    -sMAXIMUM_MEMORY=33554432 \
    --no-entry \
    wasm_entry.c \
    "$MAIN/color_pipeline.c" \
    "$MAIN/dither.c" \
    "$MAIN/palette.c" \
    -o "$OUT"

echo "[ok] wrote $OUT ($(wc -c < "$OUT") bytes)"
