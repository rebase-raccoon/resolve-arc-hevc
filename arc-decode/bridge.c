/* Experimental Linux x86-64 Resolve FFmpeg 6 -> isolated FFmpeg 8 adapter. */
#include "abi.h"
#define NULL ((void *)0)
#define EXPORT __attribute__((visibility("default")))
extern void *dlsym(void *, const char *);
extern void *dlmopen(long, const char *, int);
extern int dlinfo(void *, int, void *);
extern char *dlerror(void);
extern char *getenv(const char *);
extern int strcmp(const char *, const char *);
extern int atoi(const char *);
extern void *memcpy(void *, const void *, size_t);
extern void *calloc(size_t, size_t);
extern void free(void *);
extern int open(const char *, int, ...);
extern int dprintf(int, const char *, ...);
extern int getpid(void);
extern long readlink(const char *, char *, size_t);
extern int dladdr(const void *, void *);
extern size_t strlen(const char *);
extern void _exit(int);
extern int pthread_create(unsigned long *, const void *, void *(*)(void *), void *);
extern int pthread_join(unsigned long, void **);
extern int pthread_mutex_destroy(void *);
extern int pthread_cond_destroy(void *);
extern int pthread_once(int *, void (*)(void));
extern int pthread_mutex_lock(void *);
extern int pthread_mutex_unlock(void *);
extern int pthread_cond_wait(void *, void *);
extern int pthread_cond_broadcast(void *);
struct ArcTime { long sec, nsec; };
extern int clock_gettime(int, struct ArcTime *);
#include "repack.h"
static unsigned long now_ns(void) {
    struct ArcTime t;
    clock_gettime(1,&t);
    return (unsigned long)t.sec*1000000000UL+(unsigned long)t.nsec;
}

/* Keep every allocator, AVBufferRef, dictionary, packet and frame in its own ABI. */
typedef struct {
    unsigned (*version)(void);
    unsigned (*util_version)(void);
    const void *(*find)(const char *);
    void *(*alloc)(const void *);
    int (*open)(void *, const void *, void **);
    int (*send)(void *, const Packet *);
    int (*receive)(void *, void *);
    void (*flush)(void *);
    void (*destroy)(void **);
    int (*close)(void *);
    void *(*par_alloc)(void);
    void (*par_free)(void **);
    int (*par_from)(void *, const void *);
    int (*par_to)(void *, const void *);
    void *(*buffer_create)(uint8_t *,size_t,void (*)(void *,uint8_t *),void *,int);
    void *(*frame_alloc)(void);
    void (*frame_free)(void **);
    void (*frame_unref)(void *);
    void *(*frame_clone)(const void *);
    void (*frame_move)(void *, void *);
    int (*frame_buffer)(void *, int);
    int (*frame_copy)(void *, const void *);
    int (*frame_props)(void *, const void *);
    void (*copy_uncached)(uint8_t *const *,const long *,const uint8_t *const *,const long *,int,int,int);
    SideData *(*frame_side)(void *, int, size_t);
    const char *(*side_name)(int);
    void *(*dict_get)(const void *, const char *, const void *, int);
    int (*dict_set)(void **, const char *, const char *, int);
    void (*dict_free)(void **);
    void *(*mallocz)(size_t);
    Packet *(*packet_alloc)(void);
    void (*packet_free)(Packet **);
    void (*packet_unref)(Packet *);
    int (*packet_new)(Packet *, int);
    uint8_t *(*packet_side)(Packet *, int, size_t);
    const char *(*packet_side_name)(int);
    const char *(*pix_name)(int);
    int (*pix_fmt)(const char *);
    int (*opt_get)(void *, const char *, int, uint8_t **);
    int (*opt_set)(void *, const char *, const char *, int);
    void (*avfree)(void *);
    int (*device_type)(const char *);
    int (*device_create)(void **, int, const char *, void *, int);
    int (*transfer)(void *, const void *, int);
    int (*map)(void *, const void *, int);
    void *(*sws_get)(void *, int, int, int, int, int, int, int, void *, void *, const double *);
    int (*sws_scale)(void *, const uint8_t *const *, const int *, int, int, uint8_t *const *, const int *);
    void (*sws_free)(void *);
} API;
static API r, b;
#include "cleanup.h"
static int once, backend_once, backend_ok, fd = -1, active;
static unsigned long mutex[5]; /* glibc x86-64 PTHREAD_MUTEX_INITIALIZER */
static int software_test, vaapi, use_swscale, download_mode, prefetch_depth, stream_capable, test_stream, repack_workers, async_cleanup, pipeline_depth, use_getbuffer, dovi_hardware;
struct Pipe;
typedef struct ArcVpp ArcVpp;
extern int arc_vpp_init(void *(*lookup)(const char *));
extern ArcVpp *arc_vpp_new(void *dpy, int kind, int w, int h, uint8_t **plane0, uint8_t **plane1, int *pitch);
extern int arc_vpp_blit(ArcVpp *v, unsigned src);
extern void arc_vpp_free(ArcVpp *v);
static void *ns_handle; static int vpp_ok;
static void *ns_lookup(const char *name) { return dlsym(ns_handle,name); }

typedef struct State {
    void *original;
    Ctx8 *decoder;
    Frame8 *gpu, *cpu, *mapped;
    Packet *packet;
    void *sws;
    RepackPool *repack_pool;
    CleanupPool *cleanup_pool;
    unsigned long frames, packets, flushes, hardware_frames;
    unsigned long api_ns, send_calls, receive_calls, receive_again;
    int target, failed;
    int profile_width, profile_height, profile_format;
    unsigned long operation_mutex[5], idle[6], users;
    unsigned long send_ns, receive_ns, download_ns, allocate_ns, repack_ns, copy_ns, wait_ns;
    unsigned long map_ns, uncached_copy_ns, cleanup_ns, direct_frames, mapped_frames, transferred_frames;
    unsigned long fused_frames, staged_frames;
    int fused, queue_head, queue_count, backend_eof, draining;
    Frame8 *queue[12];
    int no_pool;
    void *alloc_ctx;
    unsigned long *alloc_mutex, alloc_mutex_store[5];
    unsigned long getbuffer_frames, getbuffer_failures;
    ArcVpp *vpp; uint8_t *vpp_p0, *vpp_p1; int vpp_pitch, vpp_w, vpp_h, vpp_kind, vpp_failed; unsigned long vpp_frames, dropped_side_data;
    int passthrough, nal_length_size;
    struct Pipe *pipe;
    unsigned long pipe_frames, pipe_waits, pipe_wait_ns;
    struct State *next;
} State;
static State *states;

