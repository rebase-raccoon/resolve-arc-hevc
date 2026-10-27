#!/usr/bin/env python3
"""Measure conversion only; these numbers do not predict Resolve playback FPS."""
import ctypes as C
from datetime import datetime
import json
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent
if '--worker' in sys.argv:
    lib = C.CDLL(None)
    run = lib.arc_repack_benchmark
    run.argtypes = [C.c_char_p, C.c_int, C.c_int, C.c_int, C.c_int, C.POINTER(C.c_ulong)]
    run.restype = C.c_int
    results = []
    for fmt in ['nv12', 'p010le', 'y210le', 'p210le']:
        row = {'format': fmt}
        for fast, name in [(0, 'swscale'), (1, 'simd')]:
            stats = (C.c_ulong*3)()
            assert run(fmt.encode(), 3840, 2160, 30, fast, stats) == 0
            row[name] = {'total_ms': stats[0]/30e6, 'allocate_ms': stats[1]/30e6, 'repack_ms': stats[2]/30e6}
        results.append(row)
    print(json.dumps(results))
else:
    env = os.environ.copy()
    env.pop('LD_LIBRARY_PATH', None)
    env.pop('RESOLVE_ARC_MODE', None)
    env.pop('RESOLVE_ARC_LOG', None)
    env['LD_PRELOAD'] = f'{root}/libtransfer_test.so:/opt/resolve/libs/libavcodec.so.60:/opt/resolve/libs/libavutil.so.58'
    all_rows = []
    for workers in (1,2,4):
        env['RESOLVE_ARC_WORKERS'] = str(workers)
        result = subprocess.run([sys.executable, str(Path(__file__).resolve()), '--worker'], env=env,
                                capture_output=True, text=True, check=True)
        rows = json.loads(result.stdout)
        for row in rows:
            row['workers'] = workers
            old, new = row['swscale']['repack_ms'], row['simd']['repack_ms']
            print(f'{row["format"]:8s}, {workers} workers: swscale {old:.2f} ms/frame; SIMD {new:.2f} ms/frame')
        all_rows.extend(rows)
    path = root/'logs'/f'repack-benchmark-{datetime.now():%Y%m%d-%H%M%S}.json'
    path.parent.mkdir(exist_ok=True)
    path.write_text(json.dumps(all_rows, indent=2)+'\n')
    print(f'3840x2160 cached CPU conversion benchmark only. Evidence: {path}')
