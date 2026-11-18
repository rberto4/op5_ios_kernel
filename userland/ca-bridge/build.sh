#!/bin/bash
# libsweet-gl.dylib (eagl.c + sweet_gl_osmesa.c over Mesa's libOSMesa.8.dylib) and
# libsweet-ca.dylib (the loader the kernel inserts into backboardd, xnu 0134 sweetinject).
#   userland/ca-bridge/build.sh OUTDIR [MESA_OUT]
# MESA_OUT holds libOSMesa.8.dylib, gl.xml and exported-gl.txt (default userland/mesa/build-mesa/out).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$1 mesa=${2:-$here/../mesa/build-mesa/out}
mkdir -p "$out"
CC=(xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0)
python3 "$here/mkeagl.py" "$here/eagl_slots.txt" "$mesa/gl.xml" "$mesa/exported-gl.txt" "$here/eagl.c" > "$out/eagl_gen.h"
"${CC[@]}" -O1 -Wall -dynamiclib -install_name /usr/local/lib/libsweet-gl.dylib -I"$out" -I"$here" \
	-framework CoreFoundation -framework IOKit -framework IOSurface -lobjc "$mesa/libOSMesa.8.dylib" \
	-o "$out/libsweet-gl.dylib" "$here/eagl.c" "$here/sweet_gl_osmesa.c" "$here/sweet_prof.c"
"${CC[@]}" -O2 -Wall -Wextra -Werror -dynamiclib -install_name /usr/local/lib/libsweet-ca.dylib \
	-framework IOKit -o "$out/libsweet-ca.dylib" "$here/sweet_ca.c"
# Mesa's own install name may name another phone's folder: ours is /usr/local/lib
old=$(otool -D "$mesa/libOSMesa.8.dylib" | tail -1)
install_name_tool -change "$old" /usr/local/lib/libOSMesa.8.dylib "$out/libsweet-gl.dylib"
codesign -s - -f "$out/libsweet-gl.dylib" "$out/libsweet-ca.dylib"
