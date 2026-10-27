# Intel Arc H.265 Decoding for DaVinci Resolve on Linux: Install Guide

## Overview

This guide makes DaVinci Resolve Studio on Linux decode H.265 on an Intel GPU, with no proxies, on any distribution. On the test machine, 4K60 10-bit 4:2:2 HEVC went from 23 to about 59 displayed frames per second, and renders stayed bit-identical to stock Resolve.

Resolve's Linux build has no VAAPI or oneVPL code, so it never uses the Intel media engine. The patch is a small library loaded with `LD_PRELOAD` that intercepts Resolve's bundled FFmpeg 6 HEVC decoder calls. It decodes each packet on the Arc through FFmpeg 8 and VAAPI (the system copy if it is exactly 8.0.1, otherwise a private build), loaded in an isolated linker namespace so the two FFmpeg versions never collide.

```
Resolve demuxes the file
  │  avcodec_send_packet / avcodec_receive_frame   (Resolve's FFmpeg 6 API)
  ▼
libresolve_arc.so   LD_PRELOAD hook
  │
  ▼
FFmpeg 8 + VAAPI    loaded in an isolated linker namespace (dlmopen)
  │
  ▼
[GPU] HEVC decode on the Intel media engine
  │
  ▼
[GPU] VAAPI video-processing blit into CPU-cached memory
  │
  ▼
4 export workers repack into frames from Resolve's own get_buffer2
  │
  ▼
Resolve grades and displays the frame
```

Resolve still demuxes the file and grades the frame; only the decode and the copy-out move to the Arc. Dolby Vision clips skip the adapter and use Resolve's own decoder.

Three details make it fast:

- **Resolve-owned frames.** Output frames are allocated through Resolve's own `get_buffer2` callback, exactly as its software decoder does. Without this, Resolve treats every frame as foreign and playback stays near 20 fps.
- **GPU copy-out.** VAAPI video processing blits each decoded surface into normal cached memory, at 4.4 ms per 4K frame instead of 17 ms for reading GPU-mapped memory.
- **Pipelined export.** Four worker threads per decoder copy and repack frames in parallel, which keeps edited timelines with many cuts at full rate.

It applies when all of these hold:

- **An Intel GPU with HEVC Main10 decode in VAAPI,** through the iHD media driver. Clips in 10-bit 4:2:2 also need the GPU to list `HEVCMain422_10`. The patch was tested on an Arc B390 (Panther Lake); other Intel generations are untested.
- **DaVinci Resolve Studio 21.1.1.** The free edition has no H.265 decoder on Linux. Other Resolve versions need the re-pin described in After updates.
- **x86-64 Linux with glibc and libva 2.x.** The distribution and its own FFmpeg version don't matter: the installer builds a private FFmpeg when the system one doesn't match.

NVIDIA GPUs don't need this, because Resolve decodes on NVDEC natively. AMD GPUs are not covered.

## Prerequisites

The installer checks every item below and stops with a message naming whatever is missing.

| Requirement | Why | How the installer checks |
| --- | --- | --- |
| x86-64 Linux, glibc | The adapter hooks Resolve's x86-64 FFmpeg ABI. | `uname -m` |
| DaVinci Resolve Studio 21.1.1 in `/opt/resolve` | Its bundled FFmpeg must be libavcodec 60.3.100; the adapter is pinned to that ABI. | Reads `avcodec_version()` from Resolve's library |
| Intel GPU render node you can open | Decoding runs on it. | First `/dev/dri/renderD*` with PCI vendor `0x8086` |
| Intel media driver (iHD) | HEVC decode and the video-processing blit. | Finds `iHD_drv_video.so` |
| libva 2.x (`libva.so.2`, `libva-drm.so.2`) | VAAPI runtime. | `ldconfig -p` |
| `cc`, `ld`, `make`, `xz`, `python3` | Building and testing. | `command -v` |
| `vainfo`, optional | Confirms HEVC Main10 decode and picks the 4:2:2 or 4:2:0 test clip. | Runs it on the render node |

