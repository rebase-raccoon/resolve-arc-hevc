#!/usr/bin/env bash
# resolve-fps.sh: start playback in Resolve first, then run this. Counts distinct displayed frames.
# Adjust GRAB_X/GRAB_Y so the 640x360 patch sits inside the viewer.
win=$(xwininfo -root -tree | grep -E '"DaVinci Resolve Studio - ' | awk '{print $1}' | head -1)
[[ -n $win ]] || { echo "No Resolve window found." >&2; exit 1; }
out=$(mktemp --suffix=.mkv)
ffmpeg -hide_banner -loglevel error -y -f x11grab -framerate 120 -draw_mouse 0 -window_id "$win" \
  -grab_x "${GRAB_X:-1100}" -grab_y "${GRAB_Y:-450}" -video_size 640x360 -i :0 -t 6 -c:v ffv1 "$out"
n=$(ffmpeg -hide_banner -i "$out" -vf 'mpdecimate=hi=1:lo=1:frac=0:max=0' -f null - 2>&1 | grep -oE 'frame= *[0-9]+' | tail -1 | grep -oE '[0-9]+')
rm -f -- "$out"
echo "$n distinct frames in 6 s = $((n / 6)) fps"
