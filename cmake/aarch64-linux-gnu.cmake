# Cross-compiling for 64-bit ARM Linux on an x86-64 Linux host, with clang and
# the distribution's aarch64-linux-gnu sysroot (Arch: aarch64-linux-gnu-gcc,
# which also brings glibc and libstdc++). The first step towards the Android
# port: it shows that the SDK, the generated code and the renderer build for
# ARM64 before Android's own differences come in.
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake ...
#   qemu-aarch64 -L /usr/aarch64-linux-gnu \
#     -E LD_LIBRARY_PATH=$HOME/Projects/xerenge-cross/aarch64-extra/usr/lib <program>
#                                                     runs the result here
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(XERENGE_CROSS_TRIPLE aarch64-linux-gnu)
set(XERENGE_CROSS_SYSROOT /usr/${XERENGE_CROSS_TRIPLE} CACHE PATH "The aarch64 sysroot")

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${XERENGE_CROSS_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${XERENGE_CROSS_TRIPLE})
set(CMAKE_ASM_COMPILER_TARGET ${XERENGE_CROSS_TRIPLE})
set(CMAKE_SYSROOT ${XERENGE_CROSS_SYSROOT})
set(CMAKE_C_FLAGS_INIT "--gcc-toolchain=/usr")
set(CMAKE_CXX_FLAGS_INIT "--gcc-toolchain=/usr")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")

# What the sysroot lacks - libX11, libxcb, wayland-client and what they need -
# unpacked from Arch Linux ARM packages into a directory of its own. pkg-config
# looks only there, with its paths rewritten into it: the host's own .pc files
# would hand the build x86 headers from /usr/include.
set(XERENGE_CROSS_EXTRA $ENV{HOME}/Projects/xerenge-cross/aarch64-extra CACHE PATH
    "aarch64 libraries missing from the sysroot (X11, xcb, wayland)")
set(ENV{PKG_CONFIG_LIBDIR} "${XERENGE_CROSS_EXTRA}/usr/lib/pkgconfig:${XERENGE_CROSS_EXTRA}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${XERENGE_CROSS_EXTRA}")
set(ENV{PKG_CONFIG_PATH} "")
foreach(kind EXE SHARED MODULE)
  string(APPEND CMAKE_${kind}_LINKER_FLAGS_INIT
         " -L${XERENGE_CROSS_EXTRA}/usr/lib -Wl,-rpath-link,${XERENGE_CROSS_EXTRA}/usr/lib")
endforeach()

# Programs from the host, libraries and headers only from the sysroots.
set(CMAKE_FIND_ROOT_PATH ${XERENGE_CROSS_SYSROOT} ${XERENGE_CROSS_EXTRA})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64 -L ${XERENGE_CROSS_SYSROOT}
    -E LD_LIBRARY_PATH=${XERENGE_CROSS_EXTRA}/usr/lib)
