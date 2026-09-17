# Compute shaders

The native plugin ships these readable GLSL sources and compiles them through
VapourSynth 80's `VSVulkan4::compileGPUShader` entry point. CMake also validates
them with `glslangValidator` when the tool is available:

```bash
glslangValidator -V shaders/unpack_y.comp -o build/unpack_y.spv
glslangValidator -V shaders/score_methods.comp -o build/score_methods.spv
```

Generated SPIR-V belongs in `build/`, not in version control.

`unpack_y.comp` handles pitched VapourSynth Y-plane buffers and normalizes
integer samples to `[0, 1]`. `score_methods.comp` is dispatched once per axis
after the in-place VkFFT 2D DCT-II and implements the upstream `sign`, `mag`,
`orig`, and `zerox` scoring methods. It writes complete width/height score
arrays, with zeroes in the detector's excluded boundary range.
