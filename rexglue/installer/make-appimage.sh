#!/bin/sh
# Builds the installer and packs it into an AppImage with its Qt libraries and
# the built game it installs (the payload).
#
#   XERENGE_PAYLOAD=<dir> ./installer/make-appimage.sh
#       ->   installer/Burnout_Revenge_Installer-x86_64.AppImage
#
# The payload directory holds bin/ (burnout and the libraries beside it) and
# tools/extract-image; CI assembles it after building the game. Needs
# linuxdeploy and linuxdeploy-plugin-qt (AUR: linuxdeploy,
# linuxdeploy-plugin-qt) and Qt 6 development files.
set -e
here=$(cd "$(dirname "$0")" && pwd)
build="$here/build"
appdir="$build/AppDir"
payload=${XERENGE_PAYLOAD:?name the built game with XERENGE_PAYLOAD=<dir>}
if [ ! -x "$payload/bin/burnout" ] || [ ! -x "$payload/tools/extract-image" ]; then
    echo "$payload needs bin/burnout and tools/extract-image" >&2
    exit 1
fi

cmake -S "$here" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$build" -j"$(nproc)"

rm -rf "$appdir"
# Where the installer looks for it (LocatePayload): ../share/xerenge/payload
# from usr/bin. linuxdeploy leaves usr/share alone.
mkdir -p "$appdir/usr/share/xerenge"
cp -a "$payload" "$appdir/usr/share/xerenge/payload"
# linuxdeploy's own strip is too old for .relr.dyn sections in current libraries.
export NO_STRIP=1
# This pulls in the wayland shell integrations the Wayland platform plugin needs.
export EXTRA_QT_MODULES="waylandcompositor"
export QMAKE=${QMAKE:-$(command -v qmake6 || command -v qmake)}
# Wayland as well as X11, and offscreen for the unattended mode. The Wayland
# platform plugin is libqwayland.so in current Qt, libqwayland-generic.so in
# older releases (Ubuntu 24.04's 6.4).
platforms="$("$QMAKE" -query QT_INSTALL_PLUGINS)/platforms"
wayland=libqwayland.so
[ -f "$platforms/$wayland" ] || wayland=libqwayland-generic.so
export EXTRA_PLATFORM_PLUGINS="$wayland;libqoffscreen.so"
export OUTPUT="$here/Burnout_Revenge_Installer-x86_64.AppImage"
cd "$build"
linuxdeploy --appdir "$appdir" \
    --executable "$build/xerenge-installer" \
    --desktop-file "$here/appimage/xerenge-installer.desktop" \
    --icon-file "$here/appimage/xerenge-installer.png" \
    --plugin qt \
    --output appimage
echo "built: $OUTPUT"
