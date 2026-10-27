#!/usr/bin/env python3
"""GPU-free forwarding/error and concurrent accounting tests for the tracer."""
import ctypes as C
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parent
if '--worker' in sys.argv:
    lib=C.CDLL(None)
    p,U,S=C.c_void_p,C.c_uint,C.c_size_t
    def call(name,types,args,result=C.c_int):
        fn=getattr(lib,name); fn.argtypes=types; fn.restype=result
        return fn(*args)
    rows={}
    rows['clFinish']=call('clFinish',[p],[None])
    rows['clWaitForEvents']=call('clWaitForEvents',[U,p],[0,None])
    for name in ('clEnqueueWriteBuffer','clEnqueueReadBuffer'):
        rows[name]=call(name,[p,p,U,S,S,p,U,p,p],[None,None,1,0,16,None,0,None,None])
    error=C.c_int(123)
    mapped=call('clEnqueueMapBuffer',[p,p,U,C.c_ulong,S,S,U,p,p,C.POINTER(C.c_int)],
                [None,None,1,1,0,16,0,None,None,C.byref(error)],p)
    rows['clEnqueueMapBuffer']=[mapped,error.value]
    rows['clEnqueueUnmapMemObject']=call('clEnqueueUnmapMemObject',[p,p,p,U,p,p],
                                        [None,None,None,0,None,None])
    rows['clEnqueueNDRangeKernel']=call('clEnqueueNDRangeKernel',[p,p,U,p,p,p,U,p,p],
                                       [None,None,1,None,None,None,0,None,None])
    region=(S*3)(16,2,1)
    for name in ('clEnqueueWriteImage','clEnqueueReadImage'):
        rows[name]=call(name,[p,p,U,p,p,S,S,p,U,p,p],
                        [None,None,1,None,region,0,0,None,0,None,None])
    for name in ('clEnqueueWriteBufferRect','clEnqueueReadBufferRect'):
        rows[name]=call(name,[p,p,U,p,p,p,S,S,S,S,p,U,p,p],
                        [None,None,1,None,None,region,0,0,0,0,None,0,None,None])
    for name in ('clEnqueueAcquireGLObjects','clEnqueueReleaseGLObjects'):
        rows[name]=call(name,[p,U,p,U,p,p],[None,0,None,0,None,None])
    fn=lib.clFinish; fn.argtypes=[p]; fn.restype=C.c_int
    with ThreadPoolExecutor(max_workers=4) as pool:
        assert all(x==rows['clFinish'] for x in pool.map(lambda _:fn(None),range(400)))
    assert rows['clFinish']!=0 and mapped is None and error.value!=123
    if '--abrupt' in sys.argv:
        import time
        time.sleep(1.05)
        fn(None)
        os._exit(0)
    print(json.dumps(rows,sort_keys=True))
else:
    with tempfile.TemporaryDirectory(prefix='arc-opencl-test-') as directory:
        env=os.environ.copy()
        env.pop('LD_LIBRARY_PATH',None)
        env.pop('RESOLVE_ARC_CL_LOG',None)
        env.pop('RESOLVE_ARC_CL_GPU_PROFILE',None)
        real=__import__('ctypes.util').util.find_library('OpenCL') or 'libOpenCL.so.1'
        env['LD_PRELOAD']=real
        command=[sys.executable,str(Path(__file__).resolve()),'--worker']
        baseline=subprocess.run(command,env=env,text=True,capture_output=True,check=True)
        env['LD_PRELOAD']=f'{root}/libopencl_trace.so:{real}'
        log=Path(directory)/'trace.log'; env['RESOLVE_ARC_CL_LOG']=str(log)
        traced=subprocess.run(command,env=env,text=True,capture_output=True,check=True)
        assert json.loads(baseline.stdout)==json.loads(traced.stdout)
        profiles={}
        for line in log.read_text().splitlines():
            if line.startswith('event=opencl_profile '):
                row=dict(item.split('=',1) for item in line.split())
                profiles[row['function']]=row
        assert len(profiles)==13
        for row in profiles.values():
            expected=401 if row['function']=='clFinish' else 1
            assert int(row['calls'])==expected and int(row['errors'])==expected
        env['RESOLVE_ARC_CL_GPU_PROFILE']='1'
        profiled=subprocess.run(command,env=env,text=True,capture_output=True,check=True)
        assert json.loads(baseline.stdout)==json.loads(profiled.stdout)
        env.pop('RESOLVE_ARC_CL_GPU_PROFILE')
        abrupt_log=Path(directory)/'abrupt.log'
        env['RESOLVE_ARC_CL_LOG']=str(abrupt_log)
        subprocess.run(command+['--abrupt'],env=env,check=True)
        snapshots=[line for line in abrupt_log.read_text().splitlines() if line.startswith('event=opencl_profile ')]
        assert any('function=clFinish calls=402 ' in line for line in snapshots)
        subprocess.run([sys.executable,str(root/'opencl-profile.py'),str(abrupt_log)],check=True,stdout=subprocess.DEVNULL)
        print('PASS: cumulative snapshots survive abrupt exit without destructors.')
        print('PASS: 13 OpenCL entrypoints preserve native error results; concurrent counters account for 401 finish calls.')
