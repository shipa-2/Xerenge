#!/bin/sh
# Builds the installer and packs it into an AppImage with its Qt libraries.
#
#   ./installer/make-appimage.sh   ->   installer/Burnout_Revenge_Installer-x86_64.AppImage
#
# Needs linuxdeploy and linuxdeploy-plugin-qt (AUR: linuxdeploy,
# linuxdeploy-plugin-qt) and Qt 6 development files. The AppImage carries the
# installer only: it fetches the project from GitHub when it runs.
set -e
here=$(cd "$(dirname "$0")" && pwd)
build="$here/build"
appdir="$build/AppDir"

cmake -S "$here" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$build" -j"$(nproc)"

rm -rf "$appdir"
# linuxdeploy's own strip is too old for .relr.dyn sections in current libraries.
export NO_STRIP=1
# Wayland as well as X11, and offscreen for the unattended mode.
export EXTRA_PLATFORM_PLUGINS="libqwayland.so;libqoffscreen.so"
# This pulls in the wayland shell integrations the Wayland platform plugin needs.
export EXTRA_QT_MODULES="waylandcompositor"
export QMAKE=${QMAKE:-$(command -v qmake6 || command -v qmake)}
export OUTPUT="$here/Burnout_Revenge_Installer-x86_64.AppImage"
cd "$build"
linuxdeploy --appdir "$appdir" \
    --executable "$build/xerenge-installer" \
    --desktop-file "$here/appimage/xerenge-installer.desktop" \
    --icon-file "$here/appimage/xerenge-installer.png" \
    --plugin qt \
    --output appimage
echo "built: $OUTPUT"
