# Installing this SimpleScreenRecorder fork on another machine

This fork adds window-follow recording modes, a private-window privacy overlay, and
aspect-ratio-preserving (no-stretch) output. Two ways to install it.

## Recommended: build from source (works on any Zorin/Ubuntu version)

A prebuilt binary is locked to one library generation (the ffmpeg and Qt versions on
the build machine). Building from source on the target machine links against whatever
that machine ships, so this path works across Zorin/Ubuntu releases.

```bash
git clone https://github.com/claudchereji/ssr.git
cd ssr
./install-from-source.sh
```

The script installs the build dependencies with `apt`, builds, and installs to `/usr`.
It auto-detects ffmpeg-vs-libav and the Qt version, enables V4L2/PipeWire/OpenGL only
if their dev packages are available (so it still builds on older releases), and skips
32-bit GLInject to avoid multilib packages. Launch from the application menu or run
`simplescreenrecorder`.

Requirements: a Debian/Ubuntu/Zorin system with Qt5 development packages available
(Qt5 ships on Ubuntu 22.04 and 24.04). Pass extra cmake flags through, e.g.
`./install-from-source.sh -DWITH_JACK=ON`.

## Alternative: prebuilt .deb (same base only)

Faster, but only valid on a machine with the **same library generation** as the build
machine (this build targets Ubuntu 24.04 / Zorin 18, ffmpeg 6, Qt5). Build the package:

```bash
./package-deb.py            # produces simplescreenrecorder-0.4.4+custom6-amd64.deb
```

Copy the `.deb` to the target and install with apt so dependencies resolve:

```bash
sudo apt install ./simplescreenrecorder-0.4.4+custom6-amd64.deb
```

Use `apt install ./file.deb`, not `dpkg -i` (which does not pull dependencies).
Dependencies are generated automatically by `dpkg-shlibdeps`, so they match the build
base exactly (install `dpkg-dev` on the build machine; otherwise a noble-correct
fallback list is used).

## Uninstall

```bash
sudo apt remove simplescreenrecorder      # if installed from the .deb
# or, if installed from source:
sudo rm -f /usr/bin/simplescreenrecorder /usr/lib/*/libssr-glinject.* \
           /usr/share/applications/be.maartenbaert.simplescreenrecorder.desktop
sudo rm -rf /usr/share/simplescreenrecorder
```

## Notes / limitations

- amd64 (x86-64) only.
- 32-bit OpenGL applications cannot be captured (32-bit GLInject is disabled to keep
  installation simple); X11 and 64-bit OpenGL recording work normally.
- JACK audio is off by default; enable with `-DWITH_JACK=ON` (needs `libjack-jackd2-dev`).
- The virtual-camera (akvcam/v4l2loopback) feature was removed, so no kernel module is
  required.
