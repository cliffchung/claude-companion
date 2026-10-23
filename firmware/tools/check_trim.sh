#!/bin/sh
# Fails if removed modules (IME/SKK/web file manager) or the old product name linger.
cd "$(dirname "$0")/../.." || exit 1
fail=0
hits=$(grep -rIl -E '(^|[^[:alnum:]])ime_|SKK|skk_|pinyin|romaji|webfm' firmware/main firmware/CMakeLists.txt)
[ -n "$hits" ] && { echo "removed-module references:"; echo "$hits"; fail=1; }
for d in firmware/main/ime firmware/main/web; do
    [ -e "$d" ] && { echo "still exists: $d"; fail=1; }
done
hits=$(grep -rIn 'Clientre' firmware/main firmware/CMakeLists.txt firmware/tools/merge_flash.sh | grep -v 'firmware/main/ui/ui_devinfo.c:.*Based on Clientre 5 by Mozgi512 (MIT)')
[ -n "$hits" ] && { echo "old product name:"; echo "$hits"; fail=1; }
[ $fail -eq 0 ] || exit 1
echo "trim: OK"
