"""compare GPU score arrays with the upstream CPU implementation."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

import vapoursynth as vs
from compare import OriginalResdet, _default_cpu_library, _default_plugin

METHODS = ("sign", "mag", "orig", "zerox")


def pattern_value(tile_x: int, tile_y: int) -> float:
    return 0.1 + 0.8 * ((tile_x * 13 + tile_y * 37 + tile_x * tile_y * 7) % 97) / 96


def checkerboard_values(width: int, height: int, tile: int) -> list[float]:
    return [
        pattern_value(x // tile, y // tile)
        for y in range(height)
        for x in range(width)
    ]


def checkerboard_clip(core: vs.Core, width: int, height: int, tile: int) -> vs.VideoNode:
    tiles_x = width // tile
    tiles_y = height // tile
    rows = []
    for y in range(tiles_y):
        tiles = [
            core.std.BlankClip(
                width=tile,
                height=tile,
                format=vs.GRAYS,
                length=1,
                color=pattern_value(x, y),
            )
            for x in range(tiles_x)
        ]
        rows.append(core.std.StackHorizontal(tiles))
    return core.std.StackVertical(rows)


def max_difference(
    expected: list[float], actual: list[float], atol: float
) -> tuple[float, int, int]:
    differences = [abs(left - right) for left, right in zip(expected, actual)]
    if not differences:
        return 0.0, 0, -1
    maximum = max(differences)
    mismatches = sum(1 for difference in differences if difference > atol)
    return maximum, mismatches, differences.index(maximum)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plugin", type=Path, default=_default_plugin())
    parser.add_argument("--cpu-library", type=Path, default=_default_cpu_library())
    parser.add_argument("--width", type=int, default=64)
    parser.add_argument("--height", type=int, default=64)
    parser.add_argument(
        "--tile", type=int, default=1,
        help="repeat each deterministic value in this many pixels (default: 1)",
    )
    parser.add_argument("--range", dest="score_range", type=int, default=1)
    parser.add_argument("--atol", type=float, default=2e-5)
    parser.add_argument("--dump", action="store_true", help="print the first score values on failure")
    parser.add_argument("--methods", nargs="+", choices=METHODS, default=list(METHODS))
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if not args.plugin.is_file():
        raise SystemExit(f"missing plugin: {args.plugin}")
    if not args.cpu_library.is_file():
        raise SystemExit(f"missing upstream library: {args.cpu_library}")
    if args.width <= 0 or args.height <= 0 or args.tile <= 0:
        raise SystemExit("width, height, and tile must be positive")
    if args.width % args.tile or args.height % args.tile:
        raise SystemExit("width and height must be divisible by tile")
    if args.score_range <= 0 or args.score_range * 2 >= min(args.width, args.height):
        raise SystemExit("range must be positive and leave meaningful samples")

    values = checkerboard_values(args.width, args.height, args.tile)
    core = vs.core
    if not core.vulkan_devices:
        raise SystemExit("no Vulkan devices found; run this script from a normal desktop session")
    core.std.LoadPlugin(str(args.plugin))
    source = checkerboard_clip(core, args.width, args.height, args.tile)
    uploaded = core.std.GPUUpload(source)
    if args.dump:
        downloaded = core.std.GPUDownload(uploaded).get_frame(0)
        print(f"source expected={values[:8]}")
        print(f"source after GPU roundtrip={struct.unpack('<8f', downloaded[0].tobytes()[:32])}")

    print(
        f"input: {args.width}x{args.height} grays block pattern, tile={args.tile}; "
        f"range={args.score_range}"
    )
    failed = False
    for method in args.methods:
        effective_range = 1 if method == "zerox" else args.score_range
        cpu = OriginalResdet(
            args.cpu_library,
            args.width,
            args.height,
            0.0,
            effective_range,
            method,
            image_values=values,
            threshold=0.0,
        )
        try:
            cpu.analyze()
            expected_width, expected_height = cpu.scores()
        finally:
            cpu.close()

        analyzed = core.resdet.Analyze(
            uploaded,
            method=method,
            range=args.score_range,
        )
        frame = analyzed.get_frame(0)
        actual_width = [float(value) for value in frame.props["resdet_width_scores"]]
        actual_height = [float(value) for value in frame.props["resdet_height_scores"]]
        width_difference = max_difference(expected_width, actual_width, args.atol)
        height_difference = max_difference(expected_height, actual_height, args.atol)
        method_failed = (
            width_difference[0] > args.atol or height_difference[0] > args.atol
        )
        failed |= method_failed
        status = "FAIL" if method_failed else "ok"
        print(
            f"{method}: {status}; "
            f"width max_abs={width_difference[0]:.7g}, mismatches={width_difference[1]}, "
            f"index={width_difference[2]}; "
            f"height max_abs={height_difference[0]:.7g}, mismatches={height_difference[1]}, "
            f"index={height_difference[2]}"
        )
        if method_failed and args.dump:
            print(f"  width expected={expected_width[:24]}")
            print(f"  width actual={actual_width[:24]}")
            print(f"  height expected={expected_height[:24]}")
            print(f"  height actual={actual_height[:24]}")

    if failed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