Install the packages with your distribution's names. Only Ubuntu was tested; the Fedora and Arch names are the usual ones but unverified.

```bash
# Ubuntu / Debian
sudo apt install intel-media-va-driver-non-free libva2 libva-drm2 vainfo build-essential python3 xz-utils

# Fedora, with RPM Fusion nonfree enabled
sudo dnf install intel-media-driver libva libva-utils gcc make xz python3

# Arch
sudo pacman -S intel-media-driver libva libva-utils base-devel python xz

# All: allow access to the GPU, then log out and back in
sudo usermod -aG video,render "$USER"
```

No FFmpeg package is required. If the system has FFmpeg 8.0.1 exactly (libavcodec 62.11.100, libavutil 60.8.100), the adapter uses it; otherwise the installer builds the bundled FFmpeg 8.0.1 source into the adapter's own folder. `ffmpeg` with libx265 is only needed for the optional regression suite in Step 4.

## Step 1: get the code

Clone the repository, or download the `resolve-arc-bundle.tar.gz` release asset, which has the same contents. Everything the install needs is included, with no machine-specific paths and no internet access needed at install time.

```bash
git clone https://github.com/rebase-raccoon/resolve-arc-hevc.git
cd resolve-arc-hevc
```

| Path in the repository | What it is |
| --- | --- |
| `install.sh` | Installer: checks, builds, validates, installs the launcher and shortcut. Also `--uninstall`. |
| `arc-decode/` | Adapter source (`bridge.c`, `pipeline.h`, `vpp.c`, `abi.h`, `repack.h`), build and launch scripts, tests, saved route. |
| `arc-decode/build-ffmpeg.sh` | Builds the private FFmpeg 8.0.1 when the system one doesn't match. |
| `arc-decode/controls/` | Two synthetic 4K HEVC validation clips: 59.94 fps 10-bit 4:2:2, and 29.97 fps 10-bit 4:2:0 HLG with HDR metadata. |
| `third_party/libva-2.23.0/` | libva headers, so no `-dev` packages are needed. MIT licence. |
| `third_party/ffmpeg-8.0.1.tar.xz` and `.asc` | FFmpeg release source and its signature. LGPL-2.1 or later. |
| `bin/resolve-arc` | The launcher. |
| `bin/resolve-fps.sh` | The playback measurement script from Step 6. |

The FFmpeg tarball's SHA-256 is pinned in `build-ffmpeg.sh` and checked before every build. To confirm the release signature yourself:

```bash
curl -fsSL https://ffmpeg.org/ffmpeg-devel.asc | gpg --import
gpg --verify third_party/ffmpeg-8.0.1.tar.xz.asc third_party/ffmpeg-8.0.1.tar.xz
# Good signature from "FFmpeg release signing key", fingerprint FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8
```

If you change the adapter, edit it in the repository and re-run `install.sh` to test.

## Step 2: install Resolve and check the GPU stack

Install Resolve Studio from Blackmagic's `.run` installer into `/opt/resolve`, activate the license, and launch it once so it creates its config folders. Then quit it and check each layer the patch depends on:

```bash
# 1. Find the Intel render node (vendor 0x8086) and check you can open it
for n in /dev/dri/renderD*; do v=$(cat /sys/class/drm/${n##*/}/device/vendor); echo "$n $v"; done
NODE=/dev/dri/renderD128   # set to the node that printed 0x8086
[ -r $NODE ] && [ -w $NODE ] && echo "access OK"

# 2. VAAPI decodes HEVC Main10, and ideally 4:2:2 10-bit (expect VAEntrypointVLD lines)
vainfo --display drm --device $NODE 2>/dev/null | grep -E 'HEVCMain(10|422_10) .*VLD'

# 3. Resolve's bundled FFmpeg is the pinned version (expect 60.3.100)
python3 -c "import ctypes; v=ctypes.CDLL('/opt/resolve/libs/libavcodec.so.60').avcodec_version(); print(v>>16, v>>8&255, v&255)"
```

