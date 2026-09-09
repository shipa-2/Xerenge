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
