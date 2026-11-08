#!/bin/bash
# macOS port of tools/mkiosroot.sh: the SWEET iOS root as one case-sensitive,
# journaled HFS+ volume in a raw image (no partition map), for rd=ufsroot0.
#
#   sudo tools/mkiosroot-macos.sh OS.dmg SYSTEMOS.dmg APPOS.dmg OUT.img [SIZE]   (SIZE default 16g)
#
# Only built-in macOS tools: hdiutil attaches the IPSW's APFS images read-only
# (the UDIF OS image and the raw cryptex images), newfs_hfs makes the volume,
# bsdtar copies (hard links, symlinks, numeric owners and modes kept; files
# are read decompressed and written plain, no xattrs/ACLs/flags, so the
# LZBITMAP decmpfs xattrs are not carried over), fsck_hfs checks the result.
# Same layout and SWEET additions as mkiosroot.sh; run
# tools/sweet-root-tweaks.py on the mounted volume afterwards (this script
# does it when TWEAKS=1, the default).
set -euo pipefail

usage() { sed -n '2,12p' "$0"; exit 2; }
[[ $# -ge 4 ]] || usage
[[ $EUID == 0 ]] || { echo "run as root (owners 0/33/272/501 must be kept)" >&2; exit 1; }
os=$(realpath "$1") sysos=$(realpath "$2") appos=$(realpath "$3") out=$4 size=${5:-16g}
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)

work=$(mktemp -d /tmp/mkiosroot.XXXXXX)
devs=()          # attached /dev/diskN, detached in reverse order
root_dev=""
cleanup() {
	set +e
	if [[ -n $root_dev ]] && mount | grep -q "^$root_dev on "; then
		umount "$work/root"
	fi
	for ((i=${#devs[@]}-1; i>=0; i--)); do
		hdiutil detach -quiet "${devs[$i]}" || hdiutil detach -force -quiet "${devs[$i]}"
	done
	# Anything still mounted under $work (e.g. attached before its device
	# could be recorded): detach by mount point.
	for m in "$work"/*; do
		if mount | grep -q " on $(realpath "$m") "; then
			hdiutil detach -quiet "$m" || hdiutil detach -force -quiet "$m"
		fi
	done
	rmdir "$work"/* "$work" 2>/dev/null
}
trap cleanup EXIT

# attach IMAGE NAME [raw]: read-only, not browsed, at $work/NAME; prints the
# device of the mounted volume. The OS image is UDIF; cryptexes are raw APFS.
plist_field() {
	python3 -I -c '
import plistlib, sys
d = plistlib.loads(sys.stdin.buffer.read())
ents = d["system-entities"]
want = sys.argv[1]
if want == "whole":
    print(min((e["dev-entry"] for e in ents), key=len))
else:
    m = [e for e in ents if "mount-point" in e]
    print(m[0][want] if m else "")
' "$1"
}
attach_ro() {
	local img=$1 name=$2 out
	mkdir "$work/$name"
	if ! out=$(hdiutil attach -plist -readonly -nobrowse -noverify -noautofsck -owners on \
			-mountpoint "$work/$name" "$img" 2>/dev/null); then
		out=$(hdiutil attach -plist -readonly -nobrowse -noverify -noautofsck -owners on \
			-imagekey diskimage-class=CRawDiskImage -mountpoint "$work/$name" "$img")
	fi
	devs+=("$(printf '%s' "$out" | plist_field whole)")
	[[ -n $(printf '%s' "$out" | plist_field mount-point) ]] || { echo "$img: no volume mounted" >&2; exit 1; }
	echo "attached $img -> $work/$name"
}
attach_ro "$os" os
attach_ro "$sysos" sysos
attach_ro "$appos" appos
[[ -x $work/os/sbin/launchd ]] || { echo "OS image has no /sbin/launchd: wrong dmg?" >&2; exit 1; }

# Raw image -> whole-disk device -> HFS+ (case-sensitive, journaled) on it.
mkfile -n "$size" "$out"
root_out=$(hdiutil attach -plist -nomount -imagekey diskimage-class=CRawDiskImage "$out")
root_dev=$(printf '%s' "$root_out" | plist_field whole)
devs+=("$root_dev")
newfs_hfs -s -J -v iOSRoot "${root_dev/disk/rdisk}"
# DiskArbitration may auto-mount the new volume under /Volumes (ignoring
# owners): take it back before mounting it ourselves.
sleep 2
diskutil unmount "$root_dev" >/dev/null 2>&1 || true
mkdir "$work/root"
mount -t hfs -o nobrowse "$root_dev" "$work/root"
# Keep Spotlight and fseventsd off the volume.
touch "$work/root/.metadata_never_index"
mkdir -p "$work/root/.fseventsd" && touch "$work/root/.fseventsd/no_log"
mdutil -i off "$work/root" >/dev/null 2>&1 || true
# Owners must stick (a volume mounted with "ignore ownership" would make
# every file 99:99): check with a throwaway file.
touch "$work/root/.ownertest" && chown 272:272 "$work/root/.ownertest"
[[ $(stat -f %u "$work/root/.ownertest") == 272 ]] || { echo "volume ignores ownership" >&2; exit 1; }
rm "$work/root/.ownertest"

copy() {
	echo "copying $1 -> ${2#"$work/root"}"
	mkdir -p "$2"
	(cd "$1" && tar --numeric-owner --no-mac-metadata --no-xattrs --no-acls --no-fflags -cf - .) |
		(cd "$2" && tar --numeric-owner --no-mac-metadata --no-xattrs --no-acls --no-fflags -xpf -)
}
copy "$work/os" "$work/root"
copy "$work/sysos" "$work/root/private/preboot/Cryptexes/OS"
copy "$work/appos" "$work/root/private/preboot/Cryptexes/App"
# hdiutil's own APFS metadata dirs, if any came along
rm -rf "$work/root/.fseventsd/"[0-9]* "$work/root/private/preboot/Cryptexes/"{OS,App}/{.fseventsd,.Trashes,.Spotlight-V100} 2>/dev/null || true

# Shared cache where dyld looks at ignition level 1: hard links, no extra space.
dsc=private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld
mkdir -p "$work/root/System/Library/Caches/com.apple.dyld"
for f in "$work/root/$dsc"/*; do
	ln "$f" "$work/root/System/Library/Caches/com.apple.dyld/${f##*/}"
done

# dyld does not search the cryptexes at ignition level 1: hard-link every
# cryptex file without a counterpart in the OS root to its root path
# (symlinks are copied, as in mkiosroot.sh).
for c in OS App; do
	(cd "$work/root/private/preboot/Cryptexes/$c" && find . ! -type d ! -path './System/Library/Caches/com.apple.dyld/*') |
	while IFS= read -r f; do
		f=${f#./}
		if [[ ! -e "$work/root/$f" && ! -L "$work/root/$f" ]]; then
			mkdir -p "$work/root/$(dirname "$f")"
			src="$work/root/private/preboot/Cryptexes/$c/$f"
			if [[ -L $src ]]; then cp -P "$src" "$work/root/$f"; else ln "$src" "$work/root/$f"; fi
			echo "linked $c:/$f"
		fi
	done
done

# logd's log stores (owner _logd 272 and a .migrated marker, see mkiosroot.sh).
db="$work/root/private/var/db"
install -d -o 272 -g 272 -m 0751 "$db/diagnostics"
install -d -o 272 -g 272 -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do
	: > "$db/$d/.migrated"; chown 272:272 "$db/$d/.migrated"; chmod 0644 "$db/$d/.migrated"
done

# System keybag for the built-in AppleKeyStore (no passcode).
install -d -m 0700 -o 0 -g 0 "$work/root/private/var/keybags"
kb="$work/root/private/var/keybags/systembag.kb"
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' > "$kb"
chown 0:0 "$kb"; chmod 0600 "$kb"

if [[ ${TWEAKS:-1} == 1 ]]; then
	python3 -I "$here/sweet-root-tweaks.py" "$work/root"
fi

# Sanity: owners and the files XNU/launchd need first.
for p in sbin/launchd usr/lib/dyld System/Library/xpc/launchd.plist; do
	stat -f "%Su:%Sg %Sp %z $p" "$work/root/$p"
done
df -h "$work/root"
rm -rf "$work/root/.Spotlight-V100" "$work/root/.Trashes" "$work/root/.DS_Store"
sync
umount "$work/root"
fsck_hfs -fn "$root_dev"
# mkfile leaves the image root-owned, 0600 with the sticky bit: hand it back
# to the user who ran sudo.
chmod 0644 "$out"
[[ -n ${SUDO_UID:-} ]] && chown "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$out"
echo "done: $out"
