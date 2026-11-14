#include "WM_Virgl.h"

#include <string.h>

/* enum virgl_context_cmd */
#define CCMD_CREATE_OBJECT         1u
#define CCMD_BIND_OBJECT           2u
#define CCMD_DESTROY_OBJECT        3u
#define CCMD_SET_VIEWPORT_STATE    4u
#define CCMD_SET_FRAMEBUFFER_STATE 5u
#define CCMD_SET_VERTEX_BUFFERS    6u
#define CCMD_DRAW_VBO              8u
#define CCMD_RESOURCE_INLINE_WRITE 9u
#define CCMD_SET_SAMPLER_VIEWS     10u
#define CCMD_SET_CONSTANT_BUFFER   12u
#define CCMD_BIND_SAMPLER_STATES   18u
#define CCMD_SET_SUB_CTX           28u
#define CCMD_CREATE_SUB_CTX        29u
#define CCMD_BIND_SHADER           31u

/* Gallium p_defines.h */
#define PIPE_BLEND_ADD              0u
#define PIPE_BLENDFACTOR_ONE        0x01u
#define PIPE_BLENDFACTOR_INV_SRC_ALPHA 0x13u
#define PIPE_TEX_WRAP_CLAMP_TO_EDGE 2u
#define PIPE_TEX_FILTER_NEAREST     0u
#define PIPE_TEX_FILTER_LINEAR      1u
#define PIPE_TEX_MIPFILTER_NONE     2u
#define PIPE_SWIZZLE_IDENTITY       (0u | (1u << 3) | (2u << 6) | (3u << 9))

/* Virgl's shader packet carries a token-count hint the host uses to size its
 * TGSI token array; the compositor's shaders are a handful of lines. */
#define WM_VIRGL_SHADER_TOKENS 300u

static uint32_t header(uint32_t command, uint32_t object, uint32_t length)
{
    return command | (object << 8) | (length << 16);
}

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* Reserves a command of `length` payload dwords and returns a pointer to its
 * header slot, or NULL (and marks the stream) when it does not fit. */
static uint32_t *begin(wm_virgl_stream_t *s, uint32_t command,
                       uint32_t object, uint32_t length)
{
    if (!s || s->overflow) return NULL;
    if (length > 0xFFFFu || s->length > s->capacity ||
        s->capacity - s->length < length + 1u) {
        s->overflow = true;
        return NULL;
    }
    uint32_t *out = &s->words[s->length];
    out[0] = header(command, object, length);
    s->length += length + 1u;
    return out;
}

void wm_virgl_stream_init(wm_virgl_stream_t *stream, uint32_t *storage,
                          uint32_t capacity_words)
{
    if (!stream) return;
    stream->words = storage;
    stream->capacity = storage ? capacity_words : 0u;
    stream->length = 0u;
    stream->overflow = false;
}

void wm_virgl_stream_reset(wm_virgl_stream_t *stream)
{
    if (!stream) return;
    stream->length = 0u;
    stream->overflow = false;
}

void wm_virgl_create_sub_ctx(wm_virgl_stream_t *s, uint32_t id)
{
    uint32_t *c = begin(s, CCMD_CREATE_SUB_CTX, 0u, 1u);
    if (c) c[1] = id;
}

void wm_virgl_set_sub_ctx(wm_virgl_stream_t *s, uint32_t id)
{
    uint32_t *c = begin(s, CCMD_SET_SUB_CTX, 0u, 1u);
    if (c) c[1] = id;
}

void wm_virgl_create_blend_premultiplied(wm_virgl_stream_t *s, uint32_t handle)
{
    /* handle, S0 (independent blend etc.), S1 (logic op), one S2 per
     * colour buffer (VIRGL_MAX_COLOR_BUFS = 8). */
    uint32_t *c = begin(s, CCMD_CREATE_OBJECT, WM_VIRGL_OBJECT_BLEND, 11u);
    if (!c) return;
    uint32_t rt = 1u |                                   /* blend enable */
                  (PIPE_BLEND_ADD << 1) |
                  (PIPE_BLENDFACTOR_ONE << 4) |
                  (PIPE_BLENDFACTOR_INV_SRC_ALPHA << 9) |
                  (PIPE_BLEND_ADD << 14) |
                  (PIPE_BLENDFACTOR_ONE << 17) |
                  (PIPE_BLENDFACTOR_INV_SRC_ALPHA << 22) |
                  (0xFu << 27);                          /* RGBA writes */
    c[1] = handle;
    c[2] = 0u;
    c[3] = 0u;
    for (uint32_t i = 0u; i < 8u; ++i) c[4u + i] = rt;
}

void wm_virgl_create_dsa_disabled(wm_virgl_stream_t *s, uint32_t handle)
{
    uint32_t *c = begin(s, CCMD_CREATE_OBJECT, WM_VIRGL_OBJECT_DSA, 5u);
    if (!c) return;
    c[1] = handle;
    c[2] = 0u;   /* depth, alpha test off */
    c[3] = 0u;   /* front stencil off */
    c[4] = 0u;   /* back stencil off */
    c[5] = 0u;   /* alpha ref */
}

