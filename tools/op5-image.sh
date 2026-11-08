#!/bin/bash
# One OnePlus 5 boot image (fastboot boot only) from a kernel, with the
# boot-args of a stage. Builds the ADT, the payload and the shim, then checks
# that the image's ramdisk is the payload and prints its sha256.
#
#   tools/op5-image.sh KERNEL NAME stage1|stage2 [EXTRA BOOT-ARGS...]
#     -> shim/out/NAME.img
#
#   stage1  read-only XNU to the root mount (no container, no writes, touch off)
#   stage2  iOS from /data/ios/root.img: needs research/root.map from
#           `ioscont map` and research/ipsw-8/d20ap.adt; cpus=4, writable
#           container, sweetfb_hwscale=0
#
# Environment: CPUS (stage2, default 4), GOLD_L / GOLD_MV (passed to
# shim/build.sh), PATH must provide the aarch64-linux-gnu-* tools and
# mkbootimg (see NOTES-OP5.md: toolbin/), UNPACK = AOSP unpack_bootimg.py
# for the check (skipped when unset).
# Two runs never give the same image: mkadt.py puts a fresh 64-byte
# /chosen random-seed (os.urandom) into every ADT.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ $# -ge 3 ]] || { sed -n '2,19p' "$0"; exit 2; }
kernel=$(realpath "$1") name=$2 stage=$3
shift 3
extra=${*:-}

case $stage in
stage1)
	adt_args=(--highmem)
	args="debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress rd=ufsroot0 wdt=-1 wdt_test=no_user_checkin sweetlog=0 sweettouch=0"
	;;
stage2)
	[[ -f research/root.map ]] || { echo "research/root.map missing (ioscont map)" >&2; exit 1; }
	[[ -f research/ipsw-8/d20ap.adt ]] || { echo "research/ipsw-8/d20ap.adt missing" >&2; exit 1; }
	adt_args=(--highmem --container research/root.map)
	args="cpus=${CPUS:-4} debug=0xc serial=3 keepsyms=1 -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=30 sweetfb_hwscale=0"
	;;
*)
	echo "stage must be stage1 or stage2" >&2; exit 2
	;;
esac
[[ -n $extra ]] && args="$args $extra"

out=shim/out
mkdir -p "$out"
python3 -I tools/mkadt.py "$out/$name.adt" "${adt_args[@]}"
python3 -I tools/mkpayload.py "$kernel" "$out/$name.adt" "$PWD/$out/$name.payload" --cmdline "$args" >/dev/null
# build.sh changes into shim/: the payload path must be absolute.
shim/build.sh "$PWD/$out/$name.payload" >/dev/null
cp "$out/shim-boot.img" "$out/$name.img"

if [[ -n ${UNPACK:-} ]]; then
	tmp=$(mktemp -d)
	python3 "$UNPACK" --boot_img "$out/$name.img" --out "$tmp" >/dev/null
	cmp "$out/$name.payload" "$tmp/ramdisk" || { echo "ramdisk != payload" >&2; rm -rf "$tmp"; exit 1; }
	rm -rf "$tmp"
	echo "check: ramdisk == payload"
fi
echo "kernel: $(shasum -a 256 "$kernel" | cut -c1-16)  $kernel"
echo "args:   $args"
shasum -a 256 "$out/$name.img"
