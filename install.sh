#!/usr/bin/env bash
# Install the Intel Arc H.265 decode patch for DaVinci Resolve Studio on Linux.
#
#   ./install.sh               check, copy, build, validate, install launcher + shortcut
#   ./install.sh --uninstall   restore the stock shortcut and remove the launcher
#
# Environment overrides:
#   ARC_DIR=...                 adapter folder           (default ~/.local/share/resolve-arc/arc-decode)
#   WRAPPER=...                 launcher path            (default ~/.local/bin/resolve-arc)
#   INSTALL_SHORTCUT=0          leave the desktop shortcut alone
#   FORCE_PRIVATE_FFMPEG=1      build the bundled FFmpeg 8.0.1 even if the system one matches
#   RESOLVE_ARC_DEVICE=/dev/dri/renderDNNN   use this render node instead of auto-detecting
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ARC_DIR=${ARC_DIR:-$HOME/.local/share/resolve-arc/arc-decode}
WRAPPER=${WRAPPER:-$HOME/.local/bin/resolve-arc}
INSTALL_SHORTCUT=${INSTALL_SHORTCUT:-1}
APPS=$HOME/.local/share/applications
DESKTOP=$APPS/com.blackmagicdesign.resolve.desktop
say() { printf '\n==> %s\n' "$*"; }
die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }
lib_path() { ldconfig -p 2>/dev/null | awk -v n="$1" '$1==n && /x86-64/ {print $NF; exit}'; }
lib_version() { python3 -I -c "import ctypes,sys; l=ctypes.CDLL(sys.argv[1]); f=getattr(l,sys.argv[2]); v=f(); print(f'{v>>16}.{(v>>8)&255}.{v&255}')" "$1" "$2" 2>/dev/null; }

if [[ ${1:-} == --uninstall ]]; then
    if [[ -f $DESKTOP.pre-arc ]]; then mv -f -- "$DESKTOP.pre-arc" "$DESKTOP"; echo "Restored $DESKTOP"
    elif [[ -f $DESKTOP ]] && grep -q resolve-arc "$DESKTOP"; then rm -f -- "$DESKTOP"; echo "Removed $DESKTOP (the system entry applies again)"; fi
    rm -f -- "$WRAPPER"; echo "Removed $WRAPPER. The adapter in $ARC_DIR was left in place; delete it by hand if you like."
    exit 0
fi

say "Checking prerequisites"
[[ $(uname -m) == x86_64 ]] || die "Only x86-64 Linux is supported."
[[ -x /opt/resolve/bin/resolve ]] || die "DaVinci Resolve Studio is not installed in /opt/resolve."
rv=$(lib_version /opt/resolve/libs/libavcodec.so.60 avcodec_version || true)
[[ $rv == 60.3.100 ]] || die "Resolve bundles libavcodec ${rv:-unknown}; the adapter is pinned to 60.3.100. See 'After updates' in the guide to re-pin."

if [[ -z ${RESOLVE_ARC_DEVICE:-} ]]; then
    for node in /dev/dri/renderD*; do
        [[ $(cat "/sys/class/drm/$(basename -- "$node")/device/vendor" 2>/dev/null) == 0x8086 ]] && { RESOLVE_ARC_DEVICE=$node; break; }
    done
fi
[[ -n ${RESOLVE_ARC_DEVICE:-} ]] || die "No Intel GPU render node found under /dev/dri."
[[ -r $RESOLVE_ARC_DEVICE && -w $RESOLVE_ARC_DEVICE ]] \
    || die "$RESOLVE_ARC_DEVICE is not accessible. Add yourself to the video and render groups, then log out and in."
echo "Intel render node: $RESOLVE_ARC_DEVICE"

