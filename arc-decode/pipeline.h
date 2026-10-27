/* Pipelined export: decoded VAAPI surfaces are read back and repacked by
 * per-context workers, several frames in flight, returned in decode order.
 * Only the caller thread (holding the context's operation lock) touches the
 * backend decoder; workers touch only their own job's frames. */
#define PIPE_MAX 8
typedef struct {
    Frame8 *gpu; Frame6 *out; int ret, state; /* 0 free, 1 queued, 2 running, 3 done */
    unsigned long download_ns, repack_ns, map_ns, fused, direct, hw, gb, gbf, vpp;
} PipeJob;
typedef struct Pipe {
    unsigned long mutex[5], work[6], done[6];
    unsigned long threads[PIPE_MAX];
    State *ws[PIPE_MAX];
    PipeJob jobs[PIPE_MAX];
    int nthreads, stop, head, count, depth, target;
} Pipe;
static void *pipe_worker(void *opaque) {
    void **args=opaque; Pipe *p=args[0]; State *ws=args[1]; free(args);
    pthread_mutex_lock(p->mutex);
    for(;;) {
        PipeJob *j=NULL;
        for(int i=0;i<p->count;i++) { PipeJob *c=&p->jobs[(p->head+i)%PIPE_MAX]; if(c->state==1) { j=c; break; } }
        if(!j) { if(p->stop) break; pthread_cond_wait(p->work,p->mutex); continue; }
        j->state=2;
        pthread_mutex_unlock(p->mutex);
        unsigned long v0=ws->vpp_frames, g0=ws->getbuffer_frames, gf0=ws->getbuffer_failures, d0=ws->download_ns, r0=ws->repack_ns, m0=ws->map_ns, f0=ws->fused_frames, x0=ws->direct_frames, h0=ws->hardware_frames;
        ws->target=p->target;
        b.frame_move(ws->gpu,j->gpu);
        int ret=export_frame(ws,j->out);
        b.frame_unref(ws->mapped); b.frame_unref(ws->gpu); b.frame_unref(ws->cpu);
        pthread_mutex_lock(p->mutex);
        j->ret=ret; j->download_ns=ws->download_ns-d0; j->repack_ns=ws->repack_ns-r0; j->map_ns=ws->map_ns-m0;
        j->fused=ws->fused_frames-f0; j->gb=ws->getbuffer_frames-g0; j->vpp=ws->vpp_frames-v0; j->gbf=ws->getbuffer_failures-gf0; j->direct=ws->direct_frames-x0; j->hw=ws->hardware_frames-h0;
        j->state=3;
        pthread_cond_broadcast(p->done);
    }
    pthread_mutex_unlock(p->mutex);
    return NULL;
}
static void pipe_free(Pipe *p) {
    if(!p) return;
    pthread_mutex_lock(p->mutex); p->stop=1; pthread_cond_broadcast(p->work); pthread_mutex_unlock(p->mutex);
    for(int i=0;i<p->nthreads;i++) pthread_join(p->threads[i],NULL);
    for(int i=0;i<PIPE_MAX;i++) {
        if(p->jobs[i].gpu) b.frame_free((void **)&p->jobs[i].gpu);
        if(p->jobs[i].out) r.frame_free((void **)&p->jobs[i].out);
        State *w=p->ws[i];
        if(w) {
            if(w->gpu) b.frame_free((void **)&w->gpu);
            if(w->cpu) b.frame_free((void **)&w->cpu);
            if(w->mapped) b.frame_free((void **)&w->mapped);
            if(w->sws) b.sws_free(w->sws);
            arc_vpp_free(w->vpp);
            free(w);
        }
    }
    free(p);
}
static Pipe *pipe_new(State *owner) {
    Pipe *p=calloc(1,sizeof(*p));
    if(!p) return NULL;
    p->depth=pipeline_depth; p->target=owner->target;
    for(int i=0;i<PIPE_MAX;i++) {
        p->jobs[i].gpu=b.frame_alloc(); p->jobs[i].out=r.frame_alloc();
        if(!p->jobs[i].gpu || !p->jobs[i].out) { pipe_free(p); return NULL; }
    }
    for(int i=0;i<p->depth;i++) {
        State *w=calloc(1,sizeof(*w));
        if(!w) { pipe_free(p); return NULL; }
        p->ws[i]=w; w->no_pool=1; w->target=owner->target; w->decoder=owner->decoder; w->alloc_ctx=owner->alloc_ctx; w->alloc_mutex=owner->alloc_mutex;
        w->gpu=b.frame_alloc(); w->cpu=b.frame_alloc(); w->mapped=b.frame_alloc();
        void **args=calloc(2,sizeof(void *));
        if(!w->gpu || !w->cpu || !w->mapped || !args) { free(args); pipe_free(p); return NULL; }
        args[0]=p; args[1]=w;
        if(pthread_create(&p->threads[i],NULL,pipe_worker,args)) { free(args); pipe_free(p); return NULL; }
        p->nthreads++;
    }
    return p;
}
/* Cancel queued work, wait for running jobs, drop every result. */
static void pipe_flush(Pipe *p) {
    pthread_mutex_lock(p->mutex);
    for(int i=0;i<p->count;i++) { PipeJob *j=&p->jobs[(p->head+i)%PIPE_MAX]; if(j->state==1) { b.frame_unref(j->gpu); j->state=3; j->ret=-1; } }
    for(;;) {
        int busy=0;
        for(int i=0;i<p->count;i++) if(p->jobs[(p->head+i)%PIPE_MAX].state==2) busy=1;
        if(!busy) break;
        pthread_cond_wait(p->done,p->mutex);
    }
    for(int i=0;i<p->count;i++) { PipeJob *j=&p->jobs[(p->head+i)%PIPE_MAX]; r.frame_unref(j->out); b.frame_unref(j->gpu); j->state=0; }
    p->head=0; p->count=0;
    pthread_mutex_unlock(p->mutex);
}
static int pipe_receive(State *s, Frame6 *dst) {
    Pipe *p=s->pipe;
    for(;;) {
        pthread_mutex_lock(p->mutex);
        PipeJob *j=p->count ? &p->jobs[p->head] : NULL;
        if(j && j->state==3) {
            int ret=j->ret;
            if(ret>=0) r.frame_move(dst,j->out);
            r.frame_unref(j->out);
            s->download_ns+=j->download_ns; s->repack_ns+=j->repack_ns; s->map_ns+=j->map_ns;
            s->fused_frames+=j->fused; s->getbuffer_frames+=j->gb; s->vpp_frames+=j->vpp; s->getbuffer_failures+=j->gbf; s->direct_frames+=j->direct; s->hardware_frames+=j->hw;
            j->state=0; p->head=(p->head+1)%PIPE_MAX; p->count--;
            pthread_mutex_unlock(p->mutex);
            if(ret<0) return ret;
            s->pipe_frames++;
            Ctx6 *ctx=s->original;
            if(ctx) {
                ctx->width=dst->width; ctx->height=dst->height; ctx->pix_fmt=dst->format;
                ctx->coded_width=s->decoder->coded_width; ctx->coded_height=s->decoder->coded_height;
                ctx->delay=s->decoder->delay+p->depth;
            }
            return 0;
        }
        int room=p->count<p->depth;
        pthread_mutex_unlock(p->mutex);
        if(room && !s->backend_eof) {
            unsigned long t=now_ns();
            int ret=b.receive(s->decoder,s->gpu);
            s->receive_ns+=now_ns()-t;
            if(ret==0) {
                if(s->gpu->format!=vaapi && !software_test) { b.frame_unref(s->gpu); return -5; }
                pthread_mutex_lock(p->mutex);
                PipeJob *n=&p->jobs[(p->head+p->count)%PIPE_MAX];
                b.frame_move(n->gpu,s->gpu); n->state=1; n->ret=0; p->count++;
                pthread_cond_broadcast(p->work);
                pthread_mutex_unlock(p->mutex);
                continue;
            }
            if(ret==-541478725) { s->backend_eof=1; continue; }
            if(ret!=-11) return ret;
            if(!s->draining) return -11;
        }
        pthread_mutex_lock(p->mutex);
        if(!p->count) { pthread_mutex_unlock(p->mutex); return s->backend_eof ? -541478725 : -11; }
        unsigned long t=now_ns(); s->pipe_waits++;
        while(p->jobs[p->head].state!=3) pthread_cond_wait(p->done,p->mutex);
        s->pipe_wait_ns+=now_ns()-t;
        pthread_mutex_unlock(p->mutex);
    }
}
