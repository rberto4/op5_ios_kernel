#!/bin/bash
# After an iOS boot, from TWRP booted with `fastboot boot` (adb as root, no
# Android su needed). Read-only:
#   1. re-maps /data/ios/root.img and compares it with research/root.map
#      (the map the ADT was built from); a difference means the container's
#      blocks moved and the ADT must be rebuilt before the next writable boot
#   2. pulls the SWEET console log (last 1025 blocks of the container) and
#      decodes it with tools/sweet-log.py
#
#   tools/op5-twrp-log.sh OUT          -> OUT.bin, OUT.txt
#
# Needs shim/out/ioscont (tools/build-ioscont.sh, or
# `zig cc -target aarch64-linux-musl -static`).
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:?usage: op5-twrp-log.sh OUT}
adb=${ADB:-adb}
img=/data/ios/root.img

"$adb" get-state | grep -qx recovery || { echo "phone not in TWRP (adb state recovery)" >&2; exit 1; }
"$adb" push shim/out/ioscont /tmp/ioscont >/dev/null
"$adb" shell chmod 755 /tmp/ioscont

map=$(mktemp)
"$adb" shell "/tmp/ioscont map $img" > "$map"
if diff <(awk '/^extent/{print $2,$3,$4}' research/root.map) <(awk '/^extent/{print $2,$3,$4}' "$map") >/dev/null; then
	echo "map: unchanged ($(grep -c '^extent' "$map") extents)"
else
	echo "map: CHANGED - rebuild the ADT (tools/op5-image.sh) before a writable boot; new map in $map" >&2
	exit 1
fi
rm -f "$map"

bytes=$("$adb" shell "stat -c %s $img" | tr -d '\r')
skip=$((bytes / 4096 - 1025))
"$adb" exec-out "dd if=$img bs=4096 skip=$skip count=1025 2>/dev/null" > "$out.bin"
got=$(stat -f %z "$out.bin" 2>/dev/null || stat -c %s "$out.bin")
[[ $got == $((1025 * 4096)) ]] || { echo "short read: $got bytes" >&2; exit 1; }
python3 -I tools/sweet-log.py "$out.bin" "$out.txt"
echo "log: $out.txt ($(wc -l < "$out.txt") lines)"
