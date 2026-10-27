#!/usr/bin/env python3
"""Compile and execute a tiny OpenCL kernel; verify private library provenance."""
import argparse
import ctypes as C
import json
from pathlib import Path


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--expected-root',type=Path,required=True)
    args=parser.parse_args(); root=args.expected_root.resolve()
    lib=C.CDLL('libOpenCL.so.1'); p=C.c_void_p; U=C.c_uint; S=C.c_size_t
    def api(name,types,result=C.c_int):
        f=getattr(lib,name); f.argtypes=types; f.restype=result; return f
    platforms=api('clGetPlatformIDs',[U,C.POINTER(p),C.POINTER(U)])
    devices=api('clGetDeviceIDs',[p,C.c_ulong,U,C.POINTER(p),C.POINTER(U)])
    info=api('clGetDeviceInfo',[p,U,S,p,C.POINTER(S)])
    def text(device,key):
        n=S(); assert info(device,key,0,None,C.byref(n))==0
        out=C.create_string_buffer(n.value); assert info(device,key,n,out,None)==0
        return out.value.decode()
    count=U(); assert platforms(0,None,C.byref(count))==0 and count.value>0,'No OpenCL platform'
    ids=(p*count.value)(); assert platforms(count,ids,None)==0
    found=[]
    for platform in ids:
        n=U()
        if devices(platform,4,0,None,C.byref(n))!=0: continue
        ds=(p*n.value)(); assert devices(platform,4,n,ds,None)==0
        for d in ds: found.append(d)
    assert len(found)==1,f'Expected one private GPU; found {len(found)}'
    device=p(found[0]); driver=text(device,0x102D); name=text(device,0x102B)
    assert driver.startswith('26.35.'),f'Unexpected driver: {driver}'
    context=queue=program=kernel=buffer=None
    err=C.c_int()
    try:
        context=api('clCreateContext',[p,U,C.POINTER(p),p,p,C.POINTER(C.c_int)],p)(None,1,C.byref(device),None,None,C.byref(err))
        assert context and err.value==0
        queue=api('clCreateCommandQueue',[p,p,C.c_ulong,C.POINTER(C.c_int)],p)(context,device,0,C.byref(err))
        assert queue and err.value==0
        source=C.c_char_p(b'__kernel void arc_private_probe(__global uint *out){uint i=get_global_id(0);out[i]=(i*17U)^0x12345U;}')
        program=api('clCreateProgramWithSource',[p,U,C.POINTER(C.c_char_p),p,C.POINTER(C.c_int)],p)(context,1,C.byref(source),None,C.byref(err))
        assert program and err.value==0
        result=api('clBuildProgram',[p,U,C.POINTER(p),C.c_char_p,p,p])(program,1,C.byref(device),None,None,None)
        if result:
            getlog=api('clGetProgramBuildInfo',[p,p,U,S,p,C.POINTER(S)])
            size=S(); getlog(program,device,0x1183,0,None,C.byref(size))
            out=C.create_string_buffer(size.value or 1); getlog(program,device,0x1183,len(out),out,None)
            raise RuntimeError(f'Kernel build failed ({result}): {out.value.decode(errors="replace")}')
        kernel=api('clCreateKernel',[p,C.c_char_p,C.POINTER(C.c_int)],p)(program,b'arc_private_probe',C.byref(err))
        assert kernel and err.value==0
        buffer=api('clCreateBuffer',[p,C.c_ulong,S,p,C.POINTER(C.c_int)],p)(context,1,256,None,C.byref(err))
        assert buffer and err.value==0
        memory=p(buffer); assert api('clSetKernelArg',[p,U,S,p])(kernel,0,C.sizeof(p),C.byref(memory))==0
        size=S(64)
        assert api('clEnqueueNDRangeKernel',[p,p,U,p,p,p,U,p,p])(queue,kernel,1,None,C.byref(size),None,0,None,None)==0
        out=(U*64)()
        assert api('clEnqueueReadBuffer',[p,p,U,S,S,p,U,p,p])(queue,buffer,1,0,256,out,0,None,None)==0
        assert list(out)==[(i*17)^0x12345 for i in range(64)],'GPU output mismatch'
        paths=set()
        for line in Path('/proc/self/maps').read_text().splitlines():
            parts=line.split(maxsplit=5)
            if len(parts)==6 and parts[5].startswith('/'):
                paths.add(Path(parts[5]).resolve())
        required=('libigdrcl.so','libigdgmm.so.12','libigc.so.2','libigdfcl.so.2','libopencl-clang2.so.22.1')
        provenance={}
        for prefix in required:
            matches=[p for p in paths if p.name.startswith(prefix)]
            assert matches,f'Library not mapped after compilation: {prefix}'
            assert all(p.is_relative_to(root) for p in matches),f'System/private library mix: {matches}'
            provenance[prefix]=[str(p) for p in matches]
        print(json.dumps({'device':name,'driver':driver,'kernel_verified':True,'libraries':provenance}))
    finally:
        for obj,release in ((buffer,'clReleaseMemObject'),(kernel,'clReleaseKernel'),(program,'clReleaseProgram'),(queue,'clReleaseCommandQueue'),(context,'clReleaseContext')):
            if obj: api(release,[p])(obj)

if __name__=='__main__': main()
