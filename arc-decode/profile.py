#!/usr/bin/env python3
"""Summarize measured adapter stages from a completed or ongoing Resolve run."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parent
paths = sorted((p for p in (root/'logs').glob('resolve-*.log') if not p.name.endswith('.opencl.log')), key=lambda p: p.stat().st_mtime)
if len(sys.argv)>1:
    path = Path(sys.argv[1])
elif paths:
    path = paths[-1]
else:
    sys.exit('No Resolve trace yet.')
latest = {}
for line in path.read_text().splitlines():
    if line.startswith('event=profile '):
        fields = dict(item.split('=', 1) for item in line.split())
        latest[fields['ctx']] = fields
print(f'Evidence: {path}')
if not latest:
    sys.exit('This run predates stage timing. The updated adapter records timings during playback.')
frames = sum(int(row['frames']) for row in latest.values())
print(f'{len(latest)} decoder contexts; {frames} delivered frames in latest timing snapshots.')
main = max(latest.values(), key=lambda row: int(row['frames']))
if 'format' in main:
    print(f'Most active context: {main["width"]}x{main["height"]} {main["format"]}; {main["frames"]} frames.')
for label, names in [('Full decoder API time', ['api_ns']), ('Decode/submission', ['send_ns', 'receive_ns']), ('GPU download', ['download_ns']),
                     ('Output allocation', ['allocate_ns']), ('Pixel repacking', ['repack_ns']),
                     ('Packet copying', ['packet_copy_ns']), ('Foreground cleanup/submission/wait', ['cleanup_ns']),
                     ('Same-decoder lock wait', ['wait_ns'])]:
    ns = sum(int(row.get(name,0)) for row in latest.values() for name in names)
    print(f'{label}: {ns/frames/1e6:.3f} ms/delivered frame')
if any('receive_calls' in row for row in latest.values()):
    print('Decoder calls: ' + ', '.join(f'{name}={sum(int(row.get(name,0)) for row in latest.values())}' for name in ('send_calls','receive_calls','receive_again')))
if any('direct_frames' in row for row in latest.values()):
    for mode,key in [('transfer','transferred_frames'),('map','mapped_frames'),('direct (including fused)','direct_frames'),('fused subset','fused_frames'),('staged conversion subset','staged_frames')]:
        count=sum(int(row.get(key,0)) for row in latest.values())
        if count:
            print(f'Readback route {mode}: {count} frames')
    depths=sorted({int(row.get('prefetch',0)) for row in latest.values()})
    print(f'Prefetch depths: {depths}')
    workers=sorted({int(row.get('workers',1)) for row in latest.values()})
    print(f'Conversion worker counts: {workers}')
    modes=sorted({int(row.get('async_cleanup',0)) for row in latest.values()})
    print(f'Background cleanup enabled: {modes}')
    background=sum(int(row.get('background_cleanup_ns',0)) for row in latest.values())
    if background:
        print(f'Background cleanup (overlaps foreground work): {background/frames/1e6:.3f} ms/frame')
    for label,key in [('Map/synchronization (within readback)','map_ns'),('Cache-aware copy (within readback)','uncached_copy_ns')]:
        ns=sum(int(row.get(key,0)) for row in latest.values())
        if ns:
            print(f'{label}: {ns/frames/1e6:.3f} ms/delivered frame')
print('These stage averages exclude Resolve effects/display, storage reads, and some adapter bookkeeping; concurrent decoder times can overlap.')
