#!/bin/bash
# Mesa 25.0.7 for the phone (iOS 16, arm64), on a macOS host with Xcode: Gallium freedreno (a4xx) behind OSMesa
# (softpipe too: OSMesa requires it, and OSMESA_SOFTWARE=1 selects it),
# with HakiOS's hakios-drm as its libdrm (requests go to NativeAdreno430's user client). Output in $OUT:
# libOSMesa.8.dylib (install name /usr/lib/libOSMesa.8.dylib, signed ad hoc). Build dir: $WORK (default build/mesa).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
WORK=${WORK:-$HERE/../../build/mesa}
OUT=${OUT:-$HERE/../../out/userland}
mkdir -p "$WORK" "$OUT"
cd "$WORK"
[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
echo "$SHA256  mesa-$VERSION.tar.xz" | shasum -a 256 -c --quiet
rm -rf mesa-$VERSION build
tar xJf mesa-$VERSION.tar.xz
for p in "$HERE"/freedreno-darwin.patch "$HERE"/osmesa-es-profile.patch; do
  patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"
done
cp -r "$HERE/hakios-drm" mesa-$VERSION/src/hakios-drm
python3 "$HERE/kernel-fence-waits.py" mesa-$VERSION

# The iPhoneOS SDK has IOKit.framework but not its headers: use the macOS SDK's (the same API).
mkdir -p inc
ln -sfn "$(xcrun --sdk macosx --show-sdk-path)/System/Library/Frameworks/IOKit.framework/Headers" inc/IOKit
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos -f clang)
CXX=$(xcrun --sdk iphoneos -f clang++)
FLAGS="'-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0', '-I$WORK/inc'"
cat > cross.ini <<INI
[binaries]
c = ['$CC', $FLAGS]
cpp = ['$CXX', $FLAGS]
objc = ['$CC', $FLAGS]
ar = '$(xcrun -f ar)'
strip = '$(xcrun -f strip)'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI
# Mesa's GLSL parser wants bison newer than macOS's 2.3
command -v brew >/dev/null && { brew list bison >/dev/null 2>&1 || brew install -q bison; export PATH="$(brew --prefix bison)/bin:$PATH"; }
python3 -m venv venv
venv/bin/pip -q install mako pyyaml packaging meson ninja
export PATH=$WORK/venv/bin:$PATH
meson setup build mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
  --prefix=/usr --libdir=lib --buildtype=release -Db_ndebug=true \
  -Dplatforms= -Dgallium-drivers=softpipe,freedreno -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled -Degl=disabled \
  -Dgbm=disabled -Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
  -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
  -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled \
  -Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log 2>&1 || { tail -60 meson-setup.log; exit 1; }
ninja -C build > ninja.log 2>&1 || { grep -m20 -B2 -A8 "error:" ninja.log || tail -60 ninja.log; exit 1; }
cp build/src/gallium/targets/osmesa/libOSMesa.8.dylib "$OUT/libOSMesa.8.dylib"
xcrun strip -x "$OUT/libOSMesa.8.dylib"
xcrun install_name_tool -id /usr/lib/libOSMesa.8.dylib "$OUT/libOSMesa.8.dylib"
mkdir -p "$OUT/include/GL"
cp mesa-$VERSION/include/GL/osmesa.h mesa-$VERSION/include/GL/gl.h mesa-$VERSION/include/GL/glext.h "$OUT/include/GL/"
cp -r mesa-$VERSION/include/GLES2 mesa-$VERSION/include/GLES3 mesa-$VERSION/include/KHR "$OUT/include/"
ls -l "$OUT/libOSMesa.8.dylib"
