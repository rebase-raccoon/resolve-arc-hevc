#!/usr/bin/env python3
"""Exercise readback selection with mocked mapping, without a GPU."""
import ctypes
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent
if '--worker' in sys.argv:
    for name in ('arc_readback_route_test','arc_cleanup_lifetime_test'):
        run = getattr(ctypes.CDLL(None),name)
        run.argtypes = []
        run.restype = ctypes.c_int
        result = run()
        assert result == 0, f'{name} failed: {result}'
    print('PASS: 300 buffer releases across background handoff, bounded reuse, waits and destruction without EOF.')
    print('PASS: direct/staged map flags, fused conversion selection, route counters, and transfer fallback.')
else:
    env = os.environ.copy()
    for name in ('LD_LIBRARY_PATH', 'RESOLVE_ARC_MODE', 'RESOLVE_ARC_LOG'):
        env.pop(name, None)
    env['LD_PRELOAD'] = f'{root}/libtransfer_test.so:/opt/resolve/libs/libavcodec.so.60:/opt/resolve/libs/libavutil.so.58'
    subprocess.run([sys.executable, str(Path(__file__).resolve()), '--worker'], env=env, check=True)
