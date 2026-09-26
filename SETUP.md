# Getting it running

This walks from a disc image to a playable build. Everything here has been run
on the machine this was developed on; nothing is assumed.

## What you need

**The game.** A dump of the retail European release, whose image is
7,834,892,288 bytes and hashes to:

```
34c1bd4d549c2c53f29d814fa5e5d1c04c533c5ca0c39e57b6c2538f44ff59b4
```

Another dump is not necessarily unusable, but `rexglue/functions.toml` names
function addresses found in *this* image, and they will not line up with a
different build. No game data is in this repository and none will be.

**A machine with Vulkan.** Developed against Mesa's `radv` on an AMD card under
X11. The window code needs the Wayland development files present to build even
when running under X11, which the check below will tell you about.

```
./scripts/check-prerequisites.sh
```

Setup and shader utilities are C++ binaries built once with `./scripts/build-tools.sh`
(installation and `bootstrap.sh` run this for you). Offline RenderDoc analysis uses
`inspect-capture.sh`, which needs the `renderdoc` package (`qrenderdoc` on PATH).

On Arch that is:

```
sudo pacman -S --needed git cmake ninja clang openssl pkgconf vulkan-icd-loader vulkan-headers libx11 libxcb wayland renderdoc
```

The installer builds from source too, so it needs the same packages; it runs the
check first and says what is missing.

## The installer

`Burnout_Revenge_Installer-x86_64.AppImage` is the shortest way. It asks for:

* the disc image;
* **bloom on** and **blur on** - both off by default; see the note on bloom
  under *When something is wrong*;
* **xenia render** - draw with the xenos backend instead of plume, off by
  default;
* the install directory (`~/Games/Burnout Revenge` unless you pick another).

Then it clones this repository into `<install>/source`, and runs the same
scripts as below: the prerequisite check, the image check and extraction, the
SDK, plume and XenosRecomp, and the build with its shader translation. It
copies the game and the libraries it loads into `<install>/bin`, writes
`xerenge.conf` and the launcher `burnout-revenge`, and adds a desktop shortcut
and an applications menu entry. Running it again over the same directory pulls
the latest sources and rebuilds.

`xerenge.conf` is read at every start, so the choices can be changed later
without reinstalling:

```
bloom = false
motion_blur = false
renderer = plume      # or xenos
```

The installed game logs to `~/.local/state/xerenge-burnout/`.

It also runs without its window, for scripting:

```
Burnout_Revenge_Installer-x86_64.AppImage --iso <image.iso> --path <directory> [--bloom] [--blur] [--xenia] [--no-shortcuts]
```

To build the AppImage from a checkout: `rexglue/installer/make-appimage.sh`,
which needs Qt 6, `linuxdeploy` and `linuxdeploy-plugin-qt` (on Arch, from the
AUR).

## One command

```
./scripts/bootstrap.sh /path/to/burnout-revenge.iso
```

It checks the environment, verifies the image against the hash above, extracts
it into `game/`, fetches and builds the SDK, plume and XenosRecomp, then builds
the title. Expect the image check and extraction to take a few minutes and the
build rather longer: the recompiler turns the whole executable into about 3.2
million lines of C++, and all of it has to be compiled.

Then:

```
cd rexglue && ./run.sh --plume
```

or `./run.sh` alone for the xenos backend.

## Or step by step

```
./scripts/build-tools.sh   # once: builds extract-image and other setup tools
./scripts/extract-image --sha256 34c1bd4d549c2c53f29d814fa5e5d1c04c533c5ca0c39e57b6c2538f44ff59b4 <image.iso> game
./scripts/setup-sdk.sh
./scripts/setup-deps.sh
./scripts/build.sh --game game
```

**`extract-image`** reads the disc directly. The image is XDVDFS rather than
ISO 9660, so ordinary tools do not open it - 7-Zip finds a stray UDF header and
stops - and this avoids depending on `extract-xiso` being installed. It finds
the game partition (0xFD90000 into this image, after the video partition), walks
the directory trees and writes the files out: 832 of them, 4.4 GiB.

**`setup-sdk.sh`** clones our fork of the ReXGlue SDK next to the project and
builds it. The fork is not a convenience: it carries fixes this title needs,
including the vector pack defect described in `rexglue/README.md`. Upstream is
kept as a second remote so the changes stay rebasable.

**`setup-deps.sh`** clones our forks of plume (the Vulkan layer the plume
renderer draws through) and XenosRecomp (the shader translator) beside the
project, and builds the translator.

**`build.sh`** points the manifest at your copy of the game, generates the
code, translates the game's shaders and builds. The recompiler recovers the
whole retail image by itself in a few seconds and refuses to emit anything while
a branch is unresolved, so a successful build means every reachable function was
recovered.

The shaders are the game's own code, so they are made here from your copy and
never kept in the repository. They come in two steps:

* `tools/rebuild_plume_shaders.sh` scans the executable for shader containers
  and translates them (about 120) into `generated/shader_cache.cpp`.
* `tools/translate_runtime_shaders.sh --from-disc` covers the ones the title
  assembles while it runs (64): each is a shader from the executable or from a
  `graphics/*.obj` file with a few vertex-fetch instructions patched to match
  the vertex layout. `tools/runtime_shaders.recipe` says which shader and which
  bits - hashes and masks only - and every rebuilt shader is checked against
  its hash, so a different disc is reported rather than turned into wrong
  shaders. The result is `generated/shader_cache_runtime.cpp`.