void wm_virgl_create_rasterizer(wm_virgl_stream_t *s, uint32_t handle)
{
    uint32_t *c = begin(s, CCMD_CREATE_OBJECT, WM_VIRGL_OBJECT_RASTERIZER, 9u);
    if (!c) return;
    c[1] = handle;
    c[2] = (1u << 1) |    /* depth clip */
           (1u << 29);    /* half-pixel centre (GL convention) */
    c[3] = float_bits(1.0f);   /* point size */
    c[4] = 0u;                 /* sprite coord enable */
    c[5] = 0u;                 /* stipple, clip planes */
    c[6] = float_bits(1.0f);   /* line width */
    c[7] = 0u;                 /* polygon offset units */
    c[8] = 0u;                 /* polygon offset scale */
    c[9] = 0u;                 /* polygon offset clamp */
}

void wm_virgl_create_shader(wm_virgl_stream_t *s, uint32_t handle,
                            uint32_t stage, const char *tgsi)
{
    if (!tgsi) return;
    uint32_t text_bytes = (uint32_t)strlen(tgsi) + 1u;   /* with the NUL */
    uint32_t text_words = (text_bytes + 3u) / 4u;
    /* handle, stage, length/offset, token hint, stream-out count. */
    uint32_t *c = begin(s, CCMD_CREATE_OBJECT, WM_VIRGL_OBJECT_SHADER,
                        5u + text_words);
    if (!c) return;
    c[1] = handle;
    c[2] = stage;
    c[3] = text_bytes;          /* complete shader: no continuation bit */
    c[4] = WM_VIRGL_SHADER_TOKENS;
    c[5] = 0u;
    c[6u + text_words - 1u] = 0u;   /* zero the padding of the last word */
    memcpy(&c[6], tgsi, text_bytes);
}

void wm_virgl_bind_shader(wm_virgl_stream_t *s, uint32_t handle,
                          uint32_t stage)
{
    uint32_t *c = begin(s, CCMD_BIND_SHADER, 0u, 2u);
    if (!c) return;
    c[1] = handle;
    c[2] = stage;
}

void wm_virgl_create_vertex_elements_pos_tex(wm_virgl_stream_t *s,
                                             uint32_t handle)
{
    uint32_t *c = begin(s, CCMD_CREATE_OBJECT,
                        WM_VIRGL_OBJECT_VERTEX_ELEMENTS, 1u + 2u * 4u);
    if (!c) return;
    c[1] = handle;
    /* src_offset, instance_divisor, vertex_buffer_index, src_format */
    c[2] = 0u;  c[3] = 0u; c[4] = 0u; c[5] = WM_VIRGL_FORMAT_R32G32_FLOAT;
    c[6] = 8u;  c[7] = 0u; c[8] = 0u; c[9] = WM_VIRGL_FORMAT_R32G32_FLOAT;
}

void wm_virgl_create_sampler_state(wm_virgl_stream_t *s, uint32_t handle,
                                   bool linear)
{
    uint32_t *c = begin(s, CCMD_CREATE_OBJECT,
                        WM_VIRGL_OBJECT_SAMPLER_STATE, 9u);
    if (!c) return;
    uint32_t filter = linear ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
    c[1] = handle;
    c[2] = (PIPE_TEX_WRAP_CLAMP_TO_EDGE << 0) |
           (PIPE_TEX_WRAP_CLAMP_TO_EDGE << 3) |
           (PIPE_TEX_WRAP_CLAMP_TO_EDGE << 6) |
           (filter << 9) |
           (PIPE_TEX_MIPFILTER_NONE << 11) |
           (filter << 13);
    c[3] = 0u;   /* lod bias */
    c[4] = 0u;   /* min lod */
    c[5] = 0u;   /* max lod */
    c[6] = 0u; c[7] = 0u; c[8] = 0u; c[9] = 0u;   /* border colour */
}

void wm_virgl_create_sampler_view_2d(wm_virgl_stream_t *s, uint32_t handle,
                                     uint32_t resource, uint32_t format)
{
    uint32_t *c = begin(s, CCMD_CREATE_OBJECT,
                        WM_VIRGL_OBJECT_SAMPLER_VIEW, 6u);
    if (!c) return;
    c[1] = handle;
    c[2] = resource;
    c[3] = format | (WM_VIRGL_TARGET_TEXTURE_2D << 24);
    c[4] = 0u;   /* first/last layer */
    c[5] = 0u;   /* first/last level */
    c[6] = PIPE_SWIZZLE_IDENTITY;
}

void wm_virgl_create_surface(wm_virgl_stream_t *s, uint32_t handle,
                             uint32_t resource, uint32_t format)
{
    uint32_t *c = begin(s, CCMD_CREATE_OBJECT, WM_VIRGL_OBJECT_SURFACE, 5u);
    if (!c) return;
    c[1] = handle;
    c[2] = resource;
    c[3] = format;
    c[4] = 0u;   /* level */
    c[5] = 0u;   /* first/last layer */
}

