#!/usr/bin/env python3
"""Summarize optional OpenCL host-call timings from a completed Resolve run."""
from pathlib import Path
import sys

root=Path(__file__).resolve().parent
paths=list((root/'logs').glob('resolve-*.opencl.log'))
if len(sys.argv)>1:
    path=Path(sys.argv[1])
elif paths:
    path=max(paths,key=lambda p:p.stat().st_mtime)
else:
    sys.exit('No OpenCL timing trace yet.')
latest={}
gpu={}
status=None
queues=[]
devices=set()
for line in path.read_text().splitlines():
    if line.startswith('event=opencl_profile '):
        row=dict(field.split('=',1) for field in line.split())
        latest[row['function']]=row
    elif line.startswith('event=opencl_gpu_profile '):
        row=dict(field.split('=',1) for field in line.split()); gpu[row['name']]=row
    elif line.startswith('event=opencl_gpu_status '):
        status=dict(field.split('=',1) for field in line.split())
    elif line.startswith('event=opencl_device '):
        row=dict(field.split('=',1) for field in line.split()); devices.add((row['name'],row['driver']))
    elif line.startswith('event=opencl_queue '):
        queues.append(dict(field.split('=',1) for field in line.split()))
print(f'OpenCL host timing: {path}')
if not latest:
    samples=sum(line.startswith('event=opencl_call ') for line in path.read_text().splitlines())
    if samples:
        sys.exit(f'{samples} call samples exist, but no cumulative snapshot was written; this trace cannot establish total wait time.')
    sys.exit('No intercepted calls recorded. Explicit symbol lookups can bypass this tracer; absence is not proof of inactivity.')
for row in sorted(latest.values(),key=lambda r:int(r['total_ns']),reverse=True):
    calls=int(row['calls'])
    print(f'{row["function"]}: {calls} calls; {int(row["total_ns"])/1e9:.3f} host seconds; '
          f'{int(row["total_ns"])/calls/1e6:.3f} ms/call; max {int(row["max_ns"])/1e6:.3f} ms; '
          f'{int(row["bytes"])/1e6:.1f} MB counted; {row["errors"]} errors')
print('Latest cumulative snapshots are saved during playback and at normal shutdown; up to one second of the tail may be absent.')
print('Host-call times can overlap across threads and exclude asynchronous GPU execution; image byte volumes are not counted.')

for name,driver in sorted(devices):
    print(f'Resolve queue device: {name}; driver {driver}')
if queues:
    print(f'Profiled queues: {sum(row["profiling"]=="1" for row in queues)}/{len(queues)} intercepted queue creations.')
if gpu:
    print('Device event timestamps, highest accumulated execution time first:')
    for row in sorted(gpu.values(),key=lambda row:int(row['execute_ns']),reverse=True)[:25]:
        n=int(row['count'])
        print(f'{row["name"]}: {n} completed events; GPU execution {int(row["execute_ns"])/1e6:.2f} ms total, '
              f'{int(row["execute_ns"])/n/1e6:.3f} ms/event, max {int(row["max_execute_ns"])/1e6:.3f} ms; '
              f'queued-to-start {int(row["queued_to_start_ns"])/n/1e6:.3f} ms/event; '
              f'submitted-to-start {int(row["submit_to_start_ns"])/n/1e6:.3f} ms/event')
    print('Device event intervals can overlap across queues; sums are not per-frame latency. Profiling may perturb performance.')
if status:
    print('Event coverage: '+', '.join(f'{key}={status[key]}' for key in ('pending','dropped','unavailable','failed')))
    if not gpu:
        print('No completed device timings available; this run cannot distinguish GPU execution from host synchronization.')
