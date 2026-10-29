# Xerenge

Bringing Burnout Revenge (Xbox 360, retail) up as a native binary: the game's
PowerPC code is recompiled to C++ ahead of time and linked into an executable,
with the console's hardware and operating system emulated around it. There is
no CPU interpreter and no JIT.

**Supported releases:** Burnout Revenge (Europe) and, since 0.4.7, Burnout
Revenge (USA) - both officially, tested end to end: menus, races, Crash mode,
replays, widescreen and the logo skip. Other dumps (the beta, other regions)
are not.

## Getting it running

You need a dump of the retail European or American release - the image
hashing to `34c1bd4d549c2c53f29d814fa5e5d1c04c533c5ca0c39e57b6c2538f44ff59b4`
(Europe) or `7abf689f52b70666ba92052bd157cfcae9ed6290dae298fbf639baa1d169f92b`
(USA) - and a machine with Vulkan. The two are not the same build (the
territory is compiled into the code), so each gets a game generated from its
own `default.xex`; SETUP.md has the detail.

**With the installer.** `Burnout_Revenge_Installer-x86_64.AppImage` asks for the
disc image, the install directory and three settings (bloom, motion blur, and
which renderer), then does everything else itself: fetches this repository and
its dependencies from GitHub, checks and extracts the image, builds the SDK and
the title, translates the shaders, and creates a desktop shortcut and an
applications menu entry. Nothing from the game is downloaded - it is all made
from your disc, on your machine. The settings land in `xerenge.conf` in the
install directory and are read at every start, so they can be changed there
later - or in the game, under Driver Details > Settings (GRAPHICS, HACKS, DEBUG). To build the AppImage yourself: `rexglue/installer/make-appimage.sh`
(needs Qt 6, linuxdeploy and linuxdeploy-plugin-qt). The payload carries the
game twice, `bin/burnout_eu` and `bin/burnout_us`, and the installer installs
the one for your disc as `bin/burnout`. CI builds the American one when its
assets carry the American release's files (`game_us/` beside `game_eu/`); an
installer made without them says so and installs the European disc only.

**By hand.**

```
./scripts/bootstrap.sh /path/to/burnout-revenge.iso
cd rexglue && ./run.sh
```

The same works on a Mac with Apple silicon, and there is an installer app for
it too; SETUP.md has what it needs. Intel Macs are not supported.

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

## Plume

