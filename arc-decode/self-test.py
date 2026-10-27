#!/usr/bin/env python3
"""Reproducible software-side regression tests; does not claim GPU success."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parent
subprocess.run(['bash', str(root/'build.sh')], check=True)
subprocess.run([sys.executable, str(root/'loader-test.py')], check=True)
subprocess.run([sys.executable, str(root/'readback-test.py')], check=True)
subprocess.run([sys.executable, str(root/'opencl-trace-test.py')], check=True)
subprocess.run([sys.executable, str(root/'opencl-events-test.py')], check=True)
with tempfile.TemporaryDirectory(prefix='arc-adapter-test-') as directory:
    for depth, chroma in [(8,420), (10,420), (8,422), (10,422)]:
        clip = Path(directory)/f'test-{chroma}-{depth}.mkv'
        settings = 'log-level=error:pools=none:frame-threads=1'
        if depth == 10:
            settings += ':master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1):max-cll=1000,400'
        subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=98x66:rate=6',
                        '-frames:v', '12', '-c:v', 'libx265', '-x265-params', settings,
                        '-pix_fmt', f'yuv{chroma}p10le' if depth == 10 else f'yuv{chroma}p', str(clip)], check=True)
        for mode in [[], ['--planes']]:
            subprocess.run([sys.executable, str(root/'decode-test.py'), str(clip),
                            '--allow-encoder-sei-differences', *mode], check=True)
        env = os.environ.copy()
        env['ARC_TEST_UC_COPY'] = '1'
        subprocess.run([sys.executable,str(root/'decode-test.py'),str(clip),'--planes',
                        '--allow-encoder-sei-differences'],env=env,check=True)
        env.pop('ARC_TEST_UC_COPY')
        env['ARC_TEST_STREAM_READ']='1'
        subprocess.run([sys.executable,str(root/'decode-test.py'),str(clip),'--planes',
                        '--allow-encoder-sei-differences'],env=env,check=True)
# Exercise tiny row groups/vector tails, streamed reads, P210, and repeated
# pool destruction with both worker counts before the temporary clips vanish.
    for workers in (2,4):
        for clip in Path(directory).glob('*.mkv'):
            env=os.environ.copy()
            env['RESOLVE_ARC_WORKERS']=str(workers)
            env['ARC_TEST_STREAM_READ']='1'
            subprocess.run([sys.executable,str(root/'decode-test.py'),str(clip),'--planes',
                            '--allow-encoder-sei-differences','--threads','2'],env=env,check=True)
            if clip.name=='test-422-10.mkv':
                env['ARC_TEST_PACKED']='p210le'
                subprocess.run([sys.executable,str(root/'decode-test.py'),str(clip),'--planes',
                                '--allow-encoder-sei-differences'],env=env,check=True)
for workers in (2,4):
    env=os.environ.copy()
    env['RESOLVE_ARC_WORKERS']=str(workers)
    env['RESOLVE_ARC_PREFETCH']='4'
    subprocess.run([sys.executable,str(root/'decode-test.py'),'--planes','--packets','8',
                    '--threads','2'],env=env,check=True)
for mode in [[], ['--planes']]:
    subprocess.run([sys.executable, str(root/'decode-test.py'), *mode], check=True)
subprocess.run([sys.executable, str(root/'decode-test.py'), '--packets', '8', '--defer-context'], check=True)
subprocess.run([sys.executable, str(root/'decode-test.py'), '--packets', '8', '--threads','2'], check=True)
env=os.environ.copy()
env['RESOLVE_ARC_PREFETCH']='4'
subprocess.run([sys.executable,str(root/'decode-test.py'),'--packets','12','--threads','2'],env=env,check=True)
env = os.environ.copy()
env['RESOLVE_ARC_DEVICE'] = '/dev/null'
negative = subprocess.run([sys.executable, str(root/'decode-test.py'), '--hardware', '--packets', '1'],
                          env=env, capture_output=True, text=True)
assert negative.returncode != 0
assert 'event=device_failed ' in negative.stdout and 'software_fallback=0' in negative.stdout
assert 'event=frame ' not in negative.stdout
print('PASS: invalid hardware device returns an error without software fallback.')
print('PASS: 4:2:0/4:2:2, 8-bit/10-bit, HDR10 side data, 4K HLG, seek/reset, packed pixel conversion, decoder context synchronization.')
print('Remaining performance gate: run hardware-bench.py from the desktop with Resolve closed.')
