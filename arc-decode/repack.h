/* Lossless SIMD repacking for Intel's common download formats. Cached-memory
 * paths use SSE2. The runtime-guarded mapped-memory path uses SSE4.1 MOVNTDQA
 * for aligned reads, bracketed by MFENCE in the caller. Unaligned reads and
 * tails use memcpy. No instruction beyond SSE2 executes without a CPU check. */
typedef uint16_t ArcWords __attribute__((vector_size(16)));
typedef uint8_t ArcBytes __attribute__((vector_size(16)));

static int stream_reads_supported(void) {
    unsigned a,b,c,d;
    __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(1),"c"(0));
    return !!(c & (1U<<19)); /* SSE4.1 */
}
static ArcWords load_words(const void *src, int streaming) {
    ArcWords v;
    if(streaming && !((size_t)src&15))
        __asm__ volatile("movntdqa (%1), %0" : "=x"(v) : "r"(src) : "memory");
    else memcpy(&v,src,16);
    return v;
}
static void copy_row(uint8_t *dst,const uint8_t *src,int bytes,int streaming) {
    if(!streaming) { memcpy(dst,src,bytes); return; }
    int x=0;
    for(;x+16<=bytes;x+=16) { ArcWords v=load_words(src+x,1); memcpy(dst+x,&v,16); }
    if(x<bytes) memcpy(dst+x,src+x,bytes-x);
}
static void unpack_msb10(uint16_t *dst, const uint16_t *src, int count, int streaming) {
    int x=0;
    for(;x+8<=count;x+=8) {
        ArcWords v=load_words(src+x,streaming); v >>= 6; memcpy(dst+x,&v,16);
    }
    for(;x<count;x++) dst[x]=src[x]>>6;
}

static int fast_repack(const Frame8 *src, Frame6 *dst, const char *format, const char *output, int streaming) {
    int width=src->width, height=src->height, cw=(width+1)/2;
    int ten=!strcmp(output,"yuv420p10le") || !strcmp(output,"yuv422p10le");
    int full=!strcmp(output,"yuv422p") || !strcmp(output,"yuv422p10le");
    int ch=full ? height : (height+1)/2;
    if(!strcmp(format,output)) {
        for(int p=0;p<3;p++) {
            int rows=p ? ch:height, bytes=(p ? cw:width)*(ten ? 2:1);
            for(int y=0;y<rows;y++) copy_row(dst->data[p]+y*dst->linesize[p],src->data[p]+y*src->linesize[p],bytes,streaming);
        }
        return 1;
    }
    if(!strcmp(format,"p010le") || !strcmp(format,"p210le")) {
        for(int y=0;y<height;y++)
            unpack_msb10((uint16_t *)(dst->data[0]+y*dst->linesize[0]),
                         (const uint16_t *)(src->data[0]+y*src->linesize[0]),width,streaming);
        for(int y=0;y<ch;y++) {
            const uint16_t *in=(const uint16_t *)(src->data[1]+y*src->linesize[1]);
            uint16_t *u=(uint16_t *)(dst->data[1]+y*dst->linesize[1]);
            uint16_t *v=(uint16_t *)(dst->data[2]+y*dst->linesize[2]);
            int x=0;
            for(;x+8<=cw;x+=8) {
                ArcWords a=load_words(in+2*x,streaming), b=load_words(in+2*x+8,streaming);
                ArcWords uu=__builtin_shufflevector(a,b,0,2,4,6,8,10,12,14)>>6;
                ArcWords vv=__builtin_shufflevector(a,b,1,3,5,7,9,11,13,15)>>6;
                memcpy(u+x,&uu,16); memcpy(v+x,&vv,16);
            }
            for(;x<cw;x++) { u[x]=in[2*x]>>6; v[x]=in[2*x+1]>>6; }
        }
        return 1;
    }
    if(!strcmp(format,"y210le")) {
        for(int y=0;y<height;y++) {
            const uint16_t *in=(const uint16_t *)(src->data[0]+y*src->linesize[0]);
            uint16_t *yy=(uint16_t *)(dst->data[0]+y*dst->linesize[0]);
            uint16_t *u=(uint16_t *)(dst->data[1]+y*dst->linesize[1]);
            uint16_t *v=(uint16_t *)(dst->data[2]+y*dst->linesize[2]);
            int x=0;
            for(;x+8<=width;x+=8) {
                ArcWords a=load_words(in+2*x,streaming), b=load_words(in+2*x+8,streaming);
                ArcWords luma=__builtin_shufflevector(a,b,0,2,4,6,8,10,12,14)>>6;
                __typeof__(__builtin_shufflevector(a,b,1,5,9,13)) uu=__builtin_shufflevector(a,b,1,5,9,13)>>6;
                __typeof__(__builtin_shufflevector(a,b,3,7,11,15)) vv=__builtin_shufflevector(a,b,3,7,11,15)>>6;
                memcpy(yy+x,&luma,16); memcpy(u+x/2,&uu,8); memcpy(v+x/2,&vv,8);
            }
            for(;x+1<width;x+=2) {
                yy[x]=in[2*x]>>6; yy[x+1]=in[2*x+2]>>6;
                u[x/2]=in[2*x+1]>>6; v[x/2]=in[2*x+3]>>6;
            }
            if(x<width) { yy[x]=in[2*x]>>6; u[x/2]=in[2*x+1]>>6; v[x/2]=in[2*x+3]>>6; }
        }
        return 1;
    }
    if(!strcmp(format,"nv12") || !strcmp(format,"nv16")) {
        for(int y=0;y<height;y++) copy_row(dst->data[0]+y*dst->linesize[0],src->data[0]+y*src->linesize[0],width,streaming);
        for(int y=0;y<ch;y++) {
            const uint8_t *in=src->data[1]+y*src->linesize[1];
            uint8_t *u=dst->data[1]+y*dst->linesize[1], *v=dst->data[2]+y*dst->linesize[2];
            int x=0;
            for(;x+16<=cw;x+=16) {
                ArcBytes a=(ArcBytes)load_words(in+2*x,streaming), b=(ArcBytes)load_words(in+2*x+16,streaming);
                ArcBytes uu=__builtin_shufflevector(a,b,0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30);
                ArcBytes vv=__builtin_shufflevector(a,b,1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31);
                memcpy(u+x,&uu,16); memcpy(v+x,&vv,16);
            }
            for(;x<cw;x++) { u[x]=in[2*x]; v[x]=in[2*x+1]; }
        }
        return 1;
    }
    return 0;
}

