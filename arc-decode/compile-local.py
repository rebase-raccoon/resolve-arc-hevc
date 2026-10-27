"""Build-only fallback for the installed x86-64 Clang/LLVM 21 C++ ABI.

Run in a disposable subprocess: C++ objects are intentionally kept alive until
process exit. Shared_ptr arguments use non-owning control blocks; large aligned
ctypes buffers provide storage. Prefer a normal compiler when one is available.
This helper is never loaded into Resolve.
"""
import ctypes as C,sys
P=C.c_void_p
l=C.CDLL('/usr/lib/x86_64-linux-gnu/libLLVM.so.21.1',mode=C.RTLD_GLOBAL)
c=C.CDLL('/usr/lib/x86_64-linux-gnu/libclang-cpp.so.21.1',mode=C.RTLD_GLOBAL)
def f(lib,n,args,ret=None):
 x=getattr(lib,n);x.argtypes=args;x.restype=ret;return x
for n in ['LLVMInitializeX86TargetInfo','LLVMInitializeX86Target','LLVMInitializeX86TargetMC','LLVMInitializeX86AsmPrinter','LLVMInitializeX86AsmParser']: f(l,n,[])()
args=[b'/usr/bin/clang-21',b'-fPIC',b'-O2',b'-fno-stack-protector',b'-nostdinc',*[x.encode() for x in sys.argv[1:]]]
a=(C.c_char_p*len(args))(*args); opts=C.create_string_buffer(64); inv=P()
f(c,'_ZN5clang16createInvocationEN4llvm8ArrayRefIPKcEENS_23CreateInvocationOptionsE',[P,P,C.c_size_t,P])(C.byref(inv),a,len(args),opts)
assert inv.value,'no invocation'
# Invocation is owned for the lifetime of this build subprocess.
shared=(P*2)(inv,None); pch=C.create_string_buffer(65536); f(c,'_ZN5clang22PCHContainerOperationsC1Ev',[P])(pch); empty=(P*2)(C.addressof(pch),None); ci=C.create_string_buffer(65536)
f(c,'_ZN5clang16CompilerInstanceC1ESt10shared_ptrINS_18CompilerInvocationEES1_INS_22PCHContainerOperationsEEPNS_11ModuleCacheE',[P,P,P,P])(ci,shared,empty,None)

vfs=P(); f(l,'_ZN4llvm3vfs17getRealFileSystemEv',[P])(C.byref(vfs))
f(c,'_ZN5clang16CompilerInstance17createDiagnosticsERN4llvm3vfs10FileSystemEPNS_18DiagnosticConsumerEb',[P,P,P,C.c_bool])(ci,vfs,None,True)
action=C.create_string_buffer(65536)
f(c,'_ZN5clang13EmitObjActionC1EPN4llvm11LLVMContextE',[P,P])(action,None)

r=f(c,'_ZN5clang16CompilerInstance13ExecuteActionERNS_14FrontendActionE',[P,P],C.c_bool)(ci,action)

sys.exit(0 if r else 1)
