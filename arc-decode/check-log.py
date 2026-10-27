#!/usr/bin/env python3
"""Require positive VA-API frame evidence from the actual Resolve executable."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parent
paths = sorted((root/'logs').glob('resolve-*.log'), key=lambda p: p.stat().st_mtime)
if len(sys.argv) > 1:
    path = Path(sys.argv[1])
elif paths:
    path = paths[-1]
else:
    sys.exit('No Resolve Arc trace yet. Run launch.sh from your desktop terminal.')
lines = path.read_text().splitlines()
print(f'Evidence: {path}')
required = [
    any(line.startswith('event=loaded ') and 'mode=vaapi ' in line and
        'executable=/opt/resolve/bin/resolve' in line for line in lines),
    any(line.startswith('event=backend ') and 'isolated=1' in line for line in lines),
    any(line.startswith('event=hardware_format ') for line in lines),
]
frames = [line for line in lines if line.startswith('event=frame ')]
errors = [line for line in lines if re.search(r'event=\w*failed|event=unsupported_|failed=1', line)]
if errors:
    print('\n'.join(errors))
    sys.exit('FAIL: adapter reported an error; Arc playback is not validated.')
if not all(required) or not frames:
    sys.exit('INCONCLUSIVE: missing Resolve/VA-API/decoded-frame evidence.')
if any(int(re.search(r'hardware_frames=(\d+)', line)[1]) <= 0 for line in frames):
    sys.exit('FAIL: decoded-frame evidence does not confirm hardware decoding.')
totals = [line for line in lines if line.startswith(('event=total ', 'event=exit_total '))]
if any(int(re.search(r'frames=(\d+)', line)[1]) !=
       int(re.search(r'hardware_frames=(\d+)', line)[1]) for line in totals):
    sys.exit('FAIL: hardware and delivered-frame totals differ.')
print('CONFIRMED: VA-API decoded HEVC frames were delivered inside Resolve through the adapter.')
print(f'Sampled frame records: {len(frames)}; seek/reset records: {sum(line.startswith("event=flush ") for line in lines)}')
if not any(line.startswith('event=exit_total ') for line in lines) and not totals:
    print('No final totals yet; quit Resolve normally and run this checker again.')
