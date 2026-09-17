# third-party notices

`vs-resdet` is released under the MIT License in `LICENSE`.

## resdet

the detection method is an independent reimplementation inspired by tab's
[resdet](https://github.com/0x09/resdet). no source code from `resdet` is
included in the plugin or linked into the native binary.

the upstream project has separate licensing for its library and tooling. its
license texts and author list are available in the `third_party/resdet`
submodule:

- `third_party/resdet/COPYING`
- `third_party/resdet/COPYING.LGPL.txt`
- `third_party/resdet/COPYING.MIT.txt`
- `third_party/resdet/AUTHORS`

## VkFFT

the plugin includes and uses the header-only Vulkan backend of VkFFT.

copyright (c) 2020-present Dmitrii Tolmachev

VkFFT is distributed under the MIT License. the complete license text is in
`third_party/VkFFT/LICENSE` and is also included in the package license
metadata.

upstream project: <https://github.com/DTolm/VkFFT>

## glslang

the native plugin uses glslang to compile its embedded compute shaders at
runtime. glslang is a separate build and runtime dependency and is not bundled
with the wheel or raw plugin artifacts.

glslang contains several upstream licenses and notices. see the official
[glslang license file](https://github.com/KhronosGroup/glslang/blob/main/LICENSE.txt)
for the applicable terms.

## VapourSynth

the plugin uses the VapourSynth 80 API and headers and requires a separately
installed VapourSynth runtime. VapourSynth is not bundled with this project.

see the official
[VapourSynth license](https://github.com/vapoursynth/vapoursynth/blob/master/COPYING.LESSER)
for its terms.