In Resolve, Preferences > System > Memory and GPU should show the Intel GPU selected with OpenCL, which needs your distribution's Intel OpenCL runtime (`intel-opencl-icd` on Ubuntu). The "Decode H.264/H.265 using hardware acceleration" checkbox does nothing on Intel under Linux, so its setting doesn't matter.

If Resolve itself fails to start on your distribution, fix that first; it is outside this guide.

## Step 3: run the installer

The installer does Steps 3 to 5 in one run. It changes nothing outside the adapter folder until validation passes. Quit Resolve first.

```bash
cd resolve-arc-hevc        # or the extracted resolve-arc-bundle folder
./install.sh
```

It runs in this order:

1. Checks every prerequisite, finds the Intel render node and media driver, and picks the 4:2:2 test clip if `vainfo` lists `HEVCMain422_10`, else the 4:2:0 one.
2. Chooses the decode backend. It uses the system FFmpeg only if it is exactly 8.0.1; otherwise it runs `build-ffmpeg.sh`, which builds a minimal FFmpeg 8.0.1 with only the HEVC decoder and VAAPI into `arc-decode/ffmpeg/`.
3. Copies the adapter to `~/.local/share/resolve-arc/arc-decode` and builds it with `build.sh`.
4. Validates decoding pixel-exact, as in Step 4.
5. Installs the launcher and repoints the shortcut, as in Step 5.

| Override | Effect |
| --- | --- |
| `ARC_DIR=/path/arc-decode` | Install the adapter somewhere else. |
| `WRAPPER=/path/resolve-arc` | Put the launcher somewhere else. |
| `INSTALL_SHORTCUT=0` | Leave the desktop shortcut alone. |
| `FORCE_PRIVATE_FFMPEG=1` | Build the private FFmpeg even if the system one matches, so distribution FFmpeg updates never affect the patch. |
| `RESOLVE_ARC_DEVICE=/dev/dri/renderDNNN` | Use this render node instead of auto-detecting. |

The private FFmpeg build needs only `cc`, `make` and `xz`. It uses the bundled libva headers through a small built-in pkg-config stand-in, and it builds without assembly, so neither `-dev` packages nor `nasm` are required. Decoding runs on the GPU, so the missing assembly doesn't affect speed. This private-FFmpeg path has not yet been run end to end; the system-FFmpeg path has. If the FFmpeg build fails, its error shows the last lines of FFmpeg's configure or make log.

By hand, the same steps are:

```bash
mkdir -p ~/.local/share/resolve-arc
cp -a arc-decode third_party ~/.local/share/resolve-arc/
cd ~/.local/share/resolve-arc/arc-decode
bash build-ffmpeg.sh   # skip if the system FFmpeg is exactly 8.0.1
bash build.sh          # prints which FFmpeg the backend links against
echo "$PWD/controls/validation-4k60-422-10bit.mov" > preflight-clip.txt
```

`build.sh` produces `libresolve_arc.so`, the preload Resolve loads, and `libarc_namespace.so`, the root of the isolated namespace that links the decode-backend FFmpeg. Keep `third_party` beside `arc-decode`: `vpp.c` builds against its libva headers.

## Step 4: validate before first use

Validation decodes a test clip twice: once with Resolve's own FFmpeg 6 in software, once through the adapter on the GPU. It passes only if every pixel, timestamp, colour field and side-data payload matches. The installer and the launcher both run it; to run it yourself:

```bash
cd ~/.local/share/resolve-arc/arc-decode
RESOLVE_ARC_DOWNLOAD=vpp RESOLVE_ARC_PIPELINE=4 RESOLVE_ARC_GETBUFFER=1 \
  python3 decode-test.py controls/validation-4k60-422-10bit.mov --hardware --defer-context
# PASS: 2 decode/seek cycles, 64 frames; pixels, timestamps, color and side data match Resolve FFmpeg.
```

Use `controls/validation-4k30-420-10bit-hlg.mov` on GPUs without 4:2:2 decode. Two deeper checks are worth running after an install or rebuild:

