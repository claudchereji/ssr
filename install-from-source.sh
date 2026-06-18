#!/bin/bash
#
# First-class source installer for this SimpleScreenRecorder fork.
#
# Builds and installs from source against whatever ffmpeg/Qt the target machine
# ships, so it works across Zorin/Ubuntu versions (unlike the prebuilt .deb, which
# is locked to one library generation). Run on the TARGET machine:
#
#     ./install-from-source.sh
#
# Any extra arguments are passed through to cmake. Do NOT run as root; the script
# calls sudo only for the apt and install steps.
#
set -euo pipefail
cd "$( dirname "${BASH_SOURCE[0]}" )"

if [ "$(id -u)" = "0" ]; then
	echo "Error: run this as a normal user, not root (it uses sudo where needed)." >&2
	exit 1
fi

if ! command -v apt-get >/dev/null 2>&1; then
	cat >&2 <<'EOF'
Error: apt-get not found. This installer targets Debian/Ubuntu/Zorin.
On other distributions install the equivalents of: a C++ toolchain, cmake,
pkg-config, Qt5 (Core/Gui/Widgets/X11Extras) dev, ffmpeg dev (avformat,
avcodec, avutil, swscale), X11 dev (x11, xext, xfixes, xi, xinerama),
ALSA dev and PulseAudio dev; then run ./simple-build-and-install.
EOF
	exit 1
fi

echo "==> Refreshing package lists ..."
sudo apt-get update -y || echo "  (apt-get update failed; continuing with cached lists)"

# Core dependencies: the build cannot succeed without these.
CORE=(
	build-essential cmake pkg-config git
	qtbase5-dev qtbase5-dev-tools libqt5x11extras5-dev qttools5-dev-tools
	libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
	libx11-dev libxext-dev libxfixes-dev libxi-dev libxinerama-dev
	libasound2-dev libpulse-dev
)
echo "==> Installing core build dependencies ..."
sudo apt-get install -y --no-install-recommends "${CORE[@]}"

# Optional features: install best-effort and disable the matching build option if
# the dev package is not available on this release (keeps older/newer Zorin building).
opt_present() { dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q "install ok installed"; }
try_install() { sudo apt-get install -y --no-install-recommends "$@" 2>/dev/null || true; }

echo "==> Installing optional feature dependencies (best-effort) ..."
try_install libv4l-dev
try_install libpipewire-0.3-dev
try_install libgl-dev libglu1-mesa-dev
# JACK is off by default (most desktops use Pulse/PipeWire); enable by installing
# libjack-jackd2-dev and passing -DWITH_JACK=ON.

WITH_V4L2=OFF;  opt_present libv4l-dev && WITH_V4L2=ON
WITH_PIPEWIRE=OFF; opt_present libpipewire-0.3-dev && WITH_PIPEWIRE=ON
WITH_GL=OFF; { opt_present libgl-dev && opt_present libglu1-mesa-dev; } && WITH_GL=ON
WITH_JACK=OFF; opt_present libjack-jackd2-dev && WITH_JACK=ON

echo "==> Feature configuration:"
echo "    V4L2=$WITH_V4L2  PipeWire=$WITH_PIPEWIRE  OpenGL/GLInject=$WITH_GL  JACK=$WITH_JACK"

# Build and install via the upstream auto-detecting script (handles ffmpeg-vs-libav
# and Qt version detection). 32-bit GLInject is disabled to avoid multilib deps.
echo "==> Building and installing ..."
ENABLE_32BIT_GLINJECT=FALSE ./simple-build-and-install \
	-DWITH_V4L2="$WITH_V4L2" \
	-DWITH_PIPEWIRE="$WITH_PIPEWIRE" \
	-DWITH_OPENGL_RECORDING="$WITH_GL" \
	-DWITH_GLINJECT="$WITH_GL" \
	-DWITH_JACK="$WITH_JACK" \
	"$@"

echo
echo "==> Done. Launch from the application menu or run: simplescreenrecorder"
