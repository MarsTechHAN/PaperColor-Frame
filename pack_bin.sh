#!/usr/bin/env bash
set -euo pipefail

# Package this ESP-IDF project into one flashable binary.
# Override IDF_PATH if your ESP-IDF is not in ~/esp/esp-idf.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
OUT_DIR="$ROOT_DIR/dist"
OUT_BIN="$OUT_DIR/paper_e6_esp32s3_single.bin"
MERGED_BIN="$ROOT_DIR/build/merged-binary.bin"

if [[ ! -f "$IDF_PATH/export.sh" ]]; then
  echo "ERROR: ESP-IDF export.sh not found: $IDF_PATH/export.sh" >&2
  echo "Set IDF_PATH, for example: IDF_PATH=~/esp/esp-idf ./pack_bin.sh" >&2
  exit 1
fi

# shellcheck disable=SC1091
source "$IDF_PATH/export.sh"

cd "$ROOT_DIR"
idf.py merge-bin

mkdir -p "$OUT_DIR"
cp "$MERGED_BIN" "$OUT_BIN"

printf '\nPack complete:\n  %s\n\n' "$OUT_BIN"
printf 'Flash with:\n  python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 "%s"\n' "$OUT_BIN"
