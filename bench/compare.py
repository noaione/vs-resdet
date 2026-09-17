"""compare the original cpu library with the native gpu plugin."""

from __future__ import annotations

import argparse
import ctypes
import platform
import time
from dataclasses import dataclass
from pathlib import Path

import vapoursynth as vs

ROOT = Path(__file__).resolve().parents[1]


class RDResolution(ctypes.Structure):
    _fields_ = [("index", ctypes.c_size_t), ("confidence", ctypes.c_float)]


@dataclass(frozen=True)
class BenchmarkResult:
    name: str
    frames: int
    seconds: float

    @property
    def fps(self) -> float:
        return self.frames / self.seconds


def _default_plugin() -> Path:
    suffix = {"Darwin": ".dylib", "Windows": ".dll"}.get(platform.system(), ".so")
    candidates = (
        ROOT / "build" / "macos-debug" / f"vsresdet{suffix}",
        ROOT / "build" / "macos-release" / f"vsresdet{suffix}",
    )
    return next((path for path in candidates if path.is_file()), candidates[0])


def _default_cpu_library() -> Path:
    names = {
        "Darwin": ("libresdet.dylib",),
        "Windows": ("resdet.dll", "libresdet.dll"),
    }.get(platform.system(), ("libresdet.so",))
    candidates = tuple(ROOT / "third_party" / "resdet" / name for name in names)
    return next((path for path in candidates if path.is_file()), candidates[0])


def _error_text(library: ctypes.CDLL, error: int) -> str:
    message = library.resdet_error_str(error)
    if message:
        return message.decode("utf-8")
    return f"error {error}"


class OriginalResdet:
    """small ctypes wrapper around upstream libresdet's analysis API."""

    def __init__(
        self,
        library_path: Path,
        width: int,
        height: int,
        value: float,
        score_range: int,
        method: str = "sign",
        image_values: list[float] | None = None,
        threshold: float | None = None,
    ) -> None:
        self.width = width
        self.height = height
        self.library = ctypes.CDLL(str(library_path))
        self._configure_api()

        self.parameters = self.library.resdet_alloc_default_parameters()
        if not self.parameters:
            raise RuntimeError("libresdet could not allocate parameters")

        error = self.library.resdet_parameters_set_range(self.parameters, score_range)
        if error:
            self.library.resdet_free(self.parameters)
            raise RuntimeError(f"libresdet range setup failed: {_error_text(self.library, error)}")
        if threshold is not None:
            error = self.library.resdet_parameters_set_threshold(self.parameters, threshold)
            if error:
                self.library.resdet_free(self.parameters)
                raise RuntimeError(
                    f"libresdet threshold setup failed: {_error_text(self.library, error)}"
                )

        create_error = ctypes.c_int(0)
        method_pointer = self.library.resdet_get_method(method.encode("ascii"))
        if not method_pointer:
            self.library.resdet_free(self.parameters)
            raise RuntimeError(f"libresdet does not provide the {method!r} method")
        self.analysis = self.library.resdet_create_analysis(
            method_pointer,
            width,
            height,
            self.parameters,
            ctypes.byref(create_error),
        )
        self.library.resdet_free(self.parameters)
        self.parameters = None
        if not self.analysis:
            raise RuntimeError(
                "libresdet analysis setup failed: "
                f"{_error_text(self.library, create_error.value)}"
            )

        image_type = ctypes.c_float * (width * height)
        self.image = image_type()
        self.image[:] = image_values if image_values is not None else [value] * (width * height)

    def _configure_api(self) -> None:
        library = self.library
        library.resdet_error_str.argtypes = [ctypes.c_int]
        library.resdet_error_str.restype = ctypes.c_char_p
        library.resdet_alloc_default_parameters.restype = ctypes.c_void_p
        library.resdet_parameters_set_range.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        library.resdet_parameters_set_range.restype = ctypes.c_int
        library.resdet_parameters_set_threshold.argtypes = [ctypes.c_void_p, ctypes.c_float]
        library.resdet_parameters_set_threshold.restype = ctypes.c_int
        library.resdet_free.argtypes = [ctypes.c_void_p]
        library.resdet_get_method.argtypes = [ctypes.c_char_p]
        library.resdet_get_method.restype = ctypes.c_void_p
        library.resdet_create_analysis.argtypes = [
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_size_t,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_int),
        ]
        library.resdet_create_analysis.restype = ctypes.c_void_p
        library.resdet_analyze_image.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float)]
        library.resdet_analyze_image.restype = ctypes.c_int
        library.resdet_analysis_results.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.POINTER(RDResolution)), ctypes.POINTER(ctypes.c_size_t),
            ctypes.POINTER(ctypes.POINTER(RDResolution)), ctypes.POINTER(ctypes.c_size_t),
        ]
        library.resdet_analysis_results.restype = ctypes.c_int
        library.resdet_destroy_analysis.argtypes = [ctypes.c_void_p]

    def analyze(self) -> None:
        error = self.library.resdet_analyze_image(self.analysis, self.image)
        if error:
            raise RuntimeError(f"libresdet analysis failed: {_error_text(self.library, error)}")

    def scores(self) -> tuple[list[float], list[float]]:
        width_results = ctypes.POINTER(RDResolution)()
        height_results = ctypes.POINTER(RDResolution)()
        width_count = ctypes.c_size_t()
        height_count = ctypes.c_size_t()
        error = self.library.resdet_analysis_results(
            self.analysis,
            ctypes.byref(width_results), ctypes.byref(width_count),
            ctypes.byref(height_results), ctypes.byref(height_count),
        )
        if error:
            raise RuntimeError(f"libresdet result extraction failed: {_error_text(self.library, error)}")

        width_scores = [0.0] * self.width
        height_scores = [0.0] * self.height
        try:
            for index in range(width_count.value):
                result = width_results[index]
                if result.index < len(width_scores):
                    width_scores[result.index] = result.confidence
            for index in range(height_count.value):
                result = height_results[index]
                if result.index < len(height_scores):
                    height_scores[result.index] = result.confidence
        finally:
            self.library.resdet_free(width_results)
            self.library.resdet_free(height_results)
        return width_scores, height_scores

    def close(self) -> None:
        if self.analysis:
            self.library.resdet_destroy_analysis(self.analysis)
            self.analysis = None