void wm_virgl_destroy_object(wm_virgl_stream_t *s, uint32_t type,
                             uint32_t handle)
{
    uint32_t *c = begin(s, CCMD_DESTROY_OBJECT, type, 1u);
    if (c) c[1] = handle;
}

void wm_virgl_bind_object(wm_virgl_stream_t *s, uint32_t type,
                          uint32_t handle)
{
    uint32_t *c = begin(s, CCMD_BIND_OBJECT, type, 1u);
    if (c) c[1] = handle;
}

void wm_virgl_set_framebuffer(wm_virgl_stream_t *s, uint32_t surface)
{
    uint32_t *c = begin(s, CCMD_SET_FRAMEBUFFER_STATE, 0u, 3u);
    if (!c) return;
    c[1] = 1u;        /* colour buffers */
    c[2] = 0u;        /* no depth/stencil surface */
    c[3] = surface;
}

void wm_virgl_set_viewport(wm_virgl_stream_t *s, uint32_t width,
                           uint32_t height)
{
    uint32_t *c = begin(s, CCMD_SET_VIEWPORT_STATE, 0u, 7u);
    if (!c) return;
    float half_w = (float)width * 0.5f;
    float half_h = (float)height * 0.5f;
    c[1] = 0u;                     /* start slot */
    c[2] = float_bits(half_w);     /* scale */
    c[3] = float_bits(half_h);
    c[4] = float_bits(0.5f);
    c[5] = float_bits(half_w);     /* translate */
    c[6] = float_bits(half_h);
    c[7] = float_bits(0.5f);
}

void wm_virgl_bind_sampler_state(wm_virgl_stream_t *s, uint32_t stage,
                                 uint32_t handle)
{
    uint32_t *c = begin(s, CCMD_BIND_SAMPLER_STATES, 0u, 3u);
    if (!c) return;
    c[1] = stage;
    c[2] = 0u;        /* start slot */
    c[3] = handle;
}

void wm_virgl_set_sampler_view(wm_virgl_stream_t *s, uint32_t stage,
                               uint32_t view)
{
    uint32_t *c = begin(s, CCMD_SET_SAMPLER_VIEWS, 0u, 3u);
    if (!c) return;
    c[1] = stage;
    c[2] = 0u;        /* start slot */
    c[3] = view;
}

void wm_virgl_set_vertex_buffer(wm_virgl_stream_t *s, uint32_t stride,
                                uint32_t offset, uint32_t resource)
{
    uint32_t *c = begin(s, CCMD_SET_VERTEX_BUFFERS, 0u, 3u);
    if (!c) return;
    c[1] = stride;
    c[2] = offset;
    c[3] = resource;
}

void wm_virgl_inline_write_buffer(wm_virgl_stream_t *s, uint32_t resource,
                                  uint32_t offset, const void *data,
                                  uint32_t bytes)
{
    if (!data || bytes == 0u || (bytes & 3u) != 0u) return;
    uint32_t *c = begin(s, CCMD_RESOURCE_INLINE_WRITE, 0u, 11u + bytes / 4u);
    if (!c) return;
    c[1] = resource;
    c[2] = 0u;        /* level */
    c[3] = 0u;        /* usage */
    c[4] = 0u;        /* stride */
    c[5] = 0u;        /* layer stride */
    c[6] = offset;    /* box: x is the byte offset in a buffer */
    c[7] = 0u;
    c[8] = 0u;
    c[9] = bytes;
    c[10] = 1u;
    c[11] = 1u;
    memcpy(&c[12], data, bytes);
}

void wm_virgl_set_constants(wm_virgl_stream_t *s, uint32_t stage,
                            const float *values, uint32_t count)
{
    if (!values || count == 0u) return;
    uint32_t *c = begin(s, CCMD_SET_CONSTANT_BUFFER, 0u, 2u + count);
    if (!c) return;
    c[1] = stage;
    c[2] = 0u;        /* buffer index */
    for (uint32_t i = 0u; i < count; ++i) c[3u + i] = float_bits(values[i]);
}

void wm_virgl_draw_arrays(wm_virgl_stream_t *s, uint32_t mode,
                          uint32_t start, uint32_t count)
{
    if (count == 0u) return;
    uint32_t *c = begin(s, CCMD_DRAW_VBO, 0u, 12u);
    if (!c) return;
    c[1] = start;
    c[2] = count;
    c[3] = mode;
    c[4] = 0u;              /* not indexed */
    c[5] = 1u;              /* instances */
    c[6] = 0u;              /* index bias */
    c[7] = 0u;              /* start instance */
    c[8] = 0u;              /* primitive restart */
    c[9] = 0u;              /* restart index */
    c[10] = start;          /* min index */
    c[11] = start + count - 1u;   /* max index */
    c[12] = 0u;             /* count from stream output */
}