Burnout Revenge draws through **plume**, this project's own GPU plugin
([`rexglue/src/plume_renderer/`](rexglue/src/plume_renderer/), the default
everywhere `./run.sh` runs it), on top of
[ReXGlue](https://github.com/rexglue/rexglue-sdk) through our fork
[rexglue-xerenge](https://github.com/shipa-2/rexglue-xerenge). The project
itself - manifest, function boundaries, app skeleton, build glue - is in
[`rexglue/`](rexglue/), with the detail in
[`rexglue/README.md`](rexglue/README.md).

Instead of emulating the Xenos command processor, plume takes the title's
Direct3D calls directly and draws with them. That one difference is where
most of what makes this build solid and fast comes from:

* **No GPU state caches to keep correct.** A command-processor emulator needs
  a texture cache for every Xenos format, an EDRAM render-target cache, and a
  shader cache kept in step with whatever the title binds. Drawing from the
  D3D calls directly needs none of that - the host GPU driver already does it.
* **Shaders are translated ahead of time**, not recompiled at runtime, so
  there is no JIT warm-up and no stutter the first time the title hits a new
  one.
* **Render targets are real host textures**, read back directly rather than
  reconstructed from an emulated EDRAM layout.
* **The frame's CPU work is spread out** - texture checks, vertex unpacking -
  over up to eight worker threads, instead of one emulation thread carrying
  all of it.
* **It runs everywhere Vulkan does**, from a desktop GPU down to a phone's,
  without a software GPU emulator in the way.

A second GPU plugin, **xenos** (`./run.sh --xenos`, the installer's "xenia
render") - ReXGlue's port of Xenia's GPU emulation, with shader translation at
runtime - is kept as the reference for when plume draws something
differently, and as how the project's earlier line, `legacy`
([`XerengeRuntime/`](XerengeRuntime/) on
[XenonRecomp-xerenge](https://github.com/shipa-2/XenonRecomp-xerenge) and
[XenosRecomp-xerenge](https://github.com/shipa-2/XenosRecomp-xerenge), frozen
but still buildable), proved that an early colour defect was in the recompiler
and not the graphics pipeline, before the move to ReXGlue.

## Where it stands

The title is fully playable on both supported releases: it boots, reaches the
title screen, loads a save, runs through car select, and races render at
60 fps. The audio is complete - engine sounds, crashes, the interface and the
licensed soundtrack - and the intro videos play. The frontend, the videos and
the 3D world are colour-correct. It runs on Linux, Windows, Android (phones
with a Vulkan 1.0+ GPU) and macOS (Apple silicon).

All of it besides the disc image and the install path is a setting, not a
patch: the installer and `xerenge.conf` cover them, and most are reachable
from the running game too, under Driver Details > Settings (GRAPHICS, HACKS,
DEBUG) - changing one there and confirming it offers a restart that applies
it at once, without reinstalling.

The game runs at its own pace, not the renderer's. Its logic steps on a fixed
timer, and each frame is held to the console's 60 Hz vblank by default - 30
fps in Crash mode, as on the console. A 30 fps lock (two logic steps a frame,
everywhere) and frame generation (a frame drawn between each two of the
game's, for 90-120 Hz screens, including through Crash mode's lock) are both
shipped settings, off by default; `XERENGE_UNLOCK_FPS=1` instead lifts the cap
entirely and lets the logic follow the wall clock.

Textures are loaded with their full mip chains from the title's memory, and
sampled with the filtering and anisotropy the title asks for; on a GPU with no
hardware texture compression (common on phones), they are re-encoded to ETC2
once and cached, rather than decompressed every time.

The frame can be anti-aliased and sharpened as it is presented
(`plume_postfx.cpp`): FXAA, after NVIDIA's FXAA 3.11, or SMAA 1x, and AMD's
FidelityFX CAS, full-screen passes from the scene target through intermediate
targets, then the usual copy onto the window. SMAA is the reference
implementation of Jimenez, Echevarria, Masia, Navarro and Gutierrez (MIT
licence, `rexglue/thirdparty/smaa`) at its HIGH preset, with edges found from
luma: three passes - edges, blending weights from its two lookup tables, the
blend - where FXAA is one. All are off by default - the console had none of
them, and with them off nothing is recorded. Colour grade, brightness and
shadow lift are adjustable too, live, over the console's own look. The
settings are live (read once a frame); `XERENGE_AA` and `XERENGE_SHARPEN` set
where they start.

The frame is drawn at the window's resolution rather than the console's 720p,
and the copies of it the post-processing reads are kept at that size. A pass
that shrinks a copy further - the bloom's first step, 1280x720 into 320x180 -
samples it with offsets made for 720p, and at a higher resolution it would
catch a different few pixels of each block every frame: small highlights
flickered through the glare and the whole picture shimmered. Such a copy is
filtered down to its guest size first (halved while a half covers it, then
the last step), at any window size and shape.

**Online.** `ealobby`, a lobby server standing in for EA's, is already
pointed at by default (a self-hosted one), so online play works without
running anything yourself; a self-hosted server also carries the Locker
(Burnout Clips: saving, sharing and recommending replays, the top 20 and
recent-players rankings) and serves the game's News screen the latest
release's notes. Running your own `ealobby` instead is one flag away for a
LAN or a server of your own.

**Achievements** are read from the title's own data and shown as it earns
them - a notice drawn over the game without pausing it, and the full list
with their descriptions under Player File > Stats.

Open:

* plume: a black frame still flashes very occasionally, and a texture
  sometimes shows wrong for half a second during a race.

## What is not here

Game images, decrypted data, extracted assets, build directories and generated
recompiler output are deliberately excluded, here and in the forks. The ReXGlue
SDK is a separate checkout rather than vendored.

## Mods

Translations into languages the game was never released in - with fonts of
their own, drawn to look like the game's - and your own music in place of EA
Trax: [`MODDING.md`](MODDING.md) has how.

## Reporting a problem

Read [CONTRIBUTING.md](CONTRIBUTING.md) first: a report needs the log, the
version, the hardware and the steps to reproduce it.

## Licence

Xerenge's own code is under the GNU General Public License, version 3 or later
([LICENSE](LICENSE)). The ReXGlue SDK and the third-party code under
`rexglue/thirdparty` keep their own licences. Burnout Revenge itself is not
covered: it is EA's and Criterion's, and you need your own disc to play.
