#!/bin/bash
# iOS side of the vibration (xnu 0141) on the running phone, over SSH.
#
#   tools/op5-haptics-root.sh install DRIVER_DIR   RedwoodAudio.driver in, VirtualAudio back, MobileGestalt cache reset
#   tools/op5-haptics-root.sh undo                 driver out, VirtualAudio put aside again (as sweet-root-tweaks.py audio)
#   tools/op5-haptics-root.sh status
#
# DRIVER_DIR holds RedwoodAudio.driver (userland/audio/build.sh). Everything moved is kept next to it
# (/var/root/haptics-backup), nothing is deleted except MobileGestalt's cache, which iOS rebuilds at the
# next boot from the device tree (then with /product/haptics: DeviceSupportsClosedLoopHaptics).
# A reboot is needed after install and after undo (mediaserverd and MobileGestalt read these at start).
set -euo pipefail
cd "$(dirname "$0")/.."
SSH=(ssh -F ../dumps/ssh_config op5)
SCP=(scp -O -F ../dumps/ssh_config)
HAL=/System/Library/Audio/Plug-Ins/HAL
VA=/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin
VA_OFF=/Library/Audio/Plug-Ins/HAL.disabled/VirtualAudio.plugin
MG=/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist
BK=/var/root/haptics-backup

case ${1:-} in
install)
	drv=${2:?DRIVER_DIR with RedwoodAudio.driver}
	[[ -f $drv/RedwoodAudio.driver/RedwoodAudio && -f $drv/RedwoodAudio.driver/Info.plist ]] || { echo "no driver in $drv" >&2; exit 1; }
	"${SSH[@]}" "mkdir -p $BK /var/root/RedwoodAudio.tmp"
	"${SCP[@]}" -r "$drv/RedwoodAudio.driver" op5:/var/root/RedwoodAudio.tmp/
	"${SSH[@]}" "set -e
		[ -d $HAL/RedwoodAudio.driver ] && mv $HAL/RedwoodAudio.driver $BK/RedwoodAudio.driver.\$(date +%s)
		mv /var/root/RedwoodAudio.tmp/RedwoodAudio.driver $HAL/ && rmdir /var/root/RedwoodAudio.tmp
		chown -R root:wheel $HAL/RedwoodAudio.driver && chmod -R 755 $HAL/RedwoodAudio.driver
		if [ -d $VA_OFF ] && [ ! -d $VA ]; then mv $VA_OFF $VA && echo 'VirtualAudio.plugin back'; fi
		if [ -f $MG ]; then cp -p $MG $BK/com.apple.MobileGestalt.plist.\$(date +%s) && rm $MG && echo 'MobileGestalt cache removed (saved in $BK)'; fi
		ls -l $HAL/RedwoodAudio.driver; ls -d $VA"
	echo "now reboot the phone (fastboot boot of the 0141 image)"
	;;
undo)
	"${SSH[@]}" "set -e; mkdir -p $BK /Library/Audio/Plug-Ins/HAL.disabled
		[ -d $HAL/RedwoodAudio.driver ] && mv $HAL/RedwoodAudio.driver $BK/RedwoodAudio.driver.\$(date +%s)
		if [ -d $VA ] && [ ! -d $VA_OFF ]; then mv $VA $VA_OFF && echo 'VirtualAudio.plugin put aside'; fi
		ls $BK"
	echo "now reboot the phone"
	;;
status)
	"${SSH[@]}" "ls -ld $HAL/RedwoodAudio.driver $VA $VA_OFF $MG /dev/sweet-haptic 2>&1
		for n in frames plays cuts max_ms duty_ms rest_ms; do echo kern.sweethaptic_\$n \$(/usr/local/bin/sweetctl kern.sweethaptic_\$n); done"
	;;
*)
	sed -n '2,11p' "$0"; exit 2
	;;
esac