Both are skipped when their output already exists; delete it to translate
again. With both linked in, the renderer has every shader from the first frame.
If a newer build of the title ever needs a shader the recipe does not cover,
`XERENGE_DUMP_UCODE=1` dumps what it loads into `generated/ucode-dump/`, and
`tools/make_runtime_shader_recipe` writes a new recipe from those dumps.

## Running

`rexglue/run.sh` sets what has to be set and keeps a numbered pair of logs per
run under `rexglue/logs/`, so runs can be compared rather than overwriting each
other. There are two renderers:

* **plume** (`./run.sh --plume`) - this project's own. It takes the title's
  Direct3D calls, draws with the shaders translated at build time, and keeps
  the title's render targets as real host textures. `--plume` also sets the
  switches it runs with (`XERENGE_D3D_TARGETS`, `XERENGE_D3D_UI`,
  `XERENGE_REAL_SHADERS`, `XERENGE_D3D_DRAWS`, `XERENGE_SECONDARY_TICKS`,
  `XERENGE_SKIP_LOGOS`), the same ones the installed launcher sets.
* **xenos** (`./run.sh`) - ReXGlue's port of Xenia's GPU emulation, which
  translates shaders at runtime. Slower; the reference when plume draws
  something differently.

Three things are not optional, and it passes all three:

* `--gpu_plugin` naming one of the two, without which every video call is
  ignored and nothing draws at all.
* `--no-vulkan_async_skip_incomplete_frames`. By default a frame that used a
  placeholder pipeline is not presented, and while shaders are still compiling
  that means no frame is ever presented.
* `SDL_VIDEODRIVER=x11`. Under Wayland the Vulkan instance still enables the
  xcb surface extension and no window appears.

The keyboard stands in for a pad: **A** is Space, **B** is the quote key,
**Start** is Return, the sticks are on WASD.

The game keeps its own pace: its logic steps on a fixed timer and each frame is
held to a 60 Hz vblank (30 in Crash mode, as on the console), whatever the
monitor's refresh rate. A few switches change that:

```
XERENGE_UNLOCK_FPS=1     draw as fast as possible; the logic follows the wall clock
XERENGE_VBLANK_HZ=<n>    pace to another vblank rate
XERENGE_NO_VBLANK_PACE=1 no vblank pacing, but the monitor's vsync still applies;
                         the logic follows the wall clock
XERENGE_INTERPOLATION=1  work in progress: interpolate frames between logic steps
```

If the frame rate stops at 75 or some other odd figure with `XERENGE_UNLOCK_FPS`,
look for an overlay that limits it - MangoHud does, and `MANGOHUD=0` turns it
off.

Useful modes:

```
./run.sh --plume          draw with plume rather than xenos
./run.sh --movie          trace the intro video path
./run.sh --trace          the SDK fork's GPU diagnostics; very verbose
./run.sh --no-bloom       turn off sky bloom and the low-resolution gaussian blur
./run.sh --no-motion-blur turn off motion blur and radial blur
./run.sh --capture        run under RenderDoc; F12 captures the frame on screen
./run.sh --gdb            run under a debugger
```

`--no-bloom` and `--no-motion-blur` mirror boma's Xenia patches for this title.
Here they are implemented as hooks in `src/render_patches.cpp`, because the guest
code is recompiled ahead of time and the byte patches Xenia writes are never
executed.

A word on `--gdb`: stopping the process mid-frame leaves Vulkan work in flight,
and the driver's reset timeout can take the display down with it. It is there
when nothing else will do, not for routine work. Prefer `--capture`, which
costs nothing and can be replayed offline:

```
./scripts/inspect-capture.sh <capture.rdc> list             # the passes in the frame
./scripts/inspect-capture.sh <capture.rdc> pixel 0.5 0.6    # what draws that point
./scripts/inspect-capture.sh <capture.rdc> draw 96 --shaders
./scripts/inspect-capture.sh <capture.rdc> textures out/
```

`pixel` takes a point as a fraction of the frame and reports every draw that
changes its colour, which is the quickest way from something visibly wrong to
the draw responsible. `draw` then prints that draw's targets, blending, bound
textures and every constant buffer.

## When something is wrong

**No window, but sound.** The Vulkan instance came up without a surface. Check
that the run went through `run.sh`, which forces X11.

**A window, but nothing drawn.** Almost always a missing `--gpu_plugin`; run through `run.sh`, which passes it.

**The build fails in the SDK's memory code.** The SSSE3 baseline is missing;
`build.sh` passes `-march=x86-64-v2`, which a project generated by `rexglue
init` does not set.

**`imgui.h` not found.** The SDK's app header includes it publicly without
carrying the include directory to consumers. `rexglue/CMakeLists.txt` names it;
a hand-written configure line has to as well.

**The sky is grey and the picture dull.** Bloom is off. The title draws its
sky pale and counts on the bloom pass to brighten it, and turning bloom off (the
installer's default, or `--no-bloom`) removes that brightness along with the
glow. A known issue, not yet fixed; set `bloom = true` in `xerenge.conf`, or
leave out `--no-bloom`, for the intended look.

**Geometry missing, or `cache MISS` in the log with plume.** A shader the build
did not translate. Delete `rexglue/generated/shader_cache_runtime.cpp` and run
`build.sh` again; if that does not help, see the note on
`make_runtime_shader_recipe` above.
