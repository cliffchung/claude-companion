#!/bin/sh
# Stage a firmware build for the Tab5's wireless update (Plan 6, spec §9.4).
#   sh tools/stage_update.sh [path/to/claude_companion.bin]
# The default is build/claude_companion.bin next to this script's parent (firmware/). The file must start with the
# ESP image magic 0xE9, hold the app descriptor magic at offset 32 and the project name claude_companion at
# offset 80. It is copied to $STAGE_DIR/claude_companion.bin.tmp, then moved onto $STAGE_DIR/claude_companion.bin
# (atomic on one file system), so the Tab5 never reads a half-written file. STAGE_DIR defaults to
# ~/.local/share/claude-companion, the default "Firmware file on Host" path (Settings > Host). The script writes
# nothing else, anywhere. Prints: staged <version>, <size> B, sha256 <first 12 hex>...
set -eu

die() { echo "stage_update: $*" >&2; exit 1; }

here=$(cd "$(dirname "$0")" && pwd)
bin=${1:-$here/../build/claude_companion.bin}
dir=${STAGE_DIR:-$HOME/.local/share/claude-companion}
[ $# -le 1 ] || die "usage: $0 [path/to/claude_companion.bin]"
[ -f "$bin" ] && [ -r "$bin" ] || die "no readable file at $bin (run idf.py build first)"

# hex of bytes [skip, skip + count)
hexat() { od -An -v -tx1 -j "$2" -N "$3" "$1" | tr -d ' \n'; }

[ "$(hexat "$bin" 0 1)" = e9 ] || die "$bin isn't an ESP app image (no 0xE9 magic)"
[ "$(hexat "$bin" 32 4)" = 3254cdab ] || die "$bin has no app descriptor (it may be the bootloader)"
# "claude_companion" and its NUL
[ "$(hexat "$bin" 80 17)" = 636c617564655f636f6d70616e696f6e00 ] || die "$bin isn't a Claude Companion build"
version=$(dd if="$bin" bs=1 skip=48 count=32 2>/dev/null | tr '\000' '\n' | head -n 1)
[ -n "$version" ] || die "$bin has no version string"

sha() { shasum -a 256 < "$1" 2>/dev/null || sha256sum < "$1"; }

mkdir -p "$dir"
tmp=$dir/claude_companion.bin.tmp
trap 'rm -f "$tmp"' EXIT
cp "$bin" "$tmp"
src_sha=$(sha "$bin")
tmp_sha=$(sha "$tmp")
[ "${src_sha%% *}" = "${tmp_sha%% *}" ] || die "the copy doesn't match $bin (was it being rebuilt?)"
size=$(wc -c < "$tmp")
mv -f "$tmp" "$dir/claude_companion.bin"
trap - EXIT
h=${tmp_sha%% *}
echo "staged $version, $((size)) B, sha256 $(printf %.12s "$h")…"
