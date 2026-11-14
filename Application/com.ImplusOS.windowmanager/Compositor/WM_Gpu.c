#include "WM_Gpu.h"

#include "WM_Damage.h"
#include "WM_Raster.h"
#include "WM_Virgl.h"
#include "../Decoration/WM_Decoration.h"
#include "../UI/WM_Dialog.h"
#include "../UI/WM_Icons.h"
#include "../UI/WM_Notification.h"
#include "../UI/WM_StartMenu.h"
#include "../UI/WM_Taskbar.h"
#include "../UI/WM_WifiPanel.h"
#include "../../../../Userland/Source/Syscalls.h"

#include <stdlib.h>
#include <string.h>

/* ---- render node ABI (Linux include/uapi/drm/virtgpu_drm.h) ---- */

#define WM_DRM_RENDER_NODE "/dev/dri/renderD128"
/* Presence of this file keeps the window manager on the CPU compositor --
 * the escape hatch for a host whose virgl renderer misbehaves. */
#define WM_GPU_DISABLE_FILE "/var/System/wm-cpu-compositor"

#define WM_IOWR(nr, size) \
    ((3ull << 30) | ((uint64_t)(size) << 16) | ((uint64_t)'d' << 8) | (nr))

#define DRM_NR_GEM_CLOSE               0x09u
#define DRM_NR_VIRTGPU_MAP             0x41u
#define DRM_NR_VIRTGPU_EXECBUFFER      0x42u
#define DRM_NR_VIRTGPU_GETPARAM        0x43u
#define DRM_NR_VIRTGPU_RESOURCE_CREATE 0x44u
#define DRM_NR_VIRTGPU_TRANSFER_TO_HOST 0x47u
#define DRM_NR_VIRTGPU_WAIT            0x48u
#define DRM_NR_IMPLUS_SCANOUT          0x70u   /* Kernel/Core/drm/DRM_Kms.c */

#define VIRTGPU_PARAM_3D_FEATURES 1u
#define VIRTGPU_WAIT_NOWAIT       1u
#define IMPLUS_SCANOUT_SET     (1u << 0)
#define IMPLUS_SCANOUT_FLUSH   (1u << 1)
#define IMPLUS_SCANOUT_RELEASE (1u << 2)
#define WM_ERR_BUSY (-16)

typedef struct { uint32_t handle, pad; } drm_gem_close_t;
typedef struct { uint64_t offset; uint32_t handle, pad; } virtgpu_map_t;
typedef struct { uint64_t param, value; } virtgpu_getparam_t;
typedef struct {
    uint32_t target, format, bind, width, height, depth;
    uint32_t array_size, last_level, nr_samples, flags;
    uint32_t bo_handle, res_handle, size, stride;
} virtgpu_resource_create_t;
typedef struct {
    uint32_t bo_handle;
    uint32_t x, y, z, w, h, d;
    uint32_t level, offset, stride, layer_stride;
} virtgpu_transfer_t;
typedef struct { uint32_t handle, flags; } virtgpu_wait_t;
typedef struct {
    uint32_t flags, size;
    uint64_t command, bo_handles;
    uint32_t num_bo_handles;
    int32_t fence_fd;
    uint32_t ring_idx, syncobj_stride;
    uint32_t num_in_syncobjs, num_out_syncobjs;
    uint64_t in_syncobjs, out_syncobjs;
} virtgpu_execbuffer_t;
typedef struct {
    uint32_t bo_handle, flags;
    int32_t x, y;
    uint32_t width, height;
} implus_scanout_t;

_Static_assert(sizeof(virtgpu_resource_create_t) == 56u, "resource_create ABI");
_Static_assert(sizeof(virtgpu_transfer_t) == 44u, "transfer ABI");
_Static_assert(sizeof(virtgpu_execbuffer_t) == 64u, "execbuffer ABI");
_Static_assert(sizeof(implus_scanout_t) == 24u, "implus_scanout ABI");

/* ---- pipeline ---- */

/* Object handles of the context-wide state; sampler views and the render
 * target surface take handles from g.next_handle above these. */
enum {
    H_BLEND = 1, H_DSA, H_RASTERIZER, H_VS, H_FS, H_VERTEX_ELEMENTS,
    H_SAMPLER, H_FIRST_DYNAMIC
};

static const char g_vertex_shader[] =
    "VERT\n"
    "DCL IN[0]\n"
    "DCL IN[1]\n"
    "DCL OUT[0], POSITION\n"
    "DCL OUT[1], GENERIC[0]\n"
    "  0: MOV OUT[0], IN[0]\n"
    "  1: MOV OUT[1], IN[1]\n"
    "  2: END\n";

/* colour = texel * CONST[0] + CONST[1]: CONST[0] = (a, a, a, a) fades a
 * premultiplied layer by a; CONST[0] = (1, 1, 1, 0), CONST[1] = (0, 0, 0, 1)
 * draws it as opaque whatever its alpha channel holds. */
static const char g_fragment_shader[] =
    "FRAG\n"
    "PROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "DCL CONST[0..1]\n"
    "DCL TEMP[0]\n"
    "  0: TEX TEMP[0], IN[0], SAMP[0], 2D\n"
    "  1: MAD OUT[0], TEMP[0], CONST[0], CONST[1]\n"
    "  2: END\n";

#define WM_GPU_CMD_WORDS     (64u * 1024u)
#define WM_GPU_MAX_QUADS     (WM_MAX_WINDOWS + 8u)
#define WM_GPU_VERTEX_STRIDE 16u                      /* float2 pos, uv */
#define WM_GPU_VB_BYTES      (WM_GPU_MAX_QUADS * 4u * WM_GPU_VERTEX_STRIDE)
#define WM_GPU_TEX_ALIGN     64u
#define WM_GPU_TEX_MAX       8192u
#define WM_GPU_CURSOR_SIZE   64u
#define WM_GPU_CURSOR_PAD    8
#define WM_GPU_TILE          64u
#define WM_GPU_MAX_TILES     ((WM_GPU_TEX_MAX / WM_GPU_TILE) * \
                              (WM_GPU_TEX_MAX / WM_GPU_TILE))
