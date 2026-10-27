/* Optional OpenCL timing. Host-only mode forwards the original operations.
 * Opt-in device timing enables queue profiling and retains/captures events,
 * without adding waits. See opencl-events.h and README.txt for limitations. */
#define EXPORT __attribute__((visibility("default")))
typedef unsigned long size_t;
typedef unsigned int U;
typedef unsigned long Flags;
typedef void *Obj;
extern void *dlsym(void *,const char *);
struct Info { const char *file; void *base; const char *symbol; void *address; };
extern int dladdr(const void *,struct Info *);
extern char *getenv(const char *);
extern int open(const char *,int,...);
extern int dprintf(int,const char *,...);
extern int pthread_once(int *,void (*)(void));
extern long syscall(long,...);
struct Time { long sec,nsec; };
extern int clock_gettime(int,struct Time *);
static int once,fd=-1;
static void init(void);
static unsigned long now(void) {
    struct Time t; clock_gettime(1,&t);
    return (unsigned long)t.sec*1000000000UL+t.nsec;
}
typedef struct { const char *name; unsigned long calls,ns,max,bytes,errors; } Metric;
static Metric metrics[]={
    {"clFinish",0,0,0,0,0}, {"clWaitForEvents",0,0,0,0,0},
    {"clEnqueueWriteBuffer",0,0,0,0,0}, {"clEnqueueReadBuffer",0,0,0,0,0},
    {"clEnqueueMapBuffer",0,0,0,0,0}, {"clEnqueueUnmapMemObject",0,0,0,0,0},
    {"clEnqueueNDRangeKernel",0,0,0,0,0}, {"clEnqueueWriteImage",0,0,0,0,0},
    {"clEnqueueReadImage",0,0,0,0,0}, {"clEnqueueWriteBufferRect",0,0,0,0,0},
    {"clEnqueueReadBufferRect",0,0,0,0,0},
    {"clEnqueueAcquireGLObjects",0,0,0,0,0}, {"clEnqueueReleaseGLObjects",0,0,0,0,0}
};
#include "opencl-events.h"
static void init(void) {
    const char *p=getenv("RESOLVE_ARC_CL_LOG");
    if(p) fd=open(p,656449,0600);
    gpu_configure();
    if(fd>=0) dprintf(fd,"event=opencl_trace_loaded gpu_events=%d diagnostic_profiling_changes_queue_properties=%d\n",gpu_enabled,gpu_enabled);
}
static void snapshot(void);
static unsigned long last_snapshot;
static void record(int index,unsigned long start,size_t bytes,int error,const void *caller,U blocking) {
    unsigned long elapsed=now()-start;
    Metric *m=&metrics[index];
    unsigned long n=__atomic_add_fetch(&m->calls,1,__ATOMIC_RELAXED);
    __atomic_add_fetch(&m->ns,elapsed,__ATOMIC_RELAXED);
    __atomic_add_fetch(&m->bytes,bytes,__ATOMIC_RELAXED);
    if(error) __atomic_add_fetch(&m->errors,1,__ATOMIC_RELAXED);
    unsigned long old=__atomic_load_n(&m->max,__ATOMIC_RELAXED);
    while(elapsed>old && !__atomic_compare_exchange_n(&m->max,&old,elapsed,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
    if(fd>=0 && (n<=2 || (elapsed>50000000UL && n%30==0))) {
        struct Info info={0};
        dladdr(caller,&info);
        dprintf(fd,"event=opencl_call function=%s tid=%ld start_ns=%lu elapsed_ns=%lu bytes=%lu error=%d blocking=%u caller_module=%s caller_symbol=%s\n",
                m->name,syscall(186),start,elapsed,bytes,error,blocking,
                info.file ? info.file:"unknown",info.symbol ? info.symbol:"unknown");
    }
    gpu_poll();
    unsigned long stamp=now();
    unsigned long previous=__atomic_load_n(&last_snapshot,__ATOMIC_RELAXED);
    if(stamp-previous>=1000000000UL &&
       __atomic_compare_exchange_n(&last_snapshot,&previous,stamp,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) snapshot();
}
#define RECORD(I,S,B,E,K) record(I,S,B,E,__builtin_return_address(0),K)
#define BEGIN(I,T) \
    pthread_once(&once,init); \
    static void *cached; \
    void *address=__atomic_load_n(&cached,__ATOMIC_ACQUIRE); \
    if(!address) { address=dlsym((void *)-1,metrics[I].name); __atomic_store_n(&cached,address,__ATOMIC_RELEASE); } \
    T fn; *(void **)(&fn)=address; \
    unsigned long started=now()
EXPORT int clFinish(Obj q) {
    typedef int (*F)(Obj); BEGIN(0,F);
    int r=fn ? fn(q):-59; RECORD(0,started,0,r,2); return r;
}
EXPORT int clWaitForEvents(U n,const Obj *events) {
    typedef int (*F)(U,const Obj *); BEGIN(1,F);
    int r=fn ? fn(n,events):-59; RECORD(1,started,0,r,2); return r;
}
#define BUFFER(NAME,I,PTR) \
EXPORT int NAME(Obj q,Obj mem,U block,size_t off,size_t size,PTR ptr,U n,const Obj *wait,Obj *event) { \
    typedef int (*F)(Obj,Obj,U,size_t,size_t,PTR,U,const Obj *,Obj *); BEGIN(I,F); GPU_EVENT; \
    int r=fn ? fn(q,mem,block,off,size,ptr,n,wait,captured):-59; GPU_TRACK(r,metrics[I].name); RECORD(I,started,size,r,block); return r; \
}
BUFFER(clEnqueueWriteBuffer,2,const void *)
BUFFER(clEnqueueReadBuffer,3,void *)
EXPORT void *clEnqueueMapBuffer(Obj q,Obj mem,U block,Flags flags,size_t off,size_t size,U n,const Obj *wait,Obj *event,int *error) {
    typedef void *(*F)(Obj,Obj,U,Flags,size_t,size_t,U,const Obj *,Obj *,int *); BEGIN(4,F); GPU_EVENT;
    int local=-59;
    void *r=fn ? fn(q,mem,block,flags,off,size,n,wait,captured,&local):(void *)0;
    if(error) *error=local;
    GPU_TRACK(local,metrics[4].name);
    RECORD(4,started,size,local,block); return r;
}
EXPORT int clEnqueueUnmapMemObject(Obj q,Obj mem,void *ptr,U n,const Obj *wait,Obj *event) {
    typedef int (*F)(Obj,Obj,void *,U,const Obj *,Obj *); BEGIN(5,F); GPU_EVENT;
    int r=fn ? fn(q,mem,ptr,n,wait,captured):-59; GPU_TRACK(r,metrics[5].name); RECORD(5,started,0,r,2); return r;
}
EXPORT int clEnqueueNDRangeKernel(Obj q,Obj kernel,U dim,const size_t *offset,const size_t *global,const size_t *local,U n,const Obj *wait,Obj *event) {
    typedef int (*F)(Obj,Obj,U,const size_t *,const size_t *,const size_t *,U,const Obj *,Obj *); BEGIN(6,F); GPU_EVENT;
    char label[128]="kernel_unknown";
    if(gpu_enabled) { kernel_info(kernel,0x1190,sizeof(label),label,(void *)0); label[127]=0; }
    int r=fn ? fn(q,kernel,dim,offset,global,local,n,wait,captured):-59; GPU_TRACK(r,label); RECORD(6,started,0,r,2); return r;
}
#define IMAGE(NAME,I,PTR) \
EXPORT int NAME(Obj q,Obj mem,U block,const size_t *origin,const size_t *region,size_t row,size_t slice,PTR ptr,U n,const Obj *wait,Obj *event) { \
    typedef int (*F)(Obj,Obj,U,const size_t *,const size_t *,size_t,size_t,PTR,U,const Obj *,Obj *); BEGIN(I,F); GPU_EVENT; \
    int r=fn ? fn(q,mem,block,origin,region,row,slice,ptr,n,wait,captured):-59; GPU_TRACK(r,metrics[I].name); RECORD(I,started,0,r,block); return r; \
}
IMAGE(clEnqueueWriteImage,7,const void *)
IMAGE(clEnqueueReadImage,8,void *)
#define RECT(NAME,I,PTR) \
EXPORT int NAME(Obj q,Obj mem,U block,const size_t *bo,const size_t *ho,const size_t *region,size_t br,size_t bs,size_t hr,size_t hs,PTR ptr,U n,const Obj *wait,Obj *event) { \
    typedef int (*F)(Obj,Obj,U,const size_t *,const size_t *,const size_t *,size_t,size_t,size_t,size_t,PTR,U,const Obj *,Obj *); BEGIN(I,F); GPU_EVENT; \
    int r=fn ? fn(q,mem,block,bo,ho,region,br,bs,hr,hs,ptr,n,wait,captured):-59; GPU_TRACK(r,metrics[I].name); \
    RECORD(I,started,region ? region[0]*region[1]*region[2]:0,r,block); return r; \
}
RECT(clEnqueueWriteBufferRect,9,const void *)
RECT(clEnqueueReadBufferRect,10,void *)
#define GL(NAME,I) \
EXPORT int NAME(Obj q,U count,const Obj *objects,U n,const Obj *wait,Obj *event) { \
    typedef int (*F)(Obj,U,const Obj *,U,const Obj *,Obj *); BEGIN(I,F); GPU_EVENT; \
    int r=fn ? fn(q,count,objects,n,wait,captured):-59; GPU_TRACK(r,metrics[I].name); RECORD(I,started,0,r,2); return r; \
}
GL(clEnqueueAcquireGLObjects,11)
GL(clEnqueueReleaseGLObjects,12)
static void snapshot(void) {
    unsigned long stamp=now();
    if(fd<0) return;
    gpu_snapshot(stamp);
    for(unsigned i=0;i<sizeof(metrics)/sizeof(metrics[0]);i++) {
        Metric *m=&metrics[i];
        unsigned long calls=__atomic_load_n(&m->calls,__ATOMIC_RELAXED);
        if(calls) dprintf(fd,"event=opencl_profile snapshot_ns=%lu function=%s calls=%lu total_ns=%lu max_ns=%lu bytes=%lu errors=%lu\n",stamp,m->name,calls,
            __atomic_load_n(&m->ns,__ATOMIC_RELAXED),__atomic_load_n(&m->max,__ATOMIC_RELAXED),
            __atomic_load_n(&m->bytes,__ATOMIC_RELAXED),__atomic_load_n(&m->errors,__ATOMIC_RELAXED));
    }
}

__attribute__((destructor)) static void report(void) { snapshot(); }
