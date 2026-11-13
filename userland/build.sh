#!/bin/bash
# Build the user-space programs for the phone (iOS 16, arm64) with Xcode's iPhoneOS SDK, sign them ad hoc and list
# their code-directory hashes (for the trust cache). Output: $OUT (default out/userland).
#   gputest            NativeAdreno430's user client from user space (started by launchd; then runs gltri)
#   libOSMesa.8.dylib  Mesa freedreno + OSMesa (mesa/build-mesa.sh), unless MESA=0
#   gltri              an OpenGL ES 2 triangle through Mesa, checked pixel by pixel
#   libsuzuran.inject.dylib  our library inside backboardd (DYLD_INSERT_LIBRARIES), for the OpenGL ES route
set -euo pipefail
cd "$(dirname "$0")"
OUT=${OUT:-../out/userland}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
: > "$OUT/cdhashes.txt"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CXX="xcrun --sdk iphoneos clang++ -arch arm64 -miphoneos-version-min=16.0 -isysroot $SDK -O2 -Wall -Wextra -Werror -std=c++17"
$CXX -o "$OUT/gputest" gputest/gputest.cpp -framework IOKit -framework CoreFoundation
# libsuzuran.inject.dylib: loaded into backboardd through DYLD_INSERT_LIBRARIES (the OpenGL ES route)
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -isysroot "$SDK" -O2 -Wall -Wextra -Werror \
  -dynamiclib -install_name /usr/lib/libsuzuran.inject.dylib -o "$OUT/libsuzuran.inject.dylib" inject/inject.c \
  -framework IOKit -framework CoreFoundation
SIGN="gputest libsuzuran.inject.dylib"
if [ "${MESA:-1}" = 1 ]; then
  OUT="$OUT" bash mesa/build-mesa.sh
  $CXX -Wno-unused-parameter -I"$OUT/include" -o "$OUT/gltri" gltri/gltri.cpp "$OUT/libOSMesa.8.dylib" \
    -framework IOKit -framework CoreFoundation
  SIGN="gputest libsuzuran.inject.dylib libOSMesa.8.dylib gltri"
fi
for name in $SIGN; do
  f="$OUT/$name"
  codesign -s - --force "$f"
  codesign -dvvv "$f" 2>&1 | grep -E "^(CDHash|Hash type|Identifier)="
  echo "$(codesign -dvvv "$f" 2>&1 | sed -n 's/^CDHash=//p') $name" >> "$OUT/cdhashes.txt"
done
rm -rf "$OUT/include"
(cd "$OUT" && shasum -a 256 $SIGN | tee SHA256SUMS)
