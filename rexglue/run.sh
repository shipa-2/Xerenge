#!/bin/sh
# Launch the recompiled title with a log you can hand over, and optionally under
# a debugger.
#
#   ./run.sh            normal run, log at logs/burnout.log
#   ./run.sh --movie    traces the intro movie path: the clip requested, the
#                       APT video state machine and the player's own status. Use
#                       this for the runs where the videos do not start - it
#                       says which step is not reached.
#   ./run.sh --trace    also enables this fork's GPU diagnostics (very verbose:
#                       it reports per draw and per swap)
#   ./run.sh --no-msaa
#                       turns off the guest's 2x multisampling. A multisampled
#                       target cannot be read back from a capture, so this is
#                       what to use when a frame has to be taken apart.
#   ./run.sh --capture
#                       runs under RenderDoc. Get to the screen with the wrong
#                       colours and press F12 to capture that frame; the capture
#                       lands in logs/capture<N>/ and can be handed over as is.
#   ./run.sh --gdb      runs under gdb. Attaching to an already-running instance
#                       does not work here - Yama's ptrace_scope is 1, so only a
#                       descendant of the debugger can be traced - which is why
#                       this starts the game from gdb rather than attaching.
#                       The guest's own memory write-watching uses SIGSEGV, so
#                       gdb is told to pass it through; stopping on it would
#                       halt on ordinary GPU memory tracking.
set -e
cd "$(dirname "$0")"
mkdir -p logs

# Resolve the SDK and the game without hard-coding one machine's layout. The
# SDK sits beside the project in the repository; a working copy kept elsewhere
# can name it with XERENGE_SDK. The game directory is whatever the manifest was
# pointed at when the project was built, so there is one place to change it.
if [ -n "$XERENGE_SDK" ]; then
    SDK_LIB="$XERENGE_SDK/out/linux-amd64"
else
    for candidate in ../rexglue-sdk ../Xerenge/rexglue-sdk; do
        [ -d "$candidate/out/linux-amd64" ] && SDK_LIB="$candidate/out/linux-amd64" && break
    done
fi
if [ -z "$SDK_LIB" ]; then
    echo "не нашёл собранный SDK; укажите его через XERENGE_SDK" >&2
    exit 1
fi

GAME=${XERENGE_GAME:-$(sed -n 's/^game_root *= *"\(.*\)"/\1/p' burnout_manifest.toml | head -1)}
if [ -z "$GAME" ] || [ ! -f "$GAME/default.xex" ]; then
    echo "каталог с игрой не найден ($GAME); проверьте game_root в burnout_manifest.toml" >&2
    exit 1
fi

# One numbered pair of logs per run. Runs sharing a file is what made the last
# comparison impossible: the interesting question is what a run where the movies
# played does differently from one where they did not, and that needs each run
# kept whole and on its own.
N=1
while [ -e "logs/console$N.log" ] || [ -e "logs/burnout$N.log" ]; do
    N=$((N + 1))
done
LOG=logs/burnout$N.log
CONSOLE=logs/console$N.log

TRACE=
TINT=
NOMSAA=
MOVIE=
MODE=run
for arg in "$@"; do
    case "$arg" in
        --trace) TRACE=1 ;;
        --movie) MOVIE=1 ;;
        --gdb)   MODE=gdb ;;
        --capture) MODE=capture ;;
        --tint) TINT=1 ;;
        --no-msaa) NOMSAA=1 ;;
    esac
done

[ -n "$TRACE" ] && XERENGE_GPU_TRACE=1 && export XERENGE_GPU_TRACE
[ -n "$MOVIE" ] && XERENGE_MOVIE_TRACE=1 && export XERENGE_MOVIE_TRACE
[ -n "$TINT" ] && XERENGE_TINT_TRACE=1 && export XERENGE_TINT_TRACE

export SDL_VIDEODRIVER=x11
export LD_LIBRARY_PATH="$SDK_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# The guest's colour targets are 2x multisampled, and a multisampled target
# cannot be read back from a capture, which makes a frame much harder to take
# apart. Turning it off costs some quality and makes the frame legible.
if [ -n "$NOMSAA" ]; then
    set -- --no-native_2x_msaa
else
    set --
fi

set -- "$@" --game_data_root "$GAME" \
       --gpu_plugin xenos \
       --no-vulkan_async_skip_incomplete_frames \
       --log_level info \
       --log_file "$LOG" \
       --log_max_file_size_mb 32 \
       --log_max_files 3

# The movie trace writes to stderr, not to the SDK's log file, so keep a console
# log beside it; both are worth handing over together.
echo "run $N"
echo "logs: $(pwd)/$LOG"
echo "      $(pwd)/$CONSOLE"

if [ "$MODE" = capture ]; then
    CAPDIR=logs/capture$N
    mkdir -p "$CAPDIR"
    # RenderDoc launches the target with an environment of its own making, which
    # drops the SDK's library path set above. Hand it a stub that restores the
    # path and replaces itself with the game: the capture hook is preloaded, so
    # it survives the exec and still sees the real process.
    STUB="$CAPDIR/launch.sh"
    cat > "$STUB" <<EOF
#!/bin/sh
export LD_LIBRARY_PATH="$(cd "$SDK_LIB" && pwd)"
exec "$(pwd)/build/burnout" "\$@"
EOF
    chmod +x "$STUB"
    echo "captures: $(pwd)/$CAPDIR (press F12 on the screen with the wrong colours)"
    exec renderdoccmd capture --working-dir "$(pwd)" \
        --capture-file "$CAPDIR/frame" --wait-for-exit \
        "$(pwd)/$STUB" "$@" 2>&1 | tee "$CONSOLE"
elif [ "$MODE" = gdb ]; then
    exec gdb -q \
        -ex "set debuginfod enabled off" \
        -ex "set pagination off" \
        -ex "handle SIGSEGV nostop noprint pass" \
        -ex "handle SIGBUS nostop noprint pass" \
        -ex run --args ./build/burnout "$@"
else
    exec ./build/burnout "$@" 2>&1 | tee "$CONSOLE"
fi
