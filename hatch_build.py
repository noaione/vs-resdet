"""Build and package the native VapourSynth plugin for wheel builds."""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
from pathlib import Path
from typing import Any

from hatchling.builders.hooks.plugin.interface import BuildHookInterface
from packaging import tags


class NativePluginHook(BuildHookInterface[Any]):
    """Compile the CMake target and place it in VapourSynth's plugin package."""

    build_directory = Path("build") / "hatch"
    plugin_directory = Path("vapoursynth") / "plugins"

    def initialize(self, version: str, build_data: dict[str, Any]) -> None:
        """Build a release plugin and add it to the wheel's package tree."""

        import vapoursynth as vs

        root = Path(self.root)
        build_directory = root / self.build_directory
        include_directory = Path(vs.get_include())
        is_windows = platform.system() == "Windows"
        generator = os.environ.get(
            "CMAKE_GENERATOR",
            "Visual Studio 17 2022" if is_windows else "Ninja",
        )

        configure_command = [
            "cmake",
            "-S",
            str(root),
            "-B",
            str(build_directory),
            "-G",
            generator,
        ]
        if is_windows:
            configure_command.extend([
                "-A",
                os.environ.get("CMAKE_GENERATOR_PLATFORM", "x64"),
            ])
        configure_command.extend([
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DVAPOURSYNTH_INCLUDE_DIR={include_directory}",
        ])

        subprocess.run(configure_command, cwd=root, check=True)

        build_command = [
            "cmake",
            "--build",
            str(build_directory),
            "--target",
            "vs-resdet",
        ]
        if is_windows:
            build_command.extend(["--config", "Release"])
        subprocess.run(build_command, cwd=root, check=True)

        extension = {
            "Darwin": ".dylib",
            "Windows": ".dll",
        }.get(platform.system(), ".so")
        native_plugin = build_directory / f"vsresdet{extension}"
        if is_windows:
            native_plugin = build_directory / "Release" / native_plugin.name
        if not native_plugin.is_file():
            raise RuntimeError(f"CMake did not produce {native_plugin}")

        destination_directory = root / self.plugin_directory
        destination_directory.mkdir(parents=True, exist_ok=True)
        staged_plugin = destination_directory / native_plugin.name
        shutil.copy2(native_plugin, staged_plugin)

        # Register the native artifact explicitly in Hatchling's inclusion
        # map. This keeps the plugin at VapourSynth's standard discovery path
        # inside the installed wheel without adding a Python package.
        build_data.setdefault("force_include", {})[str(staged_plugin)] = str(
            self.plugin_directory / native_plugin.name
        )

        # This wheel contains a native extension but is independent of the
        # Python ABI. Match BestSource's generic Python/platform wheel tag.
        build_data["pure_python"] = False
        build_data["tag"] = f"py3-none-{next(tags.platform_tags())}"

    def finalize(
        self, version: str, build_data: dict[str, Any], artifact_path: str
    ) -> None:
        """Remove the staging directory after Hatchling has copied it."""

        shutil.rmtree(Path(self.root) / self.plugin_directory.parent, ignore_errors=True)
