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
    Darwin)
        # Apple silicon only: nobody has built or run this on an Intel Mac.
        if [ "$(uname -m)" != arm64 ]; then
            echo "macOS: only Apple silicon (arm64) is set up, this is $(uname -m)" >&2
            exit 1
        fi
        XR_OS=mac
        XR_PRESET=mac-arm64
        XR_EXE=
        ;;
    *)
        XR_OS=linux
        XR_PRESET=linux-amd64
        XR_EXE=
        ;;
esac
# XR_JOBS may be set to fewer: the generated code is large, and a machine with
# little memory runs out of it at one job per core.
XR_JOBS=${XR_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}

# Every path the scripts build from $root reaches native programs too (CMake
# options, the manifest), and those read D:/..., not the shell's /d/...
if [ "$XR_OS" = windows ] && [ -n "$root" ]; then
    root=$(cd "$root" && pwd -W)
fi

# Runs a network step up to four times: a CI runner sometimes cannot reach
# github.com for half a minute, and one failed submodule fetch ends the build.
retry() {
    n=1
    until "$@"; do
        [ "$n" -ge 4 ] && return 1
        echo "== failed (attempt $n), retrying in $((n * 15)) s: $*" >&2
        sleep $((n * 15))
        n=$((n + 1))
    done
}
