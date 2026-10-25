#!/bin/sh
# Builds the release images: the individual partition binaries plus a merged full-flash image (flash at 0x0).
# Run after `idf.py build` in firmware/.
#
# The full image is for a BLANK Tab5 only: it fills every gap with 0xFF, so it overwrites NVS (0x9000: Wi-Fi, the
# device key, the host-key pins, the saved hosts, the settings). To update a Tab5 that is already set up, use
# `idf.py -p PORT flash` (keeps NVS) once, then Settings > About > Update firmware (README > Update over Wi-Fi).
set -e
echo "WARNING: the full image overwrites NVS (Wi-Fi, keys, settings). For a blank Tab5 only; use idf.py flash otherwise."
HERE=$(cd "$(dirname "$0")" && pwd)
FW="$HERE/.."
OUT="${1:-$FW/release}"
mkdir -p "$OUT/partitions" "$OUT/full"
cp "$FW/build/bootloader/bootloader.bin"            "$OUT/partitions/"
cp "$FW/build/partition_table/partition-table.bin"  "$OUT/partitions/partition_table.bin"
cp "$FW/build/ota_data_initial.bin"                 "$OUT/partitions/"
cp "$FW/build/claude_companion.bin"                 "$OUT/partitions/"
# bootloader 0x2000, partition table 0x8000, a blank otadata 0xF000 (boots app0), the app in app0 0x20000
python -m esptool --chip esp32p4 merge_bin --flash_mode dio --flash_size 16MB --flash_freq 80m \
  -o "$OUT/full/claude_companion_full_flash.bin" \
  0x2000  "$OUT/partitions/bootloader.bin" \
  0x8000  "$OUT/partitions/partition_table.bin" \
  0xF000  "$OUT/partitions/ota_data_initial.bin" \
  0x20000 "$OUT/partitions/claude_companion.bin"
ls -l "$OUT/partitions" "$OUT/full"
