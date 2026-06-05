#!/usr/bin/env python3
"""Package the current SimpleScreenRecorder build into a .deb file."""

import os
import shutil
import subprocess
import sys
import tempfile

PROJECT_ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(PROJECT_ROOT, "build-release")
PKG_DIR = os.path.join(PROJECT_ROOT, "packaging")

# Package metadata
PACKAGE_NAME = "simplescreenrecorder"
VERSION = "0.4.4+custom5"
SECTION = "video"
PRIORITY = "optional"
ARCHITECTURE = "amd64"
MAINTAINER = "Custom Build <custom@local>"
DESCRIPTION_SUMMARY = "Feature-rich screen recorder with window-follow modes"
DESCRIPTION_LONG = (
    "SimpleScreenRecorder is a feature-rich screen recorder that supports X11"
    " and OpenGL. This custom build adds:\n"
    " - Follow the active window recording mode\n"
    " - Follow the window under the cursor recording mode\n"
    " - Smooth slide transitions between windows\n"
    " - Aspect-ratio-preserving letterboxing for dynamic window sizes"
)
DEPENDS = (
    "libavcodec60, libavformat60, libavutil58, libswscale7, "
    "libqt5widgets5, libqt5x11extras5, libqt5gui5, libqt5core5a, "
    "libx11-6, libxext6, libxfixes3, libxi6, libxinerama1, "
        "libpipewire-0.3-0, libasound2, libpulse0, "
        "libstdc++6, libc6"
)
REPLACES = "simplescreenrecorder"
CONFLICTS = "simplescreenrecorder"
PROVIDES = "simplescreenrecorder"


def run(cmd, cwd=None, check=True):
    print(f"  $ {' '.join(cmd)}")
    return subprocess.run(cmd, cwd=cwd, check=check)


def main():
    print("=== SimpleScreenRecorder .deb packager ===")

    # Clean and rebuild
    if os.path.isdir(BUILD_DIR):
        print(f"Removing old {BUILD_DIR} ...")
        shutil.rmtree(BUILD_DIR)

    os.makedirs(BUILD_DIR, exist_ok=True)

    # CMake options matching the previous build
    cmake_options = [
        "-DCMAKE_INSTALL_PREFIX=/usr",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DENABLE_32BIT_GLINJECT=FALSE",
        "-DENABLE_X86_ASM=TRUE",
        "-DENABLE_FFMPEG_VERSIONS=TRUE",
        "-DWITH_QT5=TRUE",
        "-DWITH_GLINJECT=TRUE",
        "-DWITH_OPENGL_RECORDING=ON",
        "-DWITH_V4L2=ON",
        "-DWITH_PIPEWIRE=ON",
        "-DWITH_ALSA=ON",
        "-DWITH_PULSEAUDIO=ON",
        "-DWITH_JACK=OFF",
    ]

    print("Configuring with cmake ...")
    run(["cmake"] + cmake_options + [".."], cwd=BUILD_DIR)

    print("Building ...")
    run(["make", "-j", str(os.cpu_count() or 4)], cwd=BUILD_DIR)

    # Stage install into temp directory
    with tempfile.TemporaryDirectory() as tmpdir:
        stage_dir = os.path.join(tmpdir, "stage")
        debian_dir = os.path.join(stage_dir, "DEBIAN")
        os.makedirs(debian_dir, exist_ok=True)

        print(f"Staging install to {stage_dir} ...")
        run(["make", f"DESTDIR={stage_dir}", "install"], cwd=BUILD_DIR)

        # Write control file
        control_path = os.path.join(debian_dir, "control")
        with open(control_path, "w") as f:
            f.write(f"Package: {PACKAGE_NAME}\n")
            f.write(f"Version: {VERSION}\n")
            f.write(f"Section: {SECTION}\n")
            f.write(f"Priority: {PRIORITY}\n")
            f.write(f"Architecture: {ARCHITECTURE}\n")
            f.write(f"Depends: {DEPENDS}\n")
            f.write(f"Replaces: {REPLACES}\n")
            f.write(f"Conflicts: {CONFLICTS}\n")
            f.write(f"Provides: {PROVIDES}\n")
            f.write(f"Maintainer: {MAINTAINER}\n")
            f.write(f"Description: {DESCRIPTION_SUMMARY}\n")
            for line in DESCRIPTION_LONG.splitlines():
                f.write(f" {line}\n")

        # Copy maintainer scripts
        for script in ("postinst", "prerm"):
            src = os.path.join(PKG_DIR, script)
            dst = os.path.join(debian_dir, script)
            if os.path.isfile(src):
                shutil.copy2(src, dst)
                os.chmod(dst, 0o755)

        # Build the .deb
        output_name = f"{PACKAGE_NAME}-{VERSION}-{ARCHITECTURE}.deb"
        output_path = os.path.join(PROJECT_ROOT, output_name)
        if os.path.exists(output_path):
            os.remove(output_path)

        print(f"Building {output_name} ...")
        run(["dpkg-deb", "--root-owner-group", "--build", stage_dir, output_path])

        print(f"\nDone: {output_path}")
        print(f"Install with: sudo dpkg -i {output_name}")
        print("If dependencies are missing: sudo apt --fix-broken install")


if __name__ == "__main__":
    main()
