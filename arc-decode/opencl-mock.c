/* Test-only successful OpenCL event/queue API; never loaded into Resolve. */
#define EXPORT __attribute__((visibility("default")))
typedef unsigned long size_t;
typedef void *Obj;
extern void *calloc(size_t,size_t);
extern void free(void *);
extern void *memcpy(void *,const void *,size_t);
static int live,retains,releases,finishes,status=2,reject,unavailable,reenter;
EXPORT int clFinish(Obj q);
EXPORT void arc_mock_reenter(void) { reenter=1; }
static unsigned long last_flags;
static long other_value;
typedef struct { int refs; } Event;
EXPORT void arc_mock_counts(int *out) { out[0]=live; out[1]=retains; out[2]=releases; out[3]=finishes; }
EXPORT unsigned long arc_mock_flags(void) { return last_flags; }
EXPORT long arc_mock_other(void) { return other_value; }
EXPORT void arc_mock_state(int s,int r,int u) { status=s; reject=r; unavailable=u; }
EXPORT Obj clCreateCommandQueue(Obj c,Obj d,unsigned long props,int *err) {
    (void)c; (void)d; last_flags=props;
    if(reject && (props&2)) { if(err) *err=-35; return (void *)0; }
    if(err) *err=0;
    return (void *)1;
}
EXPORT Obj clCreateCommandQueueWithProperties(Obj c,Obj d,const long *props,int *err) {
    unsigned long flags=0; other_value=0;
    if(props) for(int i=0;props[i];i+=2) {
        if(props[i]==0x1093) flags=props[i+1];
        if(props[i]==0xdead) other_value=props[i+1];
    }
    return clCreateCommandQueue(c,d,flags,err);
}
EXPORT int clGetKernelInfo(Obj kernel,unsigned name,size_t size,void *out,size_t *written) {
    (void)kernel; (void)name;
    const char text[]="test_kernel";
    if(size<sizeof(text)) return -30;
    memcpy(out,text,sizeof(text)); if(written) *written=sizeof(text); return 0;
}
EXPORT int clEnqueueNDRangeKernel(Obj q,Obj kernel,unsigned dim,const size_t *offset,const size_t *global,const size_t *local,unsigned n,const Obj *wait,Obj *event) {
    (void)q; (void)kernel; (void)dim; (void)offset; (void)global; (void)local; (void)n; (void)wait;
    if(event) { Event *e=calloc(1,sizeof(*e)); e->refs=1; *event=e; live++; }
    return 0;
}
EXPORT int clRetainEvent(Obj event) { ((Event *)event)->refs++; retains++; return 0; }
EXPORT int clReleaseEvent(Obj event) { Event *e=event; releases++; if(!--e->refs) { live--; free(e); } return 0; }
EXPORT int clGetEventInfo(Obj event,unsigned name,size_t size,void *out,size_t *written) {
    (void)event; (void)name; (void)size; (void)written;
    if(reenter) { reenter=0; clFinish((Obj)1); }
    *(int *)out=status; return 0;
}
EXPORT int clGetEventProfilingInfo(Obj event,unsigned name,size_t size,void *out,size_t *written) {
    (void)event; (void)size; (void)written;
    if(unavailable) return -7;
    unsigned long value=name==0x1280 ? 100:name==0x1281 ? 200:name==0x1282 ? 300:1300;
    *(unsigned long *)out=value; return 0;
}
EXPORT int clFinish(Obj q) { (void)q; finishes++; return 0; }