static void *sym(void *handle, const char *name) {
    void *p = dlsym(handle, name);
    return p;
}
#define LOAD(A, H, F, N) do { *(void **)(&(A).F) = sym(H, N); if (!(A).F) return 0; } while (0)
static int load_api(API *a, void *h, int original) {
    LOAD((*a),h,version,"avcodec_version");
    LOAD((*a),h,util_version,"avutil_version");
    LOAD((*a),h,find,"avcodec_find_decoder_by_name");
    LOAD((*a),h,alloc,"avcodec_alloc_context3");
    LOAD((*a),h,open,"avcodec_open2");
    LOAD((*a),h,send,"avcodec_send_packet");
    LOAD((*a),h,receive,"avcodec_receive_frame");
    LOAD((*a),h,flush,"avcodec_flush_buffers");
    LOAD((*a),h,destroy,"avcodec_free_context");
    if (original) LOAD((*a),h,close,"avcodec_close");
    LOAD((*a),h,par_alloc,"avcodec_parameters_alloc");
    LOAD((*a),h,par_free,"avcodec_parameters_free");
    LOAD((*a),h,par_from,"avcodec_parameters_from_context");
    LOAD((*a),h,par_to,"avcodec_parameters_to_context");
    LOAD((*a),h,buffer_create,"av_buffer_create");
    LOAD((*a),h,frame_alloc,"av_frame_alloc");
    LOAD((*a),h,frame_free,"av_frame_free");
    LOAD((*a),h,frame_unref,"av_frame_unref");
    LOAD((*a),h,frame_clone,"av_frame_clone");
    LOAD((*a),h,frame_move,"av_frame_move_ref");
    LOAD((*a),h,frame_buffer,"av_frame_get_buffer");
    LOAD((*a),h,frame_copy,"av_frame_copy");
    LOAD((*a),h,frame_props,"av_frame_copy_props");
    LOAD((*a),h,copy_uncached,"av_image_copy_uc_from");
    LOAD((*a),h,frame_side,"av_frame_new_side_data");
    LOAD((*a),h,side_name,"av_frame_side_data_name");
    LOAD((*a),h,dict_get,"av_dict_get");
    LOAD((*a),h,dict_set,"av_dict_set");
    LOAD((*a),h,dict_free,"av_dict_free");
    LOAD((*a),h,mallocz,"av_mallocz");
    LOAD((*a),h,packet_alloc,"av_packet_alloc");
    LOAD((*a),h,packet_free,"av_packet_free");
    LOAD((*a),h,packet_unref,"av_packet_unref");
    LOAD((*a),h,packet_new,"av_new_packet");
    LOAD((*a),h,packet_side,"av_packet_new_side_data");
    LOAD((*a),h,packet_side_name,"av_packet_side_data_name");
    LOAD((*a),h,pix_name,"av_get_pix_fmt_name");
    LOAD((*a),h,pix_fmt,"av_get_pix_fmt");
    LOAD((*a),h,opt_get,"av_opt_get");
    LOAD((*a),h,opt_set,"av_opt_set");
    LOAD((*a),h,avfree,"av_free");
    return 1;
}
static void initialize(void) {
    const char *mode = getenv("RESOLVE_ARC_MODE");
    active = mode && (!strcmp(mode,"vaapi") || !strcmp(mode,"software-test"));
    software_test = mode && !strcmp(mode,"software-test");
    const char *repack=getenv("RESOLVE_ARC_REPACK");
    use_swscale=repack && !strcmp(repack,"swscale");
    const char *download=getenv("RESOLVE_ARC_DOWNLOAD");
    download_mode=download && !strcmp(download,"vpp") ? 5 :
                  download && !strcmp(download,"staged") ? 4 :
                  download && !strcmp(download,"fused") ? 3 :
                  download && !strcmp(download,"direct") ? 2 :
                  download && !strcmp(download,"map") ? 1 : 0;
    const char *depth=getenv("RESOLVE_ARC_PREFETCH");
    prefetch_depth=depth ? atoi(depth):0;
    if(prefetch_depth<0 || prefetch_depth>8) prefetch_depth=0;
    const char *cleanup=getenv("RESOLVE_ARC_ASYNC_CLEANUP");
    async_cleanup=cleanup && !strcmp(cleanup,"1");
    const char *workers=getenv("RESOLVE_ARC_WORKERS");
    repack_workers=workers ? atoi(workers):1;
    if(repack_workers!=2 && repack_workers!=4) repack_workers=1;
    const char *pipe_env=getenv("RESOLVE_ARC_PIPELINE");
    pipeline_depth=pipe_env ? atoi(pipe_env):0;
    if(pipeline_depth<2 || pipeline_depth>8) pipeline_depth=0;
    if(pipeline_depth) prefetch_depth=0;
    const char *gb_env=getenv("RESOLVE_ARC_GETBUFFER");
    use_getbuffer=gb_env && !strcmp(gb_env,"1");
    const char *dovi_env=getenv("RESOLVE_ARC_DOVI");
    dovi_hardware=dovi_env && !strcmp(dovi_env,"hardware");
    stream_capable=stream_reads_supported();
    const char *stream_test=getenv("ARC_TEST_STREAM_READ");
    test_stream=stream_capable && stream_test && !strcmp(stream_test,"1");
    const char *log = getenv("RESOLVE_ARC_LOG");
    if (log) fd = open(log,656449,0600);
    if (!load_api(&r,(void *)-1,1)) {
        dprintf(2,"Arc bridge: original FFmpeg symbols missing; cannot forward safely.\n");
        _exit(127);
    }
    char executable[4096];
    long n=readlink("/proc/self/exe",executable,sizeof(executable)-1);
    if(n<0) n=0;
    executable[n]=0;
    if (fd >= 0) dprintf(fd,"event=loaded pid=%d mode=%s codec_version=%u util_version=%u executable=%s\n",
                        getpid(), active ? (software_test ? "software-test" : "vaapi") : "disabled",r.version(),r.util_version(),executable);
    /* The public ABI views are specific to the installed Resolve FFmpeg 6.0. */
    if (r.version() != 0x3c0364 || r.util_version() != 0x3a0264) {
        if(fd>=0) dprintf(fd,"event=unsupported_original_abi\n");
        active = 0;
    }
}
static int load_backend(void) {
    /* The root DSO must come first in this namespace's dependency group:
     * its dlopen wrapper protects libva's RTLD_GLOBAL driver loads. */
    struct { const char *filename; void *base; const char *symbol; void *address; } info;
    char root_path[4096];
    if(!dladdr((const void *)load_backend,&info)) goto fail;
    size_t end=0, length=strlen(info.filename);
    for(size_t i=0;i<length;i++) if(info.filename[i]=='/') end=i+1;
    const char suffix[]="libarc_namespace.so";
    if(!end || end+sizeof(suffix)>sizeof(root_path)) goto fail;
    memcpy(root_path,info.filename,end);
    memcpy(root_path+end,suffix,sizeof(suffix));
    void *h = dlmopen(-1,root_path,2);
    if (!h || !load_api(&b,h,0)) goto fail;
    if (b.version() != 0x3e0b64 || b.util_version() != 0x3c0864) goto fail;
    long ns = 0;
    if (dlinfo(h,1,&ns)) goto fail;
    LOAD(b,h,sws_get,"sws_getCachedContext");
    LOAD(b,h,sws_scale,"sws_scale");
    LOAD(b,h,sws_free,"sws_freeContext");
    LOAD(b,h,device_type,"av_hwdevice_find_type_by_name");
    LOAD(b,h,device_create,"av_hwdevice_ctx_create");
    LOAD(b,h,transfer,"av_hwframe_transfer_data");
    LOAD(b,h,map,"av_hwframe_map");
    vaapi = b.pix_fmt("vaapi");
    ns_handle = h; vpp_ok = arc_vpp_init(ns_lookup);
    if (fd >= 0) dprintf(fd,"event=vpp_api available=%d\n",vpp_ok);
    backend_ok = 1;
    if (fd >= 0) dprintf(fd,"event=backend namespace=%ld codec_version=%u util_version=%u isolated=1 driver_loader=local\n",ns,b.version(),b.util_version());
    return 1;
fail:
    if (fd >= 0) dprintf(fd,"event=backend_failed detail=%s\n",dlerror());
    return 0;
}
static void initialize_backend(void) { (void)load_backend(); }
static State *lookup(void *ctx) {
    for (State *s=states; s; s=s->next) if (s->original == ctx) return s;
    return NULL;
}
static State *acquire(void *ctx) {
    pthread_mutex_lock(mutex);
    State *s=lookup(ctx);
    if(s) s->users++;
    pthread_mutex_unlock(mutex);
    if(s) {
        unsigned long t=now_ns();
        pthread_mutex_lock(s->operation_mutex);
        s->wait_ns+=now_ns()-t;
    }
    return s;
}
static void release(State *s) {
    pthread_mutex_unlock(s->operation_mutex);
    pthread_mutex_lock(mutex);
    if(!--s->users) pthread_cond_broadcast(s->idle);
    pthread_mutex_unlock(mutex);
}
static void profile(State *s) {
    unsigned long background=0, jobs=0;
    if(s->cleanup_pool) {
        pthread_mutex_lock(s->cleanup_pool->mutex);
        background=s->cleanup_pool->elapsed_ns; jobs=s->cleanup_pool->jobs;
        pthread_mutex_unlock(s->cleanup_pool->mutex);
    }
    if(fd>=0 && s->frames)
        dprintf(fd,"event=profile ctx=%p frames=%lu width=%d height=%d format=%s send_ns=%lu receive_ns=%lu download_ns=%lu allocate_ns=%lu repack_ns=%lu packet_copy_ns=%lu wait_ns=%lu repacker=%s map_ns=%lu uncached_copy_ns=%lu cleanup_ns=%lu direct_frames=%lu mapped_frames=%lu transferred_frames=%lu fused_frames=%lu prefetch=%d workers=%d staged_frames=%lu async_cleanup=%d background_cleanup_ns=%lu cleanup_jobs=%lu api_ns=%lu send_calls=%lu receive_calls=%lu receive_again=%lu pipeline=%d pipe_frames=%lu pipe_waits=%lu pipe_wait_ns=%lu getbuffer_frames=%lu getbuffer_failures=%lu vpp_frames=%lu\n",
                s->original,s->frames,s->profile_width,s->profile_height,r.pix_name(s->profile_format),s->send_ns,s->receive_ns,s->download_ns,s->allocate_ns,
                s->repack_ns,s->copy_ns,s->wait_ns,use_swscale ? "swscale":"simd",
                s->map_ns,s->uncached_copy_ns,s->cleanup_ns,s->direct_frames,s->mapped_frames,s->transferred_frames,s->fused_frames,prefetch_depth,s->repack_pool ? s->repack_pool->count:1,s->staged_frames,!!s->cleanup_pool,background,jobs,s->api_ns,s->send_calls,s->receive_calls,s->receive_again,pipeline_depth,s->pipe_frames,s->pipe_waits,s->pipe_wait_ns,s->getbuffer_frames,s->getbuffer_failures,s->vpp_frames);
}
static int copy_dict(API *from, API *to, void **dst, const void *src) {
    struct Entry { char *key, *value; } *e = NULL;
    while ((e=from->dict_get(src,"",e,2))) {
        int ret=to->dict_set(dst,e->key,e->value,0);
        if (ret<0) return ret;
    }
    return 0;
}
static int get_format(Ctx8 *ctx, const int *formats) {
    (void)ctx;
    for (const int *p=formats; *p!=-1; ++p) if (*p==vaapi) {
        if (fd>=0) dprintf(fd,"event=hardware_format ctx=%p format=vaapi\n",ctx);
        return *p;
    }
    if (fd>=0) dprintf(fd,"event=hardware_format_failed ctx=%p software_fallback=0\n",ctx);
    return -1;
}
static void clear_queue(State *s) {
    for(int i=0;i<12;i++) if(s->queue[i]) b.frame_free((void **)&s->queue[i]);
    s->queue_head=0; s->queue_count=0; s->backend_eof=0; s->draining=0;
}
static void pipe_free(struct Pipe *p);
static struct Pipe *pipe_new(State *owner);
static void dispose(State *s) {
    if (!s) return;
    cleanup_wait(s->cleanup_pool);
    profile(s);
    cleanup_free(s->cleanup_pool);
    if (fd>=0) dprintf(fd,"event=total ctx=%p packets=%lu frames=%lu hardware_frames=%lu flushes=%lu failed=%d\n",
                     s->original,s->packets,s->frames,s->hardware_frames,s->flushes,s->failed);
    pipe_free(s->pipe); s->pipe=NULL;
    arc_vpp_free(s->vpp); s->vpp=NULL;
    repack_pool_free(s->repack_pool);
    clear_queue(s);
    if(s->decoder) b.destroy((void **)&s->decoder);
    if(s->gpu) b.frame_free((void **)&s->gpu);
    if(s->cpu) b.frame_free((void **)&s->cpu);
    if(s->mapped) b.frame_free((void **)&s->mapped);
    if(s->packet) b.packet_free(&s->packet);
    if(s->sws) b.sws_free(s->sws);
    free(s);
}
static void remove_state(void *ctx) {
    pthread_mutex_lock(mutex);
    State **p=&states;
    while (*p && (*p)->original!=ctx) p=&(*p)->next;
    State *s=*p;
    if(s) {
        *p=s->next;
        while(s->users) pthread_cond_wait(s->idle,mutex);
    }
    pthread_mutex_unlock(mutex);
    dispose(s);
}
static State *make_state(void *ctx, void *options) {
    pthread_once(&backend_once,initialize_backend);
    if(!backend_ok) return NULL;
    Par6 *p6=r.par_alloc(); Par8 *p8=b.par_alloc();
    State *s=calloc(1,sizeof(*s));
    void *opts=NULL;
    if (!p6 || !p8 || !s) goto fail;
    s->original=ctx; s->target=-1; s->alloc_ctx=ctx; s->alloc_mutex=s->alloc_mutex_store;
    if(r.par_from(p6,ctx)<0) goto fail;
    if(p6->codec_type!=0 || p6->codec_id!=173 || p6->extradata_size<0) goto fail;
    const char *target=r.pix_name(p6->format);
    if(target) {
        if(strcmp(target,"yuv420p") && strcmp(target,"yuv420p10le") &&
           strcmp(target,"yuv422p") && strcmp(target,"yuv422p10le")) {
            if(fd>=0) dprintf(fd,"event=unsupported_input_format format=%s\n",target);
            goto fail;
        }
        s->target=p6->format;
    }
    /* Translate scalar fields, never memcpy an ABI-dependent object. */
#define CP(F) p8->F=p6->F
    CP(codec_type); CP(codec_id); CP(codec_tag); CP(bit_rate);
    CP(bits_per_coded_sample); CP(bits_per_raw_sample); CP(profile); CP(level);
    CP(width); CP(height); CP(sample_aspect_ratio); CP(field_order);
    CP(color_range); CP(color_primaries); CP(color_trc); CP(color_space);
    CP(chroma_location); CP(video_delay);
#undef CP
    p8->format=target ? b.pix_fmt(target) : -1;
    s->nal_length_size=0; /* 0 = Annex B */
    if(p6->extradata_size>=23 && p6->extradata[0]==1) s->nal_length_size=(p6->extradata[21]&3)+1;
    if(p6->extradata_size) {
        p8->extradata=b.mallocz((size_t)p6->extradata_size+64);
        if(!p8->extradata) goto fail;
        p8->extradata_size=p6->extradata_size;
        memcpy(p8->extradata,p6->extradata,p6->extradata_size);
    }
    const void *codec=b.find("hevc");
    s->decoder=b.alloc(codec);
    if(!s->decoder || b.par_to(s->decoder,p8)<0) goto fail;
    /* FFmpeg 6 and 8 share these documented AVOptions. */
    const char *names[]={"flags","flags2","skip_frame","skip_loop_filter","skip_idct","err_detect","pkt_timebase",NULL};
    for(int i=0;names[i];i++) {
        uint8_t *value=NULL;
        if(r.opt_get(ctx,names[i],0,&value)>=0 && value) {
            int ret=b.opt_set(s->decoder,names[i],(char *)value,0);
            r.avfree(value);
            if(ret<0) goto fail;
        }
    }
    if (copy_dict(&r,&b,&opts,options)<0) goto fail;
    if(!software_test) {
        const char *device=getenv("RESOLVE_ARC_DEVICE");
        if(!device) device="/dev/dri/renderD128";
        if(fd>=0) dprintf(fd,"event=device_begin device=%s\n",device);
        int ret=b.device_create(&s->decoder->hw_device_ctx,b.device_type("vaapi"),device,NULL,0);
        if(ret<0) {
            if(fd>=0) dprintf(fd,"event=device_failed device=%s error=%d\n",device,ret);
            goto fail;
        }
        if(fd>=0) dprintf(fd,"event=device_ready device=%s\n",device);
        s->decoder->get_format=get_format;
        b.opt_set(s->decoder,"threads","1",0);
        if(prefetch_depth) b.opt_set(s->decoder,"extra_hw_frames","8",0);
        if(pipeline_depth) { char n[4]; int e=pipeline_depth+4; n[0]=(char)('0'+e/10); n[1]=(char)('0'+e%10); n[2]=0; b.opt_set(s->decoder,"extra_hw_frames",n[0]=='0'?n+1:n,0); }
    }
    if(b.open(s->decoder,codec,&opts)<0) goto fail;
    s->gpu=b.frame_alloc(); s->cpu=b.frame_alloc(); s->mapped=b.frame_alloc(); s->packet=b.packet_alloc();
    if(!s->gpu || !s->cpu || !s->mapped || !s->packet) goto fail;
    if(async_cleanup && !pipeline_depth) s->cleanup_pool=cleanup_new();
    if(pipeline_depth && !(s->pipe=pipe_new(s))) goto fail;
    r.par_free((void **)&p6); b.par_free((void **)&p8); b.dict_free(&opts);
    return s;
fail:
    if(p6) r.par_free((void **)&p6);
    if(p8) b.par_free((void **)&p8);
    b.dict_free(&opts); dispose(s);
    return NULL;
}
EXPORT int avcodec_open2(void *ctx, const void *codec, void **options) {
    pthread_once(&once,initialize);
    const char *name=codec ? *(const char *const *)codec : NULL;
    if(!active || !name || strcmp(name,"hevc")) return r.open(ctx,codec,options);
    pthread_mutex_lock(mutex);
    if(lookup(ctx)) { pthread_mutex_unlock(mutex); return r.open(ctx,codec,options); }
    pthread_mutex_unlock(mutex);
    State *s=make_state(ctx,options ? *options : NULL);
    if(!s) {
        if(fd>=0) dprintf(fd,"event=open_failed ctx=%p software_fallback=0\n",ctx);
        return -5;
    }
    int ret=r.open(ctx,codec,options);
    if(ret>=0 && prefetch_depth) ((Ctx6 *)ctx)->delay+=prefetch_depth;
    if(ret>=0 && pipeline_depth) ((Ctx6 *)ctx)->delay+=pipeline_depth;
    pthread_mutex_lock(mutex);
    if(ret>=0) {
        s->next=states; states=s;
        if(fd>=0) dprintf(fd,"event=open ctx=%p backend=%s\n",ctx,software_test ? "software-test" : "vaapi");
    } else dispose(s);
    pthread_mutex_unlock(mutex);
    return ret;
}
static int packet_type(int old) {
    const char *name=r.packet_side_name(old);
    if(!name) return -1;
    for(int i=0;i<50;i++) {
        const char *n=b.packet_side_name(i);
        if(n && !strcmp(n,name)) return i;
    }
    return -1;
}
/* Dolby Vision RPUs travel in HEVC NAL unit type 62 (UNSPEC62). */
static int has_dovi_rpu(const State *s, const uint8_t *d, int size) {
    if(!d || size<=0) return 0;
    if(s->nal_length_size) {
        int n=s->nal_length_size;
        for(long i=0;i+n<size;) {
            unsigned long len=0;
            for(int k=0;k<n;k++) len=(len<<8)|d[i+k];
            i+=n;
            if(len==0 || i+(long)len>size) break;
            if(((d[i]>>1)&0x3f)==62) return 1;
            i+=(long)len;
        }
    } else {
        for(long i=0;i+3<size;i++) if(d[i]==0 && d[i+1]==0 && d[i+2]==1 && ((d[i+3]>>1)&0x3f)==62) return 1;
    }
    return 0;
}
EXPORT int avcodec_send_packet(void *ctx, const Packet *src) {
    pthread_once(&once,initialize);
    State *s=acquire(ctx);
    if(!s) return r.send(ctx,src);
    if(!s->passthrough && !dovi_hardware && !s->packets && src && has_dovi_rpu(s,src->data,src->size)) {
        /* Nothing has reached either decoder yet, so Resolve's own decoder can
         * take over cleanly and keep its Dolby Vision metadata exact. */
        s->passthrough=1;
        if(pipeline_depth) ((Ctx6 *)ctx)->delay-=pipeline_depth;
        if(prefetch_depth) ((Ctx6 *)ctx)->delay-=prefetch_depth;
        if(fd>=0) dprintf(fd,"event=dovi_passthrough ctx=%p decoder=resolve\n",ctx);
    }
    if(s->passthrough) { release(s); return r.send(ctx,src); }
    unsigned long api_start=now_ns(); s->send_calls++;
    int ret=-5;
    unsigned long t;
    if(s->failed) goto done;
    if(!src) { t=now_ns(); ret=b.send(s->decoder,NULL); s->send_ns+=now_ns()-t; if(ret>=0) s->draining=1; goto done; }
    if(src->opaque_ref || src->size<0 || src->side_data_elems<0) { ret=-38; goto done; }
    if(src->size && !src->data) { ret=-22; goto done; }
    Packet *p=s->packet;
    t=now_ns();
    b.packet_unref(p);
    if(src->data && (ret=b.packet_new(p,src->size))<0) goto done;
    if(src->size) memcpy(p->data,src->data,src->size);
    p->pts=src->pts; p->dts=src->dts; p->duration=src->duration;
    p->pos=src->pos; p->flags=src->flags; p->stream_index=src->stream_index;
    p->time_base=src->time_base; p->opaque=src->opaque;
    for(int i=0;i<src->side_data_elems;i++) {
        PacketSideData *d=&src->side_data[i]; int type=packet_type(d->type);
        if(type<0) { ret=-38; goto done; }
        uint8_t *dst=b.packet_side(p,type,d->size);
        if(!dst) { ret=-12; goto done; }
        memcpy(dst,d->data,d->size);
    }
    s->copy_ns+=now_ns()-t;
    t=now_ns(); ret=b.send(s->decoder,p); s->send_ns+=now_ns()-t;
    if(ret>=0) s->packets++;
done:
    if(ret<0 && ret!=-11 && ret!=-541478725 && !s->failed) {
        s->failed=1;
        if(fd>=0) dprintf(fd,"event=send_failed ctx=%p error=%d\n",ctx,ret);
    }
    s->api_ns+=now_ns()-api_start;
    release(s);
    return ret;
}
static int side_type(int modern) {
    const char *name=b.side_name(modern);
    if(!name) return -1;
    /* Byte or fixed-layout side data only. Reject evolving structures. */
    const char *allowed[]={"Mastering display metadata","Content light level metadata","ICC profile","H.26[45] User Data Unregistered SEI message","3x3 displaymatrix","SMPTE 12-1 timecode","ATSC A53 Part 4 Closed Captions","Ambient viewing environment","Dolby Vision RPU Data",NULL};
    int safe=0;
    for(int i=0;allowed[i];i++) if(!strcmp(name,allowed[i])) safe=1;
    if(!safe) return -1;
    for(int i=0;i<28;i++) {
        const char *n=r.side_name(i);
        if(n && !strcmp(n,name)) return i;
    }
    return -1;
}
/* Read a mapped surface using FFmpeg's cache-aware copy, keeping all refs in
 * the backend ABI. Ordinary memcpy may be extremely slow for GPU mappings. */
