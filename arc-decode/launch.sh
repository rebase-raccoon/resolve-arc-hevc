#!/usr/bin/env bash
set -euo pipefail
arc_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if pgrep -x resolve >/dev/null; then
    echo 'Save your work and quit Resolve, then run this launcher again.' >&2
    exit 1
fi
if [[ -n ${LD_PRELOAD:-} ]]; then
    echo 'This launcher needs a shell without another LD_PRELOAD experiment enabled.' >&2
    exit 1
fi
if [[ -z ${RESOLVE_ARC_DEVICE:-} ]]; then
    # First render node that belongs to an Intel GPU (vendor 0x8086).
    for node in /dev/dri/renderD*; do
        [[ $(cat "/sys/class/drm/$(basename -- "$node")/device/vendor" 2>/dev/null) == 0x8086 ]] && { RESOLVE_ARC_DEVICE=$node; break; }
    done
fi
export RESOLVE_ARC_DEVICE=${RESOLVE_ARC_DEVICE:-/dev/dri/renderD128}
if [[ ! -c $RESOLVE_ARC_DEVICE || ! -r $RESOLVE_ARC_DEVICE || ! -w $RESOLVE_ARC_DEVICE ]]; then
    echo "Cannot access $RESOLVE_ARC_DEVICE. Run this from your Linux desktop terminal." >&2
    exit 1
fi
arc_node=$(basename -- "$RESOLVE_ARC_DEVICE")
if [[ $(cat "/sys/class/drm/$arc_node/device/vendor") != 0x8086 ]]; then
    echo "$RESOLVE_ARC_DEVICE is not an Intel GPU; set RESOLVE_ARC_DEVICE to your Arc render node." >&2
    exit 1
fi
if [[ ! -f $arc_dir/libresolve_arc.so || ! -f $arc_dir/libarc_namespace.so ]]; then
    bash "$arc_dir/build.sh"
fi
echo 'Checking hardware decoding through the adapter before starting Resolve...'
arc_download=${RESOLVE_ARC_DOWNLOAD:-}
arc_prefetch=${RESOLVE_ARC_PREFETCH:-}
arc_workers=${RESOLVE_ARC_WORKERS:-}
arc_cleanup=${RESOLVE_ARC_ASYNC_CLEANUP:-}
if [[ -z $arc_download && -f $arc_dir/download-mode.txt ]]; then
    read -r arc_download arc_saved_depth arc_saved_workers arc_saved_cleanup < "$arc_dir/download-mode.txt"
    arc_prefetch=${arc_prefetch:-${arc_saved_depth:-0}}
    arc_workers=${arc_workers:-${arc_saved_workers:-1}}
    arc_cleanup=${arc_cleanup:-${arc_saved_cleanup:-0}}
fi
case ${arc_download:-transfer} in
    transfer|map|direct|fused|staged|vpp) export RESOLVE_ARC_DOWNLOAD=${arc_download:-transfer} ;;
    *) echo 'Unknown Arc readback route; expected transfer, map, direct, fused, staged, or vpp.' >&2; exit 1 ;;
esac
case ${arc_prefetch:-0} in
    [0-8]) export RESOLVE_ARC_PREFETCH=${arc_prefetch:-0} ;;
    *) echo 'Arc prefetch depth must be between 0 and 8.' >&2; exit 1 ;;
esac
case ${arc_workers:-1} in
    1|2|4) export RESOLVE_ARC_WORKERS=${arc_workers:-1} ;;
    *) echo 'Arc conversion workers must be 1, 2, or 4.' >&2; exit 1 ;;
esac
case ${arc_cleanup:-0} in
    0|1) export RESOLVE_ARC_ASYNC_CLEANUP=${arc_cleanup:-0} ;;
    *) echo 'Arc background cleanup must be 0 or 1.' >&2; exit 1 ;;
