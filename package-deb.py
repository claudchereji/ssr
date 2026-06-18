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
VERSION = "0.4.4+custom6"
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
# Fallback dependency list, used only if dpkg-shlibdeps is unavailable. The names
# below are correct for Ubuntu 24.04 / Zorin 18 (noble), including the time_t (t64)
# transition (libasound2t64, libv4l-0t64). On a different base these may differ,
# which is exactly why dpkg-shlibdeps (auto-generated, base-correct) is preferred.
FALLBACK_DEPENDS = (
    "libavcodec60, libavformat60, libavutil58, libswscale7, "
    "libqt5widgets5, libqt5x11extras5, libqt5gui5, libqt5core5a, "
    "libx11-6, libxext6, libxfixes3, libxi6, libxinerama1, "
    "libpipewire-0.3-0, libasound2t64, libpulse0, libv4l-0t64, "
    "libglu1-mesa, libglx0, libopengl0, libstdc++6, libc6"
)
REPLACES = "simplescreenrecorder"
CONFLICTS = "simplescreenrecorder"
PROVIDES = "simplescreenrecorder"


def run(cmd, cwd=None, check=True):
    print(f"  $ {' '.join(cmd)}")
    return subprocess.run(cmd, cwd=cwd, check=check)


def compute_depends(stage_dir):
    """Auto-generate the dependency list from the staged binaries using
    dpkg-shlibdeps, so SONAMEs and package names (including t64 renames) are
    always correct for the base this package is built on. Falls back to a static
    list if dpkg-shlibdeps is not installed."""
    if shutil.which("dpkg-shlibdeps") is None:
        print("  dpkg-shlibdeps not found (install 'dpkg-dev' for accurate deps); "
              "using fallback dependency list.")
        return FALLBACK_DEPENDS

    # dpkg-shlibdeps needs a debian/control relative to the working directory.
    debian_dir = os.path.join(stage_dir, "debian")
    os.makedirs(debian_dir, exist_ok=True)
    with open(os.path.join(debian_dir, "control"), "w") as f:
        f.write("Source: %s\n\nPackage: %s\nArchitecture: %s\n"
                % (PACKAGE_NAME, PACKAGE_NAME, ARCHITECTURE))

    # Collect the ELF objects we ship (main binary + glinject lib, any arch dir).
    targets = []
    for root, _dirs, files in os.walk(os.path.join(stage_dir, "usr")):
        for name in files:
            p = os.path.join(root, name)
            if name == "simplescreenrecorder" or ".so" in name:
                targets.append(os.path.relpath(p, stage_dir))
    if not targets:
        return FALLBACK_DEPENDS

    try:
        res = subprocess.run(
            ["dpkg-shlibdeps", "-O", "--ignore-missing-info", *targets],
            cwd=stage_dir, capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as e:
        print("  dpkg-shlibdeps failed; using fallback list.\n" + (e.stderr or ""))
        return FALLBACK_DEPENDS
    finally:
        # Remove the temporary debian/ tree so it is not packaged into the .deb.
        shutil.rmtree(debian_dir, ignore_errors=True)

    # Output looks like: shlibs:Depends=libc6 (>= 2.34), libavcodec60, ...
    for line in res.stdout.splitlines():
        if line.startswith("shlibs:Depends="):
            deps = line.split("=", 1)[1].strip()
            if deps:
                print("  Auto-generated dependencies via dpkg-shlibdeps.")
                return deps
    return FALLBACK_DEPENDS


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

        # Compute dependencies from the staged binaries (auto, base-correct).
        print("Computing dependencies ...")
        depends = compute_depends(stage_dir)
        print(f"  Depends: {depends}")

        # Write control file
        control_path = os.path.join(debian_dir, "control")
        with open(control_path, "w") as f:
            f.write(f"Package: {PACKAGE_NAME}\n")
            f.write(f"Version: {VERSION}\n")
            f.write(f"Section: {SECTION}\n")
            f.write(f"Priority: {PRIORITY}\n")
            f.write(f"Architecture: {ARCHITECTURE}\n")
            f.write(f"Depends: {depends}\n")
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
        print(f"Install with: sudo apt install ./{output_name}")
        print("(apt resolves dependencies automatically; 'dpkg -i' does not.)")
        print("Requires a target with the same library generation (Ubuntu 24.04 /")
        print("Zorin 18 base). For a different version, use ./install-from-source.sh.")


if __name__ == "__main__":
    main()
