#!/usr/bin/env python3
"""Prepare and run a pinned Intel OpenCL stack without installing packages."""
import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request

ROOT=Path(__file__).resolve().parent
VERSION='26.35.39758.10'
BASE=ROOT/'private-runtime'/VERSION
RELEASE='https://github.com/intel/compute-runtime/releases/download/'+VERSION+'/'
IGC='https://github.com/intel/intel-graphics-compiler/releases/download/v2.41.5/'
PACKAGES=[
 (RELEASE,'intel-opencl-icd_26.35.39758.10-0_amd64.deb','61712caaddeba3d38e4f79e2a0fb23fea25596ca2d72c3144c6eea2331ec4301'),
 (RELEASE,'libigdgmm12_22.10.0_amd64.deb','6031a63d6e8a12ce61c14efc15f2c8e727061286e3820b8594e6d00615e04d54'),
 (IGC,'intel-igc-core-2_2.41.5+22716_amd64.deb','0a6e64a663ae65a0fa02d6912ae3b6b37cf85b90c21cc423fd9fef70aaf4f628'),
 (IGC,'intel-igc-opencl-2_2.41.5+22716_amd64.deb','779e1b9e88098eb25711e9a8f67c2752665bad22f134aa40ed5649f6e1b87058'),
]

def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f,'sha256').hexdigest()

def download(url,path,expected):
    if path.is_file() and digest(path)==expected:
        return
    part=path.with_suffix('.part')
    try:
        with urllib.request.urlopen(url,timeout=45) as response,part.open('wb') as out:
            shutil.copyfileobj(response,out)
        if digest(part)!=expected:
            raise RuntimeError(f'Checksum mismatch for {path.name}; package was not extracted.')
        part.replace(path)
    finally:
        part.unlink(missing_ok=True)

def dynamic(path):
    out=subprocess.run(['readelf','-d',str(path)],check=True,text=True,capture_output=True).stdout
    needed=re.findall(r'\(NEEDED\).*?\[(.*?)\]',out)
    names=re.findall(r'\(SONAME\).*?\[(.*?)\]',out)
    return names[0] if names else path.name,needed

def inventory(tree):
    libs={}; seen=set()
    for path in tree.rglob('*.so*'):
        if not path.is_file(): continue
        actual=path.resolve()
        if not actual.is_relative_to(tree.resolve()):
            raise RuntimeError(f'Library symlink escapes private package tree: {path}')
        if actual in seen: continue
        seen.add(actual)
        with actual.open('rb') as f:
            if f.read(4)!=b'\x7fELF': continue
        soname,needs=dynamic(actual)
        if soname in libs and libs[soname]['path']!=str(actual):
            raise RuntimeError(f'Duplicate private SONAME: {soname}')
        libs[soname]={'path':str(actual),'needed':needs}
    for name in ('libigdrcl.so','libigdgmm.so.12','libigc.so.2','libigdfcl.so.2'):
        if name not in libs:
            raise RuntimeError(f'Required private library missing: {name}')
    ordered=[]; visiting=set(); done=set()
    def visit(name):
        if name in done: return
        if name in visiting: raise RuntimeError(f'Private dependency cycle: {name}')
        visiting.add(name)
        for dep in libs[name]['needed']:
            if dep in libs: visit(dep)
        visiting.remove(name); done.add(name); ordered.append(name)
    for name in libs: visit(name)
    preload=[libs[n]['path'] for n in ordered if n!='libigdrcl.so']
    return libs,preload

