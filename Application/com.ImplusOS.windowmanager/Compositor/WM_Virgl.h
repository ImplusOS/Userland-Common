#pragma once

/*
 * A minimal encoder for the virgl command stream -- the Gallium state
 * tracker protocol a VirtIO-GPU 3D context executes on the host GPU
 * (virglrenderer's vrend_decode.c is the other end). Only what the window
 * manager's compositor needs is here: state objects, TGSI shaders, sampler
 * views of textures, one render target, a vertex buffer filled inline, and
 * non-indexed draws.
 *
 * Every command is a header dword -- command, object type, payload length in
 * dwords -- followed by that payload. Values and layouts follow
 * virglrenderer's src/virgl_protocol.h and Gallium's p_defines.h; the
 * encoder deliberately has no dependency on either.
 *
 * Pure data transformation, no syscalls, so the host unit tests can check
 * the encoding.
 */

#include <stdbool.h>
#include <stdint.h>

/* Formats (enum virgl_formats; the VirtIO-GPU 2D formats share the numbers).
 * B8G8R8A8 is a little-endian 0xAARRGGBB word: the WM's own pixel layout. */
#define WM_VIRGL_FORMAT_B8G8R8A8_UNORM   1u
#define WM_VIRGL_FORMAT_R32G32_FLOAT     29u
#define WM_VIRGL_FORMAT_R8_UNORM         64u

/* Gallium pipe_texture_target. */
#define WM_VIRGL_TARGET_BUFFER     0u
#define WM_VIRGL_TARGET_TEXTURE_2D 2u

/* Resource bind flags (VIRGL_BIND_*). */
#define WM_VIRGL_BIND_RENDER_TARGET (1u << 1)
#define WM_VIRGL_BIND_SAMPLER_VIEW  (1u << 3)
#define WM_VIRGL_BIND_VERTEX_BUFFER (1u << 4)
#define WM_VIRGL_BIND_SCANOUT       (1u << 18)

/* Gallium shader stages. */
#define WM_VIRGL_SHADER_VERTEX   0u
#define WM_VIRGL_SHADER_FRAGMENT 1u

/* Gallium primitive types. */
#define WM_VIRGL_PRIM_TRIANGLE_STRIP 5u

/* enum virgl_object_type */
#define WM_VIRGL_OBJECT_BLEND           1u
#define WM_VIRGL_OBJECT_RASTERIZER      2u
#define WM_VIRGL_OBJECT_DSA             3u
#define WM_VIRGL_OBJECT_SHADER          4u
#define WM_VIRGL_OBJECT_VERTEX_ELEMENTS 5u
#define WM_VIRGL_OBJECT_SAMPLER_VIEW    6u
#define WM_VIRGL_OBJECT_SAMPLER_STATE   7u
#define WM_VIRGL_OBJECT_SURFACE         8u

typedef struct {
    uint32_t *words;
    uint32_t capacity;   /* dwords */
    uint32_t length;     /* dwords written */
    bool overflow;       /* a command did not fit; the stream is unusable */
} wm_virgl_stream_t;

void wm_virgl_stream_init(wm_virgl_stream_t *stream, uint32_t *storage,
                          uint32_t capacity_words);
void wm_virgl_stream_reset(wm_virgl_stream_t *stream);
static inline uint32_t wm_virgl_stream_bytes(const wm_virgl_stream_t *stream)
{
    return stream->length * 4u;
}

void wm_virgl_create_sub_ctx(wm_virgl_stream_t *s, uint32_t id);
void wm_virgl_set_sub_ctx(wm_virgl_stream_t *s, uint32_t id);

/* Blending for premultiplied-alpha sources: dst = src + dst * (1 - src.a),
 * on colour and alpha alike. */
void wm_virgl_create_blend_premultiplied(wm_virgl_stream_t *s, uint32_t handle);
/* No depth, stencil or alpha test. */
void wm_virgl_create_dsa_disabled(wm_virgl_stream_t *s, uint32_t handle);
/* Filled triangles, no culling, no scissor, GL pixel centres at +0.5. */
void wm_virgl_create_rasterizer(wm_virgl_stream_t *s, uint32_t handle);
/* `tgsi` is the shader in TGSI text form, NUL-terminated. */
void wm_virgl_create_shader(wm_virgl_stream_t *s, uint32_t handle,
                            uint32_t stage, const char *tgsi);
void wm_virgl_bind_shader(wm_virgl_stream_t *s, uint32_t handle,
                          uint32_t stage);
/* Two float2 attributes from vertex buffer 0: position, then texcoord. */
void wm_virgl_create_vertex_elements_pos_tex(wm_virgl_stream_t *s,
                                             uint32_t handle);
/* Clamp-to-edge, no mipmaps; bilinear when `linear`, nearest otherwise. */
void wm_virgl_create_sampler_state(wm_virgl_stream_t *s, uint32_t handle,
                                   bool linear);
void wm_virgl_create_sampler_view_2d(wm_virgl_stream_t *s, uint32_t handle,
                                     uint32_t resource, uint32_t format);
void wm_virgl_create_surface(wm_virgl_stream_t *s, uint32_t handle,
                             uint32_t resource, uint32_t format);
void wm_virgl_destroy_object(wm_virgl_stream_t *s, uint32_t type,
                             uint32_t handle);
/* Blend, DSA, rasterizer and vertex-elements objects. */
void wm_virgl_bind_object(wm_virgl_stream_t *s, uint32_t type,
                          uint32_t handle);

void wm_virgl_set_framebuffer(wm_virgl_stream_t *s, uint32_t surface);
/* Maps clip space onto a width x height target: NDC y = -1 lands on the
 * target's row 0 (GL's window-space orientation, no flip). */
void wm_virgl_set_viewport(wm_virgl_stream_t *s, uint32_t width,
                           uint32_t height);
void wm_virgl_bind_sampler_state(wm_virgl_stream_t *s, uint32_t stage,
                                 uint32_t handle);
void wm_virgl_set_sampler_view(wm_virgl_stream_t *s, uint32_t stage,
                               uint32_t view);
void wm_virgl_set_vertex_buffer(wm_virgl_stream_t *s, uint32_t stride,
                                uint32_t offset, uint32_t resource);
/* Writes `bytes` (a multiple of 4) into a PIPE_BUFFER resource at
 * `offset`, carried inside the command stream. */
void wm_virgl_inline_write_buffer(wm_virgl_stream_t *s, uint32_t resource,
                                  uint32_t offset, const void *data,
                                  uint32_t bytes);
/* Constant buffer 0 of `stage`: `count` floats. */
void wm_virgl_set_constants(wm_virgl_stream_t *s, uint32_t stage,
                            const float *values, uint32_t count);
void wm_virgl_draw_arrays(wm_virgl_stream_t *s, uint32_t mode,
                          uint32_t start, uint32_t count);