static int cache_mapped_pixels(State *s, const Frame8 *mapped) {
    Frame8 *dst=s->cpu;
    b.frame_unref(dst);
    dst->format=mapped->format; dst->width=mapped->width; dst->height=mapped->height;
    int ret=b.frame_buffer(dst,64);
    if(ret<0) return ret;
    long dl[4], sl[4]; int aligned=1;
    for(int i=0;i<4;i++) {
        dl[i]=dst->linesize[i]; sl[i]=mapped->linesize[i];
        if(dst->data[i] && (((size_t)dst->data[i]&63) || ((size_t)mapped->data[i]&63) || (dl[i]&63) || (sl[i]&63))) aligned=0;
    }
    if(aligned) b.copy_uncached(dst->data,dl,(const uint8_t *const *)mapped->data,sl,
                               mapped->format,mapped->width,mapped->height);
    else ret=b.frame_copy(dst,mapped);
    return ret;
}
static const char *vpp_names[3]={"y210le","p010le","nv12"};
/* GPU blit into a CPU-cached user-pointer surface; s->cpu becomes a
 * non-owning view of that buffer. Returns <0 to request the fallback route. */
static int vpp_readback(State *s, const Frame8 *src) {
    if(!s->vpp_kind) {
        int kind=-1;
        if(b.map(s->mapped,src,9)>=0) {
            const char *f=b.pix_name(s->mapped->format);
            for(int i=0;i<3 && f;i++) if(!strcmp(f,vpp_names[i])) kind=i;
        }
        b.frame_unref(s->mapped);
        s->vpp_kind=kind+1; /* 0 = not probed yet, 1..3 = route kind, -1 = unsupported */
        if(kind<0) { s->vpp_kind=-1; if(fd>=0) dprintf(fd,"event=vpp_unsupported_format fallback=fused\n"); }
    }
    if(s->vpp_kind<1) return -38;
    if(!s->vpp || s->vpp_w!=src->width || s->vpp_h!=src->height) {
        arc_vpp_free(s->vpp); s->vpp=NULL;
        void **dev=*(void ***)((char *)s->decoder->hw_device_ctx+8);   /* AVBufferRef.data -> AVHWDeviceContext */
        void *dpy=*(void **)(*(char **)((char *)dev+16));                /* ->hwctx -> AVVAAPIDeviceContext.display */
        s->vpp=arc_vpp_new(dpy,s->vpp_kind-1,src->width,src->height,&s->vpp_p0,&s->vpp_p1,&s->vpp_pitch);
        if(!s->vpp) { s->vpp_failed=1; if(fd>=0) dprintf(fd,"event=vpp_setup_failed fallback=fused\n"); return -38; }
        s->vpp_w=src->width; s->vpp_h=src->height;
    }
    if(arc_vpp_blit(s->vpp,(unsigned)(unsigned long)src->data[3])<0) {
        s->vpp_failed=1; if(fd>=0) dprintf(fd,"event=vpp_blit_failed fallback=fused\n"); return -38;
    }
    Frame8 *v=s->cpu;
    v->format=b.pix_fmt(vpp_names[s->vpp_kind-1]); v->width=src->width; v->height=src->height;
    v->data[0]=s->vpp_p0; v->data[1]=s->vpp_p1; v->linesize[0]=s->vpp_pitch; v->linesize[1]=s->vpp_p1 ? s->vpp_pitch:0;
    s->vpp_frames++;
    return 0;
}
static int readback(State *s, const Frame8 *src) {
    unsigned long all=now_ns(), t;
    int ret=-38;
    s->fused=0;
    b.frame_unref(s->cpu); b.frame_unref(s->mapped);
    if(download_mode==5 && vpp_ok && !s->vpp_failed && !software_test) {
        t=now_ns(); ret=vpp_readback(s,src); s->map_ns+=now_ns()-t;
        if(ret>=0) { s->download_ns+=now_ns()-all; return 0; }
        b.frame_unref(s->cpu); b.frame_unref(s->mapped);
    }
    if(download_mode) {
        t=now_ns();
        /* READ alone asks FFmpeg for a separate image populated by vaGetImage.
         * DIRECT derives an image from the decoder surface instead. Staged
         * conversion reads the separate image without another CPU copy. */
        int direct=download_mode==2 || download_mode==3 || download_mode==5;
        ret=b.map(s->mapped,src,direct ? 9:1); /* READ | optional DIRECT */
        s->map_ns+=now_ns()-t;
        if(ret>=0) {
            const char *fmt=b.pix_name(s->mapped->format);
            int supported=fmt && (!strcmp(fmt,"y210le") || !strcmp(fmt,"p210le") || !strcmp(fmt,"p010le") || !strcmp(fmt,"nv12"));
            if((download_mode==3 || download_mode==4 || download_mode==5) && stream_capable && !use_swscale && supported) {
                s->fused=1; s->fused_frames++;
                if(download_mode==4) s->staged_frames++;
            } else {
                t=now_ns(); ret=cache_mapped_pixels(s,s->mapped); s->uncached_copy_ns+=now_ns()-t;
            }
            if(ret>=0) {
                if(direct) s->direct_frames++; else s->mapped_frames++;
            }
        } else {
            if(fd>=0 && s->frames<1) dprintf(fd,"event=map_unavailable ctx=%p requested=%s error=%d fallback=transfer\n",
                                          s->original,direct ? "direct":"map",ret);
            b.frame_unref(s->mapped); b.frame_unref(s->cpu);
            ret=b.transfer(s->cpu,src,0);
            if(ret>=0) s->transferred_frames++;
        }
    } else {
        ret=b.transfer(s->cpu,src,0);
        if(ret>=0) s->transferred_frames++;
    }
    s->download_ns+=now_ns()-all;
    return ret;
}
static int export_frame(State *s, Frame6 *dst) {
    Frame8 *src=s->gpu;
    if(!software_test) {
        if(src->format!=vaapi || !src->hw_frames_ctx) return -5;
        int ret=readback(s,src);
        if(ret<0) return ret;
        s->hardware_frames++;
    }
    Frame8 *pixels=software_test ? src : (s->fused ? s->mapped:s->cpu);
    const char *fmt=b.pix_name(pixels->format);
    if(!fmt) return -38;
    const char *out=NULL;
    if(!strcmp(fmt,"p010le") || !strcmp(fmt,"yuv420p10le")) out="yuv420p10le";
    else if(!strcmp(fmt,"nv12") || !strcmp(fmt,"yuv420p")) out="yuv420p";
    else if(!strcmp(fmt,"y210le") || !strcmp(fmt,"p210le") || !strcmp(fmt,"yuv422p10le")) out="yuv422p10le";
    else if(!strcmp(fmt,"nv16") || !strcmp(fmt,"yuyv422") || !strcmp(fmt,"uyvy422") || !strcmp(fmt,"yuv422p")) out="yuv422p";
    else {
        if(fd>=0) dprintf(fd,"event=unsupported_download_format format=%s\n",fmt);
        return -38;
    }
    int target=r.pix_fmt(out);
    if(s->target>=0 && target!=s->target) {
        if(fd>=0) dprintf(fd,"event=unsupported_format_change expected=%s decoded=%s downloaded=%s\n",r.pix_name(s->target),out,fmt);
        return -38;
    }
    r.frame_unref(dst);
    dst->format=target; dst->width=src->width; dst->height=src->height;
    unsigned long t=now_ns();
    int ret=-1, via_getbuffer=0;
    /* Allocate through Resolve's own AVCodecContext.get_buffer2 (FFmpeg 6.0
     * offset 408, verified against avcodec_default_get_buffer2), exactly as
     * its software decoder does, so frames land in Resolve-managed buffers. */
    if(use_getbuffer && s->alloc_ctx) {
        int (*gb)(void *, Frame6 *, int)=*(int (**)(void *, Frame6 *, int))((char *)s->alloc_ctx+408);
        if(gb) {
            pthread_mutex_lock(s->alloc_mutex);
            ret=gb(s->alloc_ctx,dst,1); /* AV_GET_BUFFER_FLAG_REF */
            pthread_mutex_unlock(s->alloc_mutex);
            if(ret>=0 && dst->data[0] && dst->format==target && dst->width>=src->width && dst->height>=src->height) { via_getbuffer=1; s->getbuffer_frames++; }
            else { if(ret>=0) r.frame_unref(dst); s->getbuffer_failures++;
                   if(fd>=0 && s->getbuffer_failures==1) dprintf(fd,"event=getbuffer_failed ret=%d fallback=av_frame_get_buffer\n",ret);
                   dst->format=target; dst->width=src->width; dst->height=src->height; }
        }
    }
    if(!via_getbuffer) ret=r.frame_buffer(dst,64);
    s->allocate_ns+=now_ns()-t;
    if(ret<0) return ret;
    t=now_ns();
    int streaming=(!software_test && s->fused) || (software_test && test_stream);
    if(streaming) __asm__ volatile("mfence" ::: "memory");
    if(!use_swscale && repack_workers>1 && !s->repack_pool && !s->no_pool)
        s->repack_pool=repack_pool_new(repack_workers);
    int handled=!use_swscale && parallel_repack(s->repack_pool,pixels,dst,fmt,out,streaming);
    if(streaming) __asm__ volatile("mfence" ::: "memory");
    if(!handled) {
        s->sws=b.sws_get(s->sws,src->width,src->height,pixels->format,
                    src->width,src->height,b.pix_fmt(out),0x80010,NULL,NULL,NULL);
        if(!s->sws) return -12;
        if(b.sws_scale(s->sws,(const uint8_t *const *)pixels->data,pixels->linesize,0,
                   src->height,dst->data,dst->linesize)!=src->height) return -5;
    }
    s->repack_ns+=now_ns()-t;
#define CP(F) dst->F=src->F
    void *kept_opaque=dst->opaque;
    CP(pict_type); CP(sample_aspect_ratio); CP(pts); CP(pkt_dts); CP(time_base);
    CP(quality); CP(opaque); CP(repeat_pict);
    if(via_getbuffer) dst->opaque=kept_opaque; /* Resolve allocator owns this field */
    CP(flags); CP(color_range);
    CP(color_primaries); CP(color_trc); CP(colorspace); CP(chroma_location);
    CP(best_effort_timestamp); CP(decode_error_flags);
    CP(crop_top); CP(crop_bottom); CP(crop_left); CP(crop_right); CP(duration);
#undef CP
    dst->key_frame=!!(src->flags&2);
    dst->interlaced_frame=!!(src->flags&8);
    dst->top_field_first=!!(src->flags&16);
    dst->pkt_duration=src->duration;
    if((ret=copy_dict(&b,&r,&dst->metadata,src->metadata))<0) return ret;
    for(int i=0;i<src->nb_side_data;i++) {
        SideData *in=src->side_data[i]; int type=side_type(in->type);
        if(type<0) {
            /* Version-dependent structures (e.g. Dolby Vision metadata, HDR10+)
             * cannot be translated safely between ABIs: drop them, keep the frame. */
            s->dropped_side_data++;
            if(fd>=0 && s->dropped_side_data<=4) dprintf(fd,"event=dropped_side_data name=%s\n",b.side_name(in->type));
            continue;
        }
        SideData *sd=r.frame_side(dst,type,in->size);
        if(!sd) return -12;
        memcpy(sd->data,in->data,in->size);
        if((ret=copy_dict(&b,&r,&sd->metadata,in->metadata))<0) return ret;
    }
    /* Resolve reads the decoder context as well as AVFrame. The original
     * decoder is initialized but never consumes packets, so it cannot update
     * these fields itself when dimensions/format arrive in the bitstream. */
    if(s->original) {
        Ctx6 *ctx=s->original;
        ctx->width=dst->width; ctx->height=dst->height; ctx->pix_fmt=dst->format;
        ctx->coded_width=s->decoder->coded_width;
        ctx->coded_height=s->decoder->coded_height;
        ctx->delay=s->decoder->delay+prefetch_depth;
    }
    return 0;
}
#include "pipeline.h"
/* Drain the backend fully before withholding output. This preserves the
 * send/receive progress guarantee while giving queued GPU work a head start. */
