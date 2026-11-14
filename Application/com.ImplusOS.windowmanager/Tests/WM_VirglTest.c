/* Host checks for the GPU compositor's pure parts: the virgl command
 * encoder (packet lengths as virglrenderer's decoder validates them) and the
 * premultiplied "over" the layers are rasterized with. */
#include "../Compositor/WM_Raster.h"
#include "../Compositor/WM_Virgl.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t cmd(uint32_t word) { return word & 0xFFu; }
static uint32_t obj(uint32_t word) { return (word >> 8) & 0xFFu; }
static uint32_t len(uint32_t word) { return word >> 16; }

static void test_shader_packet(void)
{
    uint32_t words[256];
    wm_virgl_stream_t s;
    wm_virgl_stream_init(&s, words, 256u);
    const char *text = "FRAG\nEND\n";          /* 9 chars + NUL = 10 bytes */
    wm_virgl_create_shader(&s, 7u, 1u, text);
    assert(!s.overflow);
    assert(cmd(words[0]) == 1u && obj(words[0]) == 4u);
    /* vrend: pkt_length = length - 5 text dwords, and
     * (offlen + 3) / 4 must equal it. */
    uint32_t text_words = len(words[0]) - 5u;
    assert(words[3] == 10u);
    assert((words[3] + 3u) / 4u == text_words);
    assert(memcmp(&words[6], text, 10u) == 0);
    assert(((const uint8_t *)&words[6])[10] == 0u);
    assert(s.length == 1u + len(words[0]));
}

static void test_fixed_sizes(void)
{
    uint32_t words[512];
    wm_virgl_stream_t s;
    wm_virgl_stream_init(&s, words, 512u);
    uint32_t at = 0u;

    wm_virgl_create_blend_premultiplied(&s, 1u);
    assert(len(words[at]) == 11u);           /* VIRGL_OBJ_BLEND_SIZE */
    /* rt[0]: enable, ONE / INV_SRC_ALPHA on colour and alpha, RGBA mask */
    uint32_t rt = words[at + 4u];
    assert((rt & 1u) == 1u);
    assert(((rt >> 4) & 0x1Fu) == 0x01u && ((rt >> 9) & 0x1Fu) == 0x13u);
    assert(((rt >> 17) & 0x1Fu) == 0x01u && ((rt >> 22) & 0x1Fu) == 0x13u);
    assert((rt >> 27) == 0xFu);
    at = s.length;

    wm_virgl_create_dsa_disabled(&s, 2u);
    assert(len(words[at]) == 5u); at = s.length;
    wm_virgl_create_rasterizer(&s, 3u);
    assert(len(words[at]) == 9u); at = s.length;
    wm_virgl_create_vertex_elements_pos_tex(&s, 4u);
    assert(len(words[at]) == 9u && (len(words[at]) - 1u) % 4u == 0u);
    at = s.length;
    wm_virgl_create_sampler_state(&s, 5u, true);
    assert(len(words[at]) == 9u); at = s.length;
    wm_virgl_create_sampler_view_2d(&s, 6u, 42u, WM_VIRGL_FORMAT_B8G8R8A8_UNORM);
    assert(len(words[at]) == 6u);
    assert(words[at + 3u] == (1u | (2u << 24)));   /* format | 2D << 24 */
    at = s.length;
    wm_virgl_create_surface(&s, 7u, 43u, WM_VIRGL_FORMAT_B8G8R8A8_UNORM);
    assert(len(words[at]) == 5u); at = s.length;
    wm_virgl_set_framebuffer(&s, 7u);
    assert(len(words[at]) == 3u && words[at + 1u] == 1u); at = s.length;
    wm_virgl_set_viewport(&s, 800u, 600u);
    assert(len(words[at]) == 7u); at = s.length;
    wm_virgl_bind_shader(&s, 8u, 1u);
    assert(cmd(words[at]) == 31u && len(words[at]) == 2u); at = s.length;
    float c[8] = {0};
    wm_virgl_set_constants(&s, 1u, c, 8u);
    assert(cmd(words[at]) == 12u && len(words[at]) == 10u); at = s.length;
    uint32_t data[4] = {1u, 2u, 3u, 4u};
    wm_virgl_inline_write_buffer(&s, 9u, 64u, data, sizeof(data));
    assert(cmd(words[at]) == 9u && len(words[at]) == 11u + 4u);
    assert(words[at + 6u] == 64u && words[at + 9u] == 16u);
    assert(words[at + 12u] == 1u && words[at + 15u] == 4u);
    at = s.length;
    wm_virgl_draw_arrays(&s, WM_VIRGL_PRIM_TRIANGLE_STRIP, 8u, 4u);
    assert(cmd(words[at]) == 8u && len(words[at]) == 12u);
    assert(words[at + 1u] == 8u && words[at + 2u] == 4u &&
           words[at + 3u] == 5u && words[at + 5u] == 1u &&
           words[at + 10u] == 8u && words[at + 11u] == 11u);
    assert(!s.overflow);
}

static void test_overflow(void)
{
    uint32_t words[4];
    wm_virgl_stream_t s;
    wm_virgl_stream_init(&s, words, 4u);
    wm_virgl_create_dsa_disabled(&s, 1u);    /* 6 dwords: does not fit */
    assert(s.overflow && s.length == 0u);
    wm_virgl_stream_reset(&s);
    wm_virgl_bind_shader(&s, 1u, 0u);        /* 3 dwords */
    assert(!s.overflow && s.length == 3u);
}

static void test_premultiplied_over(void)
{
    uint32_t pixels[4] = {0u, 0u, 0xFF0000FFu, 0u};
    wm_canvas_t canvas;
    wm_canvas_init(&canvas, pixels, 4u, 1u, 4u);
    /* 50% red over transparent: premultiplied red at half coverage. */
    wm_canvas_put(&canvas, 0, 0, 0x80FF0000u);
    assert((pixels[0] >> 24) == 0x80u);
    assert(((pixels[0] >> 16) & 0xFFu) == 0x80u);
    assert((pixels[0] & 0xFFFFu) == 0u);
    /* The same over an opaque pixel stays opaque: the CPU desktop path. */
    wm_canvas_put(&canvas, 2, 0, 0x80FF0000u);
    assert((pixels[2] >> 24) == 0xFFu);
    /* Two 50% layers over transparent add up to 75% coverage. */
    wm_canvas_put(&canvas, 1, 0, 0x80000000u);
    wm_canvas_put(&canvas, 1, 0, 0x80000000u);
    assert((pixels[1] >> 24) >= 0xBFu && (pixels[1] >> 24) <= 0xC0u);
}

int main(void)
{
    test_shader_packet();
    test_fixed_sizes();
    test_overflow();
    test_premultiplied_over();
    puts("WM_VirglTest: all checks passed");
    return 0;
}
