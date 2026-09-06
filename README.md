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
cmake -S XerengeRuntime -B build-runtime
cmake --build build-runtime
ctest --test-dir build-runtime --output-on-failure
./build-runtime/xerenge-runtime path/to/game.xex
```
