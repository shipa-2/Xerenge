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

The paths in `burnout_manifest.toml` point at a local game directory outside
the repository (game data is never committed); adjust them for your layout.

## Building

The SDK lives outside this repository, at `../rexglue-sdk` relative to the
project - a separate checkout, deliberately not vendored here. Use our fork,
[rexglue-xerenge](https://github.com/shipa-2/rexglue-xerenge), which carries the
changes described below; it is a GitHub fork of
[rexglue/rexglue-sdk](https://github.com/rexglue/rexglue-sdk), so upstream stays
a remote and the changes stay rebasable onto it.

```
git clone git@github.com:shipa-2/rexglue-xerenge.git ../rexglue-sdk
git -C ../rexglue-sdk submodule update --init --recursive --depth 1
cmake --preset linux-amd64 -S ../rexglue-sdk
cmake --build ../rexglue-sdk/out/build/linux-amd64 --config Release --target rexglue
```

Then the project. Two flags are not optional and are not in the SDK's own
project template:

* `-march=x86-64-v2`, because `src/core/memory.cpp` uses SSSE3 intrinsics while
  the generated project sets no baseline architecture. The SDK's own preset does
  set it; a project made by `rexglue init` does not.
* the imgui include directory, because `rex/rex_app.h` includes `<imgui.h>`
  publicly while `rex::runtime` does not carry that directory to consumers.
  `CMakeLists.txt` here names it.

```
cmake -S . -B build -DREXSDK_DIR=$(realpath ../rexglue-sdk) \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2
cmake --build build -j$(nproc)
```

Codegen runs as part of the build. `functions.toml` carries the five function
boundaries the analyser cannot find on its own: two it reports at validation
time, and three that are only ever reached through a vtable slot, which no
static analysis can see. Everything else - the whole retail image - it recovers
by itself in about four seconds, and it refuses to emit code while any branch
is unresolved rather than writing an error marker into the source.

## Running

```
SDL_VIDEODRIVER=x11 LD_LIBRARY_PATH=../rexglue-sdk/out/linux-amd64 \
  ./build/burnout --game_data_root <game directory> --gpu_plugin xenos \
  --no-vulkan_async_skip_incomplete_frames
```

Each of those is needed:

* `--gpu_plugin xenos` - without it every `Vd*` call is ignored and nothing draws.
* `--no-vulkan_async_skip_incomplete_frames` - by default a frame that used an
  async placeholder pipeline is not presented, and while shaders keep compiling
  that means no frame is ever presented at all.
* `SDL_VIDEODRIVER=x11` - SDL otherwise comes up on Wayland while the Vulkan
  instance enables the xcb surface extension, and no window appears.

Keyboard stands in for a pad: **A = Space**, **Start = Return**, **B = Quote**,
sticks on WASD.

## What our SDK fork changes

[rexglue-xerenge](https://github.com/shipa-2/rexglue-xerenge) branches from
upstream's v0.10.0 and carries three fixes and a set of diagnostics. The first
two fixes are not specific to this title and are worth sending upstream.

The fixes:

* `XHostThread::Execute` never seeded the host floating-point policy.
  `XThread::Execute` does it before entering guest code, but the host-thread
  override does not, and a host thread runs guest code too - the audio worker
  reaches the XMA registers through the function dispatcher. The guest's first
  rounding-mode update then writes that context's cached csr, still zero, into
  MXCSR, clearing every exception mask; the next inexact result anywhere on the
  thread raises SIGFPE, and FFmpeg's MDCT setup dividing 1 by 2*pi killed the
  process.
* All six red/blue fallback shaders failed to compile: they call `texelFetch`
  on a sampler-less texture without declaring
  `GL_EXT_samplerless_texture_functions`. On any device whose image views
  cannot swizzle - MoltenVK - that path silently did not exist.
* The Vulkan pipeline cache now dumps translated shaders under `dump_shaders`,
  as the D3D12 one already did. Without it only guest microcode can be dumped,
  which says nothing about how the translation wired the shader interface.

The diagnostics compare the uniform buffer against the register file, report the
shader interface masks, and trace the swap-time channel order.

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
and renders the 3D world at 60 fps. Audio comes up on its own. Videos and the 3D
world are colour-correct.

Loading a save used to kill the process on a call to an unregistered function.
That address, like the five before it, is reached only through a vtable slot and
is now declared in `functions.toml`; nothing about the content path itself was
wrong. The `Unregistered symbolic link: rmcsave:` line a trace shows is not a
fault either - the registration and the teardown land in the same millisecond,
which is a content mount being opened and closed.

One defect is open: the 2D layer's tint is wrong, and every symptom follows from
it - the red logo reads magenta, amber text reads pink, a dark panel reads vivid
blue, and unselected menu items are not dimmed. It is measured, not guessed:

* The whole GPU chain is faithful. The uniform buffer matches the register file
  component for component, the packing maps slot 2 to c2, both shaders' SPIR-V
  is correct, and the render target, blend, gamma ramp and surface formats all
  check out.
* So the wrong value arrives in the constant. The same guest code produces
  `(0.914, 0.714, 0.184, 1)` and `(0.25, 0.25, 0.25, 1)` on the legacy runtime,
  and `(1, 1, 3.984375, 3.984375)` and `(0, 0, 1.9921875, 3.984375)` here.
  `3.984375` is `255/64` and `1.9921875` is `255/128`, where the correct values
  divide by `256` - a scale short by one or two powers of two.
* Two candidates are ruled out by experiment: forcing the D3DCOLOR form of
  `vupkd3d128` changes nothing, and the int-to-float conversion scales are
  byte-for-byte identical between the two recompilers, with no scale literal in
  either output.

The next step is to watch the constant's slot in the command stream rather than
searching guest memory for the value, which turns up bit patterns that are
also ordinary guest pointers.
