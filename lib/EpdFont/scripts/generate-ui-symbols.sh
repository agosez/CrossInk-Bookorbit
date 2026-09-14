#!/bin/bash
set -e
cd "$(dirname "$0")"

# One shared 10-point power glyph for both UI scales. Restrict the source face
# so none of the converter's default character ranges are included.
"${PYTHON:-python3}" fontconvert.py ui_symbols_10 10 \
  ../builtinFonts/source/NotoSymbols/NotoSansSymbols2-Regular.ttf \
  --no-default-intervals \
  --additional-intervals 0x23FB,0x23FB \
  > ../builtinFonts/ui_symbols_10.h
