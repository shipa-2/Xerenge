# Runtime audit, 2026-09-09

The runtime now reaches the frontend render loop on the virtual Xbox platform:
`CB4Game` reaches state 5, `CB4FrontEnd::Render` runs continuously, and Vulkan
readback contains guest-produced pixels. Interactive menu selection is still
not verified; the remaining focus is the guest frontend input/action path.

## Verified corrections

- `default_decrypted.bin` is in virtual-address layout. Read guest address A at
  offset A - 0x82000000, matching `mapPeImage`. Applying PE raw-section offsets
  to this image disassembles unrelated instructions. At 0x82114A48 the real
  sequence is `mflr r12; bl 0x8259b094; stfd f31,-0x58(r1); stwu r1,-0xb0(r1)`.
  Removed unsupported Flash object field reads derived from the wrong layout.
- `ObReferenceObjectByHandle(handle, type, outObject)` takes its output in r5,
  not r6. Guest callers 0x825AE3F0 and 0x825AE470 set r5 to stack+80 and read
  that slot after the call. The runtime now writes through r5. This only fixes
  argument placement; object validation, type checks and reference lifetime
  remain incomplete.
  Cross-check: https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xboxkrnl/xboxkrnl_ob.cc
- Loader enqueue 0x8210E168 takes path, done pointer, buffer, size in r4-r7.
  The stagehed request passes done=0x8287C6E1, frontend object+73, correctly.
  Entry-only `Prepare state=11` logging cannot identify the final wait: one
  invocation can advance through subsequent states without another entry log.
  The observed caller 0x82114EA8 belongs to state 15's loading-screen predicate.
- The guest then enters `CB4Game` states 4 and 5. `SetInitialMenuState`
  (0x82203780) is called once and `CB4FrontEnd::Render` (0x82103D28) is called
  continuously. This proves the current black/loading-looking output is after
  menu selection, in the render/resource path.
- A short GPU audit recorded 41,438 draw packets, all with source 2, and valid
  Vulkan shader modules for the active pairs. The framebuffer still contained
  only about 28,000 nonzero pixels, so packet submission alone is not evidence
  of correct menu rendering.
- The Apt callback trace now reaches `CB4AptManager::Render` (0x821FF208) after
  the frontend enters state 5. Its render flags transition from `0,0,1,0` to
  `1,1,1,0`, and `AptCBDrawRenderingUnit` (0x821F64C8) is called repeatedly.
  The trace also records texture lookup, animation, matrix and rendering-unit
  loading callbacks. This rules out a missing Apt dispatch path; remaining
  faults are in the rendering-unit data, texture interpretation, or GPU output.
- A 10-second GPU trace produced thousands of Vulkan draws, including large UI
  batches. The earlier count of two Vulkan draws came from a run without the
  current trace and was not representative of the steady-state path.
- A shader audit over 10 seconds recorded 1,904 cache hits and no shader cache
  misses or Vulkan pipeline failures. The active frontend pairs therefore
  reach valid compiled modules; shader lookup/recompilation is not the current
  cause of the missing menu. The same trace showed both large valid UI batches
  and many small packets whose fetched vertex words are zero, so the next
  check must classify fetch descriptors and rendering-unit payloads before
  changing vertex interpretation.
- The previous runtime treated `RB_COLOR_MASK=0` as all channels enabled. Xenos
  uses zero as a genuine no-write mask, including depth-only passes. The GPU
  path now preserves the lower four mask bits for both Vulkan and the software
  fallback, preventing setup/depth packets from contaminating the UI target.
- Apt animation tracing now identifies the actual main-menu assets: `Logo1`,
  `A-button`, `B-button`, `B4ButtonTextures`, and `Menu/background/bgVideo` are
  loaded and receive completion callbacks. The title is therefore past asset
  loading; the remaining failure is in rendering-unit resource binding or
  scanout composition.
- A bounded texture-payload trace confirms that the guest addresses used for
  the DXT1 UI resources contain nonzero, structurally valid blocks. The first
  64x64 resource at guest `0x6A985000` has valid RGB565 endpoints and index
  data, so the current artifact is not caused by an empty ISO/XDVDFS texture
  read. The payload trace is diagnostic-only and remains disabled by default.
- A 20-second run with `XERENGE_HOLD_A=1` reaches and stays in `CB4Game`
  state 5 with `frontendState=28` and repeatedly calls `CB4FrontEnd::Render`.
  The input service receives the button, but no state transition follows;
  this makes the remaining issue reproducible in the guest frontend path.
- An A/B runtime comparison with `XERENGE_XENOS_FORCE_SOFTWARE=1` produces
  only 2-3 nonzero framebuffer pixels, while the default Vulkan path produces
  about 22,000. The GPU path is therefore the useful rendering path; the
  software switch is retained only for diagnostics and is disabled by default.
- A frontend audit confirms that `FEMain.bin` is read to completion: its
  completion byte at `0x8287C6E1` becomes `1`, and the Flash manager observes
  `loaded=1`. The previous resource-queue hypothesis is therefore rejected;
  the remaining issue is in the Flash rendering path or its Xenos draw state.
- The frontend vertex shader constant audit reports the expected transform
  (`c0=(-1,1,0,1)`, `c1=(1/480,-1/360,0,0)`) and valid `c2` colors. Constant
  buffer endian conversion is therefore not the cause of the missing menu.
- A corrected async-loader audit shows the queue itself is drained: the loader
  queue is at `0x82847090`, both indices remain zero after requests complete,
  and the active file is closed. The earlier report of
  `writeIndex=0x3ea22223` came from an incorrect diagnostic base address.
