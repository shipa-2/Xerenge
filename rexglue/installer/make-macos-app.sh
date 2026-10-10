#!/bin/sh
# Builds the installer and packs it into a macOS app with its Qt frameworks and
# the built game it installs (the payload), the macOS counterpart of
# make-appimage.sh.
#
#   XERENGE_PAYLOAD=<dir> ./installer/make-macos-app.sh
#       ->   installer/Burnout_Revenge_Installer-macos-arm64.zip
#
# The payload directory holds bin/ (burnout, the libraries beside it and the
# vulkan/ directory the SDK stages there) and tools/extract-image. Needs Qt 6
# (brew install qt); the app carries its own copy, so players do not.
set -e
here=$(cd "$(dirname "$0")" && pwd)
build="$here/build-macos"
payload=${XERENGE_PAYLOAD:?name the built game with XERENGE_PAYLOAD=<dir>}
if [ ! -x "$payload/bin/burnout" ] || [ ! -x "$payload/tools/extract-image" ]; then
    echo "$payload needs bin/burnout and tools/extract-image" >&2
    exit 1
fi
if [ ! -f "$payload/bin/vulkan/share/vulkan/icd.d/MoltenVK_icd.json" ]; then
    echo "$payload/bin has no vulkan/ beside the game (the SDK stages it there)" >&2
    exit 1
fi
qt=${QT_DIR:-$(brew --prefix qt 2>/dev/null || true)}

cmake -S "$here" -B "$build" -DCMAKE_BUILD_TYPE=Release ${qt:+-DCMAKE_PREFIX_PATH="$qt"}
cmake --build "$build" -j"$(sysctl -n hw.ncpu)"

app="$build/Burnout Revenge Installer.app"
rm -rf "$app"
cp -R "$build/xerenge-installer.app" "$app"

# The icon, made from the AppImage's. That one is 256x256, so nothing larger.
iconset="$build/xerenge.iconset"
rm -rf "$iconset"
mkdir -p "$iconset" "$app/Contents/Resources"
icon() {
    sips -z "$1" "$1" "$here/appimage/xerenge-installer.png" --out "$iconset/$2" >/dev/null
}
icon 16 icon_16x16.png
icon 32 icon_16x16@2x.png
icon 32 icon_32x32.png
icon 64 icon_32x32@2x.png
icon 128 icon_128x128.png
icon 256 icon_128x128@2x.png
icon 256 icon_256x256.png
iconutil -c icns "$iconset" -o "$app/Contents/Resources/xerenge.icns"

"${qt:+$qt/bin/}macdeployqt" "$app"

# Where the installer looks for it (LocatePayload): ../Resources/payload from
# Contents/MacOS.
cp -R "$payload" "$app/Contents/Resources/payload"

# Nothing may still point into Homebrew or /usr/local: on a Mac without them
# the app would not start.
leaks=$(find "$app" -type f -perm +111 -exec sh -c \
    'otool -L "$1" 2>/dev/null | grep -E "^[[:space:]]+(/opt/homebrew|/usr/local)/" | sed "s|^|$1: |"' \
    _ {} \;)
if [ -n "$leaks" ]; then
    echo "$leaks" >&2
    echo "libraries outside the app are still linked; see above" >&2
    exit 1
fi

# Signed ad hoc: Apple silicon refuses to run unsigned code, and there is no
# developer certificate here. Gatekeeper still asks once (System Settings,
# Privacy & Security, Open Anyway) for a download.
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"

out="$here/Burnout_Revenge_Installer-macos-arm64.zip"
rm -f "$out"
ditto -c -k --keepParent "$app" "$out"
echo "built: $out"
