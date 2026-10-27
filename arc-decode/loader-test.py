#!/usr/bin/env python3
"""Test nested RTLD_GLOBAL loading in the protected isolated namespace."""
import ctypes as C
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent


def child():
    lib = C.CDLL(None)
    lib.dlmopen.argtypes = [C.c_long, C.c_char_p, C.c_int]
    lib.dlmopen.restype = C.c_void_p
    lib.dlsym.argtypes = [C.c_void_p, C.c_char_p]
    lib.dlsym.restype = C.c_void_p
    handle = lib.dlmopen(-1, os.fsencode(ROOT/'libarc_namespace.so'), 2)
    assert handle, 'Cannot load the isolated namespace root'
    address = lib.dlsym(handle, b'arc_namespace_open')
    assert address
    load = C.CFUNCTYPE(C.c_void_p, C.c_char_p)(address)
    plugin = load(os.fsencode(ROOT/'libnamespace_probe.so'))
    assert plugin, 'Cannot load the simulated driver'
    probe = lib.dlsym(plugin, b'arc_nested_load_probe')
    assert probe
    assert C.CFUNCTYPE(C.c_int)(probe)() == 1
    assert not lib.dlsym(None, b'arc_nested_load_probe'), 'Plugin leaked into the base namespace'
    print('PASS: dependency can perform nested driver-style RTLD_GLOBAL loads without crashing or leaking into the base namespace.')


if __name__ == '__main__':
    if '--child' in sys.argv:
        child()
    else:
        env = os.environ.copy()
        env.pop('LD_PRELOAD', None)
        env.pop('LD_LIBRARY_PATH', None)
        subprocess.run([sys.executable, str(Path(__file__).resolve()), '--child'], env=env, check=True)
