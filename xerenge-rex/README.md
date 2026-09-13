# Burnout Revenge on ReXGlue

This is where the title is brought up now. The earlier line of work, which used
XenonRecomp with a runtime written for this project (`XerengeRuntime`), is
frozen on the `legacy` branch; it still builds and runs, and it is useful as a
reference for behaviour that differs between the two.

ReXGlue recompiles the PowerPC code to C++ ahead of time, the same approach as
before - 3.2 million lines of generated C++ and 14496 native `sub_*` symbols in
the linked binary, with no CPU interpreter or JIT anywhere in the SDK. What it
brings that the previous line lacked is the system layer: a Xenia-derived GPU
with a texture cache covering every Xenos format, an EDRAM render target cache,
and shader translation to SPIR-V **at runtime**, so no precompiled shader cache
has to be built and kept in step with the title.

`burnout_manifest.toml` names a game directory outside the repository - game
data is never committed - and `../scripts/build.sh --game <dir>` points it at
yours.

## Building

`../scripts/bootstrap.sh <image>` does the whole thing; `../SETUP.md` covers the
steps separately. What is worth knowing about this project specifically:

The SDK lives outside this repository, at `../rexglue-sdk` - a separate
checkout, deliberately not vendored. Use our fork,
[rexglue-xerenge](https://github.com/shipa-2/rexglue-xerenge), which carries the
changes described below; it is a GitHub fork of
[rexglue/rexglue-sdk](https://github.com/rexglue/rexglue-sdk), so upstream stays
a remote and the changes stay rebasable onto it.

Two build flags are not optional and are not in the SDK's own project template:

* `-march=x86-64-v2`, because `src/core/memory.cpp` uses SSSE3 intrinsics while
  the generated project sets no baseline architecture. The SDK's own preset does
  set it; a project made by `rexglue init` does not.
* the imgui include directory, because `rex/rex_app.h` includes `<imgui.h>`
  publicly while `rex::runtime` does not carry that directory to consumers.
  `CMakeLists.txt` here names it.

Code generation runs as part of the build. `functions.toml` carries the six
function boundaries the analyser cannot find on its own: two it reports at
validation time, and four that are only ever reached through a vtable slot,
which no static analysis can see. Everything else - the whole retail image - it
recovers by itself in about four seconds, and it refuses to emit code while any
branch is unresolved rather than writing an error marker into the source.

## Running

```
./run.sh
```

It resolves the SDK beside the project and the game directory from
`burnout_manifest.toml`, so there is one place to change either, and it keeps a
numbered pair of logs per run under `logs/`. Three arguments are not optional
and it passes all three: `--gpu_plugin xenos`, without which nothing draws;
`--no-vulkan_async_skip_incomplete_frames`, without which no frame is presented
while shaders are still compiling; and `SDL_VIDEODRIVER=x11`, because under
Wayland the Vulkan instance still enables the xcb surface extension and no
window appears.

Keyboard stands in for a pad: **A = Space**, **Start = Return**, **B = Quote**,
sticks on WASD.

Useful flags beyond the defaults:

```
./run.sh --movie          trace the intro video path
./run.sh --trace          GPU diagnostics; very verbose
./run.sh --no-bloom       turn off sky bloom and the low-resolution gaussian blur
./run.sh --no-motion-blur turn off motion blur and radial blur
./run.sh --capture        run under RenderDoc; F12 captures the frame on screen
./run.sh --gdb            run under a debugger
```

`--no-bloom` and `--no-motion-blur` mirror boma's Xenia patches for this title.
They are implemented as hooks in `src/render_patches.cpp`, because the guest code
is recompiled ahead of time and the byte patches Xenia writes are never executed.

`--capture` runs under RenderDoc and is the tool of choice for anything visual:
it costs nothing at runtime and the capture can be replayed offline through
qrenderdoc's python API for constants, shader disassembly and pipeline state.
`--gdb` is a last resort - stopping the process mid-frame leaves Vulkan work in
flight, and the driver's reset timeout can take the display with it.

## What our SDK fork changes

[rexglue-xerenge](https://github.com/shipa-2/rexglue-xerenge) branches from
upstream's v0.10.0, carries four fixes of its own, and has upstream's
`development` branch merged in. None of the four is specific to this title and
all are worth sending upstream.

**The saturating vector packs, when the destination is also a source.**
`vpkuhus` and `vpkuwus` were expanded as a byte-at-a-time write straight into
the destination register. Titles routinely write `vpkuhus vD,vD,vD` to narrow a
vector in place, and then each write lands on data still to be read: byte 15 is
the upper half of halfword 7, so the second read of that halfword sees a value
built from a byte just written, and it saturates to `0xFF`. Both now read their
sources into locals first.

This made the whole frontend draw with the wrong colours; the title converts a
Flash colour transform through that instruction, and the bad pack turned the
identity multipliers into garbage before they reached the vertex shader.

**`XHostThread::Execute` never seeded the host floating-point policy.**
`XThread::Execute` does it before entering guest code, but the host-thread
override does not, and a host thread runs guest code too - the audio worker
reaches the XMA registers through the function dispatcher. The guest's first
rounding-mode update then writes that context's cached csr, still zero, into
MXCSR, clearing every exception mask; the next inexact result anywhere on the
thread raises SIGFPE, and FFmpeg's MDCT setup dividing 1 by 2*pi killed the
process.

**All six red/blue fallback shaders failed to compile:** they call `texelFetch`
on a sampler-less texture without declaring
`GL_EXT_samplerless_texture_functions`. On any device whose image views cannot
swizzle - MoltenVK - that path silently did not exist.

**The Vulkan pipeline cache now dumps translated shaders** under `dump_shaders`,
as the D3D12 one already did. Without it only guest microcode can be dumped,
which says nothing about how the translation wired the shader interface.

From upstream's `development`, merged: overwriting a file now truncates in place
instead of deleting and recreating, which was corrupting save data; content
results are compared against `X_ERROR_SUCCESS` rather than tested as an NTSTATUS,
so failures stopped reading as successes; some leaked event, socket and
completion port objects are released; and SDL3 and MoltenVK move up.

## Moving to plume

The Xenia-derived GPU plugin is not the path the mature project on this SDK
takes. re:Blue calls `rexglue_setup_target` with no `GPU_PLUGINS` at all: it
hooks the guest's D3D entry points with `REX_HOOK` and drives
[plume](https://github.com/zolaware/plume) directly, so it never parses a PM4
packet, never emulates EDRAM, and never touches the guest's gamma LUT. Both
defects still open here live in precisely those places.

plume is another separate checkout, at `../plume`, and needs three of its own
submodules:

```
git clone --depth 1 https://github.com/zolaware/plume.git ../plume
git -C ../plume submodule update --init --depth 1 \
    contrib/volk contrib/Vulkan-Headers contrib/VulkanMemoryAllocator
```

`src/plume_selftest.cpp` was the first step: it brings up the render interface,
lists the devices it sees and creates one, which is all that needed proving
before any of the translation layer is written. It reported the adapter and a
created device here.

It is kept out of the executable, and the reason matters before the renderer is
written: plume pulls in volk, which declares the Vulkan entry points as its own
function-pointer globals. Linking it put an uninitialised `vkCreateInstance`
into the binary, shadowing the loader's, and the title then stopped getting past
language select and lost its video. Whoever writes the renderer has to settle
that first - either the whole process resolves Vulkan through volk, or plume
stays behind its own boundary.

The layer itself is the work, and its size is worth being plain about: the
equivalent in re:Blue is 105 files and about 34000 lines - device, pipelines,
constant buffers, texture upload, vertex declarations, resolve, present,
samplers. None of it mentions their title, so it is generic Xenos-to-plume
translation rather than game-specific code, which makes it a shape to follow
rather than invent.

## Where it stands

The title boots, runs its logos, reaches the title screen, loads a save from the
memory card and reaches the car select menu with that profile's rank and cars,
and renders the 3D world at 60 fps. Audio comes up on its own. The frontend, the
videos and the 3D world are colour-correct.

Loading a save used to kill the process on a call to an unregistered function.
That address, like the others before it, is reached only through a vtable slot
and is now declared in `functions.toml`; nothing about the content path itself
was wrong. The `Unregistered symbolic link: rmcsave:` line a trace shows is not
a fault either - the registration and the teardown land in the same millisecond,
which is a content mount being opened and closed.

Three things are open.

**The intro videos intermittently fail to start.** The main thread sits in
`CCalMoviePlayer::RenderNextFrame` waiting in `KeWaitForSingleObject`; the audio
renderer and both end-of-frame callback threads wait too. A failing run is
distinguished by two extra stuck waits on auto-reset events that a succeeding
run does not have. The SDK's event machinery has been checked and signals
correctly, so the question is which guest thread should be signalling those two
and why it does not. `./run.sh --movie` reports which step is not reached.

**Gameplay races draw nothing over a black background.** Untouched since the
frontend took priority.

**The Wayland surface extension** is requested and offered by the loader but
never enabled, so runs go through X11.
