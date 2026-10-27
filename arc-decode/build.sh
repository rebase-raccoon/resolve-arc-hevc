#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
build_dir=$(mktemp -d "$PWD/.build-XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
for source in bridge transfer-test namespace-root namespace-probe opencl-trace opencl-mock; do
    if command -v cc >/dev/null; then
        cc -O2 -fPIC -fvisibility=hidden -Wall -Wextra -Werror -c "$source.c" -o "$source.o"
    else
        python3 compile-local.py "$source.c" -o "$source.o" -fvisibility=hidden -Wall -Wextra -Werror
    fi
done
# Decode backend: the private FFmpeg 8.0.1 from build-ffmpeg.sh when present,
# otherwise the system copy (it must be libavcodec 62.11.100 / libavutil 60.8.100).
if [[ -f ffmpeg/lib/libavcodec.so.62 ]]; then
    ff_libs="ffmpeg/lib/libavcodec.so.62 ffmpeg/lib/libswscale.so.9"
    ff_rpath=(-rpath '$ORIGIN/ffmpeg/lib' --enable-new-dtags)
    echo 'Linking the decode backend against the private FFmpeg in ffmpeg/lib.'
else
    sys_lib() { ldconfig -p | awk -v n="$1" '$1==n && /x86-64/ {print $NF; exit}'; }
    ff_libs="$(sys_lib libavcodec.so.62) $(sys_lib libswscale.so.9)"
    [[ $ff_libs == *libavcodec.so.62*libswscale.so.9* ]] || { echo 'No libavcodec.so.62/libswscale.so.9: install FFmpeg 8.0.x or run build-ffmpeg.sh.' >&2; exit 1; }
    ff_rpath=()
    echo "Linking the decode backend against the system FFmpeg: $ff_libs"
fi
# shellcheck disable=SC2086
ld -shared -z noexecstack -z relro -z now "${ff_rpath[@]}" -o "$build_dir/libarc_namespace.so" namespace-root.o \
    --no-as-needed $ff_libs -lc -ldl
ld -shared -z noexecstack -z relro -z now -o "$build_dir/libnamespace_probe.so" namespace-probe.o -lc -ldl
cc -O2 -fPIC -fvisibility=hidden -Wall -Wextra -Werror -I../third_party/libva-2.23.0 -c vpp.c -o vpp.o
ld -shared -z noexecstack -z relro -z now -o "$build_dir/libresolve_arc.so" bridge.o vpp.o -lc -ldl -lpthread
ld -shared -z noexecstack -z relro -z now -o "$build_dir/libtransfer_test.so" transfer-test.o vpp.o -lc -ldl -lpthread
ld -shared -z noexecstack -z relro -z now -o "$build_dir/libopencl_trace.so" opencl-trace.o -lc -ldl -lpthread
ld -shared -z noexecstack -z relro -z now -o "$build_dir/libopencl_mock.so" opencl-mock.o -lc
# Replace files by rename so a running Resolve keeps its existing mappings.
for library in libarc_namespace.so libnamespace_probe.so libresolve_arc.so libtransfer_test.so libopencl_trace.so libopencl_mock.so; do
    mv -f -- "$build_dir/$library" "$library"
done
echo 'Built libresolve_arc.so and the separate transfer test library.'
