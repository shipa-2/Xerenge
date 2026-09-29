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
#   ./run.sh --reflect
#                       reports how the reflection map the cars are lit by is
#                       resolved to memory and read back again.
#   ./run.sh --flip-swap
#                       an experiment: inverts the red/blue swap flag on the
#                       resolves that take the full path, to find out whether
#                       that path honours it at all.
#   ./run.sh --probe <register>.<component>
#                       puts one of each pixel shader's own registers on the
#                       screen in place of the colour it computes, e.g.
#                       --probe 1.y. Nothing else shows what a long shader is
#                       doing in the middle. Add --probe-textures <n> to leave
#                       shaders binding fewer than n textures alone, which keeps
#                       the menus usable while probing car bodies.
#   ./run.sh --no-bloom
#                       turns off the sky bloom and the low-resolution gaussian
#                       blur, after boma's Xenia patch for this title.
#   ./run.sh --no-motion-blur
#                       turns off motion blur and radial blur, likewise.
#   ./run.sh --no-msaa
#                       turns off the guest's 2x multisampling. A multisampled
#                       target cannot be read back from a capture, so this is
#                       what to use when a frame has to be taken apart.
#   ./run.sh --capture
#                       runs under RenderDoc. Get to the screen with the wrong
#                       colours and press F12 to capture that frame; the capture
#                       lands in logs/capture<N>/ and can be handed over as is.
#   ./run.sh --online   pretends to be signed in to Xbox Live and stands in for
#                       EA's long-gone lobby servers, to reach the online menus.
#                       A stub: nothing real is contacted. Off by default.
#   ./run.sh --real-lobby
#                       --online, but the title talks to a lobby server
#                       (tools/ealobby) instead of being answered from inside.
#                       With no --lobby-server every copy runs a lobby of its
#                       own and logs in to it; the lobbies find each other on
#                       the LAN (UDP 31859), list each other's games and carry
#                       a joined player's traffic to the host's, so any player
#                       can host and no copy has to be started first.
#   ./run.sh --lobby-server=HOST
#                       --real-lobby against a server of your own (./lobby.sh):
#                       the only lobby then, nothing local is started.
#   ./run.sh --online-address=IP
#                       the address this copy gives its peers and binds to:
#                       127.0.0.2 and 127.0.0.3 run two copies on one machine.
#   ./run.sh --windowed runs in a window; --fullscreen (the default) does not.
#   ./run.sh --fps      frames per second in the top left corner (plume).
#   ./run.sh --render-resolution=WIDTHxHEIGHT
#                       plume draws the frame at this size and scales it onto
#                       the window, e.g. 1280x720, the size the title renders
#                       at itself. A hack for slow GPUs.
#   ./run.sh --async-present
#                       plume hands a frame to the GPU and lets the title go on
#                       at once, instead of waiting for the GPU to finish it.
#                       A hack: it once showed black frames now and then.
#   ./run.sh --no-early-submit
#                       plume waits for a swap chain image before recording the
#                       frame, as it used to, instead of handing the frame to
#                       the GPU first (the default, a hack that overlaps the
#                       two).
#   ./run.sh --full-vertices
#                       plume sends every vertex in the layout wide enough for
#                       any shader, as it used to, instead of only what the
#                       draw's shader reads (the default, a hack).
#   ./run.sh --cull    plume culls the faces the title culls (experimental).
#   ./run.sh --plume    loads librexgpu-plume.so instead of xenos, with the
#                       switches it is meant to run with (as the installed
#                       launcher sets them). Saved burnout.toml has
#                       gpu_plugin=xenos, so --gpu_backend alone is ignored;
#                       this flag sets gpu_plugin too.
#   ./run.sh --verbose
#                       every category (core, cpu, apu, gpu, krnl, sys, fs) at
#                       spdlog trace level plus the noisy (per-frame) log
#                       macros, and turns on --trace and --movie too. The log
#                       file gets large fast; use for a single short repro, not
#                       a long session.
#   ./run.sh --log-level=LEVEL
#                       just the level (trace, debug, info, warn, error,
#                       critical, off) without --verbose's other switches.
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

# Windows (under Git for Windows' bash) differs in the SDK's preset, the
# executable's name and how the runtime libraries are found.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) PRESET=win-amd64; EXE=.exe; WINDOWS=1 ;;
    *) PRESET=linux-amd64; EXE= ;;