```bash
# Regression suite of the older routes; needs ffmpeg with libx265 to make fixtures
python3 self-test.py

# Your own footage under load: 3 concurrent decoders, 5 seek cycles
RESOLVE_ARC_DOWNLOAD=vpp RESOLVE_ARC_PIPELINE=4 RESOLVE_ARC_GETBUFFER=1 \
  python3 decode-test.py /path/to/clip.MP4 --hardware --defer-context --threads 3 --cycles 5 --packets 90
```

Two results look like failures but are expected:

- **A `dts` difference on one frame per GOP.** Some camera files, such as Sony XAVC HS, show `pkt_dts` off by one frame on the frame before each keyframe. FFmpeg 6 and 8 assign that field differently; the pixels match, and Resolve times frames by `pts`.
- **An assertion on `flushes=` for Dolby Vision clips.** Those clips are handed back to Resolve's own decoder by design, so the adapter records no flushes. The pixel comparison before that assertion passes.

## Step 5: the launcher and shortcut

The launcher `~/.local/bin/resolve-arc` makes the patch safe for daily use, because it fails closed to stock Resolve rather than to a broken decoder. On each launch it:

1. Waits up to 30 seconds for a closing Resolve to exit. If one is still running, it starts stock Resolve, as the normal shortcut would.
2. Finds the Intel render node, unless `RESOLVE_ARC_DEVICE` is set.
3. Builds a key from that node plus the size and timestamp of the adapter libraries, the saved route, Resolve's binary and FFmpeg, the backend FFmpeg, libva, and the iHD driver.
4. If the key differs from the last validated one, re-runs the Step 4 validation. A failure shows a desktop notification and starts stock Resolve.
5. Starts Resolve through `launch.sh` with the adapter preloaded.

It logs to `~/.local/state/resolve-arc/launcher.log`. Each Resolve session also writes a decoder log to `~/.local/share/resolve-arc/arc-decode/logs/resolve-*.log`.

The installer points the DaVinci Resolve shortcut at the launcher through a user-level copy of the desktop entry, which takes precedence over the system one. By hand:

```bash
A=~/.local/share/applications/com.blackmagicdesign.resolve.desktop
if [ -f "$A" ]; then [ -f "$A.pre-arc" ] || cp -a "$A" "$A.pre-arc"
else cp /usr/share/applications/com.blackmagicdesign.resolve.desktop "$A"; fi
sed -i "s|^Exec=.*|Exec=$HOME/.local/bin/resolve-arc %u|" "$A"
update-desktop-database ~/.local/share/applications
```

The first launch after an install or update takes about 10 seconds longer while it validates. Desktops without `.desktop` support can run `resolve-arc` directly.

## Step 6: measure real playback

Don't judge the result by Resolve's viewer frame-rate readout. On the test machine it showed a green 59.94 while only 18 distinct frames per second reached the screen. Count distinct frames on screen instead.

The installer puts this script on your path as `resolve-fps`: start playback in Resolve, then run it. It records a 640×360 patch of the viewer losslessly at 120 fps for 6 seconds and counts frames that differ from the one before. It needs `ffmpeg` with x11grab and `xwininfo`, and works on X11 and on Wayland desktops where Resolve runs under XWayland, which is the default:

```bash
#!/usr/bin/env bash
# resolve-fps.sh: start playback in Resolve first, then run this.
win=$(xwininfo -root -tree | grep -E '"DaVinci Resolve Studio - ' | awk '{print $1}' | head -1)
ffmpeg -hide_banner -loglevel error -y -f x11grab -framerate 120 -draw_mouse 0 -window_id "$win" \
  -grab_x 1100 -grab_y 450 -video_size 640x360 -i :0 -t 6 -c:v ffv1 /tmp/resolve-fps.mkv
n=$(ffmpeg -hide_banner -i /tmp/resolve-fps.mkv -vf 'mpdecimate=hi=1:lo=1:frac=0:max=0' -f null - 2>&1 \
  | grep -oE 'frame= *[0-9]+' | tail -1 | grep -oE '[0-9]+')
echo "$n distinct frames in 6 s = $((n / 6)) fps"
```

