/* Test-only entrypoint for Intel's semiplanar -> Resolve planar conversion. */
#include "bridge.c"
EXPORT int arc_transfer_test(Frame6 *input, Frame6 *output) {
    pthread_once(&once,initialize);
    pthread_once(&backend_once,initialize_backend);
    if(!backend_ok) return -5;
    software_test=1;
    const char *name=r.pix_name(input->format), *packed=NULL;
    if(name && !strcmp(name,"yuv420p10le")) packed="p010le";
    if(name && !strcmp(name,"yuv420p")) packed="nv12";
    if(name && !strcmp(name,"yuv422p10le")) packed="y210le";
    if(name && !strcmp(name,"yuv422p")) packed="yuyv422";
    const char *requested=getenv("ARC_TEST_PACKED");
    if(requested) {
        if(name && !strcmp(name,"yuv422p10le") && !strcmp(requested,"p210le")) packed=requested;
        else return -38;
    }
    if(!packed) return -38;
    Frame8 *f=b.frame_alloc();
    if(!f) return -12;
    int ret;
    f->width=input->width; f->height=input->height; f->format=b.pix_fmt(packed);
    if((ret=b.frame_buffer(f,64))<0) goto end;
    void *sws=b.sws_get(NULL,f->width,f->height,b.pix_fmt(name),f->width,f->height,
                        f->format,0x80010,NULL,NULL,NULL);
    if(!sws) { ret=-12; goto end; }
    ret=b.sws_scale(sws,(const uint8_t *const *)input->data,input->linesize,0,
                    f->height,f->data,f->linesize);
    b.sws_free(sws);
    if(ret!=f->height) { ret=-5; goto end; }
#define CP(F) f->F=input->F
    CP(pict_type); CP(sample_aspect_ratio); CP(pts); CP(pkt_dts); CP(time_base);
    CP(quality); CP(opaque); CP(repeat_pict); CP(color_range); CP(color_primaries);
    CP(color_trc); CP(colorspace); CP(chroma_location); CP(best_effort_timestamp);
    CP(decode_error_flags); CP(crop_top); CP(crop_bottom); CP(crop_left); CP(crop_right);
#undef CP
    f->duration=input->pkt_duration;
    f->flags=input->flags | (input->key_frame ? 2:0) | (input->interlaced_frame ? 8:0) | (input->top_field_first ? 16:0);
    if((ret=copy_dict(&r,&b,&f->metadata,input->metadata))<0) goto end;
    for(int i=0;i<input->nb_side_data;i++) {
        SideData *in=input->side_data[i];
        const char *n=r.side_name(in->type); int type=-1;
        for(int j=0;j<50;j++) {
            const char *modern=b.side_name(j);
            if(n && modern && !strcmp(n,modern)) { type=j; break; }
        }
        if(type<0) { ret=-38; goto end; }
        SideData *sd=b.frame_side(f,type,in->size);
        if(!sd) { ret=-12; goto end; }
        memcpy(sd->data,in->data,in->size);
        if((ret=copy_dict(&r,&b,&sd->metadata,in->metadata))<0) goto end;
    }
    const char *uc=getenv("ARC_TEST_UC_COPY");
    if(uc && !strcmp(uc,"1")) {
        Frame8 *cached=b.frame_alloc();
        if(!cached) { ret=-12; goto end; }
        State copy={0}; copy.cpu=cached;
        ret=cache_mapped_pixels(&copy,f);
        if(ret>=0) ret=b.frame_props(cached,f);
        if(ret<0) { b.frame_free((void **)&cached); goto end; }
        b.frame_free((void **)&f); f=cached;
    }
    State s={0}; s.gpu=f; s.target=input->format;
    ret=export_frame(&s,output);
    repack_pool_free(s.repack_pool);
    if(s.sws) b.sws_free(s.sws);
end:
    b.frame_free((void **)&f);
    return ret;
}

/* Isolate pixel repacking cost from decoding, file I/O, Python and hashing. */
EXPORT int arc_repack_benchmark(const char *packed, int width, int height,
                               int iterations, int fast, unsigned long *stats) {
    pthread_once(&once,initialize);
    pthread_once(&backend_once,initialize_backend);
    if(!backend_ok || width<=0 || height<=0 || iterations<=0) return -22;
    const char *planar=NULL;
    int full=0;
    if(!strcmp(packed,"nv12")) planar="yuv420p";
    if(!strcmp(packed,"p010le")) planar="yuv420p10le";
    if(!strcmp(packed,"y210le") || !strcmp(packed,"p210le")) { planar="yuv422p10le"; full=1; }
    if(!planar) return -38;
    software_test=1; use_swscale=!fast;
    Frame8 *src=b.frame_alloc(); Frame6 *dst=r.frame_alloc();
    State s={0}; int ret=-12;
    if(!src || !dst) goto end;
    src->format=b.pix_fmt(packed); src->width=width; src->height=height;
    if((ret=b.frame_buffer(src,64))<0) goto end;
    for(int p=0;p<3;p++) if(src->data[p]) {
        int rows=p && !full ? (height+1)/2:height;
        for(int y=0;y<rows;y++)
            for(int x=0;x<src->linesize[p];x++) src->data[p][y*src->linesize[p]+x]=(uint8_t)(x+y);
    }
    s.gpu=src; s.target=r.pix_fmt(planar);
    /* Warm caches, allocator and swscale context before collecting timings. */
    for(int i=0;i<3;i++) if((ret=export_frame(&s,dst))<0) goto end;
    s.allocate_ns=0; s.repack_ns=0;
    unsigned long t=now_ns();
    for(int i=0;i<iterations;i++) if((ret=export_frame(&s,dst))<0) goto end;
    stats[0]=now_ns()-t; stats[1]=s.allocate_ns; stats[2]=s.repack_ns;
    ret=0;
end:
    repack_pool_free(s.repack_pool);
    if(s.sws) b.sws_free(s.sws);
    if(src) b.frame_free((void **)&src);
    if(dst) r.frame_free((void **)&dst);
    return ret;
}

