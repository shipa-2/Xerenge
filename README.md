# Xerenge

Bringing Burnout Revenge (Xbox 360, retail) up as a native binary: the game's
PowerPC code is recompiled to C++ ahead of time and linked into an executable,
with the console's hardware and operating system emulated around it. There is
no CPU interpreter and no JIT.

## Getting it running

You need a dump of the retail European release - the image hashing to
`34c1bd4d549c2c53f29d814fa5e5d1c04c533c5ca0c39e57b6c2538f44ff59b4` - and a
machine with Vulkan. Then:

```
./scripts/bootstrap.sh /path/to/burnout-revenge.iso
cd xerenge-rex && ./run.sh
```

That verifies the image, extracts it, builds the SDK and builds the title.
[`SETUP.md`](SETUP.md) covers the steps separately, what each one does, and what
to do when something goes wrong. The scripts are:

| | |
|---|---|
| `scripts/check-prerequisites.sh` | says what is missing before a build can fail halfway |
| `scripts/extract-image.py` | reads the disc image directly; XDVDFS, which ordinary tools do not open |
| `scripts/setup-sdk.sh` | fetches and builds the recompiler and system layer |
| `scripts/build.sh` | points the manifest at your game directory and builds |
| `scripts/bootstrap.sh` | all of the above, in order |
| `scripts/inspect-capture.sh` | reads a RenderDoc capture offline: passes, the draw behind a given pixel, full state, textures |
| `xerenge-rex/run.sh` | runs it, with the flags that are not optional and a log per run |

## Two lines of work

**`main` - ReXGlue.** Current. The recompiler and system layer come from
[ReXGlue](https://github.com/rexglue/rexglue-sdk), used through our fork
[rexglue-xerenge](https://github.com/shipa-2/rexglue-xerenge), which carries
fixes found while bringing this title up - including the recompiler defect that
made the entire frontend draw blue. The project itself - manifest, function
boundaries, app skeleton, build glue - is in [`xerenge-rex/`](xerenge-rex/), with the
detail in [`xerenge-rex/README.md`](xerenge-rex/README.md).

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
has, with shader translation happening at runtime so there is no cache to keep
in step at all.

## Where it stands

The title boots, plays its logo videos, reaches the title screen, loads a save
from the memory card and reaches the car select menu with that profile's rank
and cars, and renders the 3D world at 60 fps. Audio comes up on its own. The
frontend, the videos and the 3D world are colour-correct.

Open:

* The intro videos intermittently fail to start. The main thread sits in the
  movie player waiting on an event that nothing signals; the failing runs show
  two extra stuck waits on auto-reset events that the succeeding runs do not.
* Gameplay races draw nothing over a black background.
* The Wayland surface extension is offered by the loader but never enabled, so
  runs go through X11.

## What is not here

Game images, decrypted data, extracted assets, build directories and generated
recompiler output are deliberately excluded, here and in the forks. The ReXGlue
SDK is a separate checkout rather than vendored.
