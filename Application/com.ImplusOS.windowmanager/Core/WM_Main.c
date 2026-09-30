#include "../WindowManager.h"

#include "WM_Assets.h"
#include "WM_Display.h"
#include "WM_EventQueue.h"
#include "../Animation/WM_Animation.h"
#include "../Compositor/WM_Compositor.h"
#include "../Compositor/WM_Damage.h"
#include "../Font/WM_Font.h"
#include "../Input/WM_Input.h"
#include "../IPC/WM_IPC.h"
#include "../SceneGraph/WM_Node.h"
#include "../Theme/WM_Theme.h"
#include "../UI/WM_Notification.h"
#include "../UI/WM_StartMenu.h"
#include "../UI/WM_Taskbar.h"
#include "../UI/WM_WifiPanel.h"
#include "../../../../Userland/Source/Syscalls.h"
#include "../../../../Userland/API/Source/Process.h"
#include "../../../../Userland/API/Source/Serial.h"
#include "../../../../Userland/API/Source/Time.h"

#include <string.h>

wm_state_t g_wm_state;

#define CLOCK_UPDATE_INTERVAL_MS 1000u
#define WM_IPC_DRAIN_BUDGET 64u
#define WM_EVENT_PROCESS_BUDGET 64u
#define WM_POINTER_IDLE_POLL_MS 0u
#define WM_POINTER_ACTIVE_POLL_MS 0u
#define WM_POINTER_ACTIVE_WINDOW_MS 96u
/* Every sleep here is rounded up to a scheduler tick (250 Hz -> 4 ms), so
 * these are "yield at least once per loop" values, not busy-spins: a longer
 * idle sleep only adds its own remainder to pointer latency, because input is
 * harvested solely when a process reads it (TIMER_RUN_DRIVER_POLLS_IN_IRQ is
 * off), and a 30 ms sleep here used to hold a click for most of that time. */
#define WM_IDLE_SLEEP_MS 1u
#define WM_ACTIVE_SLEEP_MS 1u
#define WM_DEFERRED_START_DELAY_MS 500u
#define WM_DEFERRED_STEP_DELAY_MS 50u
#define WM_FRAME_INTERVAL_MS 16u

/* [ktr] WC -- third stage of the keystroke trace whose kernel half is
 * Kernel/Source/Debug/KeyTrace.h: this loop rendered a frame after a key was
 * consumed. RD->OUT is the input path, OUT->MD is X getting the key and the
 * client painting it, MD->WC is this compositor, and WC->the harness seeing
 * new pixels is the flip. Off unless the build passes -DKEY_TRACE=1, which
 * for this Makefile means OPT_FLAGS (the only hook it has on extra flags).
 *
 * One line per keystroke, not per frame: Chromium's window gets damaged
 * several times between a key and the frame that shows it, and tracing every
 * one would put ~60 synchronous UART writes a second in this loop -- the
 * [wmdbg] line above records exactly how much that costs. */
#ifndef KEY_TRACE
#define KEY_TRACE 0
#endif
static uint32_t s_key_owed = 0u;

#if KEY_TRACE
static void wm_key_trace_frame(void)
{
    char line[48];
    uint64_t ms = get_uptime_ms();
    uint32_t n = 0u;
    char digits[20];
    uint32_t nd = 0u;

    /* Rate-limit: log up to 4 frames after a keystroke to catch the actual
     * paint frame, avoiding mis-attribution to an unrelated dirty frame. */
    static uint32_t s_frame_count = 0u;
    if (s_key_owed == 0u) {
        s_frame_count = 0u;
        return;
    }
    line[n++] = '['; line[n++] = 'k'; line[n++] = 't';
    line[n++] = 'r'; line[n++] = ']'; line[n++] = ' ';
    line[n++] = 'W'; line[n++] = 'C'; line[n++] = ' ';
    do {
        digits[nd++] = (char)('0' + (char)(ms % 10u));
        ms /= 10u;
    } while (ms != 0u);
    while (nd != 0u) {
        line[n++] = digits[--nd];
    }
    line[n++] = ' ';
    do {
        digits[nd++] = (char)('0' + (char)(s_frame_count % 10u));
        s_frame_count /= 10u;
    } while (s_frame_count != 0u);
    while (nd != 0u) {
        line[n++] = digits[--nd];
    }
    line[n++] = '\n';
    line[n] = '\0';
    serial_write_string(line);
    s_frame_count++;
    if (s_frame_count >= 4u) {
        s_key_owed = 0u;
        s_frame_count = 0u;
    }
}
#else
static void wm_key_trace_frame(void)
{
    (void)s_key_owed;
}
#endif