- The virtual XInput service now exposes standard buttons, triggers and sticks,
  with packet numbers changing only when buttons change. Runtime traces confirm
  that automatic A reaches `XamInputGetState` as `0x1000`, but the frontend's
  compact object callback is not reached in the current boot path.
- The GLFW close callback is wired to the runtime loop's shutdown flag; a
  close-window test left no `xerenge-runtime` process behind.
- The EALogo trace narrows the remaining boot stop: the global EALogo state
  stays at `1`, the movie manager ready byte at `0x82A5900C`'s associated
  resource flag remains zero, and the video object is repeatedly observed in
  state `55` with no decoder object. The guest therefore never receives the
  movie completion transition that should release the frontend into its menu.

## Work order and acceptance criteria

1. Resolve the EALogo/movie completion contract: trace the movie open, decoder
   creation, frame advancement and completion callback, then implement the
   missing XFile/XAudio/video service behavior that keeps the guest state
   machine moving.
2. Resolve the frontend input object/vtable path after loading: identify the
   caller that consumes the XInput result, bind its real callback contract, and
   verify A/D-pad navigation changes the guest menu state.
3. Capture the current frontend draw state after loading: movie/render-unit
   identity, vertex bounds, active shader pair, texture descriptor and resolved
   framebuffer bounds. Use that correlation to identify the first missing or
   clipped menu batch.
4. Correct the kernel object/thread contract. Separate handles and guest object
   addresses, validate object types and lifetime, implement suspended startup
   and wait/signalling consistently. Audit the existing resource-worker bypass,
   forced context state=2, and device-selector resumption of unrelated threads.
   Remove each workaround only alongside a verified replacement.
5. Keep asynchronous file completion evidence-backed. The current audit shows
   the FEMain completion callback is already delivered; remove or narrow any
   remaining workaround only after the same callback path remains verified.
6. Once the actual wait is known, implement the implicated audio/movie/XAM
   service contract. Do not assume sound or a logo movie is the blocker merely
   from a black frame.
7. Audit GPU command/resolve/present ownership separately. Current code turns a
   zero color-write mask into all channels enabled and restores old software
   pixels over black Vulkan pixels. These behaviors need focused register and
   framebuffer tests, not more coordinate flips. Preserve the working Vulkan
   initialization while investigating command semantics.

Success means the guest reaches and responds in its real menu, supported by
state/callback traces and framebuffer readback; no forced menu flags or window
screenshots. Keep generated game code, images and shader caches out of Git.

## Validation of this change

- Canonical CMake build completed with all available cores.
- Existing `validate_minimal_xex` CTest passed; it does not test kernel object ABI.
- A bounded runtime run selected the RX 6800 XT Vulkan backend, loaded through
  the stagehed request, reached frontend state 5, and produced nonzero guest
  framebuffer readback. Log: `/tmp/xerenge-60.log` (local only).
- `XERENGE_APT_TRACE=1` shows the Apt rendering callbacks and manager flags;
  `XERENGE_XENOS_DRAW_TRACE=1 XERENGE_XENOS_VULKAN_TRACE=1` shows sustained
  Vulkan UI submission. Logs were kept in `/tmp` and are not repository data.
- A 45-second run with media and frontend tracing opened the complete early
  frontend set, including `FEMain.bin` and `stagehed.bin`, then reached
  `Frontend menu function` with `CB4Game current=5`, `frontendState=28`, and
  `stagehedDone=1`. The async loader remained drained afterwards. This moves
  the active investigation from disk loading to menu framebuffer composition
  and the later XMV request path.
- The ISO contains real XMV entries; the second XDVDFS directory record for
  `EA_FrP.xmv` resolves to a valid WMV9/WMAPro stream when extracted. The
  runtime has not requested that file during the observed early boot window,
  so adding a decoder before confirming the guest request would be premature.
- The movie trace now covers the state-machine entry points and confirms that
  the video object reaches state 55 before any decoder object is installed.
- The earlier conclusion that a missing callback dispatch blocked the logo
  was invalid: a shared trace limit was exhausted by generic string calls.
  Event 0 at `8220BC40` is constructor initialization, not evidence that
  subsequent movie commands are absent.

## Logo startup correction, 2026-09-10

- The user confirmed that the display snapshot correction removed flickering.
  Pixels and dimensions are published together; PM4 swap metadata no longer
  resizes previously published pixels. No window screenshots were taken.
- Independent per-function trace limits reveal repeated `821FF458` movie
  commands with both gates enabled. The ready byte is at
  `0x82A538C0 + 22401`; the earlier diagnostic used a base 32 bytes too low.
- The command contains `_name=#lookupVideo1`, and the title's lookup table
  already contains `EA_FrP`. Before the fix, `strtok` returned null for the
  value following `_SizeY`, leaving the movie name as `none`.
- The generated direct thunks at `825C6B6C` through `825C6B9C` were NOP bodies,
  bypassing the implemented KeTls services. Route all four direct TLS calls
  through the same service used by named imports. The guest CRT now retains
  its tokenizer continuation across calls.
- Runtime validation after this fix parsed every parameter, called `821FEAC0`
  and `8235ACD0`, and opened `D:\ovid\EA_FrP.xmv`. Reads reached offsets 0,
  0x20000, 0x40000, 0x60000, and 0x80000. This proves movie startup/file access,
  not successful decoding, presentation, or arrival at the interactive menu.
- The mounted directory tree resolves that file to sector `0x18A91C`, size
  `0x6EBF8`. ffprobe identifies WMV3 1280x720 with WMAv2 audio, duration
  4.033 seconds. The earlier 15.4-second candidate came from a different raw
  directory record and is not the file opened by this runtime.
- Remaining work: follow execution after the movie reads, validate decoder
  and completion behavior, and then verify the logo-to-menu transition.
