"""Run the native Analyze filter against a GPU-resident VapourSynth clip."""

from __future__ import annotations

import argparse
from pathlib import Path

import vapoursynth as vs


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("plugin", type=Path)
    parser.add_argument("--device", type=int, default=-1)
    parser.add_argument("--width", type=int, default=64)
    parser.add_argument("--height", type=int, default=64)
    parser.add_argument("--range", dest="score_range", type=int, default=1)
    args = parser.parse_args()

    core = vs.core
    print(core)
    devices = core.vulkan_devices
    if not devices:
        raise SystemExit("No Vulkan devices found; run this from a normal macOS desktop session.")
    core.std.LoadPlugin(str(args.plugin))

    source = core.std.BlankClip(
        width=args.width,
        height=args.height,
        format=vs.GRAY32,
        length=1,
        gpu=1,
    )
    clip = core.std.GPUUpload(source)
    analyzed = core.resdet.Analyze(clip, range=args.score_range)
    frame = analyzed.get_frame(0)
    width_scores = frame.props["resdet_width_scores"]
    height_scores = frame.props["resdet_height_scores"]
    assert len(width_scores) == args.width
    assert len(height_scores) == args.height
    assert "resdet_method" not in frame.props
    assert "resdet_width_bounds" not in frame.props
    assert "resdet_height_bounds" not in frame.props
    assert "resdet_range" not in frame.props

    debug_frame = core.resdet.Analyze(
        clip, range=args.score_range, debug=True
    ).get_frame(0)
    assert debug_frame.props["resdet_method"] == "sign"
    assert list(debug_frame.props["resdet_width_bounds"]) == [
        args.score_range, args.width - args.score_range
    ]
    assert list(debug_frame.props["resdet_height_bounds"]) == [
        args.score_range, args.height - args.score_range
    ]
    assert debug_frame.props["resdet_range"] == args.score_range
    print(
        f"GPU Analyze passed: device={args.device} "
        f"width_scores={len(width_scores)} height_scores={len(height_scores)}"
    )


if __name__ == "__main__":
    main()
