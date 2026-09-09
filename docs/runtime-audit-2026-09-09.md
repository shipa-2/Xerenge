# Runtime audit, 2026-09-09

The main-menu milestone is not verified. A working Vulkan device and visible
loading assets do not establish correct Xbox platform behavior.

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

## Work order and acceptance criteria

1. Capture the *current* frontend state and guest call stack after loading,
   together with the resource worker state and pending requests. The 30-second
   audit run repeatedly executes 0x82104DD0; frequency alone does not prove
   this worker is the main-thread blocker. Locate the unmet condition and its
   actual producer before modifying either.
2. Correct the kernel object/thread contract. Separate handles and guest object
   addresses, validate object types and lifetime, implement suspended startup
   and wait/signalling consistently. Audit the existing resource-worker bypass,
   forced context state=2, and device-selector resumption of unrelated threads.
   Remove each workaround only alongside a verified replacement.
3. Trace asynchronous file completion through guest queue consumption and its
   callback. Replace the forced resource-ready byte at 0x82D40F09 with the real
   service completion. Validate data, completion status and wakeup order.
4. Once the actual wait is known, implement the implicated audio/movie/XAM
   service contract. Do not assume sound or a logo movie is the blocker merely
   from a black frame.
5. Audit GPU command/resolve/present ownership separately. Current code turns a
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
- A bounded 30-second run selected the RX 6800 XT Vulkan backend, loaded through
  the stagehed request and continued executing guest code. Main-menu progress
  is not established by this run. Log: `/tmp/xerenge-abi-audit.log` (local only).
- `XERENGE_APT_TRACE=1` shows the Apt rendering callbacks and manager flags;
  `XERENGE_XENOS_DRAW_TRACE=1 XERENGE_XENOS_VULKAN_TRACE=1` shows sustained
  Vulkan UI submission. Logs were kept in `/tmp` and are not repository data.
