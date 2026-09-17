# Compute shaders

The native plugin ships these readable GLSL sources and compiles them through
VapourSynth 80's `VSVulkan4::compileGPUShader` entry point. CMake also validates
them with `glslangValidator` when the tool is available:

```bash
glslangValidator -V shaders/unpack_y.comp -o build/unpack_y.spv
glslangValidator -V shaders/sign_scores.comp -o build/sign_scores.spv
```

Generated SPIR-V belongs in `build/`, not in version control.

`unpack_y.comp` handles pitched VapourSynth Y-plane buffers and normalizes
integer samples to `[0, 1]`. `sign_scores.comp` is dispatched once per axis
after the in-place VkFFT 2D DCT-II and writes complete width/height score
arrays, with zeroes in the detector's excluded boundary range.