def prepare():
    for tool in ('dpkg-deb','readelf','ldd'):
        if not shutil.which(tool): raise RuntimeError(f'Required tool missing: {tool}')
    downloads=BASE/'packages'; downloads.mkdir(parents=True,exist_ok=True)
    for base,name,sha in PACKAGES:
        print(f'Checking/downloading {name}...',flush=True)
        download(base+name,downloads/name,sha)
    tree=BASE/'root'
    if not tree.exists():
        stage=Path(tempfile.mkdtemp(prefix='.extract-',dir=BASE))
        try:
            for _,name,_ in PACKAGES:
                subprocess.run(['dpkg-deb','-x',str(downloads/name),str(stage)],check=True)
            stage.rename(tree)
        finally:
            if stage.exists(): shutil.rmtree(stage)
    libs,preload=inventory(tree)
    dirs=sorted({str(Path(row['path']).parent) for row in libs.values()})
    env=os.environ.copy(); env.pop('LD_PRELOAD',None)
    env['LD_LIBRARY_PATH']=':'.join(dirs)
    for row in libs.values():
        check=subprocess.run(['ldd',row['path']],env=env,text=True,capture_output=True)
        if check.returncode or 'not found' in check.stdout or 'not found' in check.stderr:
            raise RuntimeError('Private library dependencies unavailable:\n'+check.stdout+check.stderr)
    icd=BASE/'vendors'; icd.mkdir(exist_ok=True)
    (icd/'intel.icd').write_text(libs['libigdrcl.so']['path']+'\n')
    manifest={'version':VERSION,'compiler':'2.41.5','root':str(tree),'libraries':libs,
              'preload':preload,'icd_directory':str(icd),'packages':[
                  {'url':b+n,'sha256':h} for b,n,h in PACKAGES]}
    (BASE/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest

def environment(manifest):
    env=os.environ.copy()
    for key in ('LD_PRELOAD','LD_LIBRARY_PATH','OCL_ICD_FILENAMES'):
        env.pop(key,None)
    env['OCL_ICD_VENDORS']=manifest['icd_directory']
    env['OPENCL_VENDOR_PATH']=manifest['icd_directory']
    env['RESOLVE_ARC_PRIVATE_PRELOAD']=':'.join(manifest['preload'])
    env['RESOLVE_ARC_PRIVATE_ROOT']=manifest['root']
    # Host-only tracing keeps event-profiling overhead out of comparison FPS.
    env['RESOLVE_ARC_TRACE_OPENCL']='1'
    env['RESOLVE_ARC_CL_GPU_PROFILE']='0'
    return env

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepare-only',action='store_true')
    args=parser.parse_args()
    if os.environ.get('LD_PRELOAD'):
        sys.exit('Use a terminal without another LD_PRELOAD experiment enabled.')
    if subprocess.run(['pgrep','-x','resolve'],capture_output=True).returncode==0:
        sys.exit('Save your work and quit Resolve before preparing the private runtime comparison.')
    try:
        manifest=prepare()
        print(f'Private stack prepared: {BASE}',flush=True)
        if args.prepare_only: return
        env=environment(manifest)
        probe_env=env.copy(); probe_env['LD_PRELOAD']=env['RESOLVE_ARC_PRIVATE_PRELOAD']
        probe_env.pop('RESOLVE_ARC_CL_LOG',None)
        probe=subprocess.run([sys.executable,str(ROOT/'runtime-probe.py'),'--expected-root',manifest['root']],
                             env=probe_env,text=True,capture_output=True,timeout=120)
        report=ROOT/'logs'/f'private-runtime-probe-{datetime.now():%Y%m%d-%H%M%S}.json'
        report.parent.mkdir(exist_ok=True)
        if probe.returncode:
            report.with_suffix('.stderr.txt').write_text(probe.stdout+'\n'+probe.stderr)
            raise RuntimeError(f'Private GPU/compiler probe failed; Resolve was not started. Evidence: {report.with_suffix(".stderr.txt")}')
        data=json.loads(probe.stdout); report.write_text(json.dumps(data,indent=2)+'\n')
        print(f'GPU compile/execute passed: {data["device"]}; driver {data["driver"]}',flush=True)
        print(f'Library provenance: {report}',flush=True)
        print('Launching with the private OpenCL stack. Initial kernel compilation can be slow.',flush=True)
        sys.exit(subprocess.run(['bash',str(ROOT/'launch.sh')],env=env).returncode)
    except (OSError,RuntimeError,subprocess.SubprocessError) as exc:
        sys.exit(f'Private runtime setup stopped: {exc}\nSystem packages were not installed or changed.')

if __name__=='__main__': main()
