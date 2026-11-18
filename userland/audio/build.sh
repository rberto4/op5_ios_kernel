#!/bin/bash
# RedwoodAudio.driver (the AudioServerPlugIn of ios-topaz, MIT; "Actuator" -> /dev/sweet-haptic, xnu 0141).
#   userland/audio/build.sh OUTDIR  -> OUTDIR/RedwoodAudio.driver/{RedwoodAudio,Info.plist}
# On the phone: /System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver (see NOTES-OP5.md, 0141 test).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$1
mkdir -p "$out/RedwoodAudio.driver"
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O1 -Wall -bundle \
	-framework CoreFoundation -o "$out/RedwoodAudio.driver/RedwoodAudio" "$here/audio_plugin.c"
codesign -s - -f "$out/RedwoodAudio.driver/RedwoodAudio"
plutil -lint "$here/audio_plugin.plist"
cp "$here/audio_plugin.plist" "$out/RedwoodAudio.driver/Info.plist"
