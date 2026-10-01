#!/bin/bash
# Packages the already-built Linux Standalone/VST3/LV2 binaries (see build-linux.sh) into a
# single .deb - Alan's request, 2026-09-07, to round out the Linux release alongside the
# Windows/macOS builds the CI already produces.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.."; pwd)
BUILD_DIR="$ROOT/Builds/LinuxMakefile/build"

for f in "$BUILD_DIR/JiV-881" "$BUILD_DIR/JiV-881.vst3" "$BUILD_DIR/JiV-881.lv2"; do
  if [ ! -e "$f" ]; then
    echo "Missing $f - run build-linux.sh first." >&2
    exit 1
  fi
done

# The JUCERPROJECT tag's own version="..." attribute (not the XML declaration's version="1.0" on
# line 1, which a plain grep for version="..." would match first).
VERSION=$(python3 -c "
import re
with open('$ROOT/VirtualJiV.jucer') as f:
    text = f.read()
print(re.search(r'<JUCERPROJECT.*?version=\"([0-9.]+)\"', text, re.S).group(1))
")

ARCH=amd64
PKG_NAME=jiv881
PKG_ROOT="$ROOT/build/deb-root"
# Filename deliberately doesn't embed $VERSION - this project's GitHub release is a rolling
# "latest" prerelease (rebuilt on every push to main, see .github/workflows/main.yml and the
# README's own download links), so a stable filename avoids every version bump silently
# breaking the README's link. The real package version still lives in DEBIAN/control below.
DEB_OUT="$ROOT/build/${PKG_NAME}_${ARCH}.deb"

rm -rf "$PKG_ROOT"
mkdir -p "$PKG_ROOT/DEBIAN" \
         "$PKG_ROOT/usr/bin" \
         "$PKG_ROOT/usr/lib/vst3" \
         "$PKG_ROOT/usr/lib/lv2" \
         "$PKG_ROOT/usr/share/applications"

install -m 755 "$BUILD_DIR/JiV-881" "$PKG_ROOT/usr/bin/jiv881"
cp -r "$BUILD_DIR/JiV-881.vst3" "$PKG_ROOT/usr/lib/vst3/"
cp -r "$BUILD_DIR/JiV-881.lv2" "$PKG_ROOT/usr/lib/lv2/"

cat > "$PKG_ROOT/usr/share/applications/jiv881.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Virtual JiV-881
Comment=Roland JV-880 emulator
Exec=/usr/bin/jiv881
Terminal=false
Categories=Audio;Music;AudioVideo;
EOF

# Direct link deps (confirmed via ldd) plus the X11 libs JUCE's GUI/audio-device backends dlopen
# at runtime rather than link against directly - both matter for the app to actually run.
cat > "$PKG_ROOT/DEBIAN/control" <<EOF
Package: $PKG_NAME
Version: $VERSION
Section: sound
Priority: optional
Architecture: $ARCH
Maintainer: luginf
Homepage: https://github.com/luginf/virtual-jiv-881
Depends: libasound2, libfreetype6, libfontconfig1, libcurl4, libx11-6, libxext6, libxrender1, libxrandr2, libxinerama1, libxcursor1
Description: Virtual JiV-881 - Roland JV-880 emulator
 Emulator of the Roland JV-880 rompler synthesizer, based on Nuked-SC55.
 Installs the standalone application (jiv881) plus VST3 and LV2 plugins.
 You'll still need to supply your own ROM dumps - see the project README.
EOF

dpkg-deb --build --root-owner-group "$PKG_ROOT" "$DEB_OUT"
echo "Built $DEB_OUT"
