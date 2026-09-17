# benchmarking

`compare.py` measures the native `resdet.Analyze` plugin against the original
cpu `libresdet` implementation.

the benchmark uses the same synthetic `gray32` value for both paths. setup and
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

the script selects the release plugin first and falls back to the debug build.
the cpu path uses the upstream `sign` method, which matches the gpu plugin's
current score calculation.

## latest result

measured on an Apple M4 desktop with the debug plugin, using a 1280x720
synthetic `gray32` clip, 20 timed frames, one warmup frame, and `range=1`:

```text
vs-resdet gpu:       459.52 fps
original resdet cpu:  94.25 fps
gpu speedup:           4.88x
```