def benchmark_cpu(args: argparse.Namespace) -> BenchmarkResult:
    if not args.cpu_library.is_file():
        raise SystemExit(
            f"missing {args.cpu_library}; build upstream libresdet first:\n"
            "  cd third_party/resdet\n"
            "  ./configure --disable-everything --enable-shared\n"
            "  make -j2 lib"
        )

    worker = OriginalResdet(
        args.cpu_library,
        args.width,
        args.height,
        args.value,
        args.score_range,
        args.method,
    )
    try:
        worker.analyze()
        for _ in range(args.warmup):
            worker.analyze()

        started = time.perf_counter()
        for _ in range(args.frames):
            worker.analyze()
        return BenchmarkResult("original resdet cpu", args.frames, time.perf_counter() - started)
    finally:
        worker.close()


def benchmark_gpu(args: argparse.Namespace) -> BenchmarkResult:
    if not args.plugin.is_file():
        raise SystemExit(
            f"missing {args.plugin}; build the plugin first with:\n"
            "  cmake --preset macos-debug\n"
            "  cmake --build --preset macos-debug"
        )

    core = vs.core
    if not core.vulkan_devices:
        raise SystemExit("no Vulkan devices found; run the GPU benchmark from a normal desktop session")

    core.std.LoadPlugin(str(args.plugin))
    source = core.std.BlankClip(
        width=args.width,
        height=args.height,
        format=vs.GRAY32,
        length=args.frames + args.warmup + 1,
        color=args.value,
    )
    clip = core.std.GPUUpload(source)
    analyzed = core.resdet.Analyze(clip, method=args.method, range=args.score_range)

    first_frame = analyzed.get_frame(0)
    if "resdet_width_scores" not in first_frame.props:
        raise RuntimeError("GPU benchmark did not receive resdet frame properties")
    for frame_index in range(1, args.warmup + 1):
        analyzed.get_frame(frame_index)

    started = time.perf_counter()
    for frame_index in range(args.warmup + 1, args.warmup + args.frames + 1):
        analyzed.get_frame(frame_index)
    return BenchmarkResult("vs-resdet gpu", args.frames, time.perf_counter() - started)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plugin", type=Path, default=_default_plugin())
    parser.add_argument("--cpu-library", type=Path, default=_default_cpu_library())
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--frames", type=int, default=50)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--range", dest="score_range", type=int, default=1)
    parser.add_argument(
        "--method",
        choices=("sign", "mag", "orig", "zerox"),
        default="sign",
        help="detector method to benchmark",
    )
    parser.add_argument("--value", type=float, default=0.5, help="constant synthetic input value")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.width <= 0 or args.height <= 0 or args.frames <= 0 or args.warmup < 0:
        raise SystemExit("width, height, and frames must be positive; warmup cannot be negative")
    if args.score_range <= 0:
        raise SystemExit("range must be positive")

    print(
        f"input: synthetic gray32 clip, {args.width}x{args.height}; "
        f"method={args.method}, frames={args.frames}, warmup={args.warmup}, "
        f"range={1 if args.method == 'zerox' else args.score_range}"
    )
    gpu = benchmark_gpu(args)
    cpu = benchmark_cpu(args)
    print(f"{gpu.name}: {gpu.fps:.2f} fps ({gpu.seconds:.3f}s/{gpu.frames} frames)")
    print(f"{cpu.name}: {cpu.fps:.2f} fps ({cpu.seconds:.3f}s/{cpu.frames} frames)")
    print(f"gpu speedup: {gpu.fps / cpu.fps:.2f}x")


if __name__ == "__main__":
    main()