esac
# Pipelined export with Resolve-owned frame allocation (2026-10-26 defaults).
export RESOLVE_ARC_PIPELINE=${RESOLVE_ARC_PIPELINE:-4}
export RESOLVE_ARC_GETBUFFER=${RESOLVE_ARC_GETBUFFER:-1}
echo "Export pipeline depth: $RESOLVE_ARC_PIPELINE"
echo "Resolve frame allocator: $RESOLVE_ARC_GETBUFFER"
echo "Background frame cleanup: $RESOLVE_ARC_ASYNC_CLEANUP"
echo "Conversion workers: $RESOLVE_ARC_WORKERS"
echo "GPU readback route: $RESOLVE_ARC_DOWNLOAD"
echo "Frame prefetch depth: $RESOLVE_ARC_PREFETCH"
arc_test_clip=${RESOLVE_ARC_TEST_CLIP:-}
if [[ -z $arc_test_clip && -f $arc_dir/preflight-clip.txt ]]; then
    IFS= read -r arc_test_clip < "$arc_dir/preflight-clip.txt"
fi
if [[ ${RESOLVE_ARC_SKIP_PREFLIGHT:-0} == 1 ]]; then
    echo 'Preflight skipped: this exact library/driver set was already validated.'
elif [[ -n $arc_test_clip ]]; then
    echo "Preflight clip: $arc_test_clip"
    python3 "$arc_dir/decode-test.py" "$arc_test_clip" --hardware --defer-context
else
    python3 "$arc_dir/decode-test.py" --hardware --defer-context
fi
mkdir -p "$arc_dir/logs"
export RESOLVE_ARC_MODE=vaapi
export RESOLVE_ARC_LOG
RESOLVE_ARC_LOG=$(mktemp "$arc_dir/logs/resolve-$(date +%Y%m%d-%H%M%S)-XXXXXX.log")
echo "Arc decoder evidence: $RESOLVE_ARC_LOG"
arc_preload="${RESOLVE_ARC_PRE_PRELOAD:+$RESOLVE_ARC_PRE_PRELOAD:}$arc_dir/libresolve_arc.so"
if [[ -n ${RESOLVE_ARC_PRIVATE_PRELOAD:-} ]]; then
    python3 - "${RESOLVE_ARC_PRIVATE_ROOT:-}" "$RESOLVE_ARC_PRIVATE_PRELOAD" "$arc_dir" <<'PYGUARD'
from pathlib import Path
import sys
root=Path(sys.argv[1]).resolve()
allowed=(Path(sys.argv[3])/"private-runtime").resolve()
assert root.is_relative_to(allowed), 'Private runtime root is outside the prepared diagnostics tree'
for name in sys.argv[2].split(':'):
    path=Path(name).resolve()
    assert path.is_relative_to(root) and path.is_file(), f'Invalid private preload: {path}'
PYGUARD
    arc_preload="$arc_preload:$RESOLVE_ARC_PRIVATE_PRELOAD"
    echo "Private OpenCL dependency root: $RESOLVE_ARC_PRIVATE_ROOT"
    echo "event=private_runtime root=$RESOLVE_ARC_PRIVATE_ROOT" >> "$RESOLVE_ARC_LOG"
fi
if [[ ${RESOLVE_ARC_CL_GPU_PROFILE:-0} == 1 ]]; then
    export RESOLVE_ARC_TRACE_OPENCL=1
fi
if [[ ${RESOLVE_ARC_TRACE_OPENCL:-0} == 1 ]]; then
    if [[ ! -f $arc_dir/libopencl_trace.so ]]; then
        bash "$arc_dir/build.sh"
    fi
    export RESOLVE_ARC_CL_LOG="${RESOLVE_ARC_LOG%.log}.opencl.log"
    arc_preload="$arc_preload:$arc_dir/libopencl_trace.so"
    echo "OpenCL host timing: $RESOLVE_ARC_CL_LOG"
fi
echo 'Play the original HEVC clip with proxies, optimized media, and render cache disabled; seek several times.'
cd /opt/resolve
set +e
LD_PRELOAD="$arc_preload" /opt/resolve/bin/resolve "$@"
arc_status=$?
set -e
python3 "$arc_dir/profile.py" "$RESOLVE_ARC_LOG" || true
if [[ ${RESOLVE_ARC_TRACE_OPENCL:-0} == 1 ]]; then
    python3 "$arc_dir/opencl-profile.py" "$RESOLVE_ARC_CL_LOG" || true
fi
exit "$arc_status"