esac

# Resolve the SDK and the game without hard-coding one machine's layout. The
# SDK sits beside the project in the repository; a working copy kept elsewhere
# can name it with XERENGE_SDK. The game directory is whatever the manifest was
# pointed at when the project was built, so there is one place to change it.
if [ -n "$XERENGE_SDK" ]; then
    SDK_LIB="$XERENGE_SDK/out/$PRESET"
else
    for candidate in ../rexglue-sdk ../Xerenge/rexglue-sdk; do
        [ -d "$candidate/out/$PRESET" ] && SDK_LIB="$candidate/out/$PRESET" && break
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
REFLECT=
FLIPSWAP=
PROBE=
PROBETEX=
NOBLOOM=
NOMOTIONBLUR=
NOMSAA=
MOVIE=
LOGLEVEL=info
NOISY=
MODE=run
for arg in "$@"; do
    case "$arg" in
        --trace) TRACE=1 ;;
        --movie) MOVIE=1 ;;
        --gdb)   MODE=gdb ;;
        --capture) MODE=capture ;;
        --tint) TINT=1 ;;
        --reflect) REFLECT=1 ;;
        --flip-swap) FLIPSWAP=1 ;;
        --probe) PROBE=$2 ;;
        --probe-textures) PROBETEX=$2 ;;
        --no-bloom) NOBLOOM=1 ;;
        --no-motion-blur) NOMOTIONBLUR=1 ;;
        --no-msaa) NOMSAA=1 ;;
        --verbose) LOGLEVEL=trace; NOISY=1; TRACE=1; MOVIE=1 ;;
        --log-level=*) LOGLEVEL=${arg#*=} ;;
        --plume)   GPU_BACKEND=plume ;;
        --online)  ONLINE=1 ;;
        --real-lobby) ONLINE=1; REAL_LOBBY=1 ;;
        --lobby-server=*) ONLINE=1; REAL_LOBBY=1; LOBBY_SERVER=${arg#*=} ;;
        --online-address=*) ONLINE_ADDRESS=${arg#*=} ;;
        --render-resolution=*) RENDER_RESOLUTION=${arg#*=} ;;
        --async-present) ASYNC_PRESENT=1 ;;
        --no-early-submit) ACQUIRE_FIRST=1 ;;
        --full-vertices) FULL_VERTICES=1 ;;
        --cull) CULL=1 ;;
        --fps) FPS_SHOW=1 ;;
        --windowed) WINDOWED=1 ;;
        --fullscreen) WINDOWED= ;;
        # Any other option goes to the game as it is: its own settings, e.g.
        # --no-fullscreen --window_width=1280 --window_height=720.
        --*) GAME_ARGS="$GAME_ARGS $arg" ;;
    esac
done

GPU_BACKEND=${GPU_BACKEND:-xenos}

# plume draws from the title's Direct3D calls, into real render targets, with
# the translated shaders - the same switches the installed launcher sets. Any
# already set in the environment are left as they are.
if [ "$GPU_BACKEND" = plume ]; then
    for switch in XERENGE_D3D_TARGETS XERENGE_D3D_UI XERENGE_SKIP_LOGOS \
                  XERENGE_REAL_SHADERS XERENGE_D3D_DRAWS XERENGE_SECONDARY_TICKS; do
        eval "[ -n \"\${$switch+x}\" ] || export $switch=1"
    done
fi

[ -n "$TRACE" ] && XERENGE_GPU_TRACE=1 && export XERENGE_GPU_TRACE
[ -n "$MOVIE" ] && XERENGE_MOVIE_TRACE=1 && export XERENGE_MOVIE_TRACE
[ -n "$TINT" ] && XERENGE_TINT_TRACE=1 && export XERENGE_TINT_TRACE
# Reports how the small textures the cars are lit by are written and read back:
# the channel swap, byte order, format and exponent bias on each side.
[ -n "$REFLECT" ] && XERENGE_REFLECT_TRACE=1 && export XERENGE_REFLECT_TRACE
[ -n "$FLIPSWAP" ] && XERENGE_RESOLVE_SWAP=flip && export XERENGE_RESOLVE_SWAP
[ -n "$PROBE" ] && XERENGE_SHADER_PROBE=$PROBE && export XERENGE_SHADER_PROBE
[ -n "$PROBETEX" ] && XERENGE_SHADER_PROBE_TEXTURES=$PROBETEX && export XERENGE_SHADER_PROBE_TEXTURES
[ -n "$NOBLOOM" ] && XERENGE_NO_BLOOM=1 && export XERENGE_NO_BLOOM
[ -n "$NOMOTIONBLUR" ] && XERENGE_NO_MOTION_BLUR=1 && export XERENGE_NO_MOTION_BLUR

