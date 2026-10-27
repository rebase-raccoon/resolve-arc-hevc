#!/usr/bin/env python3
"""Compare verified GPU readback routes without starting Resolve."""
import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent


def snapshots(path):
    latest = {}
    for line in path.read_text().splitlines():
        if line.startswith('event=profile '):
            row = dict(field.split('=',1) for field in line.split())
            latest[row['ctx']] = row
    return list(latest.values())


def valid_evidence(stats,depth,workers,cleanup):
    if not stats or sum(int(s['frames']) for s in stats)<=0:
        return False
    return all(int(s.get('prefetch',0))==depth and int(s.get('workers',1))==workers and
               int(s.get('async_cleanup',0))==cleanup and
               int(s.get('direct_frames',0))==int(s['frames']) and
               int(s.get('fused_frames',0))==int(s['frames']) and
               (not cleanup or int(s.get('cleanup_jobs',0))==int(s['frames']))
               for s in stats)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('clip', nargs='?', type=Path)
    parser.add_argument('--packets', type=int, default=24)
    parser.add_argument('--throughput-packets',type=int,default=120)
    args = parser.parse_args()
    clip = args.clip
    if clip is None:
        clip = Path(os.environ.get('RESOLVE_ARC_TEST_CLIP') or (ROOT/'preflight-clip.txt').read_text().strip())
    if not clip.is_file():
        sys.exit(f'Preflight clip is missing: {clip}')
    device = Path(os.environ.get('RESOLVE_ARC_DEVICE','/dev/dri/renderD128'))
    if not device.is_char_device() or not os.access(device,os.R_OK|os.W_OK):
        sys.exit(f'Cannot access {device}. Run this benchmark from your Linux desktop terminal.')
    if subprocess.run(['pgrep','-x','resolve'],capture_output=True).returncode == 0:
        sys.exit('Quit Resolve before benchmarking to avoid competing for the GPU.')
    print(f'Clip: {clip}',flush=True)
    print('Testing synchronous/background cleanup with four workers and queue depths 4 and 8. Resolve will stay closed.',flush=True)
    rows = []
    keys = ['send_ns','receive_ns','download_ns','allocate_ns','repack_ns','packet_copy_ns','cleanup_ns']
    for mode, depth, workers, cleanup in [('fused',4,4,0), ('fused',4,4,1),
                                         ('fused',8,4,0), ('fused',8,4,1)]:
        env = os.environ.copy()
        env.pop('LD_PRELOAD',None)
        env.pop('LD_LIBRARY_PATH',None)
        env['RESOLVE_ARC_DOWNLOAD'] = mode
        env['RESOLVE_ARC_PREFETCH'] = str(depth)
        env['RESOLVE_ARC_REPACK'] = 'simd'
        env['RESOLVE_ARC_WORKERS'] = str(workers)
        env['RESOLVE_ARC_ASYNC_CLEANUP'] = str(cleanup)
        label=f'{mode} (queue {depth}, workers {workers}, background cleanup {cleanup})'
        print(f'\nChecking {label}...',flush=True)
        result = subprocess.run([sys.executable,str(ROOT/'decode-test.py'),str(clip),'--hardware',
                                 '--defer-context','--packets',str(args.packets)],
                                env=env,text=True,capture_output=True)
        row = {'mode':mode,'prefetch':depth,'workers':workers,'async_cleanup':cleanup,'verified':False,'stdout':result.stdout,'stderr':result.stderr}
        rows.append(row)
        evidence = next((line[len('Adapter evidence: '):] for line in result.stdout.splitlines()
                         if line.startswith('Adapter evidence: ')),None)
        if result.returncode or not evidence:
            print(f'{label}: failed; preserving diagnostics in the benchmark report.',flush=True)
            continue
        row['evidence'] = evidence
        stats = snapshots(Path(evidence))
        frames = sum(int(s['frames']) for s in stats)
        if not valid_evidence(stats,depth,workers,cleanup):
            print(f'{label}: requested route unavailable or cleanup incomplete; excluded.',flush=True)
            continue
        row.update(frames=frames)
        row['ms_per_frame'] = {key:sum(int(s.get(key,0)) for s in stats)/frames/1e6 for key in keys}
        row['adapter_ms'] = sum(row['ms_per_frame'].values())
        print(f'{label}: full pixels and metadata match. Measuring continuous decode without pixel hashing...',flush=True)
        timed=subprocess.run([sys.executable,str(ROOT/'throughput-test.py'),str(clip),
                              '--packets',str(args.throughput_packets)],env=env,text=True,capture_output=True)
        row['throughput_stdout']=timed.stdout
        row['throughput_stderr']=timed.stderr
        if timed.returncode:
            print(f'{label}: throughput test failed; preserving diagnostics.',flush=True)
            continue
        perf=json.loads(timed.stdout)
        perf_stats=snapshots(Path(perf['evidence']))
        if (not valid_evidence(perf_stats,depth,workers,cleanup) or
            sum(int(s['frames']) for s in perf_stats)!=sum(c['frames'] for c in perf['cycles'])):
            print(f'{label}: throughput route evidence incomplete; excluded.',flush=True)
            continue
        row.update(verified=True,throughput=perf,throughput_ms=perf['ms_per_frame'])
        print(f'{label}: continuous decode {perf["ms_per_frame"]:.2f} ms/frame ({perf["fps"]:.1f} fps).',flush=True)
    valid = [row for row in rows if row['verified']]
    baseline = next((row for row in valid if row['mode']=='fused' and row['prefetch']==4 and not row['async_cleanup']),None)
    selected = None
    if baseline:
        best = min(valid,key=lambda row:row['throughput_ms'])
        selected = best if best['throughput_ms']<baseline['throughput_ms']*0.95 else baseline
        config = ROOT/'download-mode.txt'
        temp = config.with_suffix('.tmp')
        temp.write_text(f'{selected["mode"]} {selected["prefetch"]} {selected["workers"]} {selected["async_cleanup"]}\n')
        temp.replace(config)
        print(f'\nSelected {selected["mode"]}, queue depth {selected["prefetch"]}, workers {selected["workers"]}, background cleanup {selected["async_cleanup"]}; the launcher will use this verified route.',flush=True)
        if selected is baseline:
            print('No alternative improved continuous decode time by at least 5%.',flush=True)
    else:
        print('\nThe fused baseline failed; configuration was left unchanged.',flush=True)
    report = {'clip':str(clip),'device':str(device),'selected':selected['mode'] if selected else None,
              'prefetch':selected['prefetch'] if selected else None,
              'workers':selected['workers'] if selected else None,
              'async_cleanup':selected['async_cleanup'] if selected else None,
              'measurement':'continuous decode including demux and final cleanup, without pixel hashing',
              'routes':rows}
    path = ROOT/'logs'/f'hardware-benchmark-{datetime.now():%Y%m%d-%H%M%S}.json'
    path.write_text(json.dumps(report,indent=2)+'\n')
    print(f'Report: {path}')
    print('Selection uses continuous decode throughput with cleanup fully drained; Resolve effects/display are excluded.')
    if selected is None:
        sys.exit(1)


if __name__ == '__main__':
    main()
