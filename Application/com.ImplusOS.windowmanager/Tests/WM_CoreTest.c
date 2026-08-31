#include "../Compositor/WM_Damage.h"
#include "../Compositor/WM_Raster.h"
#include "../Core/WM_Display.h"
#include "../Core/WM_EventQueue.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static display_topology_t g_stub_topology;
static display_monitor_info_t g_stub_monitors[DISPLAY_MAX_MONITORS];
static int64_t g_stub_display_status = -1;

uint32_t get_display_width(void) { return g_stub_topology.width; }
uint32_t get_display_height(void) { return g_stub_topology.height; }

int64_t display_get_topology(display_topology_t *out_topology)
{
    if (g_stub_display_status < 0 || out_topology == NULL) return -1;
    *out_topology = g_stub_topology;
    return 0;
}

int64_t display_get_monitor_info(uint32_t monitor_index,
                                 display_monitor_info_t *out_info)
{
    if (g_stub_display_status < 0 || out_info == NULL ||
        monitor_index >= g_stub_topology.monitor_count) return -1;
    *out_info = g_stub_monitors[monitor_index];
    return 0;
}

bool wm_compositor_resize(wm_state_t *state, uint32_t width, uint32_t height)
{
    if (!state || width == 0u || height == 0u) return false;
    state->compositor.framebuffer_width = width;
    state->compositor.framebuffer_height = height;
    return true;
}

void wm_compositor_generate_background(wm_state_t *state) { (void)state; }
void wm_compositor_damage_all(wm_state_t *state)
{
    if (state) wm_region_add_full(&state->compositor.damage);
}

static void test_damage_merge_and_clip(void)
{
    wm_region_t region;
    wm_region_reset(&region);
    wm_rect_t bounds = {0, 0, 100u, 100u};
    wm_region_add(&region, (wm_rect_t){10, 10, 20u, 20u}, bounds);
    wm_region_add(&region, (wm_rect_t){25, 25, 20u, 20u}, bounds);
    assert(region.count == 1u);
    assert(region.rects[0].x == 10);
    assert(region.rects[0].y == 10);
    assert(region.rects[0].w == 35u);
    assert(region.rects[0].h == 35u);

    wm_region_add(&region, (wm_rect_t){-10, -10, 15u, 15u}, bounds);
    assert(region.count == 2u);
    assert(region.rects[1].x == 0);
    assert(region.rects[1].y == 0);
    assert(region.rects[1].w == 5u);
    assert(region.rects[1].h == 5u);
}

static void test_damage_overflow(void)
{
    wm_region_t region;
    wm_region_reset(&region);
    wm_rect_t bounds = {0, 0, 4096u, 4096u};
    for (uint32_t i = 0; i <= WM_MAX_DAMAGE_RECTS; ++i)
        wm_region_add(&region,
            (wm_rect_t){(int32_t)(i * 20u), 10, 2u, 2u}, bounds);
    assert(!region.full);
    assert(region.count == 1u);
    assert(region.rects[0].x == 0);
    assert(region.rects[0].y == 10);
    assert(region.rects[0].w == WM_MAX_DAMAGE_RECTS * 20u + 2u);
    assert(region.rects[0].h == 2u);
}

static void test_raster_clip_and_blend(void)
{
    uint32_t pixels[8u * 8u];
    memset(pixels, 0, sizeof(pixels));
    wm_canvas_t canvas;
    wm_canvas_init(&canvas, pixels, 8u, 8u, 8u);
    wm_canvas_set_clip(&canvas, (wm_rect_t){2, 2, 4u, 4u});
    wm_canvas_fill(&canvas, (wm_rect_t){0, 0, 8u, 8u}, 0xFFFF0000u);
    assert(pixels[0] == 0u);
    assert(pixels[2u * 8u + 2u] == 0xFFFF0000u);
    assert(pixels[5u * 8u + 5u] == 0xFFFF0000u);
    assert(pixels[6u * 8u + 6u] == 0u);

    wm_canvas_set_clip(&canvas, (wm_rect_t){0, 0, 8u, 8u});
    pixels[3u * 8u + 3u] = 0xFF0000FFu;
    wm_canvas_put(&canvas, 3, 3, 0x80FF0000u);
    assert(pixels[3u * 8u + 3u] != 0xFF0000FFu);
    assert(pixels[3u * 8u + 3u] != 0x80FF0000u);
}

static void test_rounded_corners(void)
{
    uint32_t pixels[10u * 10u];
    memset(pixels, 0, sizeof(pixels));
    wm_canvas_t canvas;
    wm_canvas_init(&canvas, pixels, 10u, 10u, 10u);
    wm_canvas_fill_rounded(&canvas, (wm_rect_t){1, 1, 8u, 8u}, 3u, 0xFFFFFFFFu);
    assert((pixels[1u * 10u + 1u] >> 24u) <= 96u);
    assert(pixels[4u * 10u + 4u] == 0xFFFFFFFFu);
    assert((pixels[1u * 10u + 3u] >> 24u) > 0u);
}