#define WM_GPU_MAX_BOS       (WM_MAX_WINDOWS + 8u)
#define WM_GPU_CURSOR_FILL   0xFFFFFFFFu   /* as WM_Compositor.c's sprite */

typedef struct {
    uint32_t bo;            /* GEM handle; 0 = none */
    uint32_t resource;      /* host resource id */
    uint32_t view;          /* sampler view object */
    uint32_t width;         /* allocated size */
    uint32_t height;
    uint32_t bytes;
    uint32_t *pixels;       /* CPU mapping of the backing store */
} gpu_texture_t;

typedef struct {
    const wm_window_t *window;  /* identity check against id reuse */
    gpu_texture_t texture;
    uint64_t signature;     /* decoration inputs at the last full raster */
    bool valid;             /* texture holds this window as of `signature` */
    uint32_t seen_frame;
} gpu_window_t;

typedef struct {
    float x0, y0, x1, y1;   /* screen pixels, before NDC */
    float u0, v0, u1, v1;
    float c0[4];
    float c1[4];
    uint32_t view;
} gpu_quad_t;

static struct {
    bool active;
    int32_t fd;
    uint32_t width;         /* desktop / render target size */
    uint32_t height;
    uint32_t next_handle;
    uint32_t frame;

    wm_virgl_stream_t cmd;
    uint32_t *cmd_words;

    gpu_texture_t target;   /* render target on scanout */
    uint32_t target_surface;
    gpu_texture_t vertices;
    gpu_texture_t background;
    bool background_dirty;
    gpu_texture_t overlay;
    uint8_t *overlay_tiles; /* 1 = tile may hold non-transparent pixels */
    uint32_t tiles_x, tiles_y;
    gpu_texture_t cursor;
    wm_cursor_style_t cursor_style;
    uint64_t cursor_theme;
    bool cursor_valid;
    bool scanout_lost;

    gpu_window_t windows[WM_MAX_WINDOWS + 1u];
    gpu_quad_t quads[WM_GPU_MAX_QUADS];
    uint32_t quad_count;
    uint32_t bos[WM_GPU_MAX_BOS];
    uint32_t bo_count;
} g = { .fd = -1 };

/* ---- small helpers ---- */

