#!/usr/bin/env bash
# Build the private FFmpeg 8.0.1 the adapter's decode backend runs on.
#
# Output: ./ffmpeg/lib/{libavcodec.so.62,libavutil.so.60,libswscale.so.9}, each
# with RUNPATH $ORIGIN, so they are found next to each other and never mix with
# the distribution's FFmpeg. Only the HEVC decoder, its parser and VAAPI are
# enabled. Needs: cc, make, xz; the system libva.so.2 + libva-drm.so.2 at run
# time. No -dev packages, pkg-config or nasm are required.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
arc=$PWD
third=$(cd .. && pwd)/third_party
src=$third/ffmpeg-8.0.1.tar.xz
sha=05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41
[[ -f $src ]] || { echo "Missing $src" >&2; exit 1; }
echo "$sha  $src" | sha256sum -c --quiet - || { echo "FFmpeg source checksum mismatch" >&2; exit 1; }

if [[ -f $arc/ffmpeg/.built-8.0.1 ]]; then echo "Private FFmpeg 8.0.1 already built."; exit 0; fi

find_lib() { # print the absolute path of a shared library the dynamic loader would use
    local p
    p=$(ldconfig -p 2>/dev/null | awk -v n="$1" '$1==n && /x86-64/ {print $NF; exit}')
    [[ -n $p ]] || for d in /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib /usr/local/lib; do [[ -e $d/$1 ]] && { p=$d/$1; break; }; done
    [[ -n $p ]] || { echo "Cannot find $1 (install libva)" >&2; exit 1; }
    readlink -f "$p"
}

work=$(mktemp -d "$arc/.ffmpeg-build-XXXXXX")
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/link" "$work/bin"
ln -s "$(find_lib libva.so.2)" "$work/link/libva.so"
ln -s "$(find_lib libva-drm.so.2)" "$work/link/libva-drm.so"

# Minimal pkg-config answering only for libva and libva-drm.
cat > "$work/bin/pkg-config" <<EOF
#!/bin/sh
inc="-I$third/libva-2.23.0"; libs="-L$work/link"
mods=""; want=""
case " \$* " in *" --version "*) echo 1.8.1; exit 0 ;; esac
set -- \$*
for a in "\$@"; do case \$a in
  --exists|--cflags|--libs|--modversion) want="\$want \$a" ;;
  --*) ;;
  libva|libva-drm) mods="\$mods \$a" ;;
  '>='*|'<='*|'='*|[0-9]*) ;;
  *) exit 1 ;;
esac; done
[ -n "\$mods" ] || exit 1
out=""
for w in \$want; do case \$w in
  --cflags) out="\$out \$inc" ;;
  --libs) for m in \$mods; do case \$m in libva) out="\$out \$libs -lva" ;; libva-drm) out="\$out \$libs -lva-drm -lva" ;; esac; done ;;
  --modversion) out="\$out 1.23.0" ;;
esac; done
[ -n "\$out" ] && echo \$out
exit 0
EOF
chmod +x "$work/bin/pkg-config"

tar -xJf "$src" -C "$work"
cd "$work/ffmpeg-8.0.1"
./configure --prefix="$arc/ffmpeg" --pkg-config="$work/bin/pkg-config" \
    --enable-shared --disable-static --disable-programs --disable-doc --disable-autodetect \
    --disable-everything --disable-avformat --disable-avdevice --disable-avfilter --disable-swresample \
    --disable-network --disable-x86asm \
    --enable-vaapi --enable-decoder=hevc --enable-parser=hevc --enable-hwaccel=hevc_vaapi --enable-swscale \
    --extra-ldflags='-Wl,-rpath,\$$ORIGIN -Wl,--enable-new-dtags' > "$work/configure.log" 2>&1 \
    || { tail -30 "$work/configure.log" >&2; exit 1; }
grep -q '^CONFIG_VAAPI=yes' ffbuild/config.mak && grep -q '^CONFIG_HEVC_VAAPI_HWACCEL=yes' ffbuild/config.mak \
    || { echo "VAAPI was not enabled; see configure.log" >&2; tail -30 "$work/configure.log" >&2; exit 1; }
make -j"$(nproc)" > "$work/make.log" 2>&1 || { tail -30 "$work/make.log" >&2; exit 1; }
make install > /dev/null
rm -rf -- "$arc/ffmpeg/include" "$arc/ffmpeg/share" "$arc/ffmpeg/lib/pkgconfig"
touch "$arc/ffmpeg/.built-8.0.1"
echo "Built private FFmpeg 8.0.1 in $arc/ffmpeg/lib"
