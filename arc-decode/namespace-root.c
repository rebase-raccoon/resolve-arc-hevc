/* Root of the isolated FFmpeg/VA-API namespace.
 *
 * libva loads its driver using RTLD_GLOBAL. glibc dlopen can segfault with
 * that flag when called from a non-base namespace. Inside this namespace all
 * such loads must remain local. Dependencies still resolve in the root's
 * dependency group. No changes are made to installed libva or Intel drivers.
 * https://github.com/intel/libva/blob/master/va/va.c
 * https://man7.org/linux/man-pages/man3/dlmopen.3.html
 */
#define EXPORT __attribute__((visibility("default")))
extern void *dlsym(void *, const char *);
extern void _exit(int);

EXPORT void *dlopen(const char *name, int flags) {
    /* RTLD_NEXT is evaluated inside this DSO, so this resolves the isolated
     * libc's implementation and preserves the caller's namespace. */
    void *(*next)(const char *, int);
    *(void **)(&next) = dlsym((void *)-1,"dlopen");
    if (!next) _exit(127);
    return next(name, flags & ~256); /* RTLD_GLOBAL -> RTLD_LOCAL */
}

/* Exercise a dependency's nested driver-style dlopen in the regression test. */
EXPORT void *arc_namespace_open(const char *name) {
    return dlopen(name, 2 | 256);
}
