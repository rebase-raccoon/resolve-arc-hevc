# Intel Arc H.265 decoding for DaVinci Resolve Studio on Linux

Resolve Studio for Linux never uses Intel's media engine: it has no VAAPI or
oneVPL code, so H.265 decodes entirely on the CPU. This project adds it. An
`LD_PRELOAD` adapter decodes HEVC on the Intel GPU through VAAPI and hands
Resolve frames in its own buffers. Output is pixel-identical to Resolve's
software decoder, and nothing in `/opt/resolve` is modified.

On a Framework Laptop 13 Pro (Arc B390, Panther Lake), 4K60 10-bit 4:2:2 HEVC
went from 23 to about 59 displayed fps, with Resolve's CPU use falling from
8.3 cores to 2.1.

## Quick start

Quit Resolve first:

    git clone https://github.com/rebase-raccoon/resolve-arc-hevc.git
    cd resolve-arc-hevc
    ./install.sh              # check, build, validate, install launcher + shortcut
    ./install.sh --uninstall  # undo the launcher and shortcut

The installer validates the adapter pixel-exact against Resolve's own decoder
before changing anything outside its folder. The launcher re-validates after
any Resolve, FFmpeg, libva or driver update and falls back to stock Resolve if
anything no longer matches.

## Requirements

- x86-64 Linux with glibc and libva 2.x
- DaVinci Resolve Studio 21.1.1 in `/opt/resolve` (bundled libavcodec 60.3.100)
- An Intel GPU with HEVC Main10 VAAPI decode through the iHD media driver
- `cc`, `ld`, `make`, `xz`, `python3`

If the system FFmpeg is not exactly 8.0.1 (libavcodec 62.11.100), the
installer builds the bundled, signature-checked FFmpeg 8.0.1 source privately;
no -dev packages are needed.

The full guide, [docs/GUIDE.md](docs/GUIDE.md), covers prerequisites per
distribution, validation, measuring playback, re-pinning after updates, and
troubleshooting.

## Status

Tested on one machine: Arc B390, Ubuntu 26.04, kernel 7.0 (xe), iHD 26.1.2,
libva 2.23, with the system FFmpeg 8.0.1. Other Intel GPUs, other
distributions, and the private-FFmpeg build path are untested. Reports are
welcome; please include your GPU, distribution, installer output and
`resolve-fps` numbers.

Only HEVC 8- and 10-bit 4:2:0 and 4:2:2 are handled; 12-bit and 4:4:4 are not.

## Contents

- `install.sh`                   installer
- `bin/resolve-arc`              launcher (validates once per driver/library set, falls back to stock Resolve)
- `bin/resolve-fps.sh`           measures displayed fps (the Resolve viewer readout is unreliable)
- `arc-decode/`                  adapter source, build scripts, tests
- `arc-decode/controls/`         synthetic 4K HEVC validation clips (4:2:2 10-bit, 4:2:0 10-bit HLG)
- `third_party/libva-2.23.0/`    libva headers (MIT)
- `third_party/ffmpeg-8.0.1.*`   FFmpeg source and release signature (LGPL-2.1-or-later)
- `docs/GUIDE.md`                install and maintenance guide

## License

LGPL-2.1-or-later; see [LICENSE](LICENSE). Bundled third-party code keeps its
own license: libva headers are MIT, FFmpeg is LGPL-2.1-or-later.

DaVinci Resolve is a trademark of Blackmagic Design. This project is not
affiliated with or endorsed by Blackmagic Design or Intel.
