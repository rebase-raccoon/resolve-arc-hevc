#!/usr/bin/env python3
"""GPU-free tests for verified downloads, private dependencies and environment."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from unittest.mock import patch

root=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('private_runtime',root/'private-runtime.py')
m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
with tempfile.TemporaryDirectory(prefix='arc-private-test-') as directory:
    temp=Path(directory)
    source=temp/'package'; source.write_bytes(b'verified local fixture')
    target=temp/'download.deb'; expected=hashlib.sha256(source.read_bytes()).hexdigest()
    m.download(source.as_uri(),target,expected)
    assert target.read_bytes()==source.read_bytes()
    target.write_bytes(b'corrupt cache')
    m.download(source.as_uri(),target,expected)
    assert m.digest(target)==expected
    try:
        m.download(source.as_uri(),temp/'bad.deb','0'*64)
        raise AssertionError('Mismatched package accepted')
    except RuntimeError: pass
    assert not (temp/'bad.deb').exists() and not (temp/'bad.part').exists()
    tree=temp/'root'; tree.mkdir()
    def lib(name,deps=()):
        path=tree/name
        subprocess.run(['ld','-shared','-soname',name,'-o',str(path),str(root/'namespace-probe.o'),
                        '--no-as-needed',*[str(tree/dep) for dep in deps],'-lc'],check=True,capture_output=True)
    lib('libigdgmm.so.12'); lib('libigc.so.2')
    lib('libigdfcl.so.2',('libigc.so.2',))
    lib('libigdrcl.so',('libigdgmm.so.12','libigdfcl.so.2'))
    libs,preload=m.inventory(tree)
    names=[Path(p).name for p in preload]
    assert 'libigdrcl.so' not in names and names.index('libigc.so.2')<names.index('libigdfcl.so.2')
    (tree/'escape.so').symlink_to(source)
    try:
        m.inventory(tree); raise AssertionError('Escaping link accepted')
    except RuntimeError: pass
    (tree/'escape.so').unlink()
    shutil.copyfile(tree/'libigc.so.2',tree/'duplicate.so')
    try:
        m.inventory(tree); raise AssertionError('Duplicate SONAME accepted')
    except RuntimeError: pass
    manifest={'icd_directory':str(temp/'vendors'),'preload':preload,'root':str(tree)}
    with patch.dict(os.environ,{'LD_PRELOAD':'old-experiment.so','LD_LIBRARY_PATH':'old-path'}):
        env=m.environment(manifest)
        assert 'LD_PRELOAD' not in env and 'LD_LIBRARY_PATH' not in env
        assert env['OCL_ICD_VENDORS']==str(temp/'vendors')
        assert env['RESOLVE_ARC_PRIVATE_PRELOAD']==':'.join(preload)
        assert os.environ['LD_PRELOAD']=='old-experiment.so'
print('PASS: verified download/cache, rejected checksum mismatch, dependency order, isolation guards and child-only environment.')