Set GRAB\_X and GRAB\_Y (environment variables of the installed script), or adjust `-grab_x` and `-grab_y` so the patch sits inside the viewer at your window size. Use a static camera shot with some motion, and calibrate once with an intra-frame clip such as ProRes or DNxHR, which should read close to the timeline rate.

Two more signs the Arc is doing the work:

- The decoder log under `logs/resolve-*.log` shows `event=open … backend=vaapi`, and `vpp_frames` and `getbuffer_frames` equal `frames` in its `event=profile` lines.
- Resolve's CPU use during 4K60 playback drops sharply. On the test machine it was about 2 cores, against 8 cores for software decode. Check with `top -p $(pgrep -f /opt/resolve/bin/resolve)`.

## After updates

Updates never leave you with a broken decoder: the launcher re-validates on its own and falls back to stock Resolve if anything no longer matches. What you have to do depends on what changed.

| What changed | What happens | What to do |
| --- | --- | --- |
| iHD driver, libva, kernel, Intel OpenCL | The launcher re-validates on the next start. | Usually nothing. If validation fails, see Troubleshooting. |
| Distribution FFmpeg, with the private backend | Nothing: the backend uses its own FFmpeg in `arc-decode/ffmpeg/`. | Nothing. |
| Distribution FFmpeg, with the system backend, still 8.0.1 | Re-validates and keeps working. | Nothing. |
| Distribution FFmpeg, with the system backend, now a different version | The backend refuses to load, validation fails, stock Resolve starts. | Re-run `./install.sh`; it switches to the private FFmpeg. |
| Resolve, same bundled FFmpeg 60.3.100 | Re-validates. The `get_buffer2` offset could still move if Blackmagic rebuilt FFmpeg. | Run the full tests from Step 4. |
| Resolve with a new bundled FFmpeg | The adapter disables itself, validation fails, stock Resolve starts. | Re-pin the Resolve side (below). |

The pins are three lines in `bridge.c`:

| Line | Pins | Encoded as |
| --- | --- | --- |
| `if (r.version() != 0x3c0364 …` | Resolve libavcodec 60.3.100, libavutil 58.2.100 | `0x3c0364` = (60 << 16) + (3 << 8) + 100 |
| `if (b.version() != 0x3e0b64 …` | System libavcodec 62.11.100, libavutil 60.8.100 | `0x3e0b64` = (62 << 16) + (11 << 8) + 100 |
| `… s->alloc_ctx+408` | Offset of `get_buffer2` in Resolve's `AVCodecContext` | Bytes |

To re-pin after an FFmpeg change on either side:

1. Compare the new FFmpeg's public `AVFrame`, `AVPacket`, `AVCodecContext` and `AVCodecParameters` headers against the hand-written views in `abi.h`: `Frame6`, `Ctx6`, `Par6` for Resolve's side, `Frame8`, `Ctx8`, `Par8` for the system side. Fix any field that moved.
2. Find the new `get_buffer2` offset by scanning a fresh context for the default allocator:

   ```bash
   LD_LIBRARY_PATH=/opt/resolve/libs python3 -c "
   import ctypes as C; a=C.CDLL('/opt/resolve/libs/libavcodec.so.60')
   a.avcodec_alloc_context3.restype=C.c_void_p; a.avcodec_alloc_context3.argtypes=[C.c_void_p]
   a.avcodec_find_decoder_by_name.restype=C.c_void_p
   ctx=a.avcodec_alloc_context3(a.avcodec_find_decoder_by_name(b'hevc'))
   t=C.cast(a.avcodec_default_get_buffer2,C.c_void_p).value; w=(C.c_uint64*200).from_address(ctx)
   print('get_buffer2 offset:',[i*8 for i,x in enumerate(w) if x==t])"
   ```

   Replace `libavcodec.so.60` if the soname changed.
3. Update the version constants and the offset, then `bash build.sh`.
4. Run all of Step 4, including `self-test.py` and the three-decoder test on your own clips.
5. Re-check playback with the Step 6 script, then commit the change.