/* Per-second timing line on COM1, used while chasing pointer latency: it
 * splits one loop iteration into IPC / input / misc / render and reports the
 * worst time from "raw mouse event consumed" to "frame flushed".
 *
 * Off by default: the line itself costs ~130 UART characters of synchronous
 * busy-wait inside this loop (~11 ms at 115200 baud), which showed up as
 * gap_max=12 ms and smeared the very latencies it reports. Build with
 * -DWM_DEBUG_TIMING=1 to bring it back while diagnosing. */
#ifndef WM_DEBUG_TIMING
#define WM_DEBUG_TIMING 0
#endif
enum {
    WM_DBG_IPC = 0,     /* IPC drain + queued event handling */
    WM_DBG_INPUT,       /* keyboard + pointer poll, incl. route_mouse() */
    WM_DBG_MISC,        /* deferred work, panels, clock, animation ticks */
    WM_DBG_RENDER,      /* wm_compositor_render() */
    WM_DBG_PHASES
};

typedef struct {
    uint64_t window_start_ms;
    uint64_t gap_max_ms;                /* slowest loop iteration */
    uint64_t phase_max_ms[WM_DBG_PHASES];
    uint64_t in2out_max_ms;             /* mouse consumed -> frame flushed */
    uint64_t input_pending_ms;          /* 0 = no unconsumed measurement */
    uint32_t iterations;
    uint32_t render_count;
    uint32_t input_count;
} wm_dbg_t;

static wm_dbg_t g_wm_dbg;

static void wm_dbg_reset_window(uint64_t now_ms)
{
    g_wm_dbg.window_start_ms  = now_ms;
    g_wm_dbg.gap_max_ms       = 0u;
    g_wm_dbg.in2out_max_ms    = 0u;
    g_wm_dbg.input_pending_ms = 0u;
    g_wm_dbg.iterations       = 0u;
    g_wm_dbg.render_count     = 0u;
    g_wm_dbg.input_count      = 0u;
    for (uint32_t i = 0; i < WM_DBG_PHASES; ++i)
        g_wm_dbg.phase_max_ms[i] = 0u;
}

static void wm_dbg_acc(uint32_t phase, uint64_t duration_ms)
{
    if (phase < WM_DBG_PHASES && duration_ms > g_wm_dbg.phase_max_ms[phase])
        g_wm_dbg.phase_max_ms[phase] = duration_ms;
}

static void wm_dbg_flush_if_due(uint64_t now_ms)
{
#if WM_DEBUG_TIMING
    if (now_ms - g_wm_dbg.window_start_ms < 1000u) return;

    serial_write_string("[wmdbg] t=");
    serial_write_uint64(now_ms);
    serial_write_string(" gap=");
    serial_write_uint64(g_wm_dbg.gap_max_ms);
    serial_write_string(" ipc=");
    serial_write_uint64(g_wm_dbg.phase_max_ms[WM_DBG_IPC]);
    serial_write_string(" in=");
    serial_write_uint64(g_wm_dbg.phase_max_ms[WM_DBG_INPUT]);
    serial_write_string(" misc=");
    serial_write_uint64(g_wm_dbg.phase_max_ms[WM_DBG_MISC]);
    serial_write_string(" rnd=");
    serial_write_uint64(g_wm_dbg.render_count);
    serial_write_char('/');
    serial_write_uint64(g_wm_dbg.phase_max_ms[WM_DBG_RENDER]);
    serial_write_string(" in2out=");
    serial_write_uint64(g_wm_dbg.in2out_max_ms);
    serial_write_string(" ev=");
    serial_write_uint64(g_wm_dbg.input_count);
    serial_write_char('\n');
    wm_dbg_reset_window(now_ms);
#else
    (void)now_ms;
#endif
}

