# Xerenge next steps

This workspace adapts the Xbox 360 recompilation tools for legacy shader
containers found in early game builds. The repositories published for the work
contain source changes and documentation only. Game images, decrypted XEX
files, extracted assets, build directories, generated HLSL, and shader caches
stay local.

## Current result

- `XenosRecomp-xerenge` recognizes legacy `0x102A0E00` pixel and `0x102A0E01`
  vertex containers.
- Legacy metadata is normalized before the existing recompiler pipeline runs.
- The reference scan finds 131 raw containers: 64 pixel, 67 vertex, and 119
  unique shaders.
- The generated cache was checked locally by decoding its compressed SPIR-V;
  the generated cache itself is intentionally excluded from Git.
- `XenonRecomp-xerenge` can try both retail and devkit AES keys for the
  uncompressed XEX path used by the beta image.

## Execution order

1. Keep the two tool forks buildable from clean clones and record the exact
   upstream commit they are based on.
2. Add a small fixture format or synthetic container generator that contains no
   game data. Use it to test container bounds checks, stage detection, metadata
   normalization, and duplicate shader handling in CI.
3. Add differential checks for vertex declarations, interpolator mappings,
   sampler registers, constant registers, and export registers. The current
   fallback mappings make compilation robust; these checks will identify cases
   that still need exact metadata interpretation.
4. Validate generated DXIL and SPIR-V against a native rendering harness using
   captured constant, texture, vertex, and framebuffer inputs. Compilation
   success alone does not establish visual correctness.
5. Replace compatibility fallbacks with exact legacy metadata decoding as the
   harness exposes mismatches. Keep each change isolated and attach a small
   source-only regression fixture.
6. Add CI for clean CMake builds, the source-only fixtures, and a repository
   audit that rejects game data and generated shader caches.

## Publication boundary

Only source, build instructions, and source-only tests belong in the public
repositories. The following remain local and ignored: ISO/XEX files, decrypted
images, extracted directories, build trees, generated HLSL, and generated C++
shader caches.