Moving the backend to a new FFmpeg release also means updating the source tarball name and SHA-256 in `build-ffmpeg.sh`, and the sonames `libavcodec.so.62` and `libswscale.so.9` wherever they appear in `build.sh`, `build-ffmpeg.sh`, `install.sh` and `bin/resolve-arc`.

## Troubleshooting

Start with `~/.local/state/resolve-arc/launcher.log`, then the newest `~/.local/share/resolve-arc/arc-decode/logs/resolve-*.log`.

| Symptom or log line | Cause | Fix |
| --- | --- | --- |
| Notification "Adapter validation failed", Resolve runs on software | A library or driver changed and no longer matches. | Run the Step 4 command by hand to see the error, then follow After updates. |
| Installer: "Resolve bundles libavcodec …; pinned to 60.3.100" | A different Resolve version. | Re-pin the Resolve side, or install Resolve Studio 21.1.1. |
| `event=unsupported_original_abi` | Resolve's bundled FFmpeg is not 60.3.100. | Same as above. |
| `event=backend_failed` | The backend FFmpeg is not 62.11.100 or is missing. | Re-run `./install.sh`, or `FORCE_PRIVATE_FFMPEG=1 ./install.sh`. |
| `build-ffmpeg.sh`: "VAAPI was not enabled" | FFmpeg's configure couldn't use the bundled libva headers or the system `libva.so.2`. | Check `ldconfig -p \| grep libva` lists both libva libraries; the error shows the configure log tail. |
| "No Intel GPU render node found" or "is not accessible" | No Intel GPU, or you are not in the `render` group in this session. | `sudo usermod -aG video,render $USER`, log out and in. On multi-GPU systems set `RESOLVE_ARC_DEVICE`. |
| "VAAPI reports no HEVC Main10 decode" | The GPU or driver lacks HEVC 10-bit decode. | Install the full Intel media driver (non-free variant where the distribution splits it). |
| `event=vpp_setup_failed` or `vpp_blit_failed` | The driver refused the video-processing blit. | Frames fall back to the slower fused route automatically. Report the GPU and driver version. |
| `event=dovi_passthrough` | A Dolby Vision clip, decoded by Resolve's own decoder by design. | Nothing. `RESOLVE_ARC_DOVI=hardware` decodes it on the GPU without the Dolby Vision metadata structure. |
| Smooth from local disk, stutters from a network share | Storage is too slow. A 200 Mbit/s camera clip needs about 25 MB/s sustained. | Copy originals locally, use wired networking, or mount the share with kernel CIFS or NFS instead of a FUSE mount. |
| A 3-second looped clip plays slowly | Each loop forces a seek and decoder warm-up. | Test with longer clips. |
| H.264 clips still use CPU | The adapter intercepts HEVC only. | Expected. |
| Two Resolve windows after launching | A previous Resolve was still exiting after 30 seconds. | Quit both, then launch once. |
| No audio on MP4 clips | Resolve for Linux can't decode AAC. This patch doesn't change that. | Remux the audio to PCM: `ffmpeg -i in.mp4 -map 0:v:0 -map 0:a? -c:v copy -c:a pcm_s24le out.mov`. |
| `launch.sh` refuses to start | Another `LD_PRELOAD` is set in the shell. | Launch from the shortcut or a clean shell. |

## Rollback

The patch never modifies `/opt/resolve`, system libraries, projects or media, so rolling back only touches the shortcut and the launcher.

- **For one session:** run `/opt/resolve/bin/resolve` directly.
- **Permanently:** run `./install.sh --uninstall` from the repository. It restores the `.pre-arc` shortcut backup, or removes the user-level entry so the system one applies, and deletes the launcher. The adapter folder stays in place.
- **By hand:** `mv ~/.local/share/applications/com.blackmagicdesign.resolve.desktop.pre-arc ~/.local/share/applications/com.blackmagicdesign.resolve.desktop` and `rm ~/.local/bin/resolve-arc`.