static const char *const WM_FONT_PATH =
    "/Userland/com.ImplusOS.windowmanager"
    "/Resource/Fonts/NotoSansJP-Regular.ttf";

static wm_rect_t wm_service_bounds(const wm_state_t *state)
{
    if (!state) return (wm_rect_t){0, 0, 0, 0};
    return (wm_rect_t){0, 0,
        state->compositor.framebuffer_width,
        state->compositor.framebuffer_height};
}

static wm_rect_t wm_service_expand_rect(wm_rect_t rect, int32_t amount)
{
    return (wm_rect_t){
        rect.x - amount,
        rect.y - amount,
        rect.w + (uint32_t)(amount * 2),
        rect.h + (uint32_t)(amount * 2)
    };
}

static void wm_service_damage_rect(wm_state_t *state, wm_rect_t rect)
{
    if (!state || rect.w == 0u || rect.h == 0u) return;
    wm_region_add(&state->compositor.damage, rect,
                  wm_service_bounds(state));
}

static void wm_service_damage_visible_chrome(wm_state_t *state)
{
    if (!state) return;
    wm_service_damage_rect(state,
        wm_service_expand_rect(wm_taskbar_rect(state), 6));
    if (state->launcher_open) {
        wm_service_damage_rect(state,
            wm_service_expand_rect(wm_start_menu_rect(state), 8));
    }
    if (state->notification_center_open) {
        wm_service_damage_rect(state,
            wm_service_expand_rect(wm_notification_center_rect(state), 8));
    }
    if (state->wifi_panel.open) {
        wm_service_damage_rect(state,
            wm_service_expand_rect(wm_wifi_panel_rect(state), 8));
    }
}

static bool wm_service_do_deferred_work(wm_state_t *state)
{
    if (!state) return false;

    if (!state->font.loaded) {
        (void)wm_font_init(&state->font, WM_FONT_PATH);
        wm_compositor_generate_background(state);   /* now with desktop-icon labels */
        wm_compositor_damage_all(state);
        return true;
    }

    if (state->assets.metadata_loaded && !state->assets.icons_loaded) {
        bool loaded_one = wm_assets_load_next_icon(&state->assets);
        /* The desktop-icon grid is painted into the static background, and
         * it draws each icon with the artwork of the launcher entry it
         * points at -- which only exists once the icons have been loaded.
         * Regenerate once, when the last one lands, or the desktop keeps
         * the generic glyphs it was drawn with at startup. */
        if (state->assets.icons_loaded) {
            wm_compositor_generate_background(state);
            wm_compositor_damage_all(state);
            return true;
        }
        if (loaded_one) {
            wm_service_damage_visible_chrome(state);
            return true;
        }
    }

    return false;
}

void wm_service_init(wm_state_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->launcher_hover_index = -1;
    state->launcher_selected_index = -1;
    state->wifi_panel.selected_index = -1;
    /* Sentinel: no valid search_len can ever equal this, so the Start Menu's
     * filter cache is always (re)built the first time it's needed instead
     * of matching zero-initialized state and staying empty. */
    state->filter_generation = UINT32_MAX;
    state->running = true;
    wm_scene_init(&state->scene);
    wm_event_queue_init(&state->event_queue);
    wm_theme_set_defaults(&state->theme);

    /* The user's own theme first, then the M3 light scheme shipped with the
     * image. Failing both, wm_theme_set_defaults() above has already left a
     * complete M3 baseline theme in place, so the shell never draws with an
     * empty palette. */
    if (!wm_theme_load(&state->theme, "/var/System/shell.theme")) {
        (void)wm_theme_load(&state->theme,
            "/Userland/com.ImplusOS.windowmanager"
            "/Resource/Themes/Material.theme");
    }

    (void)wm_assets_init_metadata(&state->assets);
    (void)wm_assets_load_logo(&state->assets);

    (void)wm_assets_reload_wallpaper(&state->assets);

    (void)wm_display_update_from_system(state);
    uint32_t width  = state->display_topology.width;
    uint32_t height = state->display_topology.height;
    if (width == 0u || height == 0u) {
        width = get_display_width();
        height = get_display_height();
        wm_display_set_fallback(state, width, height);
    }
    if (!wm_compositor_init(state, width, height)) {
        state->running = false;
        return;
    }
    state->scene.cursor_x       = width  / 2u;
    state->scene.cursor_y       = height / 2u;
    state->scene.cursor_visible = true;
    state->scene.cursor_style   = WM_CURSOR_DEFAULT;

    (void)wm_taskbar_update_clock(state);
    wm_taskbar_start_ntp(state);

    wm_compositor_generate_background(state);
    wm_compositor_damage_all(state);
    state->compositor.next_frame_ms = get_uptime_ms();

    /* Claims exclusive ownership of the raw keyboard/mouse stream from the
     * kernel (see Kernel/Core/syscall/Syscall_InputOwner.h) -- without it
     * we'd be a compositor with no input. */
    if (window_register_service() != 0) {
        state->running = false;
    }
}