static int receive_backend(State *s) {
    if(!prefetch_depth) return b.receive(s->decoder,s->gpu);
    while(s->queue_count<12 && !s->backend_eof) {
        int ret=b.receive(s->decoder,s->gpu);
        if(ret==-541478725) { s->backend_eof=1; break; }
        if(ret==-11) break;
        if(ret<0) return ret;
        Frame8 *held=b.frame_clone(s->gpu);
        b.frame_unref(s->gpu);
        if(!held) return -12;
        int slot=(s->queue_head+s->queue_count)%12;
        s->queue[slot]=held; s->queue_count++;
    }
    if(s->queue_count && (s->queue_count>prefetch_depth || s->draining || s->backend_eof)) {
        Frame8 *held=s->queue[s->queue_head];
        s->queue[s->queue_head]=NULL;
        s->queue_head=(s->queue_head+1)%12; s->queue_count--;
        b.frame_move(s->gpu,held); b.frame_free((void **)&held);
        return 0;
    }
    return s->backend_eof && !s->queue_count ? -541478725:-11;
}
EXPORT int avcodec_receive_frame(void *ctx, Frame6 *dst) {
    pthread_once(&once,initialize);
    State *s=acquire(ctx);
    if(!s) return r.receive(ctx,dst);
    if(s->passthrough) { release(s); return r.receive(ctx,dst); }
    unsigned long api_start=now_ns(); s->receive_calls++;
    r.frame_unref(dst);
    if(s->pipe) {
        int pr=s->failed ? -5 : pipe_receive(s,dst);
        if(pr>=0) { s->frames++; s->profile_width=dst->width; s->profile_height=dst->height; s->profile_format=dst->format;
            if(s->frames==1 || s->frames%60==0) profile(s);
            if(fd>=0 && (s->frames<=3 || s->frames%120==0))
                dprintf(fd,"event=frame ctx=%p number=%lu hardware_frames=%lu width=%d height=%d format=%s pts=%ld pipeline=%d\n",ctx,s->frames,s->hardware_frames,dst->width,dst->height,r.pix_name(dst->format),dst->pts,pipeline_depth); }
        if(pr<0 && pr!=-11 && pr!=-541478725 && !s->failed) { s->failed=1; r.frame_unref(dst); if(fd>=0) dprintf(fd,"event=receive_failed ctx=%p error=%d pipeline=1\n",ctx,pr); }
        if(pr==-11) s->receive_again++;
        s->api_ns+=now_ns()-api_start;
        release(s);
        return pr;
    }
    unsigned long t=now_ns();
    int ret=s->failed ? -5 : receive_backend(s);
    s->receive_ns+=now_ns()-t;
    if(ret>=0) {
        ret=export_frame(s,dst);
        t=now_ns();
        if(s->cleanup_pool) cleanup_submit(s->cleanup_pool,&s->mapped,&s->gpu,&s->cpu);
        else { b.frame_unref(s->mapped); b.frame_unref(s->gpu); b.frame_unref(s->cpu); }
        s->cleanup_ns+=now_ns()-t;
        if(ret>=0) {
            s->frames++;
            s->profile_width=dst->width; s->profile_height=dst->height; s->profile_format=dst->format;
            if(s->frames==1 || s->frames%60==0) profile(s);
            if(fd>=0 && (s->frames<=3 || s->frames%120==0))
                dprintf(fd,"event=frame ctx=%p number=%lu hardware_frames=%lu width=%d height=%d format=%s pts=%ld primaries=%d transfer=%d colorspace=%d\n",
                        ctx,s->frames,s->hardware_frames,dst->width,dst->height,r.pix_name(dst->format),dst->pts,dst->color_primaries,dst->color_trc,dst->colorspace);
        }
    }
    if(ret<0 && ret!=-11) {
        t=now_ns(); cleanup_wait(s->cleanup_pool); s->cleanup_ns+=now_ns()-t;
    }
    if(ret<0 && ret!=-11 && ret!=-541478725 && !s->failed) {
        s->failed=1; r.frame_unref(dst);
        if(fd>=0) dprintf(fd,"event=receive_failed ctx=%p error=%d\n",ctx,ret);
    }
    if(ret==-11) s->receive_again++;
    s->api_ns+=now_ns()-api_start;
    release(s);
    return ret;
}
EXPORT void avcodec_flush_buffers(void *ctx) {
    pthread_once(&once,initialize);
    State *s=acquire(ctx);
    if(s && s->passthrough) { release(s); r.flush(ctx); return; }
    if(s) {
        unsigned long t=now_ns(); cleanup_wait(s->cleanup_pool); s->cleanup_ns+=now_ns()-t;
        clear_queue(s);
        if(s->pipe) pipe_flush(s->pipe);
        b.flush(s->decoder); b.frame_unref(s->mapped); b.frame_unref(s->gpu); b.frame_unref(s->cpu);
        b.packet_unref(s->packet); s->failed=0; s->flushes++;
        if(fd>=0) dprintf(fd,"event=flush ctx=%p count=%lu\n",ctx,s->flushes);
    }
    if(s) release(s);
    r.flush(ctx);
}
EXPORT void avcodec_free_context(void **ctx) {
    pthread_once(&once,initialize);
    if(ctx) remove_state(*ctx);
    r.destroy(ctx);
}
EXPORT int avcodec_close(void *ctx) {
    pthread_once(&once,initialize);
    remove_state(ctx);
    return r.close(ctx);
}
__attribute__((destructor)) static void report_remaining(void) {
    /* Do not destroy decoder resources during shared-library finalization. */
    if(fd<0) return;
    for(State *s=states;s;s=s->next) {
        profile(s);
        dprintf(fd,"event=exit_total ctx=%p packets=%lu frames=%lu hardware_frames=%lu flushes=%lu failed=%d\n",
                s->original,s->packets,s->frames,s->hardware_frames,s->flushes,s->failed);
    }
}
