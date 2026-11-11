#!/bin/bash
# Build the user-space test programs for the phone (iOS 16, arm64) with Xcode's iPhoneOS SDK, sign them ad hoc
# and list their code-directory hashes (for the trust cache). Output: $OUT (default out/userland).
set -euo pipefail
cd "$(dirname "$0")"
OUT=${OUT:-../out/userland}
mkdir -p "$OUT"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CXX="xcrun --sdk iphoneos clang++ -arch arm64 -miphoneos-version-min=16.0 -isysroot $SDK -O2 -Wall -Wextra -Werror -std=c++17"
$CXX -o "$OUT/gputest" gputest/gputest.cpp -framework IOKit -framework CoreFoundation
for f in "$OUT"/gputest; do
  codesign -s - --force "$f"
  codesign -dvvv "$f" 2>&1 | grep -E "^(CDHash|Hash type|Identifier)="
  echo "$(codesign -dvvv "$f" 2>&1 | sed -n 's/^CDHash=//p') $(basename "$f")" >> "$OUT/cdhashes.txt"
done
shasum -a 256 "$OUT"/* | tee "$OUT/SHA256SUMS"
