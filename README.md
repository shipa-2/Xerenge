# Xerenge

Bringing Burnout Revenge (Xbox 360, retail) up as a native binary: the game's
PowerPC code is recompiled to C++ ahead of time and linked into an executable,
with the console's hardware and operating system emulated around it. There is
no CPU interpreter and no JIT.

## Getting it running

You need a dump of the retail European release - the image hashing to
`34c1bd4d549c2c53f29d814fa5e5d1c04c533c5ca0c39e57b6c2538f44ff59b4` - and a
machine with Vulkan.

**With the installer.** `Burnout_Revenge_Installer-x86_64.AppImage` asks for the
disc image, the install directory and three settings (bloom, motion blur, and
which renderer), then does everything else itself: fetches this repository and
its dependencies from GitHub, checks and extracts the image, builds the SDK and
the title, translates the shaders, and creates a desktop shortcut and an
applications menu entry. Nothing from the game is downloaded - it is all made
from your disc, on your machine. The settings land in `xerenge.conf` in the
install directory and are read at every start, so they can be changed there
later. To build the AppImage yourself: `rexglue/installer/make-appimage.sh`
(needs Qt 6, linuxdeploy and linuxdeploy-plugin-qt).

**By hand.**

```
./scripts/bootstrap.sh /path/to/burnout-revenge.iso
cd rexglue && ./run.sh --plume
```

That verifies the image, extracts it, builds the SDK and builds the title.
[`SETUP.md`](SETUP.md) covers the steps separately, what each one does, and what
to do when something goes wrong. The scripts are:

| | |
|---|---|
| `scripts/check-prerequisites.sh` | says what is missing before a build can fail halfway |
| `scripts/extract-image` | reads the disc image directly; XDVDFS, which ordinary tools do not open (built by `scripts/build-tools.sh`) |
| `scripts/setup-sdk.sh` | fetches and builds the recompiler and system layer |
| `scripts/setup-deps.sh` | fetches plume and XenosRecomp (our forks) and builds the shader translator |
| `scripts/build.sh` | points the manifest at your game directory, generates the code, translates the shaders and builds |
| `scripts/bootstrap.sh` | all of the above, in order |
| `scripts/inspect-capture.sh` | reads a RenderDoc capture offline: passes, the draw behind a given pixel, full state, textures |
| `rexglue/run.sh` | runs it, with the flags that are not optional and a log per run |
| `rexglue/installer/` | the Qt 6 installer and the script that packs it into an AppImage |

### Shaders

The title's shaders are its own code, so none are kept here: the build
translates them from your copy of the game. Most are found by scanning the
executable. The rest the title assembles while it runs - a shader from the
executable, or from a `graphics/*.obj` file on the disc, with a few
vertex-fetch instructions patched to match the vertex layout. Those are rebuilt
from the disc by the recipe in `rexglue/tools/runtime_shaders.recipe`, which
holds only hashes and patch masks, and each result is checked against its hash.
So every shader is there from the first frame, without running the game once to
collect them. `rexglue/tools/make_runtime_shader_recipe` (built by
`scripts/build-tools.sh`) refreshes the recipe from the shaders a run dumps
(`XERENGE_DUMP_UCODE=1`).

## Two lines of work

**`main` - ReXGlue.** Current. The recompiler and system layer come from
[ReXGlue](https://github.com/rexglue/rexglue-sdk), used through our fork
[rexglue-xerenge](https://github.com/shipa-2/rexglue-xerenge), which carries
fixes found while bringing this title up - including the recompiler defect that
made the entire frontend draw blue. The project itself - manifest, function
boundaries, app skeleton, build glue - is in [`rexglue/`](rexglue/), with the
detail in [`rexglue/README.md`](rexglue/README.md).

**`legacy` - XenonRecomp.** Frozen, and still buildable. A runtime written for
this project ([`XerengeRuntime/`](XerengeRuntime/)) on top of
[XenonRecomp-xerenge](https://github.com/shipa-2/XenonRecomp-xerenge) and
[XenosRecomp-xerenge](https://github.com/shipa-2/XenosRecomp-xerenge). It stays
useful as a second opinion: rendering the same frame correctly there is what
bounded the colour defect to the recompiler rather than the graphics pipeline.

The move was not about the recompilation approach, which is the same in both. It
was about the system layer. The previous line reached the point of needing a
texture cache for every Xenos format, an EDRAM render target cache, and a shader
cache kept in step with whatever the title binds - all of which ReXGlue already
has.

**Two renderers.** `main` draws through either of two GPU plugins:

* **plume** (`./run.sh --plume`, the installer's default) - this project's own,
  in [`rexglue/src/plume_renderer/`](rexglue/src/plume_renderer/). It takes the
  title's Direct3D calls rather than emulating the command processor, draws with
  the shaders translated ahead of time, and keeps the title's render targets as
  real host textures. It spreads the frame's CPU work (texture checks, vertex
  unpacking) over up to eight worker threads.
* **xenos** (`./run.sh`, the installer's "xenia render") - ReXGlue's port of
  Xenia's GPU emulation, with shader translation at runtime. Slower, and the
  reference when plume draws something differently.

## Where it stands

The title is fully playable: it boots, reaches the title screen, loads a save,
runs through car select, and races render at 60 fps. The audio is complete -
engine sounds, crashes, the interface and the licensed soundtrack - and the
intro videos play. The frontend, the videos and the 3D world are
colour-correct.

The game runs at its own pace, not the renderer's. Its logic steps on a fixed
timer, and each frame is held to the console's 60 Hz vblank - 30 fps in Crash
mode, as on the console - so a faster machine or a high refresh-rate monitor
does not speed it up. `XERENGE_UNLOCK_FPS=1` lifts the cap: frames are drawn as
fast as they can be, and the logic follows the wall clock instead.
Interpolating the frames drawn between logic steps is work in progress
(`XERENGE_INTERPOLATION=1`); without it, frames drawn between two steps show
the same moment.

Textures are loaded with their full mip chains from the title's memory, and
sampled with the filtering and anisotropy the title asks for.

Open:

* The Wayland surface extension is offered by the loader but never enabled, so
  runs go through X11.
* plume: a black frame still flashes very occasionally, and a texture
  sometimes shows wrong for half a second during a race.
* With bloom turned off (`--no-bloom`, or the installer's unchecked "bloom on")
  the sky looks grey and the frame dull. The title draws its sky pale and
  counts on the bloom pass to brighten it; the patch, after boma's for Xenia,
  empties the bloom kernel, so that brightness never arrives. Shrinking the
  kernel instead leaves quarter-resolution halos around edges, and raising the
  exposure brightens the dark areas along with the sky - neither is a fix yet.

## What is not here

Game images, decrypted data, extracted assets, build directories and generated
recompiler output are deliberately excluded, here and in the forks. The ReXGlue
SDK is a separate checkout rather than vendored.