/* GPU-free control-flow regression for staged/direct mapping and fallback.
 * Only callable in this test DSO, before any concurrent decoding starts. */
static int mock_map_flags, mock_map_failure, mock_transfer_calls;
static int mock_map(void *dst, const void *src, int flags) {
    (void)src;
    mock_map_flags=flags;
    if(mock_map_failure) return -38;
    Frame8 *f=dst;
    f->width=98; f->height=66; f->format=b.pix_fmt("y210le");
    return b.frame_buffer(f,64);
}
static int mock_transfer(void *dst, const void *src, int flags) {
    (void)src; (void)flags;
    mock_transfer_calls++;
    Frame8 *f=dst;
    f->width=98; f->height=66; f->format=b.pix_fmt("y210le");
    return b.frame_buffer(f,64);
}
EXPORT int arc_readback_route_test(void) {
    pthread_once(&once,initialize);
    pthread_once(&backend_once,initialize_backend);
    if(!backend_ok) return -5;
    int (*saved_map)(void *,const void *,int)=b.map;
    int (*saved_transfer)(void *,const void *,int)=b.transfer;
    int saved_mode=download_mode, saved_stream=stream_capable, saved_sws=use_swscale;
    State s={0};
    Frame8 *src=b.frame_alloc();
    s.cpu=b.frame_alloc(); s.mapped=b.frame_alloc();
    int ret=-12;
    if(!src || !s.cpu || !s.mapped) goto end;
    b.map=mock_map; b.transfer=mock_transfer;
    stream_capable=1; use_swscale=0;
    for(int mode=0;mode<=4;mode++) {
        download_mode=mode;
        for(int fail=0;fail<=1;fail++) {
            mock_map_failure=fail; mock_map_flags=0; mock_transfer_calls=0;
            s.fused_frames=s.direct_frames=s.mapped_frames=s.transferred_frames=s.staged_frames=0;
            if((ret=readback(&s,src))<0) goto end;
            int direct=mode==2 || mode==3;
            int fallback=!mode || fail;
            int fused=!fallback && (mode==3 || mode==4);
            if(mock_map_flags!=(mode ? (direct ? 9:1):0) ||
               mock_transfer_calls!=fallback || s.fused!=fused ||
               s.fused_frames!=(unsigned long)fused ||
               s.staged_frames!=(unsigned long)(fused && mode==4) ||
               s.direct_frames!=(unsigned long)(!fallback && direct) ||
               s.mapped_frames!=(unsigned long)(!fallback && !direct) ||
               s.transferred_frames!=(unsigned long)fallback) { ret=-22; goto end; }
        }
    }
    ret=0;
end:
    b.map=saved_map; b.transfer=saved_transfer;
    download_mode=saved_mode; stream_capable=saved_stream; use_swscale=saved_sws;
    if(src) b.frame_free((void **)&src);
    if(s.cpu) b.frame_free((void **)&s.cpu);
    if(s.mapped) b.frame_free((void **)&s.mapped);
    return ret;
}

/* Track real backend buffer release callbacks across handoff, wait and free.
 * A successful free must join the last pending batch even without EOF. */
typedef struct { unsigned long mutex[5]; int freed; } CleanupProbe;
static void cleanup_probe_release(void *opaque,uint8_t *data) {
    CleanupProbe *p=opaque;
    free(data);
    pthread_mutex_lock(p->mutex); p->freed++; pthread_mutex_unlock(p->mutex);
}
EXPORT int arc_cleanup_lifetime_test(void) {
    pthread_once(&once,initialize);
    pthread_once(&backend_once,initialize_backend);
    if(!backend_ok) return -5;
    CleanupProbe probe={0};
    CleanupPool *pool=cleanup_new();
    Frame8 *mapped=b.frame_alloc(), *gpu=b.frame_alloc(), *cpu=b.frame_alloc();
    int ret=-12;
    if(!pool || !mapped || !gpu || !cpu) goto end;
    for(int job=0;job<100;job++) {
        Frame8 *frames[]={mapped,gpu,cpu};
        for(int i=0;i<3;i++) {
            uint8_t *data=calloc(1,16);
            if(!data) goto end;
            frames[i]->buf[0]=b.buffer_create(data,16,cleanup_probe_release,&probe,0);
            if(!frames[i]->buf[0]) { free(data); goto end; }
            frames[i]->data[0]=data;
        }
        cleanup_submit(pool,&mapped,&gpu,&cpu);
        if(job%7==0) {
            cleanup_wait(pool);
            if(probe.freed!=(job+1)*3) { ret=-22; goto end; }
        }
    }
    cleanup_free(pool); pool=NULL;
    ret=probe.freed==300 ? 0:-22;
end:
    cleanup_free(pool);
    if(mapped) b.frame_free((void **)&mapped);
    if(gpu) b.frame_free((void **)&gpu);
    if(cpu) b.frame_free((void **)&cpu);
    pthread_mutex_destroy(probe.mutex);
    return ret;
}
