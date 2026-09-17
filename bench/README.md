# benchmarking

`compare.py` measures the native `resdet.Analyze` plugin against the original
cpu `libresdet` implementation.

`compare_output.py` compares the width and height score arrays from both
implementations. it uses a deterministic `grays` input so the gpu and cpu see
the same float samples. the default per-pixel pattern avoids making exact-zero
dct coefficients the main test; use `--tile` to repeat each value over a block
when testing that case.

the speed benchmark uses the same synthetic `gray32` value for both paths. setup and
the configured warmup frames are excluded from the reported time. the gpu
benchmark includes VapourSynth frame evaluation and waits for the result, but
does not include plugin compilation.

## build the cpu reference

if `third_party/resdet/libresdet.dylib` is missing, build the upstream library:

```bash
cd third_party/resdet
./configure --disable-everything --enable-shared
make -j2 lib
cd ../..
```

## run

run this from a normal desktop session so VapourSynth can access the gpu:

```bash
source .venv/bin/activate
python bench/compare.py
```

the defaults are 1280x720, 50 timed frames, one warmup frame, and `range=1`.
override them for another workload:

```bash
python bench/compare.py \
  --plugin build/macos-release/vsresdet.dylib \
  --cpu-library third_party/resdet/libresdet.dylib \
  --width 1920 --height 1080 --frames 200 --warmup 5 --range 1
```

the script selects the debug plugin first and falls back to the release build.
the cpu path uses the upstream `sign` method, which matches the gpu plugin's
score calculation.

## output consistency

run this from a normal desktop session with a working vulkan device:

```bash
python bench/compare_output.py \
  --plugin build/macos-debug/vsresdet.dylib \
  --width 64 --height 48 --range 2 --dump
```

the script checks `sign`, `mag`, `orig`, and `zerox`. it exits with status 1
when a score differs by more than `--atol` (default `2e-5`).

## latest result

measured on an Apple M4 desktop with the debug plugin, using a 1280x720
synthetic `gray32` clip, 100 timed frames, five warmup frames, and `range=1`:

```text
vs-resdet gpu:         742.62 fps
original resdet cpu:   94.84 fps
gpu speedup:           7.83x
```

## latest consistency result

measured on the same Apple M4 desktop with the debug plugin, using a 64x48
`grays` per-pixel pattern, `range=2`, and the default `2e-5` tolerance:

```text
sign:   ok; width max_abs=5.960464e-08; height max_abs=0
mag:    ok; width max_abs=2.980232e-08; height max_abs=0
orig:   ok; width max_abs=2.980232e-08; height max_abs=0
zerox:  ok; width max_abs=2.980232e-08; height max_abs=0
```
