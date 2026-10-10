#!/bin/sh
# Reports whether this machine can build and run the title, and names what is
# missing rather than letting a build fail halfway through.
set -e

missing=0

need_command() {
    if command -v "$1" >/dev/null 2>&1; then
        printf '  %-16s present\n' "$1"
    else
        printf '  %-16s MISSING - %s\n' "$1" "$2"
        missing=$((missing + 1))
    fi
}

need_library() {
    if pkg-config --exists "$1" 2>/dev/null; then
        printf '  %-16s present\n' "$1"
    else
        printf '  %-16s MISSING - %s\n' "$1" "$2"
        missing=$((missing + 1))
    fi
}

echo "build tools:"
need_command git       "version control"
need_command cmake     "the build system, 3.25 or newer"
need_command ninja     "the SDK's and the shader translator's build presets use it"
need_command clang     "the project is built with clang; gcc is untested"
need_command clang++   "likewise"
need_command pkg-config "finding libraries"

echo
if [ "$(uname -s)" = Darwin ]; then
    # The SDK builds its own Vulkan loader and MoltenVK on a Mac and stages
    # them beside the game; there is no X11 or Wayland to look for.
    echo "libraries:"
    echo "  (none needed on macOS)"
else
    echo "libraries:"
    need_library vulkan      "the Vulkan loader and headers"
    need_library x11-xcb     "the SDK window code requires it unconditionally"
    need_library wayland-client "required to build, even when running under X11"
fi

echo
echo "to run:"
if command -v vulkaninfo >/dev/null 2>&1; then
    if vulkaninfo --summary >/dev/null 2>&1; then
        printf '  %-16s present\n' "Vulkan driver"
    else
        printf '  %-16s MISSING - vulkaninfo reports no device\n' "Vulkan driver"
        missing=$((missing + 1))
    fi
else
    printf '  %-16s not checked (no vulkaninfo)\n' "Vulkan driver"
fi

echo
if [ "$missing" -eq 0 ]; then
    echo "everything is in place."
else
    echo "missing: $missing."
    if [ "$(uname -s)" = Darwin ]; then
        echo "on macOS: xcode-select --install, then brew install cmake ninja pkgconf"
    else
        echo "on Arch: sudo pacman -S --needed git cmake ninja clang pkgconf vulkan-icd-loader vulkan-headers libx11 libxcb wayland"
    fi
    exit 1
fi
