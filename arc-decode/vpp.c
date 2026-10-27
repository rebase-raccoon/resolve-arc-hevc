/* GPU blit of a decoded VA surface into a CPU-cached user-pointer surface.
 * libva entry points are resolved inside the backend linker namespace, since
 * the VADisplay belongs to that namespace's libva instance. */
#include <va/va.h>
#include <va/va_vpp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define V __attribute__((visibility("hidden")))
static VAStatus (*p_create_surfaces)(VADisplay, unsigned, unsigned, unsigned, VASurfaceID *, unsigned, VASurfaceAttrib *, unsigned);
static VAStatus (*p_destroy_surfaces)(VADisplay, VASurfaceID *, int);
static VAStatus (*p_create_config)(VADisplay, VAProfile, VAEntrypoint, VAConfigAttrib *, int, VAConfigID *);
static VAStatus (*p_destroy_config)(VADisplay, VAConfigID);
static VAStatus (*p_create_context)(VADisplay, VAConfigID, int, int, int, VASurfaceID *, int, VAContextID *);
static VAStatus (*p_destroy_context)(VADisplay, VAContextID);
static VAStatus (*p_create_buffer)(VADisplay, VAContextID, VABufferType, unsigned, unsigned, void *, VABufferID *);
static VAStatus (*p_destroy_buffer)(VADisplay, VABufferID);
static VAStatus (*p_begin)(VADisplay, VAContextID, VASurfaceID);
static VAStatus (*p_render)(VADisplay, VAContextID, VABufferID *, int);
static VAStatus (*p_end)(VADisplay, VAContextID);
static VAStatus (*p_sync)(VADisplay, VASurfaceID);
V int arc_vpp_init(void *(*lookup)(const char *)) {
#define L(P, N) if (!(*(void **)&P = lookup(N))) return 0
    L(p_create_surfaces, "vaCreateSurfaces"); L(p_destroy_surfaces, "vaDestroySurfaces");
    L(p_create_config, "vaCreateConfig"); L(p_destroy_config, "vaDestroyConfig");
    L(p_create_context, "vaCreateContext"); L(p_destroy_context, "vaDestroyContext");
    L(p_create_buffer, "vaCreateBuffer"); L(p_destroy_buffer, "vaDestroyBuffer");
    L(p_begin, "vaBeginPicture"); L(p_render, "vaRenderPicture"); L(p_end, "vaEndPicture"); L(p_sync, "vaSyncSurface");
#undef L
    return 1;
}
typedef struct ArcVpp { VADisplay dpy; VAConfigID cfg; VAContextID ctx; VASurfaceID surf; uint8_t *mem; size_t size; int w, h, have_cfg, have_ctx, have_surf; } ArcVpp;
V void arc_vpp_free(ArcVpp *v) {
    if (!v) return;
    if (v->have_ctx) p_destroy_context(v->dpy, v->ctx);
    if (v->have_surf) p_destroy_surfaces(v->dpy, &v->surf, 1);
    if (v->have_cfg) p_destroy_config(v->dpy, v->cfg);
    free(v->mem); free(v);
}
/* kind: 0 = Y210 (4:2:2 10-bit packed), 1 = P010 (4:2:0 10-bit), 2 = NV12 (4:2:0 8-bit) */
V ArcVpp *arc_vpp_new(void *dpy, int kind, int w, int h, uint8_t **plane0, uint8_t **plane1, int *pitch) {
    unsigned fourcc, rt; size_t bpp, pt, size, uvoff = 0; int planes;
    if (kind == 0) { fourcc = VA_FOURCC_Y210; rt = VA_RT_FORMAT_YUV422_10; bpp = 4; planes = 1; }
    else if (kind == 1) { fourcc = VA_FOURCC_P010; rt = VA_RT_FORMAT_YUV420_10; bpp = 2; planes = 2; }
    else if (kind == 2) { fourcc = VA_FOURCC_NV12; rt = VA_RT_FORMAT_YUV420; bpp = 1; planes = 2; }
    else return NULL;
    pt = ((size_t)w * bpp + 127) & ~(size_t)127;
    size_t ah = ((size_t)h + 31) & ~(size_t)31;
    size = pt * ah; if (planes == 2) { uvoff = size; size += pt * (ah / 2); }
    size = (size + 4095) & ~(size_t)4095;
    ArcVpp *v = calloc(1, sizeof(*v)); if (!v) return NULL;
    v->dpy = dpy; v->w = w; v->h = h; v->size = size;
    if (posix_memalign((void **)&v->mem, 4096, size)) { free(v); return NULL; }
    memset(v->mem, 0, size);
    VASurfaceAttribExternalBuffers ext; memset(&ext, 0, sizeof(ext));
    uintptr_t ptr = (uintptr_t)v->mem;
    ext.pixel_format = fourcc; ext.width = w; ext.height = h; ext.data_size = size; ext.num_planes = planes;
    ext.pitches[0] = pt; ext.pitches[1] = pt; ext.offsets[0] = 0; ext.offsets[1] = uvoff; ext.buffers = &ptr; ext.num_buffers = 1;
    VASurfaceAttrib at[3]; memset(at, 0, sizeof(at));
    at[0].type = VASurfaceAttribPixelFormat; at[0].flags = VA_SURFACE_ATTRIB_SETTABLE; at[0].value.type = VAGenericValueTypeInteger; at[0].value.value.i = fourcc;
    at[1].type = VASurfaceAttribMemoryType; at[1].flags = VA_SURFACE_ATTRIB_SETTABLE; at[1].value.type = VAGenericValueTypeInteger; at[1].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_USER_PTR;
    at[2].type = VASurfaceAttribExternalBufferDescriptor; at[2].flags = VA_SURFACE_ATTRIB_SETTABLE; at[2].value.type = VAGenericValueTypePointer; at[2].value.value.p = &ext;
    if (p_create_surfaces(dpy, rt, w, h, &v->surf, 1, at, 3) != VA_STATUS_SUCCESS) goto fail;
    v->have_surf = 1;
    if (p_create_config(dpy, VAProfileNone, VAEntrypointVideoProc, NULL, 0, &v->cfg) != VA_STATUS_SUCCESS) goto fail;
    v->have_cfg = 1;
    if (p_create_context(dpy, v->cfg, w, h, VA_PROGRESSIVE, &v->surf, 1, &v->ctx) != VA_STATUS_SUCCESS) goto fail;
    v->have_ctx = 1;
    *plane0 = v->mem; *plane1 = planes == 2 ? v->mem + uvoff : NULL; *pitch = (int)pt;
    return v;
fail:
    arc_vpp_free(v); return NULL;
}
V int arc_vpp_blit(ArcVpp *v, unsigned src) {
    VAProcPipelineParameterBuffer pp; memset(&pp, 0, sizeof(pp));
    VARectangle reg = { 0, 0, (uint16_t)v->w, (uint16_t)v->h };
    pp.surface = src; pp.surface_region = &reg; pp.output_region = &reg;
    pp.surface_color_standard = VAProcColorStandardNone; pp.output_color_standard = VAProcColorStandardNone;
    VABufferID buf;
    if (p_create_buffer(v->dpy, v->ctx, VAProcPipelineParameterBufferType, sizeof(pp), 1, &pp, &buf) != VA_STATUS_SUCCESS) return -1;
    int ok = p_begin(v->dpy, v->ctx, v->surf) == VA_STATUS_SUCCESS
          && p_render(v->dpy, v->ctx, &buf, 1) == VA_STATUS_SUCCESS
          && p_end(v->dpy, v->ctx) == VA_STATUS_SUCCESS
          && p_sync(v->dpy, v->surf) == VA_STATUS_SUCCESS;
    p_destroy_buffer(v->dpy, buf);
    return ok ? 0 : -1;
}
