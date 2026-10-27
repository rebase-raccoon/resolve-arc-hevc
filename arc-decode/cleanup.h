/* One bounded background batch per decoder. The main thread swaps whole
 * backend-owned frame objects only after the preceding batch is empty.
 * Output frames contain independent Resolve-owned pixels before submission. */
typedef struct {
    unsigned long mutex[5], ready[6], done[6], thread;
    Frame8 *mapped, *gpu, *cpu;
    int busy, stop;
    unsigned long jobs, elapsed_ns;
} CleanupPool;
static void *cleanup_worker(void *opaque) {
    CleanupPool *p=opaque;
    pthread_mutex_lock(p->mutex);
    for(;;) {
        while(!p->busy && !p->stop) pthread_cond_wait(p->ready,p->mutex);
        if(p->stop) break;
        pthread_mutex_unlock(p->mutex);
        unsigned long t=now_ns();
        b.frame_unref(p->mapped); b.frame_unref(p->gpu); b.frame_unref(p->cpu);
        unsigned long elapsed=now_ns()-t;
        pthread_mutex_lock(p->mutex);
        p->elapsed_ns+=elapsed; p->jobs++; p->busy=0;
        pthread_cond_broadcast(p->done);
    }
    pthread_mutex_unlock(p->mutex);
    return NULL;
}
static void cleanup_wait(CleanupPool *p) {
    if(!p) return;
    pthread_mutex_lock(p->mutex);
    while(p->busy) pthread_cond_wait(p->done,p->mutex);
    pthread_mutex_unlock(p->mutex);
}
static void cleanup_free(CleanupPool *p) {
    if(!p) return;
    cleanup_wait(p);
    pthread_mutex_lock(p->mutex);
    p->stop=1;
    pthread_cond_broadcast(p->ready);
    pthread_mutex_unlock(p->mutex);
    pthread_join(p->thread,NULL);
    b.frame_free((void **)&p->mapped); b.frame_free((void **)&p->gpu); b.frame_free((void **)&p->cpu);
    pthread_cond_destroy(p->ready); pthread_cond_destroy(p->done); pthread_mutex_destroy(p->mutex);
    free(p);
}
static CleanupPool *cleanup_new(void) {
    CleanupPool *p=calloc(1,sizeof(*p));
    if(!p) return NULL;
    p->mapped=b.frame_alloc(); p->gpu=b.frame_alloc(); p->cpu=b.frame_alloc();
    if(!p->mapped || !p->gpu || !p->cpu || pthread_create(&p->thread,NULL,cleanup_worker,p)) {
        b.frame_free((void **)&p->mapped); b.frame_free((void **)&p->gpu); b.frame_free((void **)&p->cpu);
        free(p); return NULL;
    }
    return p;
}
static void cleanup_submit(CleanupPool *p,Frame8 **mapped,Frame8 **gpu,Frame8 **cpu) {
    pthread_mutex_lock(p->mutex);
    while(p->busy) pthread_cond_wait(p->done,p->mutex);
    Frame8 *tmp=p->mapped; p->mapped=*mapped; *mapped=tmp;
    tmp=p->gpu; p->gpu=*gpu; *gpu=tmp;
    tmp=p->cpu; p->cpu=*cpu; *cpu=tmp;
    p->busy=1;
    pthread_cond_broadcast(p->ready);
    pthread_mutex_unlock(p->mutex);
}