for so in libva.so.2 libva-drm.so.2; do [[ -n $(lib_path $so) ]] || die "Missing $so. Install libva."; done
ihd=""
drvpath=${LIBVA_DRIVERS_PATH:-}
for d in ${drvpath//:/ } /usr/lib/x86_64-linux-gnu/dri /usr/lib64/dri /usr/lib/dri /usr/local/lib/dri; do
    [[ -e $d/iHD_drv_video.so ]] && { ihd=$d/iHD_drv_video.so; break; }
done
[[ -n $ihd ]] || die "Intel media driver (iHD_drv_video.so) not found. Install intel-media-driver / intel-media-va-driver-non-free."
echo "Intel media driver: $ihd"
for tool in cc ld make xz python3 ldconfig; do command -v $tool >/dev/null || die "Missing $tool. Install your distribution's build tools."; done
pgrep -f '^/opt/resolve/bin/resolve' >/dev/null && [[ ${ALLOW_RUNNING:-0} != 1 ]] && die "Quit DaVinci Resolve first."

clip=validation-4k30-420-10bit-hlg.mov
if command -v vainfo >/dev/null; then
    caps=$(LIBVA_DRIVER_NAME=iHD vainfo --display drm --device "$RESOLVE_ARC_DEVICE" 2>/dev/null || true)
    grep -q 'VAProfileHEVCMain10 .*VAEntrypointVLD' <<<"$caps" || die "VAAPI reports no HEVC Main10 decode on $RESOLVE_ARC_DEVICE."
    grep -q 'VAProfileHEVCMain422_10 .*VAEntrypointVLD' <<<"$caps" && clip=validation-4k60-422-10bit.mov
else
    echo "vainfo not installed; validating with the 4:2:0 clip."
fi

backend=private
if [[ ${FORCE_PRIVATE_FFMPEG:-0} != 1 ]]; then
    sc=$(lib_path libavcodec.so.62); su=$(lib_path libavutil.so.60); ss=$(lib_path libswscale.so.9)
    if [[ -n $sc && -n $su && -n $ss && $(lib_version "$sc" avcodec_version) == 62.11.100 && $(lib_version "$su" avutil_version) == 60.8.100 ]]; then
        backend=system
    fi
fi
echo "Decode backend FFmpeg: $backend"

say "Copying the adapter to $ARC_DIR"
mkdir -p -- "$ARC_DIR" "$(dirname -- "$ARC_DIR")/third_party"
cp -a -- "$here/arc-decode/." "$ARC_DIR/"
cp -a -- "$here/third_party/." "$(dirname -- "$ARC_DIR")/third_party/"
printf '%s\n' "$ARC_DIR/controls/$clip" > "$ARC_DIR/preflight-clip.txt"
if [[ $backend == private ]]; then
    say "Building the private FFmpeg 8.0.1 (a few minutes)"
    bash "$ARC_DIR/build-ffmpeg.sh"
else
    rm -rf -- "$ARC_DIR/ffmpeg"
fi

say "Building the adapter"
bash "$ARC_DIR/build.sh"

say "Validating Arc decoding against Resolve's own decoder (pixel-exact)"
read -r mode _ < "$ARC_DIR/download-mode.txt"
RESOLVE_ARC_DOWNLOAD=${mode:-vpp} RESOLVE_ARC_PIPELINE=4 RESOLVE_ARC_GETBUFFER=1 RESOLVE_ARC_DEVICE=$RESOLVE_ARC_DEVICE \
    python3 "$ARC_DIR/decode-test.py" "$ARC_DIR/controls/$clip" --hardware --defer-context \
    || die "Validation failed. Nothing outside $ARC_DIR was changed. See Troubleshooting in the guide."

say "Installing the launcher at $WRAPPER"
mkdir -p -- "$(dirname -- "$WRAPPER")"
sed "s|^ARC=.*|ARC=$ARC_DIR|" "$here/bin/resolve-arc" > "$WRAPPER"
chmod 755 -- "$WRAPPER"
install -m 755 "$here/bin/resolve-fps.sh" "$(dirname -- "$WRAPPER")/resolve-fps"
rm -f -- "${XDG_STATE_HOME:-$HOME/.local/state}/resolve-arc/validated"

if [[ $INSTALL_SHORTCUT == 1 ]]; then
    say "Pointing the DaVinci Resolve shortcut at the launcher"
    mkdir -p -- "$APPS"
    if [[ -f $DESKTOP ]]; then
        [[ -f $DESKTOP.pre-arc ]] || cp -a -- "$DESKTOP" "$DESKTOP.pre-arc"
    else
        sys=""
        for f in /usr/share/applications/com.blackmagicdesign.resolve.desktop /usr/local/share/applications/com.blackmagicdesign.resolve.desktop; do
            [[ -f $f ]] && { sys=$f; break; }
        done
        [[ -n $sys ]] || die "No Resolve desktop entry found to copy. Run $WRAPPER directly instead."
        cp -- "$sys" "$DESKTOP"
    fi
    sed -i "s|^Exec=.*|Exec=$WRAPPER %u|" "$DESKTOP"
    command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" 2>/dev/null || true
fi

say "Done"
echo "Launch Resolve from its usual shortcut, or run: $WRAPPER"
echo "Launcher log: ${XDG_STATE_HOME:-$HOME/.local/state}/resolve-arc/launcher.log"
echo "Decoder logs: $ARC_DIR/logs/"