if [ -n "$WINDOWS" ]; then
    # The libraries built with the game first, then the SDK's.
    export PATH="$(pwd)/build:$SDK_LIB:$PATH"
else
    export SDL_VIDEODRIVER=x11
    export LD_LIBRARY_PATH="$SDK_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

# The guest's colour targets are 2x multisampled, and a multisampled target
# cannot be read back from a capture, which makes a frame much harder to take
# apart. Turning it off costs some quality and makes the frame legible.
if [ -n "$NOMSAA" ]; then
    set -- --no-native_2x_msaa
else
    set --
fi

# --online: answer as a console signed in to Xbox Live, and stand in for EA's
# lobby servers during the login, to reach the online menus. A stub - nothing
# real is contacted, and there is no one to play against.
[ -n "$ONLINE" ] && set -- "$@" --online
# --real-lobby: --online without the stand-ins inside the title, for talking to
# a real lobby server (tools/ealobby).
[ -n "$REAL_LOBBY" ] && set -- "$@" --online_fake_lobby=false
[ -n "$LOBBY_SERVER" ] && set -- "$@" --lobby_server="$LOBBY_SERVER"
[ -n "$ONLINE_ADDRESS" ] && set -- "$@" --online_address="$ONLINE_ADDRESS"

[ -n "$NOISY" ] && set -- "$@" --log_noisy=true
# Fullscreen unless --windowed: said outright both ways, so a burnout.toml
# saved with the other does not decide it.
if [ -n "$WINDOWED" ]; then
    set -- "$@" --no-fullscreen
else
    set -- "$@" --fullscreen
fi
[ -n "$RENDER_RESOLUTION" ] && XERENGE_RENDER_RESOLUTION=$RENDER_RESOLUTION && export XERENGE_RENDER_RESOLUTION
[ -n "$ASYNC_PRESENT" ] && XERENGE_ASYNC_PRESENT=1 && export XERENGE_ASYNC_PRESENT
[ -n "$ACQUIRE_FIRST" ] && XERENGE_ACQUIRE_FIRST=1 && export XERENGE_ACQUIRE_FIRST
[ -n "$FULL_VERTICES" ] && XERENGE_FULL_VERTICES=1 && export XERENGE_FULL_VERTICES
[ -n "$CULL" ] && XERENGE_CULL=1 && export XERENGE_CULL
[ -n "$FPS_SHOW" ] && XERENGE_FPS_SHOW=1 && export XERENGE_FPS_SHOW
# shellcheck disable=SC2086 # a list of options, split on purpose
[ -n "$GAME_ARGS" ] && set -- "$@" $GAME_ARGS

set -- "$@" --game_data_root "$GAME" \
       --gpu_plugin "$GPU_BACKEND" \
       --gpu_backend "$GPU_BACKEND" \
       --no-vulkan_async_skip_incomplete_frames \
       --log_level "$LOGLEVEL" \
       --log_file "$LOG" \
       --log_max_file_size_mb 32 \
       --log_max_files 3

# The movie trace writes to stderr, not to the SDK's log file, so keep a console
# log beside it; both are worth handing over together.
echo "run $N"
echo "gpu:  $GPU_BACKEND"
echo "logs: $(pwd)/$LOG"
echo "      $(pwd)/$CONSOLE"

if [ "$MODE" = capture ]; then
    CAPDIR=logs/capture$N
    mkdir -p "$CAPDIR"
    # plume sets RenderDoc's capture path itself, over --capture-file below.
    export XERENGE_CAPTURE_DIR="$(pwd)/$CAPDIR"
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
    exec ./build/burnout$EXE "$@" 2>&1 | tee "$CONSOLE"
fi