/* Persistent per-decoder workers. Callers serialize dispatch and join every
 * batch before unmapping pixels. Boundaries are even rows for 4:2:0 chroma. */
typedef struct RepackPool RepackPool;
typedef struct {
    RepackPool *pool;
    unsigned long thread;
    int index, result;
} RepackWorker;
struct RepackPool {
    unsigned long mutex[5], ready[6], done[6];
    unsigned long generation;
    int count, completed, stop;
    const Frame8 *src;
    Frame6 *dst;
    const char *format, *output;
    int streaming;
    RepackWorker workers[3];
};
static int repack_slice(RepackPool *p,int index) {
    Frame8 src=*p->src;
    Frame6 dst=*p->dst;
    int units=(src.height+1)/2;
    int start=2*(units*index/p->count), end=2*(units*(index+1)/p->count);
    if(start>src.height) start=src.height;
    if(end>src.height) end=src.height;
    int full=!strcmp(p->output,"yuv422p") || !strcmp(p->output,"yuv422p10le");
    for(int plane=0;plane<3;plane++) {
        int row=plane && !full ? start/2:start;
        if(src.data[plane]) src.data[plane]+=row*src.linesize[plane];
        if(dst.data[plane]) dst.data[plane]+=row*dst.linesize[plane];
    }
    src.height=dst.height=end-start;
    if(p->streaming) __asm__ volatile("mfence" ::: "memory");
    int result=fast_repack(&src,&dst,p->format,p->output,p->streaming);
    if(p->streaming) __asm__ volatile("mfence" ::: "memory");
    return result;
}
static void *repack_worker(void *opaque) {
    RepackWorker *w=opaque;
    RepackPool *p=w->pool;
    unsigned long seen=0;
    pthread_mutex_lock(p->mutex);
    for(;;) {
        while(!p->stop && seen==p->generation) pthread_cond_wait(p->ready,p->mutex);
        if(p->stop) break;
        seen=p->generation;
        pthread_mutex_unlock(p->mutex);
        w->result=repack_slice(p,w->index);
        pthread_mutex_lock(p->mutex);
        p->completed++;
        pthread_cond_broadcast(p->done);
    }
    pthread_mutex_unlock(p->mutex);
    return NULL;
}
static void repack_pool_free(RepackPool *p) {
    if(!p) return;
    pthread_mutex_lock(p->mutex);
    p->stop=1;
    pthread_cond_broadcast(p->ready);
    pthread_mutex_unlock(p->mutex);
    for(int i=0;i<p->count-1;i++) pthread_join(p->workers[i].thread,NULL);
    pthread_cond_destroy(p->ready);
    pthread_cond_destroy(p->done);
    pthread_mutex_destroy(p->mutex);
    free(p);
}
static RepackPool *repack_pool_new(int count) {
    RepackPool *p=calloc(1,sizeof(*p));
    if(!p) return NULL;
    p->count=1;
    for(int i=0;i<count-1;i++) {
        RepackWorker *w=&p->workers[i];
        w->pool=p; w->index=i;
        if(pthread_create(&w->thread,NULL,repack_worker,w)) {
            repack_pool_free(p);
            return NULL;
        }
        p->count++;
    }
    return p;
}
static int parallel_repack(RepackPool *p,const Frame8 *src,Frame6 *dst,
                           const char *format,const char *output,int streaming) {
    if(!p) return fast_repack(src,dst,format,output,streaming);
    pthread_mutex_lock(p->mutex);
    p->src=src; p->dst=dst; p->format=format; p->output=output;
    p->streaming=streaming; p->completed=0; p->generation++;
    pthread_cond_broadcast(p->ready);
    pthread_mutex_unlock(p->mutex);
    int result=repack_slice(p,p->count-1);
    pthread_mutex_lock(p->mutex);
    while(p->completed!=p->count-1) pthread_cond_wait(p->done,p->mutex);
    for(int i=0;i<p->count-1;i++) result &= p->workers[i].result;
    pthread_mutex_unlock(p->mutex);
    return result;
}