static uint64_t fnv(uint64_t hash, const void *data, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0u; i < bytes; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint32_t align_up(uint32_t value, uint32_t align)
{
    return (value + align - 1u) / align * align;
}

static void log_failure(const char *what, int64_t rc)
{
    serial_write_string("[wm-gpu] ");
    serial_write_string(what);
    serial_write_string(" failed rc=");
    serial_write_uint64((uint64_t)rc);
    serial_write_string("\n");
}

static int64_t gpu_ioctl(uint32_t nr, void *arg, uint32_t size)
{
    return file_ioctl(g.fd, WM_IOWR(nr, size), arg);
}

/* ---- textures ---- */

static bool texture_wait_idle(const gpu_texture_t *t)
{
    if (!t->bo) return true;
    for (uint32_t spin = 0u; spin < 100000u; ++spin) {
        virtgpu_wait_t wait = { t->bo, VIRTGPU_WAIT_NOWAIT };
        int64_t rc = gpu_ioctl(DRM_NR_VIRTGPU_WAIT, &wait, sizeof(wait));
        if (rc == 0) return true;
        if (rc != WM_ERR_BUSY) return false;
        process_yield();
    }
    return false;
}

static void texture_release(gpu_texture_t *t)
{
    if (!t->bo) {
        memset(t, 0, sizeof(*t));
        return;
    }
    /* The host's sampler view goes first, in a stream of its own: the
     * GEM close below detaches and destroys the resource it refers to. */
    if (t->view && g.fd >= 0) {
        uint32_t words[4];
        wm_virgl_stream_t s;
        wm_virgl_stream_init(&s, words, 4u);
        wm_virgl_destroy_object(&s, WM_VIRGL_OBJECT_SAMPLER_VIEW, t->view);
        virtgpu_execbuffer_t exec;
        memset(&exec, 0, sizeof(exec));
        exec.size = wm_virgl_stream_bytes(&s);
        exec.command = (uint64_t)(uintptr_t)words;
        (void)gpu_ioctl(DRM_NR_VIRTGPU_EXECBUFFER, &exec, sizeof(exec));
    }
    (void)texture_wait_idle(t);
    if (t->pixels) (void)os_munmap(t->pixels, t->bytes);
    drm_gem_close_t close_args = { t->bo, 0u };
    (void)gpu_ioctl(DRM_NR_GEM_CLOSE, &close_args, sizeof(close_args));
    memset(t, 0, sizeof(*t));
}

static bool resource_create(gpu_texture_t *t, uint32_t target,
                            uint32_t format, uint32_t bind,
                            uint32_t width, uint32_t height,
                            uint32_t backing_bytes, bool map)
{
    memset(t, 0, sizeof(*t));
    virtgpu_resource_create_t create;
    memset(&create, 0, sizeof(create));
    create.target = target;
    create.format = format;
    create.bind = bind;
    create.width = width;
    create.height = height;
    create.depth = 1u;
    create.array_size = 1u;
    create.size = backing_bytes;
    create.stride = target == WM_VIRGL_TARGET_BUFFER ? 0u : width * 4u;
    int64_t rc = gpu_ioctl(DRM_NR_VIRTGPU_RESOURCE_CREATE, &create,
                           sizeof(create));
    if (rc < 0 || create.bo_handle == 0u) {
        log_failure("RESOURCE_CREATE", rc);
        return false;
    }
    t->bo = create.bo_handle;
    t->resource = create.res_handle;
    t->width = width;
    t->height = height;
    t->bytes = create.size;
    if (!map) return true;

    virtgpu_map_t m = { 0u, t->bo, 0u };
    rc = gpu_ioctl(DRM_NR_VIRTGPU_MAP, &m, sizeof(m));
    if (rc < 0) {
        log_failure("MAP", rc);
        texture_release(t);
        return false;
    }
    t->pixels = (uint32_t *)file_dev_mmap(g.fd, m.offset, t->bytes);
    if (!t->pixels) {
        log_failure("dev_mmap", -1);
        texture_release(t);
        return false;
    }
    return true;
}

/* A sampled B8G8R8A8 texture with a CPU mapping. Its sampler view is created
 * in the frame's command stream, ahead of any draw that uses it. */
static bool texture_create(gpu_texture_t *t, uint32_t width, uint32_t height)
{
    if (width == 0u || height == 0u ||
        width > WM_GPU_TEX_MAX || height > WM_GPU_TEX_MAX) return false;
    if (!resource_create(t, WM_VIRGL_TARGET_TEXTURE_2D,
                         WM_VIRGL_FORMAT_B8G8R8A8_UNORM,
                         WM_VIRGL_BIND_SAMPLER_VIEW, width, height,
                         width * height * 4u, true)) return false;
    t->view = g.next_handle++;
    wm_virgl_create_sampler_view_2d(&g.cmd, t->view, t->resource,
                                    WM_VIRGL_FORMAT_B8G8R8A8_UNORM);
    return true;
}

static bool texture_upload(const gpu_texture_t *t, wm_rect_t r)
{
    r = wm_rect_intersection(r, (wm_rect_t){0, 0, t->width, t->height});
    if (r.w == 0u || r.h == 0u) return true;
    virtgpu_transfer_t tr;
    memset(&tr, 0, sizeof(tr));
    tr.bo_handle = t->bo;
    tr.x = (uint32_t)r.x;
    tr.y = (uint32_t)r.y;
    tr.w = r.w;
    tr.h = r.h;
    tr.d = 1u;
    tr.stride = t->width * 4u;
    tr.offset = ((uint32_t)r.y * t->width + (uint32_t)r.x) * 4u;
    int64_t rc = gpu_ioctl(DRM_NR_VIRTGPU_TRANSFER_TO_HOST, &tr, sizeof(tr));
    if (rc < 0) {
        log_failure("TRANSFER_TO_HOST", rc);
        return false;
    }
    return true;
}

static void texture_clear(const gpu_texture_t *t, wm_rect_t r)
{
    r = wm_rect_intersection(r, (wm_rect_t){0, 0, t->width, t->height});
    for (uint32_t row = 0u; row < r.h; ++row)
        memset(&t->pixels[((uint32_t)r.y + row) * t->width + (uint32_t)r.x],
               0, (size_t)r.w * sizeof(uint32_t));
}

static void canvas_for(wm_canvas_t *canvas, const gpu_texture_t *t)
{
    wm_canvas_init(canvas, t->pixels, t->width, t->height, t->width);
}

/* ---- device ---- */

static bool submit(void)
{
    if (g.cmd.overflow) {
        log_failure("command stream overflow", -1);
        return false;
    }
    if (g.cmd.length == 0u) return true;
    virtgpu_execbuffer_t exec;
    memset(&exec, 0, sizeof(exec));
    exec.size = wm_virgl_stream_bytes(&g.cmd);
    exec.command = (uint64_t)(uintptr_t)g.cmd.words;
    exec.bo_handles = (uint64_t)(uintptr_t)g.bos;
    exec.num_bo_handles = g.bo_count;
    exec.fence_fd = -1;
    int64_t rc = gpu_ioctl(DRM_NR_VIRTGPU_EXECBUFFER, &exec, sizeof(exec));
    wm_virgl_stream_reset(&g.cmd);
    if (rc < 0) {
        log_failure("EXECBUFFER", rc);
        return false;
    }
    return true;
}

static int64_t scanout(uint32_t flags, wm_rect_t r)
{
    implus_scanout_t so;
    memset(&so, 0, sizeof(so));
    so.bo_handle = g.target.bo;
    so.flags = flags;
    if (flags & IMPLUS_SCANOUT_SET) {
        so.width = g.width;
        so.height = g.height;
    } else {
        so.x = r.x;
        so.y = r.y;
        so.width = r.w;
        so.height = r.h;
    }
    return gpu_ioctl(DRM_NR_IMPLUS_SCANOUT, &so, sizeof(so));
}

static void release_window_layers(void)
{
    for (uint32_t i = 0u; i <= WM_MAX_WINDOWS; ++i) {
        texture_release(&g.windows[i].texture);
        memset(&g.windows[i], 0, sizeof(g.windows[i]));
    }
}

/* The size-dependent objects: render target, background and overlay. */
static void release_surfaces(void)
{
    if (g.target_surface) {
        wm_virgl_stream_reset(&g.cmd);
        wm_virgl_destroy_object(&g.cmd, WM_VIRGL_OBJECT_SURFACE,
                                g.target_surface);
        g.bo_count = 0u;
        (void)submit();
        g.target_surface = 0u;
    }
    texture_release(&g.target);
    texture_release(&g.background);
    texture_release(&g.overlay);
    free(g.overlay_tiles);
    g.overlay_tiles = NULL;
}

static bool create_surfaces(uint32_t width, uint32_t height)
{
    g.width = width;
    g.height = height;
    /* The render target is only ever drawn and scanned out, never
     * transferred, so its guest backing is a token page. */
    if (!resource_create(&g.target, WM_VIRGL_TARGET_TEXTURE_2D,
                         WM_VIRGL_FORMAT_B8G8R8A8_UNORM,
                         WM_VIRGL_BIND_RENDER_TARGET |
                         WM_VIRGL_BIND_SAMPLER_VIEW |
                         WM_VIRGL_BIND_SCANOUT,
                         width, height, 4096u, false)) return false;
    g.target_surface = g.next_handle++;
    wm_virgl_create_surface(&g.cmd, g.target_surface, g.target.resource,
                            WM_VIRGL_FORMAT_B8G8R8A8_UNORM);
    if (!texture_create(&g.background, width, height) ||
        !texture_create(&g.overlay, width, height)) return false;
    g.tiles_x = (width + WM_GPU_TILE - 1u) / WM_GPU_TILE;
    g.tiles_y = (height + WM_GPU_TILE - 1u) / WM_GPU_TILE;
    g.overlay_tiles = (uint8_t *)calloc((size_t)g.tiles_x * g.tiles_y, 1u);
    if (!g.overlay_tiles) return false;
    g.background_dirty = true;
    return true;
}

static void shutdown_device(void)
{
    if (g.fd >= 0 && g.target.bo) {
        (void)scanout(IMPLUS_SCANOUT_RELEASE, (wm_rect_t){0, 0, 0u, 0u});
    }
    release_window_layers();
    texture_release(&g.cursor);
    release_surfaces();
    texture_release(&g.vertices);
    if (g.fd >= 0) (void)file_close(g.fd);
    free(g.cmd_words);
    memset(&g, 0, sizeof(g));
    g.fd = -1;
}

bool wm_gpu_active(void)
{
    return g.active;
}

bool wm_gpu_init(wm_state_t *state)
{
    if (!state || g.active) return g.active;
    uint32_t width = state->compositor.framebuffer_width;
    uint32_t height = state->compositor.framebuffer_height;
    if (width == 0u || height == 0u ||
        width > WM_GPU_TEX_MAX || height > WM_GPU_TEX_MAX) return false;

    file_stat_t st;
    memset(&st, 0, sizeof(st));
    if (file_stat(WM_GPU_DISABLE_FILE, &st) >= 0 && st.exists) {
        serial_write_string("[wm-gpu] disabled by " WM_GPU_DISABLE_FILE "\n");
        return false;
    }

    g.fd = file_open(WM_DRM_RENDER_NODE, 2u /* O_RDWR */);
    if (g.fd < 0) {
        g.fd = -1;
        return false;
    }
    uint32_t has_3d = 0u;
    virtgpu_getparam_t param = { VIRTGPU_PARAM_3D_FEATURES,
                                 (uint64_t)(uintptr_t)&has_3d };
    if (gpu_ioctl(DRM_NR_VIRTGPU_GETPARAM, &param, sizeof(param)) < 0 ||
        has_3d == 0u) {
        serial_write_string("[wm-gpu] no 3D backend; CPU compositing\n");
        shutdown_device();
        return false;
    }

    g.cmd_words = (uint32_t *)malloc(WM_GPU_CMD_WORDS * sizeof(uint32_t));
    if (!g.cmd_words) {
        shutdown_device();
        return false;
    }
    wm_virgl_stream_init(&g.cmd, g.cmd_words, WM_GPU_CMD_WORDS);
    g.next_handle = H_FIRST_DYNAMIC;

    wm_virgl_create_sub_ctx(&g.cmd, 1u);
    wm_virgl_set_sub_ctx(&g.cmd, 1u);
    wm_virgl_create_blend_premultiplied(&g.cmd, H_BLEND);
    wm_virgl_create_dsa_disabled(&g.cmd, H_DSA);
    wm_virgl_create_rasterizer(&g.cmd, H_RASTERIZER);
    wm_virgl_create_shader(&g.cmd, H_VS, WM_VIRGL_SHADER_VERTEX,
                           g_vertex_shader);
    wm_virgl_create_shader(&g.cmd, H_FS, WM_VIRGL_SHADER_FRAGMENT,
                           g_fragment_shader);
    wm_virgl_create_vertex_elements_pos_tex(&g.cmd, H_VERTEX_ELEMENTS);
    wm_virgl_create_sampler_state(&g.cmd, H_SAMPLER, true);

    if (!resource_create(&g.vertices, WM_VIRGL_TARGET_BUFFER,
                         WM_VIRGL_FORMAT_R8_UNORM,
                         WM_VIRGL_BIND_VERTEX_BUFFER, WM_GPU_VB_BYTES, 1u,
                         WM_GPU_VB_BYTES, false) ||
        !create_surfaces(width, height) ||
        !texture_create(&g.cursor, WM_GPU_CURSOR_SIZE, WM_GPU_CURSOR_SIZE)) {
        shutdown_device();
        return false;
    }
    g.bos[0] = g.target.bo;
    g.bos[1] = g.vertices.bo;
    g.bos[2] = g.background.bo;
    g.bos[3] = g.overlay.bo;
    g.bos[4] = g.cursor.bo;
    g.bo_count = 5u;
    if (!submit()) {
        shutdown_device();
        return false;
    }
    int64_t rc = scanout(IMPLUS_SCANOUT_SET, (wm_rect_t){0, 0, 0u, 0u});
    if (rc < 0) {
        log_failure("SCANOUT_SET", rc);
        shutdown_device();
        return false;
    }
    g.active = true;
    serial_write_string("[wm-gpu] compositing on the GPU (virgl), ");
    serial_write_uint32(width);
    serial_write_string("x");
    serial_write_uint32(height);
    serial_write_string("\n");
    return true;
}

void wm_gpu_shutdown(void)
{
    if (g.fd >= 0) shutdown_device();
}

void wm_gpu_background_changed(void)
{
    g.background_dirty = true;
}

void wm_gpu_resize(wm_state_t *state)
{
    if (!g.active || !state) return;
    uint32_t width = state->compositor.framebuffer_width;
    uint32_t height = state->compositor.framebuffer_height;
    if (width == g.width && height == g.height) return;
    (void)scanout(IMPLUS_SCANOUT_RELEASE, (wm_rect_t){0, 0, 0u, 0u});
    release_surfaces();
    wm_virgl_stream_reset(&g.cmd);
    bool ok = width <= WM_GPU_TEX_MAX && height <= WM_GPU_TEX_MAX &&
              create_surfaces(width, height);
    if (ok) {
        g.bos[0] = g.target.bo;
        g.bos[1] = g.vertices.bo;
        g.bos[2] = g.background.bo;
        g.bos[3] = g.overlay.bo;
        g.bo_count = 4u;
        ok = submit() &&
             scanout(IMPLUS_SCANOUT_SET, (wm_rect_t){0, 0, 0u, 0u}) >= 0;
    }
    if (!ok) {
        serial_write_string("[wm-gpu] resize failed; CPU compositing\n");
        shutdown_device();
        return;
    }
    g.scanout_lost = false;
}

/* ---- layers ---- */

static uint64_t theme_signature(const wm_state_t *state)
{
    uint64_t h = fnv(1469598103934665603ull, &state->theme,
                     sizeof(state->theme));
    return fnv(h, &state->font.loaded, sizeof(state->font.loaded));
}

static uint64_t window_signature(const wm_window_t *w, uint64_t theme)
{
    struct {
        uint32_t w, h;
        uint8_t flags[9];
    } key;
    memset(&key, 0, sizeof(key));
    key.w = w->frame.w;
    key.h = w->frame.h;
    key.flags[0] = w->is_system;
    key.flags[1] = w->has_focus;
    key.flags[2] = w->maximized;
    key.flags[3] = w->hover_close;
    key.flags[4] = w->hover_maximize;
    key.flags[5] = w->hover_minimize;
    key.flags[6] = w->surface_opaque;
    key.flags[7] = w->has_icon;
    key.flags[8] = w->surface != NULL;
    uint64_t h = fnv(theme, &key, sizeof(key));
    h = fnv(h, w->title, strnlen(w->title, sizeof(w->title)));
    if (w->has_icon) h = fnv(h, w->icon, sizeof(w->icon));
    return h;
}

static uint32_t window_margin(const wm_state_t *state, const wm_window_t *w)
{
    return w->is_system ? 0u : state->theme.shadow_size * 2u + 6u;
}

static uint32_t window_title(const wm_state_t *state, const wm_window_t *w)
{
    return w->is_system ? 0u : state->theme.title_height;
}

/* Rasterizes the window, at rest, into `region` of its layer: frame at the
 * margin, scale 1, opacity 1. The live position and animation state are
 * what the GPU applies when it draws the layer. */
static void raster_window(wm_state_t *state, wm_window_t *w,
                          gpu_texture_t *t, wm_rect_t region)
{
    region = wm_rect_intersection(region, (wm_rect_t){0, 0, t->width,
                                                      t->height});
    if (region.w == 0u || region.h == 0u) return;
    texture_clear(t, region);
    wm_canvas_t canvas;
    canvas_for(&canvas, t);
    wm_canvas_set_clip(&canvas, region);

    wm_rect_t frame = w->frame;
    float alpha = w->visual_alpha;
    float scale = w->visual_scale;
    float offset = w->visual_offset_y;
    int32_t margin = (int32_t)window_margin(state, w);
    w->frame.x = margin;
    w->frame.y = margin;
    w->visual_alpha = 1.0f;
    w->visual_scale = 1.0f;
    w->visual_offset_y = 0.0f;
    state->compositor.layer_render = true;
    wm_decoration_draw_window(state, &canvas, w);
    state->compositor.layer_render = false;
    w->frame = frame;
    w->visual_alpha = alpha;
    w->visual_scale = scale;
    w->visual_offset_y = offset;
}

static wm_rect_t content_to_layer(const wm_state_t *state,
                                  const wm_window_t *w, wm_rect_t r)
{
    r = wm_rect_intersection(r, (wm_rect_t){0, 0, w->frame.w, w->frame.h});
    if (w->is_system) return r;
    int32_t m = (int32_t)window_margin(state, w);
    r.x += m + 1;
    r.y += m + (int32_t)window_title(state, w) + 1;
    return r;
}

static bool update_window_layer(wm_state_t *state, wm_window_t *w,
                                uint64_t theme)
{
    gpu_window_t *e = &g.windows[w->id];
    if (e->window != w) {
        e->window = w;
        e->valid = false;
    }
    uint32_t margin = window_margin(state, w);
    uint32_t need_w = w->frame.w + margin * 2u;
    uint32_t need_h = w->frame.h + window_title(state, w) + margin * 2u;
    if (need_w == 0u || need_h == 0u ||
        need_w > WM_GPU_TEX_MAX || need_h > WM_GPU_TEX_MAX) {
        /* Larger than any texture: not drawn, but not a GPU failure. */
        texture_release(&e->texture);
        e->valid = false;
        return true;
    }

    gpu_texture_t *t = &e->texture;
    /* Rounded up so an interactive resize reallocates every 64 pixels, not
     * every pointer frame; shrunk again once well oversized. */
    if (!t->bo || t->width < need_w || t->height < need_h ||
        (uint64_t)t->width * t->height >
            (uint64_t)align_up(need_w, WM_GPU_TEX_ALIGN) *
            align_up(need_h, WM_GPU_TEX_ALIGN) * 4u) {
        texture_release(t);
        if (!texture_create(t, wm_min_u32(align_up(need_w, WM_GPU_TEX_ALIGN),
                                          WM_GPU_TEX_MAX),
                            wm_min_u32(align_up(need_h, WM_GPU_TEX_ALIGN),
                                       WM_GPU_TEX_MAX)))
            return false;
        e->valid = false;
    }

    uint64_t signature = window_signature(w, theme);
    bool in_transaction = w->transaction_depth != 0u;
    if (!e->valid || e->signature != signature) {
        if (!texture_wait_idle(t)) return false;
        /* One texel past the window is cleared too: a scaled draw samples
         * bilinearly across the layer's edge. */
        wm_rect_t all = {0, 0, need_w + 1u, need_h + 1u};
        raster_window(state, w, t, all);
        if (!texture_upload(t, all)) return false;
        e->signature = signature;
        e->valid = true;
    } else if (!in_transaction && !wm_region_is_empty(&w->damage)) {
        if (!texture_wait_idle(t)) return false;
        wm_rect_t content = {0, 0, w->frame.w, w->frame.h};
        uint32_t n = w->damage.full ? 1u : w->damage.count;
        for (uint32_t i = 0u; i < n; ++i) {
            wm_rect_t r = content_to_layer(state, w,
                w->damage.full ? content : w->damage.rects[i]);
            raster_window(state, w, t, r);
            if (!texture_upload(t, r)) return false;
        }
    }
    /* A client mid-transaction keeps its damage for the commit. */
    if (!in_transaction) wm_region_reset(&w->damage);
    return true;
}

static bool update_overlay(wm_state_t *state, const wm_rect_t *rects,
                           uint32_t count, uint64_t now_ms)
{
    gpu_texture_t *t = &g.overlay;
    bool waited = false;
    for (uint32_t i = 0u; i < count; ++i) {
        wm_rect_t r = wm_rect_intersection(rects[i],
                                           (wm_rect_t){0, 0, g.width, g.height});
        if (r.w == 0u || r.h == 0u) continue;
        if (!waited) {
            if (!texture_wait_idle(t)) return false;
            waited = true;
        }
        texture_clear(t, r);
        wm_canvas_t canvas;
        canvas_for(&canvas, t);
        wm_canvas_set_clip(&canvas, r);
        wm_taskbar_draw(state, &canvas);
        wm_start_menu_draw(state, &canvas);
        wm_notification_draw(state, &canvas, now_ms);
        wm_wifi_panel_draw(state, &canvas);
        wm_dialog_draw(state, &canvas);

        /* Most damage -- a window dragged across the desktop -- never
         * touches the shell. Uploading it anyway would put back the
         * bandwidth this module exists to save, so a coarse tile map
         * remembers where the overlay can be non-transparent and the
         * rectangle is only sent when it was or is so somewhere. */
        bool upload = false;
        uint32_t tx0 = (uint32_t)r.x / WM_GPU_TILE;
        uint32_t ty0 = (uint32_t)r.y / WM_GPU_TILE;
        uint32_t tx1 = ((uint32_t)r.x + r.w - 1u) / WM_GPU_TILE;
        uint32_t ty1 = ((uint32_t)r.y + r.h - 1u) / WM_GPU_TILE;
        for (uint32_t ty = ty0; ty <= ty1; ++ty) {
            for (uint32_t tx = tx0; tx <= tx1; ++tx) {
                wm_rect_t tile = {(int32_t)(tx * WM_GPU_TILE),
                                  (int32_t)(ty * WM_GPU_TILE),
                                  WM_GPU_TILE, WM_GPU_TILE};
                wm_rect_t part = wm_rect_intersection(tile, r);
                bool any = false;
                for (uint32_t row = 0u; row < part.h && !any; ++row) {
                    const uint32_t *p = &t->pixels[
                        ((uint32_t)part.y + row) * t->width + (uint32_t)part.x];
                    for (uint32_t col = 0u; col < part.w; ++col) {
                        if (p[col]) { any = true; break; }
                    }
                }
                uint8_t *flag = &g.overlay_tiles[ty * g.tiles_x + tx];
                bool covered = part.w == WM_GPU_TILE && part.h == WM_GPU_TILE;
                if (*flag || any) upload = true;
                *flag = (uint8_t)(any || (!covered && *flag));
            }
        }
        if (upload && !texture_upload(t, r)) return false;
    }
    return true;
}

static bool update_cursor(wm_state_t *state, uint64_t theme)
{
    if (g.cursor_valid && g.cursor_style == state->scene.cursor_style &&
        g.cursor_theme == theme) return true;
    gpu_texture_t *t = &g.cursor;
    if (!texture_wait_idle(t)) return false;
    wm_rect_t all = {0, 0, t->width, t->height};
    texture_clear(t, all);
    wm_canvas_t canvas;
    canvas_for(&canvas, t);
    wm_icon_draw_cursor(&canvas, WM_GPU_CURSOR_PAD, WM_GPU_CURSOR_PAD,
                        state->scene.cursor_style, WM_GPU_CURSOR_FILL,
                        state->theme.color.scrim);
    if (!texture_upload(t, all)) return false;
    g.cursor_style = state->scene.cursor_style;
    g.cursor_theme = theme;
    g.cursor_valid = true;
    return true;
}

/* ---- drawing ---- */

static void add_quad(const gpu_texture_t *t, float x0, float y0,
                     float x1, float y1, uint32_t tex_w, uint32_t tex_h,
                     float alpha, bool force_opaque)
{
    if (g.quad_count >= WM_GPU_MAX_QUADS || !t->view) return;
    gpu_quad_t *q = &g.quads[g.quad_count++];
    q->x0 = x0; q->y0 = y0; q->x1 = x1; q->y1 = y1;
    q->u0 = 0.0f;
    q->v0 = 0.0f;
    q->u1 = (float)tex_w / (float)t->width;
    q->v1 = (float)tex_h / (float)t->height;
    if (force_opaque) {
        q->c0[0] = q->c0[1] = q->c0[2] = alpha; q->c0[3] = 0.0f;
        q->c1[0] = q->c1[1] = q->c1[2] = 0.0f;  q->c1[3] = alpha;
    } else {
        q->c0[0] = q->c0[1] = q->c0[2] = q->c0[3] = alpha;
        q->c1[0] = q->c1[1] = q->c1[2] = q->c1[3] = 0.0f;
    }
    q->view = t->view;
}

static void add_window_quad(const wm_state_t *state, const wm_window_t *w)
{
    const gpu_window_t *e = &g.windows[w->id];
    if (!e->valid) return;
    float m = (float)window_margin(state, w);
    float total_h = (float)(w->frame.h + window_title(state, w));
    float tex_w = (float)w->frame.w + 2.0f * m;
    float tex_h = total_h + 2.0f * m;
    float scale = w->visual_scale > 0.0f ? w->visual_scale : 1.0f;
    float alpha = w->visual_alpha;
    if (alpha > 1.0f) alpha = 1.0f;
    /* Scaled about the centre of the window body, as the CPU path does. */
    float cx = (float)w->frame.x + (float)w->frame.w * 0.5f;
    float cy = (float)w->frame.y + total_h * 0.5f + w->visual_offset_y;
    float x0 = (float)w->frame.x - m;
    float y0 = (float)w->frame.y - m + w->visual_offset_y;
    float qx0 = cx + (x0 - cx) * scale;
    float qy0 = cy + (y0 - cy) * scale;
    float qx1 = cx + (x0 + tex_w - cx) * scale;
    float qy1 = cy + (y0 + tex_h - cy) * scale;
    if (scale > 0.999f && scale < 1.001f) {
        /* At rest, keep texels on pixels exactly. */
        qx0 = x0; qy0 = y0; qx1 = x0 + tex_w; qy1 = y0 + tex_h;
    }
    add_quad(&e->texture, qx0, qy0, qx1, qy1,
             (uint32_t)tex_w, (uint32_t)tex_h, alpha, false);
}

static void encode_frame(void)
{
    float vertices[WM_GPU_MAX_QUADS * 16u];
    float sx = 2.0f / (float)g.width;
    float sy = 2.0f / (float)g.height;
    for (uint32_t i = 0u; i < g.quad_count; ++i) {
        const gpu_quad_t *q = &g.quads[i];
        /* Screen pixels to clip space. Clip y = -1 lands on the render
         * target's row 0, and the scanout shows row 0 at the top (as
         * verified on QEMU's virgl scanout), so screen y maps straight
         * through, like the texture rows the layers were uploaded with. */
        float l = q->x0 * sx - 1.0f, r = q->x1 * sx - 1.0f;
        float t = q->y0 * sy - 1.0f, b = q->y1 * sy - 1.0f;
        float *v = &vertices[i * 16u];
        v[0]  = l; v[1]  = t; v[2]  = q->u0; v[3]  = q->v0;
        v[4]  = l; v[5]  = b; v[6]  = q->u0; v[7]  = q->v1;
        v[8]  = r; v[9]  = t; v[10] = q->u1; v[11] = q->v0;
        v[12] = r; v[13] = b; v[14] = q->u1; v[15] = q->v1;
    }

    wm_virgl_stream_t *s = &g.cmd;
    wm_virgl_set_framebuffer(s, g.target_surface);
    wm_virgl_set_viewport(s, g.width, g.height);
    wm_virgl_bind_object(s, WM_VIRGL_OBJECT_BLEND, H_BLEND);
    wm_virgl_bind_object(s, WM_VIRGL_OBJECT_DSA, H_DSA);
    wm_virgl_bind_object(s, WM_VIRGL_OBJECT_RASTERIZER, H_RASTERIZER);
    wm_virgl_bind_object(s, WM_VIRGL_OBJECT_VERTEX_ELEMENTS,
                         H_VERTEX_ELEMENTS);
    wm_virgl_bind_shader(s, H_VS, WM_VIRGL_SHADER_VERTEX);
    wm_virgl_bind_shader(s, H_FS, WM_VIRGL_SHADER_FRAGMENT);
    wm_virgl_bind_sampler_state(s, WM_VIRGL_SHADER_FRAGMENT, H_SAMPLER);
    if (g.quad_count != 0u)
        wm_virgl_inline_write_buffer(s, g.vertices.resource, 0u, vertices,
                                     g.quad_count * 4u * WM_GPU_VERTEX_STRIDE);
    wm_virgl_set_vertex_buffer(s, WM_GPU_VERTEX_STRIDE, 0u,
                               g.vertices.resource);
    for (uint32_t i = 0u; i < g.quad_count; ++i) {
        const gpu_quad_t *q = &g.quads[i];
        float constants[8] = {
            q->c0[0], q->c0[1], q->c0[2], q->c0[3],
            q->c1[0], q->c1[1], q->c1[2], q->c1[3]
        };
        wm_virgl_set_constants(s, WM_VIRGL_SHADER_FRAGMENT, constants, 8u);
        wm_virgl_set_sampler_view(s, WM_VIRGL_SHADER_FRAGMENT, q->view);
        wm_virgl_draw_arrays(s, WM_VIRGL_PRIM_TRIANGLE_STRIP, i * 4u, 4u);
    }
}

static bool present(wm_rect_t r)
{
    int64_t rc = scanout(IMPLUS_SCANOUT_FLUSH, r);
    if (rc >= 0 && !g.scanout_lost) return true;
    /* A layout or mode change put the driver's own resources back on the
     * monitors. Same size: take them again. Otherwise the WM's display
     * reconfiguration resizes the layers and wm_gpu_resize() does it. */
    if (scanout(IMPLUS_SCANOUT_SET, (wm_rect_t){0, 0, 0u, 0u}) >= 0) {
        g.scanout_lost = false;
        return scanout(IMPLUS_SCANOUT_FLUSH,
                       (wm_rect_t){0, 0, g.width, g.height}) >= 0;
    }
    g.scanout_lost = true;
    return true;
}

static bool render_frame(wm_state_t *state, const wm_rect_t *rects,
                         uint32_t count, bool full, uint64_t now_ms)
{
    wm_rect_t screen = {0, 0, g.width, g.height};
    uint64_t theme = theme_signature(state);
    ++g.frame;
    g.quad_count = 0u;
    g.bo_count = 0u;
    g.bos[g.bo_count++] = g.target.bo;
    g.bos[g.bo_count++] = g.vertices.bo;

    if (g.background_dirty && state->compositor.background) {
        if (!texture_wait_idle(&g.background)) return false;
        for (uint32_t row = 0u; row < g.height; ++row)
            memcpy(&g.background.pixels[row * g.background.width],
                   &state->compositor.background[row * g.width],
                   (size_t)g.width * sizeof(uint32_t));
        if (!texture_upload(&g.background, screen)) return false;
        g.background_dirty = false;
    }
    g.bos[g.bo_count++] = g.background.bo;
    add_quad(&g.background, 0.0f, 0.0f, (float)g.width, (float)g.height,
             g.width, g.height, 1.0f, true);

    for (uint32_t layer = WM_LAYER_NORMAL; layer <= WM_LAYER_OVERLAY; ++layer) {
        for (wm_window_t *w = state->scene.layer_bottom[layer]; w;
             w = w->z_prev) {
            if (w->id == 0u || w->id > WM_MAX_WINDOWS) continue;
            if (!w->visible || w->minimized || w->visual_alpha <= 0.0f ||
                !w->surface || w->frame.w == 0u || w->frame.h == 0u)
                continue;
            if (!update_window_layer(state, w, theme)) return false;
            g.windows[w->id].seen_frame = g.frame;
            if (g.bo_count < WM_GPU_MAX_BOS && g.windows[w->id].texture.bo)
                g.bos[g.bo_count++] = g.windows[w->id].texture.bo;
            add_window_quad(state, w);
        }
    }
    /* Layers of windows that are gone, hidden or minimized. */
    for (uint32_t id = 1u; id <= WM_MAX_WINDOWS; ++id) {
        gpu_window_t *e = &g.windows[id];
        if (e->texture.bo && e->seen_frame != g.frame) {
            texture_release(&e->texture);
            memset(e, 0, sizeof(*e));
        }
    }
    /* Their damage was consumed on the CPU path's behalf as well. */
    for (uint32_t layer = WM_LAYER_DESKTOP; layer < WM_LAYER_COUNT; ++layer)
        for (wm_window_t *w = state->scene.layer_bottom[layer]; w; w = w->z_prev)
            if (w->id == 0u || w->id > WM_MAX_WINDOWS ||
                g.windows[w->id].seen_frame != g.frame)
                wm_region_reset(&w->damage);

    wm_rect_t full_rect = screen;
    if (!update_overlay(state, full ? &full_rect : rects, full ? 1u : count,
                        now_ms))
        return false;
    g.bos[g.bo_count++] = g.overlay.bo;
    add_quad(&g.overlay, 0.0f, 0.0f, (float)g.width, (float)g.height,
             g.width, g.height, 1.0f, false);

    if (state->scene.cursor_visible) {
        if (!update_cursor(state, theme)) return false;
        g.bos[g.bo_count++] = g.cursor.bo;
        float x = (float)state->scene.cursor_x - (float)WM_GPU_CURSOR_PAD;
        float y = (float)state->scene.cursor_y - (float)WM_GPU_CURSOR_PAD;
        add_quad(&g.cursor, x, y, x + (float)WM_GPU_CURSOR_SIZE,
                 y + (float)WM_GPU_CURSOR_SIZE, WM_GPU_CURSOR_SIZE,
                 WM_GPU_CURSOR_SIZE, 1.0f, false);
    }

    encode_frame();
    if (!submit()) return false;

    /* The whole scene is redrawn each frame -- a few dozen quads are
     * nothing to a GPU -- but only the damaged part is announced. */
    wm_rect_t shown = {0, 0, 0u, 0u};
    if (full) {
        shown = screen;
    } else {
        int64_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
        for (uint32_t i = 0u; i < count; ++i) {
            wm_rect_t r = wm_rect_intersection(rects[i], screen);
            if (!r.w || !r.h) continue;
            if (r.x < x0) x0 = r.x;
            if (r.y < y0) y0 = r.y;
            if ((int64_t)r.x + r.w > x1) x1 = (int64_t)r.x + r.w;
            if ((int64_t)r.y + r.h > y1) y1 = (int64_t)r.y + r.h;
        }
        if (x1 > x0 && y1 > y0)
            shown = (wm_rect_t){(int32_t)x0, (int32_t)y0,
                                (uint32_t)(x1 - x0), (uint32_t)(y1 - y0)};
    }
    if (shown.w == 0u || shown.h == 0u) return true;
    return present(shown);
}

bool wm_gpu_render(wm_state_t *state, const wm_rect_t *rects, uint32_t count,
                   bool full, uint64_t now_ms)
{
    if (!g.active || !state) return false;
    if (state->compositor.framebuffer_width != g.width ||
        state->compositor.framebuffer_height != g.height) {
        wm_gpu_resize(state);
        if (!g.active) return false;
        full = true;
    }
    wm_virgl_stream_reset(&g.cmd);
    if (render_frame(state, rects, count, full, now_ms)) return true;
    serial_write_string("[wm-gpu] frame failed; back to CPU compositing\n");
    shutdown_device();
    return false;
}