To fall back to the previous decode route without uninstalling, put `fused 4 4 0` in `arc-decode/download-mode.txt` and launch with `RESOLVE_ARC_PIPELINE=0 RESOLVE_ARC_GETBUFFER=0`. That route is slower; it's only useful for comparison.

## Reference

### Settings

| Setting | Default | Effect |
| --- | --- | --- |
| `RESOLVE_ARC_DOWNLOAD` | `vpp`, from `download-mode.txt` | Readback route. `vpp` blits to cached memory; `fused` maps GPU memory directly. |
| `RESOLVE_ARC_PIPELINE` | `4` | Frames exported in parallel per decoder, 2 to 8. `0` disables. |
| `RESOLVE_ARC_GETBUFFER` | `1` | Allocate frames through Resolve's own allocator. `0` disables. |
| `RESOLVE_ARC_DOVI` | unset | `hardware` decodes Dolby Vision clips on the Arc and drops the Dolby Vision metadata structure. |
| `RESOLVE_ARC_DEVICE` | `first Intel render node` | VAAPI render node to decode on. Set it on systems with more than one Intel GPU. |
| `RESOLVE_ARC_SKIP_PREFLIGHT` | set by the launcher | Skips `launch.sh`'s own preflight after the launcher validated. |

### Files on an installed machine

| Path | Role |
| --- | --- |
| `~/.local/share/resolve-arc/arc-decode/` | Adapter source, built libraries, tests, decoder logs in `logs/` |
| `~/.local/share/resolve-arc/arc-decode/ffmpeg/` | Private FFmpeg 8.0.1, only when the system one didn't match |
| `~/.local/share/resolve-arc/third_party/` | libva headers and the FFmpeg source |
| `~/.local/bin/resolve-arc`, `~/.local/bin/resolve-fps` | Launcher and playback measurement script |
| `~/.local/state/resolve-arc/` | Launcher log and the validated-stack key |
| `~/.local/share/applications/com.blackmagicdesign.resolve.desktop` | Shortcut override, with its `.pre-arc` backup |

### Tested configuration

Framework Laptop 13 Pro, Intel Core Ultra X7 358H with Arc B390 (Panther Lake), Ubuntu 26.04, kernel 7.0 with the `xe` driver, iHD 26.1.2, libva 2.23, Resolve Studio 21.1.1, system FFmpeg 8.0.1 backend. Displayed frames per second, counted as in Step 6, from local disk on a 59.94 fps timeline:

| Footage | Stock Resolve | With the patch |
| --- | --- | --- |
| Sony 4K60 10-bit 4:2:2 HEVC, continuous | 22.8 | 58.3 to 59.0 |
| Same clip, 20 cuts of 2 seconds | 14.7 | 55.2 to 57.5 |

Resolve used 2.1 CPU cores with the patch against 8.3 without. Results on other Intel GPUs will differ; measure them with Step 6. A 24-second mixed timeline of Sony, Pixel, iPhone, Panasonic 5.9K and H.264 clips rendered to DNxHR bit-identical with and without the patch, 1,421 of 1,421 frames.

### Known limits

- Only HEVC is intercepted. H.264, ProRes and other codecs use Resolve's usual paths.
- HEVC 4:2:0 and 4:2:2 at 8 and 10 bits are supported. 12-bit and 4:4:4 clips are rejected by the adapter and fail to decode, so keep such clips off timelines that use it, or launch stock Resolve for them.
- AAC audio is still unsupported in Resolve for Linux.
- The adapter is pinned to Resolve's FFmpeg 60.3.100 and to FFmpeg 8.0.1 for the backend; other Resolve versions need the re-pin in After updates.

* Tested on one GPU generation, Arc B390 (Panther Lake). Older Intel GPUs, Arc discrete cards, and distributions other than Ubuntu 26.04 are untested; the validation step decides whether a machine is safe to use.
* The private-FFmpeg build path is written and its pkg-config stand-in is unit-tested, but a full build and validation on a non-matching system has not been run yet.
