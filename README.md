# Xerenge

Xerenge is a source-only integration effort for processing legacy Xbox 360
shader containers with the Xenos and Xenon recompilation tools.

The implementation lives in two public forks:

- [XenosRecomp-xerenge](https://github.com/shipa-2/XenosRecomp-xerenge)
- [XenonRecomp-xerenge](https://github.com/shipa-2/XenonRecomp-xerenge)

The detailed development plan is in [`docs/next-steps.md`](docs/next-steps.md).
Game images, extracted/decrypted data, build directories, and generated shader
outputs are deliberately excluded from this repository and from the two forks.

The first runnable milestone is in [`XerengeRuntime`](XerengeRuntime/). It
validates an XEX2 image, initializes GLFW and Vulkan, opens a diagnostic window,
and provides the base into which the CPU and Xbox 360 graphics services will be
connected.

```sh
cmake -S . -B build-runtime
cmake --build build-runtime
ctest --test-dir build-runtime --output-on-failure
./build-runtime/xerenge-runtime path/to/game.xex
./build-runtime/xerenge-runtime --inspect path/to/game.xex
./build-runtime/xerenge-runtime --extract-image path/to/plain.xex image.bin
./build-runtime/xerenge-runtime --map-image path/to/game.xex
./build-runtime/xerenge-runtime --imports path/to/game.xex
./build-runtime/xerenge-runtime --services path/to/game.xex
```

`--inspect` currently reads the XEX2 header and security metadata. It does not
execute the image yet. `--extract-image` now unwraps AES image keys using the
retail/devkit candidates and expands the XEX basic compression format. The
Burnout beta image is recovered as a valid PE image; its local output matches
the independently produced decrypted image byte-for-byte. Normal LZX
compression and PPC execution remain separate stages.

`--map-image` validates the decoded PE32 image, creates its guest address-space
layout, checks the entry point and section ranges, and prints the mapped section
table. It does not call guest code yet.

`--imports` parses the XEX import-library metadata and reports the service
groups that must be bound before guest code can run.

`--services` creates one diagnostic trap binding for every import descriptor.
These traps are intentionally explicit: unsupported Xbox services stop with
their library, import index, and guest thunk address instead of silently
returning an invalid value.
