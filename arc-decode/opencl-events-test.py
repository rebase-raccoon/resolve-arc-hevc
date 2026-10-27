#!/usr/bin/env python3
"""Test device-event profiling ownership and queue properties using a mock ICD."""
import ctypes as C
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parent
if '--worker' in sys.argv:
    lib=C.CDLL(None); p=C.c_void_p; L=C.c_long; U=C.c_uint; S=C.c_size_t
    def fn(name,args,ret=C.c_int):
        f=getattr(lib,name); f.argtypes=args; f.restype=ret; return f
    state=fn('arc_mock_state',[C.c_int]*3)
    counts=fn('arc_mock_counts',[C.POINTER(C.c_int)])
    flags=fn('arc_mock_flags',[],C.c_ulong)
    other=fn('arc_mock_other',[],L)
    create=fn('clCreateCommandQueue',[p,p,C.c_ulong,C.POINTER(C.c_int)],p)
    props=fn('clCreateCommandQueueWithProperties',[p,p,C.POINTER(L),C.POINTER(C.c_int)],p)
    enqueue=fn('clEnqueueNDRangeKernel',[p,p,U,p,p,p,U,p,C.POINTER(p)])
    release=fn('clReleaseEvent',[p]); finish=fn('clFinish',[p])
    err=C.c_int(123)
    assert create(1,1,0,C.byref(err))==1 and err.value==0 and flags()==2
    original=(L*5)(0xdead,17,0x1093,1,0)
    assert props(1,1,original,C.byref(err))==1 and flags()==3 and other()==17
    assert list(original)==[0xdead,17,0x1093,1,0]
    assert props(1,1,None,C.byref(err))==1 and flags()==2
    state(2,1,0)
    assert create(1,1,0,C.byref(err))==1 and flags()==0 and err.value==0
    state(2,0,0)
    out=(C.c_int*4)()
    event=p()
    assert enqueue(1,1,1,None,None,None,0,None,C.byref(event))==0
    counts(out); assert out[0]==1 and out[1]==1 and out[2]==0 and out[3]==0
    release(event); counts(out); assert out[0]==1
    state(0,0,0); finish(1); counts(out); assert out[0]==0
    state(2,0,0)
    for _ in range(520): assert enqueue(1,1,1,None,None,None,0,None,None)==0
    counts(out); assert out[0]==512 and out[3]==1, list(out)
    state(0,0,0); finish(1); counts(out); assert out[0]==0
    state(0,0,1); enqueue(1,1,1,None,None,None,0,None,None)
    state(-5,0,0); enqueue(1,1,1,None,None,None,0,None,None)
    counts(out); assert out[0]==0 and out[3]==2
    state(0,0,0)
    fn('arc_mock_reenter',[])()
    enqueue(1,1,1,None,None,None,0,None,None)
    counts(out); assert out[0]==0 and out[3]==3
    print(json.dumps({'released':out[2],'extra_finish_calls':out[3]-3}))
else:
    with tempfile.TemporaryDirectory(prefix='arc-gpu-events-') as directory:
        env=os.environ.copy(); env.pop('LD_LIBRARY_PATH',None)
        env['LD_PRELOAD']=f'{root}/libopencl_trace.so:{root}/libopencl_mock.so'
        env['RESOLVE_ARC_CL_GPU_PROFILE']='1'
        log=Path(directory)/'gpu.log'; env['RESOLVE_ARC_CL_LOG']=str(log)
        r=subprocess.run([sys.executable,str(Path(__file__).resolve()),'--worker'],env=env,text=True,capture_output=True,timeout=15)
        assert r.returncode==0,(r.stdout,r.stderr,log.read_text())
        metrics={}; statuses=[]
        for line in log.read_text().splitlines():
            if line.startswith('event=opencl_gpu_profile '):
                row=dict(x.split('=',1) for x in line.split()); metrics[row['name']]=row
            if line.startswith('event=opencl_gpu_status '):
                statuses.append(dict(x.split('=',1) for x in line.split()))
        m=metrics['test_kernel']; s=statuses[-1]
        assert int(m['count'])==514 and int(m['execute_ns'])==514000
        assert int(m['queued_to_start_ns'])==514*200
        assert {k:int(s[k]) for k in ('pending','dropped','unavailable','failed')}==dict(pending=0,dropped=8,unavailable=1,failed=1)
        print('PASS: profiled queue properties, fallback, event ownership, bounded pending events, timestamps, errors and callback reentrancy; no added finish calls.')
