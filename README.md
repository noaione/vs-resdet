# vapoursynth-resdet

`vapoursynth-resdet` is a VapourSynth plugin that detects the likely source resolution
of an upscaled image or video frame.

it runs the analysis on the gpu. the input must already be gpu-resident. the
plugin returns the clip and adds width and height score arrays to each output
frame.

the wheel contains only the native plugin. it does not install a python module.

## install

install the wheel with pip:

```bash
python -m pip install vapoursynth_resdet-*.whl
```

the native library is installed in VapourSynth's plugin directory. after
installation, VapourSynth can load it as `resdet.Analyze`.

## use

`Analyze` accepts a gpu-resident clip:

```python
import vapoursynth as vs

core = vs.core
gpu_clip = core.std.GPUUpload(clip)
result = core.resdet.Analyze(gpu_clip, range=1)
frame = result.get_frame(0)

width_scores = frame.props["resdet_width_scores"]
height_scores = frame.props["resdet_height_scores"]
```

pass `debug=True` to attach the method, bounds, and range properties:

```python
debug_result = core.resdet.Analyze(gpu_clip, range=1, debug=True)
debug_frame = debug_result.get_frame(0)

method = debug_frame.props["resdet_method"]
width_bounds = debug_frame.props["resdet_width_bounds"]
height_bounds = debug_frame.props["resdet_height_bounds"]
score_range = debug_frame.props["resdet_range"]
```

the current implementation analyzes the Y plane and supports
integer samples up to 32 bits plus 16-bit and 32-bit float samples.

## build from source

you need VapourSynth 80, CMake 3.25+, a Vulkan SDK, and `glslangValidator`.
Ninja is used on macOS and Linux; Windows wheel builds use the installed
Visual Studio generator and MSVC.

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements-dev.txt

cmake --preset macos-debug
cmake --build --preset macos-debug
python -m build
```

Make sure you set the environment variable before building, for most of the platforms:

```bash
source /path/to/vulkan/install/X.Y.Z/setup-env.sh
```

## test

```bash
source .venv/bin/activate
python tests/gpu_analyze.py build/macos-debug/vsresdet.dylib
```

## benchmark

compare the gpu plugin with the original cpu implementation:

```bash
source .venv/bin/activate
python bench/compare.py
```

see [bench/README.md](bench/README.md) for setup and options.

## attribution

the detection algorithm and cpu reference come from tab's
[resdet](https://github.com/0x09/resdet). see `third_party/resdet/AUTHORS` and
`third_party/resdet/COPYING` for attribution and licensing.

the gpu DCT uses Dmitrii Tolmachev's
[VkFFT](https://github.com/DTolm/VkFFT). see `third_party/VkFFT/LICENSE` for
its license.