void wm_service_main_loop(void)
{
    wm_service_init(&g_wm_state);
    if (!g_wm_state.running) {
        for (;;) sleep_ms(1000u);
    }

    uint64_t last_clock_ms = 0u;
    uint64_t last_display_poll_ms = 0u;
    uint64_t last_pointer_poll_ms = 0u;
    uint64_t pointer_active_until_ms = 0u;
    uint64_t next_deferred_work_ms = get_uptime_ms() + WM_DEFERRED_START_DELAY_MS;
    uint64_t prev_iter_ms = get_uptime_ms();
    wm_dbg_reset_window(prev_iter_ms);

    while (g_wm_state.running) {
        bool did_work = false;
        uint64_t iter_ms = get_uptime_ms();
        uint64_t gap_ms = iter_ms - prev_iter_ms;
        prev_iter_ms = iter_ms;
        if (gap_ms > g_wm_dbg.gap_max_ms) g_wm_dbg.gap_max_ms = gap_ms;
        ++g_wm_dbg.iterations;
        wm_dbg_flush_if_due(iter_ms);
        uint64_t phase_ms = iter_ms;
        ipc_message_t incoming;
        uint32_t ipc_drained = 0u;
        while (ipc_drained < WM_IPC_DRAIN_BUDGET &&
               ipc_receive_message(&incoming) == 0) {
            did_work = true;
            ++ipc_drained;
            if (!wm_event_queue_push_coalesced(&g_wm_state.event_queue,
                                               &incoming)) {
                ipc_message_t oldest;
                if (wm_event_queue_pop(&g_wm_state.event_queue, &oldest))
                    wm_ipc_handle_message(&g_wm_state, &oldest);
                if (!wm_event_queue_push_coalesced(&g_wm_state.event_queue,
                                                   &incoming))
                    wm_ipc_handle_message(&g_wm_state, &incoming);
            }
        }
        ipc_message_t event;
        uint32_t events_processed = 0u;
        while (events_processed < WM_EVENT_PROCESS_BUDGET &&
               wm_event_queue_pop(&g_wm_state.event_queue, &event)) {
            did_work = true;
            ++events_processed;
            wm_ipc_handle_message(&g_wm_state, &event);
        }
        {
            uint64_t now_phase_ms = get_uptime_ms();
            wm_dbg_acc(WM_DBG_IPC, now_phase_ms - phase_ms);
            phase_ms = now_phase_ms;
        }

        if (wm_input_poll_raw_keyboard(&g_wm_state)) {
            did_work = true;
            s_key_owed = 1u;
        }

        uint64_t now_ms = get_uptime_ms();
        uint32_t pointer_poll_ms =
            now_ms < pointer_active_until_ms ?
                WM_POINTER_ACTIVE_POLL_MS : WM_POINTER_IDLE_POLL_MS;
        if (now_ms - last_pointer_poll_ms >= pointer_poll_ms) {
            last_pointer_poll_ms = now_ms;
            if (wm_input_poll_raw_mouse(&g_wm_state)) {
                did_work = true;
                pointer_active_until_ms = now_ms + WM_POINTER_ACTIVE_WINDOW_MS;
                ++g_wm_dbg.input_count;
                if (g_wm_dbg.input_pending_ms == 0u)
                    g_wm_dbg.input_pending_ms = get_uptime_ms();
            }
        }
        {
            uint64_t now_phase_ms = get_uptime_ms();
            wm_dbg_acc(WM_DBG_INPUT, now_phase_ms - phase_ms);
            phase_ms = now_phase_ms;
        }

        if (now_ms - last_display_poll_ms >= 250u) {
            last_display_poll_ms = now_ms;
            (void)wm_display_reconfigure_if_needed(&g_wm_state);
        }
        if (!did_work && now_ms >= next_deferred_work_ms &&
            now_ms >= pointer_active_until_ms &&
            wm_service_do_deferred_work(&g_wm_state)) {
            did_work = true;
            next_deferred_work_ms = now_ms + WM_DEFERRED_STEP_DELAY_MS;
        }
        if (wm_wifi_panel_poll(&g_wm_state, now_ms)) {
            wm_service_damage_rect(&g_wm_state,
                wm_service_expand_rect(wm_wifi_panel_rect(&g_wm_state), 8));
            did_work = true;
        }

        wm_taskbar_poll_ntp(&g_wm_state);
        if (g_wm_state.ntp.ntp_ready &&
            g_wm_state.ntp.ntp_local_port == 0u &&
            now_ms - g_wm_state.ntp.ntp_poll_start_ms >= NTP_REFRESH_MS) {
            wm_taskbar_start_ntp(&g_wm_state);
        }

        if (now_ms - last_clock_ms >= CLOCK_UPDATE_INTERVAL_MS) {
            last_clock_ms = now_ms;
            if (wm_taskbar_update_clock(&g_wm_state)) {
                wm_rect_t clock_rect = wm_taskbar_clock_rect(&g_wm_state);
                wm_region_add(&g_wm_state.compositor.damage, clock_rect,
                    (wm_rect_t){0, 0, g_wm_state.compositor.framebuffer_width,
                                 g_wm_state.compositor.framebuffer_height});
                g_wm_state.compositor.next_frame_ms = 0u;
                did_work = true;
            }
        }
        {
            uint64_t now_phase_ms = get_uptime_ms();
            wm_dbg_acc(WM_DBG_MISC, now_phase_ms - phase_ms);
            phase_ms = now_phase_ms;
        }

        bool animating     = wm_animation_tick(&g_wm_state, now_ms);
        bool notifications = wm_notification_tick(&g_wm_state, now_ms);

        bool pending_frame = wm_compositor_has_pending_frame(&g_wm_state);
        if (now_ms >= g_wm_state.compositor.next_frame_ms &&
            (animating || notifications || pending_frame)) {
            wm_compositor_render(&g_wm_state, now_ms);
            g_wm_state.compositor.next_frame_ms = now_ms + WM_FRAME_INTERVAL_MS;
            wm_key_trace_frame();
            did_work = true;
            ++g_wm_dbg.render_count;
            uint64_t flushed_ms = get_uptime_ms();
            wm_dbg_acc(WM_DBG_RENDER, flushed_ms - phase_ms);
            if (g_wm_dbg.input_pending_ms != 0u) {
                uint64_t in2out_ms = flushed_ms - g_wm_dbg.input_pending_ms;
                if (in2out_ms > g_wm_dbg.in2out_max_ms)
                    g_wm_dbg.in2out_max_ms = in2out_ms;
                g_wm_dbg.input_pending_ms = 0u;
            }
        } else if (!animating && !notifications && !pending_frame) {
            if (g_wm_dbg.input_pending_ms != 0u &&
                now_ms - g_wm_dbg.input_pending_ms > 100u)
                g_wm_dbg.input_pending_ms = 0u;   /* no frame: not measurable */
            sleep_ms((did_work || now_ms < pointer_active_until_ms) ?
                         WM_ACTIVE_SLEEP_MS : WM_IDLE_SLEEP_MS);
            continue;
        }
        if (g_wm_dbg.input_pending_ms != 0u &&
            now_ms - g_wm_dbg.input_pending_ms > 100u)
            g_wm_dbg.input_pending_ms = 0u;
        sleep_ms((did_work || now_ms < pointer_active_until_ms) ?
                     1u : WM_ACTIVE_SLEEP_MS);
    }
}