static void test_icon_fallback(void)
{
    uint32_t pixels[12u * 12u];
    memset(pixels, 0, sizeof(pixels));
    wm_canvas_t canvas;
    wm_canvas_init(&canvas, pixels, 12u, 12u, 12u);
    wm_canvas_draw_icon(&canvas, (wm_rect_t){2, 2, 4u, 4u}, NULL, 255u, 0u, 0xFFFFFFFFu);
    assert(pixels[2u * 12u + 2u] == 0xFFFFFFFFu);
    assert(pixels[5u * 12u + 5u] == 0xFFFFFFFFu);
}

static void test_event_queue(void)
{
    wm_event_queue_t queue;
    wm_event_queue_init(&queue);
    ipc_message_t input;
    memset(&input, 0, sizeof(input));
    input.sender_pid = 42;
    input.size = 4u;
    input.data[0] = 'T';
    assert(wm_event_queue_push(&queue, &input));
    ipc_message_t output;
    assert(wm_event_queue_pop(&queue, &output));
    assert(output.sender_pid == 42);
    assert(output.data[0] == 'T');
    assert(!wm_event_queue_pop(&queue, &output));

    for (uint32_t i = 0u; i < WM_EVENT_QUEUE_SIZE; ++i) {
        input.sender_pid = (int32_t)i;
        assert(wm_event_queue_push(&queue, &input));
    }
    input.sender_pid = 999;
    assert(!wm_event_queue_push(&queue, &input));
    assert(queue.dropped == 1u);
    assert(wm_event_queue_pop(&queue, &output));
    assert(output.sender_pid == 0);
    assert(wm_event_queue_push(&queue, &input));
}

static void test_display_monitor_selection(void)
{
    wm_state_t state;
    memset(&state, 0, sizeof(state));
    state.compositor.framebuffer_width = 3200u;
    state.compositor.framebuffer_height = 1080u;
    state.monitor_count = 2u;
    state.monitors[0].bounds = (wm_rect_t){0, 0, 1920u, 1080u};
    state.monitors[1].bounds = (wm_rect_t){1920, 0, 1280u, 720u};

    assert(wm_display_monitor_at_point(&state, 100, 100) == 0u);
    assert(wm_display_monitor_at_point(&state, 2400, 100) == 1u);
    assert(wm_display_monitor_for_rect(&state,
        (wm_rect_t){1800, 20, 400u, 300u}) == 1u);
}

static void test_display_work_area(void)
{
    wm_state_t state;
    memset(&state, 0, sizeof(state));
    state.compositor.framebuffer_width = 3200u;
    state.compositor.framebuffer_height = 1080u;
    state.theme.dock_height = 40u;
    state.monitor_count = 2u;
    state.monitors[0].bounds = (wm_rect_t){0, 0, 1920u, 1080u};
    state.monitors[1].bounds = (wm_rect_t){1920, 0, 1280u, 1080u};

    wm_rect_t area0 = wm_display_work_area_for_monitor(&state, 0u);
    wm_rect_t area1 = wm_display_work_area_for_monitor(&state, 1u);
    assert(area0.x == 6);
    assert(area0.w == 1908u);
    assert(area0.h < 1080u);
    assert(area1.x == 1926);
    assert(area1.w == 1268u);
    assert(area1.h == area0.h);
}

static void test_display_reconfigure(void)
{
    wm_state_t state;
    memset(&state, 0, sizeof(state));
    state.compositor.framebuffer_width = 1024u;
    state.compositor.framebuffer_height = 768u;
    state.display_topology.generation = 1u;
    state.scene.cursor_x = 900u;
    state.scene.cursor_y = 700u;

    memset(&g_stub_topology, 0, sizeof(g_stub_topology));
    memset(g_stub_monitors, 0, sizeof(g_stub_monitors));
    g_stub_topology.generation = 2u;
    g_stub_topology.monitor_count = 2u;
    g_stub_topology.width = 2048u;
    g_stub_topology.height = 768u;
    g_stub_monitors[0].index = 0u;
    g_stub_monitors[0].width = 1024u;
    g_stub_monitors[0].height = 768u;
    g_stub_monitors[1].index = 1u;
    g_stub_monitors[1].x = 1024;
    g_stub_monitors[1].width = 1024u;
    g_stub_monitors[1].height = 768u;
    g_stub_display_status = 0;

    assert(wm_display_reconfigure_if_needed(&state));
    assert(state.compositor.framebuffer_width == 2048u);
    assert(state.monitor_count == 2u);
    assert(state.scene.cursor_x == 900u);
    assert(state.compositor.damage.full);
    g_stub_display_status = -1;
}

int main(void)
{
    test_damage_merge_and_clip();
    test_damage_overflow();
    test_raster_clip_and_blend();
    test_rounded_corners();
    test_icon_fallback();
    test_event_queue();
    test_display_monitor_selection();
    test_display_work_area();
    test_display_reconfigure();
    return 0;
}
