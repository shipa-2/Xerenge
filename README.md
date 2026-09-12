# Xerenge

Bringing Burnout Revenge (Xbox 360, retail) up as a native binary: the game's
PowerPC code is recompiled to C++ ahead of time and linked into an executable,
with the console's hardware and operating system emulated around it. There is
no CPU interpreter and no JIT.

## Two lines of work

**`main` - ReXGlue.** Current. The recompiler and system layer come from
[ReXGlue](https://github.com/rexglue/rexglue-sdk), used through our fork
[rexglue-xerenge](https://github.com/shipa-2/rexglue-xerenge), which carries
fixes and diagnostics found while bringing this title up. The project itself -
manifest, function boundaries, app skeleton, build glue - is in
[`rexglue/`](rexglue/), with build and run instructions in
[`rexglue/README.md`](rexglue/README.md).

**`legacy` - XenonRecomp.** Frozen, and still buildable. A runtime written for
this project ([`XerengeRuntime/`](XerengeRuntime/)) on top of
[XenonRecomp-xerenge](https://github.com/shipa-2/XenonRecomp-xerenge) and
[XenosRecomp-xerenge](https://github.com/shipa-2/XenosRecomp-xerenge). It
remains useful as a reference: it renders the frontend's colours correctly,
which is how the one open defect on the ReXGlue side was isolated to a single
shader constant rather than to the graphics pipeline.

The move was not about the recompilation approach, which is the same in both. It
was about the system layer. The previous line reached the point of needing a
texture cache for every Xenos format, an EDRAM render target cache, and a shader
cache kept in step with whatever the title binds - all of which ReXGlue already
has, with shader translation happening at runtime so there is no cache to keep
in step at all.

## Where it stands

The title boots, plays its logo videos, reaches the title screen and the
save/load prompt, and renders the 3D world at 60 fps. Audio comes up on its own.
The 3D world and the videos are colour-correct.

One defect is open: the tint constant the 2D layer multiplies by arrives wrong,
so the red logo reads magenta, amber text reads pink, a dark panel reads vivid
blue, and unselected menu items are not dimmed. It is bounded by measurement
rather than guesswork - the whole GPU chain was verified faithful, and the same
guest code produces the correct constant on the legacy runtime.
[`rexglue/README.md`](rexglue/README.md) records the measurements.

## What is not here

Game images, decrypted data, extracted assets, build directories and generated
recompiler output are deliberately excluded, here and in the forks. The ReXGlue
SDK is a separate checkout rather than vendored.
