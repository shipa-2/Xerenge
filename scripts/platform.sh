# Sourced by the other scripts: what differs between the platforms they build
# on. On Windows they run under Git for Windows' bash (MSYS2), which reports
# itself as MINGW64_NT-... or MSYS_NT-...
#
#   XR_OS       linux | windows
#   XR_PRESET   the SDK's CMake preset
#   XR_EXE      the executable suffix
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        XR_OS=windows
        XR_PRESET=win-amd64
        XR_EXE=.exe
        ;;
    *)
        XR_OS=linux
        XR_PRESET=linux-amd64
        XR_EXE=
        ;;
esac
XR_JOBS=$(nproc 2>/dev/null || echo 4)

# Every path the scripts build from $root reaches native programs too (CMake
# options, the manifest), and those read D:/..., not the shell's /d/...
if [ "$XR_OS" = windows ] && [ -n "$root" ]; then
    root=$(cd "$root" && pwd -W)
fi
