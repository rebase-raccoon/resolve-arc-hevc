/* Diagnostic-only device timestamps. Poll completed events without waiting;
 * keep one owned event reference, with a fixed bound on outstanding events. */
extern int pthread_mutex_lock(void *);
extern int pthread_mutex_unlock(void *);
static int gpu_enabled;
static int (*ev_info)(Obj,U,size_t,void *,size_t *);
static int (*ev_profile)(Obj,U,size_t,void *,size_t *);
static int (*ev_retain)(Obj), (*ev_release)(Obj);
static int (*kernel_info)(Obj,U,size_t,void *,size_t *);
static unsigned long gpu_mutex[5], gpu_dropped, gpu_unavailable, gpu_failed;
typedef struct { Obj event; char label[128]; } Pending;
typedef struct { char label[128]; unsigned long count,exec_ns,queued_ns,submit_ns,max_ns; } GpuMetric;
static Pending pending[512];
static int polling;
static GpuMetric gpu_metrics[128];
static void label_copy(char *dst,const char *src) {
    int i=0;
    for(;i<127 && src[i];i++) dst[i]=(src[i]==' ' || src[i]=='\n' || src[i]=='\t') ? '_':src[i];
    dst[i]=0;
}
static int labels_equal(const char *a,const char *b) {
    for(int i=0;i<128;i++) { if(a[i]!=b[i]) return 0; if(!a[i]) return 1; }
    return 1;
}
static void gpu_configure(void) {
    const char *mode=getenv("RESOLVE_ARC_CL_GPU_PROFILE");
    if(!mode || mode[0]!='1' || mode[1]) return;
    *(void **)(&ev_info)=dlsym((void *)-1,"clGetEventInfo");
    *(void **)(&ev_profile)=dlsym((void *)-1,"clGetEventProfilingInfo");
    *(void **)(&ev_retain)=dlsym((void *)-1,"clRetainEvent");
    *(void **)(&ev_release)=dlsym((void *)-1,"clReleaseEvent");
    *(void **)(&kernel_info)=dlsym((void *)-1,"clGetKernelInfo");
    gpu_enabled=ev_info && ev_profile && ev_retain && ev_release && kernel_info;
}
static void gpu_poll(void) {
    if(!gpu_enabled) return;
    int idle=0;
    if(!__atomic_compare_exchange_n(&polling,&idle,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return;
    for(int i=0;i<512;i++) {
        pthread_mutex_lock(gpu_mutex);
        Pending item=pending[i];
        if(!item.event) { pthread_mutex_unlock(gpu_mutex); continue; }
        /* Reserve the slot while querying outside the lock: runtime queries
         * may invoke application callbacks that reenter these wrappers. */
        pending[i].event=(Obj)-1;
        pthread_mutex_unlock(gpu_mutex);
        Pending *p=&item; int status=3;
        int err=ev_info(p->event,0x11D3,sizeof(status),&status,(void *)0);
        if(!err && status>0) {
            pthread_mutex_lock(gpu_mutex); pending[i]=item; pthread_mutex_unlock(gpu_mutex);
            continue;
        }
        unsigned long queued=0,submit=0,start=0,end=0;
        int failed=err || status<0;
        int unavailable=0;
        if(!failed) unavailable=ev_profile(p->event,0x1280,sizeof(queued),&queued,(void *)0) ||
                ev_profile(p->event,0x1281,sizeof(submit),&submit,(void *)0) ||
                ev_profile(p->event,0x1282,sizeof(start),&start,(void *)0) ||
                ev_profile(p->event,0x1283,sizeof(end),&end,(void *)0) ||
                end<start || start<submit || submit<queued;
        pthread_mutex_lock(gpu_mutex);
        if(failed) gpu_failed++;
        else if(unavailable) gpu_unavailable++;
        else {
            int slot=-1;
            for(int j=0;j<128;j++) {
                if(!gpu_metrics[j].label[0]) { slot=j; label_copy(gpu_metrics[j].label,p->label); break; }
                if(labels_equal(gpu_metrics[j].label,p->label)) { slot=j; break; }
            }
            if(slot<0) gpu_dropped++;
            else {
                GpuMetric *m=&gpu_metrics[slot]; unsigned long duration=end-start;
                m->count++; m->exec_ns+=duration; m->queued_ns+=start-queued; m->submit_ns+=start-submit;
                if(duration>m->max_ns) m->max_ns=duration;
            }
        }
        pthread_mutex_unlock(gpu_mutex);
        ev_release(p->event);
        pthread_mutex_lock(gpu_mutex); pending[i].event=(void *)0; pthread_mutex_unlock(gpu_mutex);
    }
    __atomic_store_n(&polling,0,__ATOMIC_RELEASE);
}
static void gpu_track(Obj event,int app_owned,const char *label) {
    if(!gpu_enabled || !event) return;
    /* The caller hasn't received its event yet, so retaining is race-free. */
    if(app_owned && ev_retain(event)) return;
    pthread_mutex_lock(gpu_mutex);
    for(int i=0;i<512;i++) if(!pending[i].event) {
        pending[i].event=event; label_copy(pending[i].label,label);
        pthread_mutex_unlock(gpu_mutex); return;
    }
    gpu_dropped++;
    pthread_mutex_unlock(gpu_mutex);
    ev_release(event);
}
static void gpu_snapshot(unsigned long stamp) {
    if(!gpu_enabled || fd<0) return;
    pthread_mutex_lock(gpu_mutex);
    unsigned long outstanding=0;
    for(int i=0;i<512;i++) if(pending[i].event) outstanding++;
    for(int i=0;i<128;i++) if(gpu_metrics[i].count) {
        GpuMetric *m=&gpu_metrics[i];
        dprintf(fd,"event=opencl_gpu_profile snapshot_ns=%lu name=%s count=%lu execute_ns=%lu queued_to_start_ns=%lu submit_to_start_ns=%lu max_execute_ns=%lu\n",
                stamp,m->label,m->count,m->exec_ns,m->queued_ns,m->submit_ns,m->max_ns);
    }
    dprintf(fd,"event=opencl_gpu_status snapshot_ns=%lu pending=%lu dropped=%lu unavailable=%lu failed=%lu\n",
            stamp,outstanding,gpu_dropped,gpu_unavailable,gpu_failed);
    pthread_mutex_unlock(gpu_mutex);
}
#define GPU_EVENT \
    Obj private_event=(void *)0; \
    Obj *captured=(gpu_enabled && !event) ? &private_event:event
#define GPU_TRACK(R,LABEL) do { \
    if(gpu_enabled && !(R) && captured) gpu_track(*captured,!!event,LABEL); \
} while(0)

/* Record the device used by real Resolve queues, independently of the
 * standalone compiler probe. This also works with host-only tracing. */
static void queue_identity(Obj queue) {
    if(!queue || fd<0) return;
    int (*queue_info)(Obj,U,size_t,void *,size_t *);
    int (*device_info)(Obj,U,size_t,void *,size_t *);
    *(void **)(&queue_info)=dlsym((void *)-1,"clGetCommandQueueInfo");
    *(void **)(&device_info)=dlsym((void *)-1,"clGetDeviceInfo");
    Obj device=(void *)0;
    char name[128]={0},driver[128]={0},safe_name[128],safe_driver[128];
    if(!queue_info || !device_info || queue_info(queue,0x1091,sizeof(device),&device,(void *)0)) return;
    if(device_info(device,0x102B,sizeof(name),name,(void *)0) ||
       device_info(device,0x102D,sizeof(driver),driver,(void *)0)) return;
    name[127]=0; driver[127]=0; label_copy(safe_name,name); label_copy(safe_driver,driver);
    dprintf(fd,"event=opencl_device queue=%p name=%s driver=%s\n",queue,safe_name,safe_driver);
}

/* Enable CL_QUEUE_PROFILING_ENABLE (bit 1) only in this diagnostic mode.
 * Failed profiled creation is retried with the original properties. */
EXPORT Obj clCreateCommandQueue(Obj context,Obj device,Flags props,int *error) {
    pthread_once(&once,init);
    Obj (*fn)(Obj,Obj,Flags,int *);
    *(void **)(&fn)=dlsym((void *)-1,"clCreateCommandQueue");
    if(!fn) { if(error) *error=-59; return (void *)0; }
    int local=-59;
    Obj q=fn(context,device,gpu_enabled ? props|2:props,&local);
    int enabled=gpu_enabled && !!q;
    if(!q && gpu_enabled) q=fn(context,device,props,&local);
    if(error) *error=local;
    queue_identity(q);
    if(fd>=0) dprintf(fd,"event=opencl_queue api=legacy profiling=%d error=%d\n",enabled,local);
    return q;
}
EXPORT Obj clCreateCommandQueueWithProperties(Obj context,Obj device,const long *props,int *error) {
    pthread_once(&once,init);
    Obj (*fn)(Obj,Obj,const long *,int *);
    *(void **)(&fn)=dlsym((void *)-1,"clCreateCommandQueueWithProperties");
    if(!fn) { if(error) *error=-59; return (void *)0; }
    long modified[67]; int n=0,found=0,valid=1;
    if(gpu_enabled && props) {
        while(n<64 && props[n]) {
            modified[n]=props[n]; modified[n+1]=props[n+1];
            if(props[n]==0x1093) { modified[n+1]|=2; found=1; }
            n+=2;
        }
        if(n==64 && props[n]) valid=0;
    }
    if(!found) { modified[n++]=0x1093; modified[n++]=2; }
    modified[n]=0;
    const long *requested=gpu_enabled && valid ? modified:props;
    int local=-59;
    Obj q=fn(context,device,requested,&local);
    int enabled=gpu_enabled && valid && !!q;
    if(!q && gpu_enabled && valid) q=fn(context,device,props,&local);
    if(error) *error=local;
    queue_identity(q);
    if(fd>=0) dprintf(fd,"event=opencl_queue api=properties profiling=%d error=%d\n",enabled,local);
    return q;
}
