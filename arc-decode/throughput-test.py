#!/usr/bin/env python3
"""Continuous decode throughput, excluding pixel hashing; verify pixels separately."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parent

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('clip',type=Path)
    parser.add_argument('--packets',type=int,default=120)
    parser.add_argument('--cycles',type=int,default=3)
    parser.add_argument('--software',action='store_true')
    parser.add_argument('--worker',action='store_true',help=argparse.SUPPRESS)
    args=parser.parse_args()
    if args.worker:
        import faulthandler
        from importlib.util import spec_from_file_location, module_from_spec
        faulthandler.enable()
        spec=spec_from_file_location('decode_test',ROOT/'decode-test.py')
        module=module_from_spec(spec)
        spec.loader.exec_module(module)
        timings=[]
        module.worker(args.clip,args.packets,args.cycles,timings)
        # Drop the first cycle as warmup, but include startup/drain in every
        # measured cycle. receive(EOF) waits for all background cleanup.
        measured=timings[1:]
        assert measured and len({row['frames'] for row in timings})==1
        elapsed=sum(row['elapsed_ns'] for row in measured)
        frames=sum(row['frames'] for row in measured)
        print(json.dumps({'cycles':timings,'measured_frames':frames,
                          'ms_per_frame':elapsed/frames/1e6,'fps':frames*1e9/elapsed}))
        return
    if args.cycles<2 or args.packets<1:
        parser.error('Use at least two cycles and one packet.')
    env=os.environ.copy()
    for name in ('LD_LIBRARY_PATH','ARC_TEST_TRANSFER'):
        env.pop(name,None)
    bundled='/opt/resolve/libs'
    env['LD_PRELOAD']=':'.join([str(ROOT/'libresolve_arc.so')]+[
        f'{bundled}/{name}' for name in ('libavformat.so.60','libavcodec.so.60','libavutil.so.58')])
    env['RESOLVE_ARC_MODE']='software-test' if args.software else 'vaapi'
    env['ARC_BENCH_NO_HASH']='1'
    env['ARC_DEFER_CONTEXT']='1'
    (ROOT/'logs').mkdir(exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix='throughput-',suffix='.log',dir=ROOT/'logs',delete=False) as f:
        log=Path(f.name)
    env['RESOLVE_ARC_LOG']=str(log)
    command=[sys.executable,str(Path(__file__).resolve()),str(args.clip),'--worker',
             '--packets',str(args.packets),'--cycles',str(args.cycles)]
    result=subprocess.run(command,env=env,text=True,capture_output=True,timeout=240)
    if result.stderr:
        log.with_suffix('.stderr.txt').write_text(result.stderr)
    if result.returncode:
        sys.exit(f'Throughput worker failed ({result.returncode}); evidence: {log}\n{result.stderr}')
    report=json.loads(result.stdout)
    report.update(evidence=str(log),pixel_verification=False)
    print(json.dumps(report))

if __name__=='__main__':
    main()
