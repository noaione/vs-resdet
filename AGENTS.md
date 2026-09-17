# agent notes

## project

`vs-resdet` is a native VapourSynth 80 plugin. the repository does not contain
a runtime python package. python is used only for the build hook, tests, and
benchmark tooling.

the plugin entry point is `resdet.Analyze`. it requires a gpu-resident video
node, reads plane 0 (Y), unpacks and normalizes samples to `float`, performs a
2d DCT-II with VkFFT, calculates width and height sign scores in Vulkan compute
shaders, and attaches the results as frame properties.

the registered function signature is:

```text
Analyze(clip:vnode:gpu; range:int:opt;) -> clip:vnode:gpu;
```

frame properties are `resdet_width_scores`, `resdet_height_scores`,
`resdet_width_bounds`, `resdet_height_bounds`, and `resdet_range`.

## source layout

- `src/vs_resdet.cpp`: filter setup, VSVulkan4 integration, barriers, VkFFT,
  and frame-property output.
- `src/vkfft_dispatch.hpp`: Vulkan function-pointer dispatch adapter required
  because VSVulkan4 does not expose linked Vulkan prototypes.
- `shaders/unpack_y.comp`: sample unpacking and normalization.
- `shaders/sign_scores.comp`: gpu sign-score calculation.
- `src/shader_sources.hpp.in`: embeds GLSL source for runtime compilation.
- `bench/compare.py`: reusable ctypes CPU-vs-GPU FPS benchmark.
- `tests/gpu_analyze.py`: small end-to-end GPU/VapourSynth check.
- `hatch_build.py`: Release CMake build and native wheel staging.

## build and packaging

the normal macOS build is:

```bash
cmake --preset macos-debug
cmake --build --preset macos-debug
```

`glslangValidator` is required at build time. CMake discovers VapourSynth
headers through the active Python interpreter when possible. the CMake cache
variable `VAPOURSYNTH_INCLUDE_DIR` remains overrideable.

`python -m build` uses Hatchling. the custom hook builds a Release native
library and force-includes it at:

```text
vapoursynth/plugins/vsresdet.dylib
```

use `.dll` or `.so` on the corresponding platform. keep the plugin-only wheel
layout; do not add a Python package just to make the wheel non-empty.

the source distribution must include `src`, `shaders`, `bench`, `tests`, the
build hook, and the vendored VkFFT headers needed for an isolated rebuild.

## dependencies

VkFFT is a git submodule on `develop`, pinned to:

```text
e687182171ed817535bbaf9ee553fdc324ce9d29
```

do not update that submodule unless explicitly requested. `third_party/resdet`
is the upstream CPU reference used by the benchmark and should remain an
independent submodule.

## validation

the GPU test and benchmark need a normal logged-in macOS desktop session with
Metal/MoltenVK access. a headless shell can still run CMake, shader validation,
Python syntax checks, and the CPU benchmark path, but it cannot validate GPU
frame evaluation.

for changes affecting the native plugin, run:

```bash
cmake --preset macos-debug
cmake --build --preset macos-debug
python -m build
python tests/gpu_analyze.py build/macos-debug/vsresdet.dylib
```

then inspect the wheel and confirm it contains only the native plugin beneath
`vapoursynth/plugins/` plus wheel metadata.

preserve existing staged and unstaged work. do not use `git reset`,
`git checkout` cleanup, or broad destructive commands.
