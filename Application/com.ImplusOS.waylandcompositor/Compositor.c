/*
 * Scene, output, input and main loop.
 *
 * The Wayland protocol lives in Wayland.c and knows nothing about how
 * pixels reach the screen or where input comes from. This file supplies
 * both, in either of two backends chosen at startup:
 *
 *   panel  -- no window manager is running. The compositor claims the raw
 *             HID stream (which is also what makes it the kernel's input
 *             owner, the same claim com.ImplusOS.windowmanager makes) and
 *             scans out to the panel framebuffer. A Wayland session then
 *             needs nothing else in userland.
 *   hosted -- a window manager is running and owns the panel. The whole
 *             Wayland output becomes one WM window, and input arrives
 *             through the WM's routing, already in window coordinates.
 *
 * Selected automatically, or forced with a launch argument: "panel" or
 * "hosted".
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "Compositor.h"
#include "Process.h"
#include "Graphics.h"
#include "Input.h"
#include "Memory.h"
#include "Serial.h"

extern uint64_t syscall1(uint64_t, uint64_t);
extern uint64_t syscall2(uint64_t, uint64_t, uint64_t);

#define SYS_UNIX_SOCKET   220
#define SYS_UNIX_BIND     221
#define SYS_UNIX_LISTEN   222
#define SYS_UNIX_ACCEPT   223

#define WL_SOCK_PATH   "/tmp/wayland-0"
#define FRAME_MS       16u          /* ~60 Hz ceiling on recomposites */
#define IDLE_NAP_MS    4u

/* ---- shared state ---------------------------------------------------- */
wlc_window_t g_windows[WLC_MAX_WINDOWS];
int32_t      g_zorder[WLC_MAX_WINDOWS];
uint32_t     g_zcount;
wlc_output_t g_out;
bool         g_dirty;

/* ---- logging --------------------------------------------------------- */
void wlc_log(const char *s) { serial_write_string(s); }

void wlc_logf_u32(const char *prefix, uint32_t value)
{
    serial_write_string(prefix);
    serial_write_uint32(value);
    serial_write_string("\n");
}

/* Same, without the newline, for building one line out of several fields. */
void wlc_log_u32(const char *prefix, uint32_t value)
{
    serial_write_string(prefix);
    serial_write_uint32(value);
}

uint32_t wlc_now_ms(void) { return (uint32_t)get_uptime_ms(); }

/* ------------------------------------------------------------------ */
/* scene                                                                */
/* ------------------------------------------------------------------ */

static void zorder_remove(int32_t index)
{
    uint32_t out = 0;
    for (uint32_t i = 0; i < g_zcount; i++) {
        if (g_zorder[i] != index) g_zorder[out++] = g_zorder[i];
    }
    g_zcount = out;
}

void wlc_window_raise(int32_t index)
{
    if (index < 0 || (uint32_t)index >= WLC_MAX_WINDOWS) return;
    zorder_remove(index);
    if (g_zcount < WLC_MAX_WINDOWS) g_zorder[g_zcount++] = index;
    g_dirty = true;
}

int32_t wlc_window_alloc(wlc_client_t *c, uint32_t surface, uint32_t xdg_surface)
{
    for (uint32_t i = 0; i < WLC_MAX_WINDOWS; i++) {
        if (g_windows[i].used) continue;
        wlc_window_t *w = &g_windows[i];
        uint32_t *keep = w->pixels;
        uint32_t keep_cap = w->pix_cap;
        memset(w, 0, sizeof(*w));
        w->pixels = keep;              /* reuse the slot's allocation */
        w->pix_cap = keep_cap;
        w->used = true;
        w->client = c;
        w->surface = surface;
        w->xdg_surface = xdg_surface;
        wlc_window_raise((int32_t)i);
        return (int32_t)i;
    }
    return -1;
}

void wlc_window_free(int32_t index)
{
    if (index < 0 || (uint32_t)index >= WLC_MAX_WINDOWS) return;
    if (!g_windows[index].used) return;

    zorder_remove(index);
    g_windows[index].used = false;
    g_windows[index].mapped = false;
    g_windows[index].client = NULL;
    g_windows[index].surface = 0u;
    g_windows[index].w = g_windows[index].h = 0u;

    /* The pixel buffer stays allocated for the next window in this slot. */
    wlc_seat_pointer_leave_all();
    wlc_seat_refresh_focus();
    g_dirty = true;
}

/* Toplevels are centred, then cascaded so a second window does not land
 * exactly on the first. Popups are positioned by their xdg_positioner and
 * skip this. */
void wlc_window_place(int32_t index)
{
    static uint32_t cascade;
    wlc_window_t *w = &g_windows[index];
    if (w->popup) return;

    int32_t step = (int32_t)((cascade % 6u) * 32u);
    cascade++;

    w->x = ((int32_t)g_out.w - (int32_t)w->w) / 2 + step;
    w->y = ((int32_t)g_out.h - (int32_t)w->h) / 2 + step;
    if (w->x + (int32_t)w->w > (int32_t)g_out.w)
        w->x = (int32_t)g_out.w - (int32_t)w->w;
    if (w->y + (int32_t)w->h > (int32_t)g_out.h)
        w->y = (int32_t)g_out.h - (int32_t)w->h;
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;
}

int32_t wlc_window_at(int32_t x, int32_t y, int32_t *out_local_x,
                      int32_t *out_local_y)
{
    for (uint32_t i = g_zcount; i > 0u; i--) {
        int32_t index = g_zorder[i - 1u];
        wlc_window_t *w = &g_windows[index];
        if (!w->used || !w->mapped || !w->w || !w->h) continue;
        if (x < w->x || y < w->y) continue;
        if (x >= w->x + (int32_t)w->w || y >= w->y + (int32_t)w->h) continue;
        if (out_local_x) *out_local_x = x - w->x;
        if (out_local_y) *out_local_y = y - w->y;
        return index;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* interactive grabs                                                    */
/* ------------------------------------------------------------------ */

/* A client-side-decorated window is dragged by the client asking for it.
 * Between xdg_toplevel.move and the button coming up, the pointer belongs
 * to the compositor: the window follows it and the client sees nothing,
 * which is what stops GTK from also treating the drag as a click. */
static struct {
    wlc_grab_kind_t kind;
    int32_t  window;
    uint32_t edges;
    int32_t  anchor_x, anchor_y;     /* pointer where the grab started */
    int32_t  origin_x, origin_y;     /* window position when it started */
    uint32_t origin_w, origin_h;     /* window size when it started      */
} g_grab;

extern void wlc_toplevel_request_size(int32_t index, uint32_t w, uint32_t h);

static int32_t g_ptr_x, g_ptr_y;

void wlc_grab_begin(wlc_grab_kind_t kind, int32_t index, uint32_t edges)
{
    if (index < 0 || (uint32_t)index >= WLC_MAX_WINDOWS) return;
    wlc_window_t *w = &g_windows[index];
    if (!w->used || !w->mapped || w->popup) return;

    g_grab.kind = kind;
    g_grab.window = index;
    g_grab.edges = edges;
    g_grab.anchor_x = g_ptr_x;
    g_grab.anchor_y = g_ptr_y;
    g_grab.origin_x = w->x;
    g_grab.origin_y = w->y;
    g_grab.origin_w = w->w;
    g_grab.origin_h = w->h;

    /* Whatever the pointer was over stops receiving events for the duration,
     * or the client keeps drawing hover states under a window that is
     * moving out from under the cursor. */
    wlc_seat_pointer_leave_all();
    wlc_window_raise(index);
}

void wlc_grab_end(void)
{
    g_grab.kind = WLC_GRAB_NONE;
    g_grab.window = -1;
}

bool wlc_grab_active(void) { return g_grab.kind != WLC_GRAB_NONE; }

/* The smallest a window may be dragged down to. Below this a GTK client
 * has nothing left to grab and the window becomes unrecoverable. */
#define WLC_MIN_SIZE 120u

bool wlc_grab_motion(int32_t x, int32_t y)
{
    if (g_grab.kind == WLC_GRAB_NONE) return false;
    int32_t index = g_grab.window;
    if (index < 0 || (uint32_t)index >= WLC_MAX_WINDOWS ||
        !g_windows[index].used || !g_windows[index].mapped) {
        wlc_grab_end();
        return false;
    }
    wlc_window_t *w = &g_windows[index];
    int32_t dx = x - g_grab.anchor_x;
    int32_t dy = y - g_grab.anchor_y;

    if (g_grab.kind == WLC_GRAB_MOVE) {
        w->x = g_grab.origin_x + dx;
        w->y = g_grab.origin_y + dy;
        /* Keep enough of the window on the output to grab it again. The
         * title bar is the client's, at the top of the surface, so the top
         * edge is the one that must stay reachable. */
        int32_t max_x = (int32_t)g_out.w - 40;
        int32_t max_y = (int32_t)g_out.h - 40;
        if (w->x > max_x) w->x = max_x;
        if (w->y > max_y) w->y = max_y;
        if (w->x + (int32_t)w->w < 40) w->x = 40 - (int32_t)w->w;
        if (w->y < 0) w->y = 0;
        g_dirty = true;
        return true;
    }

    /* Resize. The compositor cannot change the surface itself -- only the
     * client can produce a buffer of a different size -- so this computes
     * the size and asks, and the window changes shape when the client
     * commits. Dragging a left or top edge also moves the origin, so the
     * opposite edge stays put. */
    int32_t new_w = (int32_t)g_grab.origin_w;
    int32_t new_h = (int32_t)g_grab.origin_h;
    if (g_grab.edges & WLC_EDGE_RIGHT)  new_w = (int32_t)g_grab.origin_w + dx;
    if (g_grab.edges & WLC_EDGE_LEFT)   new_w = (int32_t)g_grab.origin_w - dx;
    if (g_grab.edges & WLC_EDGE_BOTTOM) new_h = (int32_t)g_grab.origin_h + dy;
    if (g_grab.edges & WLC_EDGE_TOP)    new_h = (int32_t)g_grab.origin_h - dy;
    if (new_w < (int32_t)WLC_MIN_SIZE) new_w = (int32_t)WLC_MIN_SIZE;
    if (new_h < (int32_t)WLC_MIN_SIZE) new_h = (int32_t)WLC_MIN_SIZE;
    if (new_w > (int32_t)g_out.w) new_w = (int32_t)g_out.w;
    if (new_h > (int32_t)g_out.h) new_h = (int32_t)g_out.h;

    if (g_grab.edges & WLC_EDGE_LEFT)
        w->x = g_grab.origin_x + ((int32_t)g_grab.origin_w - new_w);
    if (g_grab.edges & WLC_EDGE_TOP)
        w->y = g_grab.origin_y + ((int32_t)g_grab.origin_h - new_h);

    wlc_toplevel_request_size(index, (uint32_t)new_w, (uint32_t)new_h);
    g_dirty = true;
    return true;
}

int32_t wlc_window_focus_candidate(void)
{
    for (uint32_t i = g_zcount; i > 0u; i--) {
        int32_t index = g_zorder[i - 1u];
        if (g_windows[index].used && g_windows[index].mapped) return index;
    }
    return -1;
}

void wlc_damage_all(void) { g_dirty = true; }

/* ------------------------------------------------------------------ */
/* compositing                                                          */
/* ------------------------------------------------------------------ */

/* Source-over with premultiplied alpha, which is what wl_shm ARGB8888 is.
 * XRGB buffers were forced opaque on the way in, so they take the fast
 * path. */
static inline uint32_t blend(uint32_t src, uint32_t dst)
{
    uint32_t a = src >> 24;
    if (a == 0xffu) return src;
    if (a == 0u) return dst;
    uint32_t ia = 255u - a;
    uint32_t rb = ((((dst & 0x00ff00ffu) * ia) >> 8) & 0x00ff00ffu);
    uint32_t g  = ((((dst & 0x0000ff00u) * ia) >> 8) & 0x0000ff00u);
    return src + rb + g;
}

static void paint_background(uint32_t *dst)
{
    /* A quiet vertical gradient, so an empty session reads as "running"
     * rather than "the screen is broken". */
    for (uint32_t y = 0; y < g_out.h; y++) {
        uint32_t t = (g_out.h > 1u) ? (y * 255u) / (g_out.h - 1u) : 0u;
        uint32_t r = 0x14u + (0x08u * t) / 255u;
        uint32_t g = 0x18u + (0x0au * t) / 255u;
        uint32_t b = 0x1eu + (0x10u * t) / 255u;
        uint32_t c = 0xff000000u | (r << 16) | (g << 8) | b;
        uint32_t *row = dst + (size_t)y * g_out.w;
        for (uint32_t x = 0; x < g_out.w; x++) row[x] = c;
    }
}

static void paint_window(uint32_t *dst, const wlc_window_t *w)
{
    if (!w->pixels || !w->w || !w->h) return;

    int32_t x0 = w->x < 0 ? 0 : w->x;
    int32_t y0 = w->y < 0 ? 0 : w->y;
    int32_t x1 = w->x + (int32_t)w->w;
    int32_t y1 = w->y + (int32_t)w->h;
    if (x1 > (int32_t)g_out.w) x1 = (int32_t)g_out.w;
    if (y1 > (int32_t)g_out.h) y1 = (int32_t)g_out.h;
    if (x1 <= x0 || y1 <= y0) return;

    for (int32_t y = y0; y < y1; y++) {
        const uint32_t *s = w->pixels + (size_t)(y - w->y) * w->w + (size_t)(x0 - w->x);
        uint32_t *d = dst + (size_t)y * g_out.w + (size_t)x0;
        for (int32_t x = x0; x < x1; x++) {
            *d = blend(*s, *d);
            ++s; ++d;
        }
    }
}

/* The pointer sprite. There is no WM to draw one in panel mode, and a
 * Wayland client's own cursor surface is not composited, so this is the
 * only cursor the session has. */
static const char *const k_cursor[] = {
    "X          ",
    "XX         ",
    "XoX        ",
    "XooX       ",
    "XoooX      ",
    "XooooX     ",
    "XoooooX    ",
    "XooooooX   ",
    "XoooooooX  ",
    "XooooooooX ",
    "XoooooXXXXX",
    "XooXooX    ",
    "XoX XooX   ",
    "XX  XooX   ",
    "X    XooX  ",
    "     XooX  ",
    "      XXX  ",
};
#define CURSOR_H ((int32_t)(sizeof(k_cursor) / sizeof(k_cursor[0])))

static void paint_cursor(uint32_t *dst)
{
    for (int32_t row = 0; row < CURSOR_H; row++) {
        int32_t y = g_ptr_y + row;
        if (y < 0 || y >= (int32_t)g_out.h) continue;
        const char *line = k_cursor[row];
        for (int32_t col = 0; line[col]; col++) {
            int32_t x = g_ptr_x + col;
            if (x < 0 || x >= (int32_t)g_out.w) continue;
            if (line[col] == 'X')      dst[(size_t)y * g_out.w + (size_t)x] = 0xff000000u;
            else if (line[col] == 'o') dst[(size_t)y * g_out.w + (size_t)x] = 0xffffffffu;
        }
    }
}

static void compose(void)
{
    uint32_t *dst = g_out.canvas;
    if (!dst) return;

    /*
     * Hosted, the session surface holds the Wayland windows and nothing
     * else: everywhere a client is not, it is transparent and the desktop
     * behind shows through. Painting a background there instead put a grey
     * slab over the whole work area, which is what a Wayland window looked
     * like it was sitting on. On the panel there is nothing behind it, so
     * the background is what keeps an empty session from reading as a
     * broken screen.
     */
    if (g_out.hosted)
        memset(dst, 0, (size_t)g_out.w * g_out.h * sizeof(uint32_t));
    else
        paint_background(dst);
    for (uint32_t i = 0; i < g_zcount; i++) {
        const wlc_window_t *w = &g_windows[g_zorder[i]];
        if (w->used && w->mapped) paint_window(dst, w);
    }
    if (!g_out.hosted) paint_cursor(dst);
}

static void output_flush(void)
{
    if (g_out.hosted) {
        window_damage(g_out.win, 0u, 0u, g_out.w, g_out.h);
        window_end_transaction(g_out.win);
        return;
    }

    if (g_out.fb) {
        uint32_t stride = g_out.fb_stride < g_out.w ? g_out.w : g_out.fb_stride;
        for (uint32_t y = 0; y < g_out.h; y++) {
            memcpy(&g_out.fb[(size_t)y * stride],
                   &g_out.canvas[(size_t)y * g_out.w],
                   (size_t)g_out.w * sizeof(uint32_t));
        }
    } else {
        /* No mapped scanout: fall back to the driver's own fill, coalescing
         * runs of one colour. Slow, but it is the difference between a
         * visible session and a black screen (the window manager keeps the
         * same fallback). */
        for (uint32_t y = 0; y < g_out.h; y++) {
            const uint32_t *row = &g_out.canvas[(size_t)y * g_out.w];
            uint32_t x = 0u;
            while (x < g_out.w) {
                uint32_t run = 1u;
                while (x + run < g_out.w && row[x + run] == row[x]) run++;
                draw_fill_rect(x, y, run, 1u, row[x]);
                x += run;
            }
        }
    }
    display_rect_t r = { 0, 0, g_out.w, g_out.h };
    draw_present_rects(&r, 1u);
}

/* ------------------------------------------------------------------ */
/* output setup                                                         */
/* ------------------------------------------------------------------ */

static uint32_t panel_stride(uint32_t width)
{
    display_mode_info_t mode;
    memset(&mode, 0, sizeof(mode));
    if (display_get_monitor_mode_info(0u, 0u, &mode) >= 0 && mode.stride >= width) {
        return mode.stride;
    }
    return width;
}

static bool output_open_panel(void)
{
    uint32_t w = get_display_width();
    uint32_t h = get_display_height();
    if (w == 0u || h == 0u) { w = 1024u; h = 768u; }

    /* Claiming the raw HID stream is also what registers this process as
     * the kernel's input owner, so nothing else can take input away
     * mid-session. Without it input_read_* is denied. */
    if (window_register_service() != 0) {
        wlc_log("[wl] could not claim input ownership\n");
        return false;
    }

    g_out.canvas = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
    if (!g_out.canvas) { wlc_log("[wl] out of memory for the output\n"); return false; }
    g_out.w = w;
    g_out.h = h;
    g_out.hosted = false;
    g_out.win = 0u;
    g_out.fb = (uint32_t *)sys_get_display_framebuffer();
    g_out.fb_stride = panel_stride(w);
    g_ptr_x = (int32_t)(w / 2u);
    g_ptr_y = (int32_t)(h / 2u);
    wlc_logf_u32("[wl] panel output, width=", w);
    return true;
}

/* Room left at the bottom for the window manager's taskbar, which the
 * session sits above rather than under. */
#define HOSTED_TASKBAR_H 48u

static bool output_open_hosted(void)
{
    uint32_t sw = get_display_width();
    uint32_t sh = get_display_height();
    if (sw == 0u || sh == 0u) { sw = 1024u; sh = 768u; }

    uint32_t w = sw;
    uint32_t h = sh > HOSTED_TASKBAR_H ? sh - HOSTED_TASKBAR_H : sh;

    /*
     * The session's surface, not an application window. It carries no
     * title, no decoration and no taskbar entry: what belongs on the
     * taskbar is the Wayland clients, and a second frame around the whole
     * output only made the windows inside it harder to reach. Marking it a
     * system window is what tells the compositor all of that -- see
     * com.ImplusOS.windowmanager/Decoration/WM_Decoration.c, which returns
     * early for one, and WM_Taskbar.c, which skips it.
     */
    window_id_t win = window_create_ex(0u, 0u, w, h, 0u, "Wayland");
    if (win == 0u) { wlc_log("[wl] window_create failed\n"); return false; }
    window_set_system(win, true);

    uint32_t bw = 0, bh = 0;
    uint32_t *pixels = window_get_backing_store(win, &bw, &bh);
    if (!pixels || bw == 0u || bh == 0u) {
        wlc_log("[wl] no backing store for the Wayland window\n");
        window_destroy(win);
        return false;
    }

    g_out.canvas = pixels;
    g_out.w = bw;
    g_out.h = bh;
    g_out.hosted = true;
    g_out.win = win;
    g_out.fb = NULL;
    g_out.fb_stride = 0u;

    /* Not opaque: the parts of this surface no client is drawing on have to
     * let the desktop through. */
    (void)window_set_surface_opaque(win, false);
    (void)window_subscribe_keyboard(win);
    (void)window_subscribe_mouse(win);
    window_show(win);
    window_raise(win);
    wlc_logf_u32("[wl] hosted as the session surface, width=", bw);
    return true;
}

/* ------------------------------------------------------------------ */
/* input                                                                */
/* ------------------------------------------------------------------ */

/* Set-1 scancodes and evdev keycodes agree over 0x01..0x58, which covers
 * everything but the 0xE0-prefixed keys. */
static uint16_t evdev_from_scancode(uint16_t sc)
{
    switch (sc) {
    case 0xE01Cu: return 96;   /* KP_ENTER   */
    case 0xE01Du: return 97;   /* RIGHTCTRL  */
    case 0xE035u: return 98;   /* KP_SLASH   */
    case 0xE037u: return 99;   /* SYSRQ      */
    case 0xE038u: return 100;  /* RIGHTALT   */
    case 0xE047u: return 102;  /* HOME       */
    case 0xE048u: return 103;  /* UP         */
    case 0xE049u: return 104;  /* PAGEUP     */
    case 0xE04Bu: return 105;  /* LEFT       */
    case 0xE04Du: return 106;  /* RIGHT      */
    case 0xE04Fu: return 107;  /* END        */
    case 0xE050u: return 108;  /* DOWN       */
    case 0xE051u: return 109;  /* PAGEDOWN   */
    case 0xE052u: return 110;  /* INSERT     */
    case 0xE053u: return 111;  /* DELETE     */
    case 0xE05Bu: return 125;  /* LEFTMETA   */
    case 0xE05Cu: return 126;  /* RIGHTMETA  */
    case 0xE05Du: return 127;  /* COMPOSE    */
    default: break;
    }
    if (sc >= 0x01u && sc <= 0x58u) return sc;
    return 0u;
}

/* Linux button codes, which is what wl_pointer.button carries. */
#define BTN_LEFT   0x110u
#define BTN_RIGHT  0x111u
#define BTN_MIDDLE 0x112u

static uint8_t g_buttons;          /* last delivered button state */

static void deliver_buttons(uint8_t buttons)
{
    static const struct { uint8_t bit; uint32_t code; } map[] = {
        { INPUT_MOUSE_BTN_LEFT,   BTN_LEFT   },
        { INPUT_MOUSE_BTN_RIGHT,  BTN_RIGHT  },
        { INPUT_MOUSE_BTN_MIDDLE, BTN_MIDDLE },
    };
    for (uint32_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        bool was = (g_buttons & map[i].bit) != 0u;
        bool now = (buttons & map[i].bit) != 0u;
        if (was != now) wlc_seat_pointer_button(map[i].code, now);
    }
    /* Bits 3 and 4 are the wheel on the USB HID path, reported as buttons
     * that pulse rather than as a wheel delta (see the window manager's
     * WM_Input.c, which reads them the same way). */
    if ((buttons & 0x08u) && !(g_buttons & 0x08u)) wlc_seat_pointer_axis(-1);
    if ((buttons & 0x10u) && !(g_buttons & 0x10u)) wlc_seat_pointer_axis(1);
    g_buttons = buttons;
}

static bool input_poll_panel(void)
{
    bool any = false;

    input_mouse_event_t raw;
    int32_t dx = 0, dy = 0, wheel = 0;
    uint8_t buttons = g_buttons;
    uint32_t moves = 0;
    bool have_abs = false;
    int32_t abs_x = 0, abs_y = 0;
    while (input_read_mouse(&raw) > 0) {
        if ((raw.flags & INPUT_MOUSE_FLAG_ABSOLUTE) != 0u) {
            abs_x = (int32_t)(((uint64_t)raw.x *
                               (g_out.w > 0u ? g_out.w - 1u : 0u)) /
                              INPUT_MOUSE_ABS_MAX);
            abs_y = (int32_t)(((uint64_t)raw.y *
                               (g_out.h > 0u ? g_out.h - 1u : 0u)) /
                              INPUT_MOUSE_ABS_MAX);
            have_abs = true;
            dx = 0;
            dy = 0;
        } else {
            dx += (int16_t)raw.x;
            dy += (int16_t)raw.y;
        }
        wheel += raw.wheel;
        buttons = raw.buttons;
        ++moves;
    }
    if (moves > 0u) {
        any = true;
        int32_t nx = (have_abs ? abs_x : g_ptr_x) + dx;
        int32_t ny = (have_abs ? abs_y : g_ptr_y) + dy;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx >= (int32_t)g_out.w) nx = (int32_t)g_out.w - 1;
        if (ny >= (int32_t)g_out.h) ny = (int32_t)g_out.h - 1;
        if (nx != g_ptr_x || ny != g_ptr_y) {
            g_ptr_x = nx;
            g_ptr_y = ny;
            g_dirty = true;          /* the pointer sprite moved */
        }
        wlc_seat_pointer_motion(g_ptr_x, g_ptr_y);
        deliver_buttons(buttons);
        if (wheel != 0) wlc_seat_pointer_axis(wheel > 0 ? -1 : 1);
    }

    input_keyboard_event_t key;
    while (input_read_keyboard(&key) > 0) {
        any = true;
        uint16_t code = evdev_from_scancode(key.keycode);
        if (code != 0u) wlc_seat_key(code, key.pressed != 0u, key.modifiers);
    }
    return any;
}

static bool input_poll_hosted(void)
{
    bool any = false;

    /* The WM already tracks the pointer and hands over window-local
     * coordinates, which are output coordinates here. */
    input_mouse_event_t m;
    while (window_input_mouse_poll(&m) > 0) {
        any = true;
        g_ptr_x = (int32_t)m.x;
        g_ptr_y = (int32_t)m.y;
        wlc_seat_pointer_motion(g_ptr_x, g_ptr_y);
        deliver_buttons(m.buttons);
        if (m.wheel != 0) wlc_seat_pointer_axis(m.wheel > 0 ? -1 : 1);
    }

    input_keyboard_event_t key;
    while (window_input_keyboard_poll(&key) > 0) {
        any = true;
        uint16_t code = evdev_from_scancode(key.keycode);
        if (code != 0u) wlc_seat_key(code, key.pressed != 0u, key.modifiers);
    }
    return any;
}

static bool input_poll(void)
{
    return g_out.hosted ? input_poll_hosted() : input_poll_panel();
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

static int32_t u_socket(void) { return (int32_t)syscall1(SYS_UNIX_SOCKET, 1u); }
static int32_t u_bind(int32_t fd, const char *p)
{
    return (int32_t)syscall2(SYS_UNIX_BIND, (uint64_t)fd, (uint64_t)(uintptr_t)p);
}
static int32_t u_listen(int32_t fd)
{
    return (int32_t)syscall2(SYS_UNIX_LISTEN, (uint64_t)fd, 8u);
}
static int32_t u_accept(int32_t fd)
{
    return (int32_t)syscall1(SYS_UNIX_ACCEPT, (uint64_t)fd);
}

/* Move every mapped toplevel back inside the output, for when the output
 * changes size under them (the WM disappearing, say). */
static void reclamp_windows(void)
{
    for (uint32_t i = 0; i < WLC_MAX_WINDOWS; i++) {
        wlc_window_t *w = &g_windows[i];
        if (!w->used || !w->mapped) continue;
        if (w->x + (int32_t)w->w > (int32_t)g_out.w)
            w->x = (int32_t)g_out.w - (int32_t)w->w;
        if (w->y + (int32_t)w->h > (int32_t)g_out.h)
            w->y = (int32_t)g_out.h - (int32_t)w->h;
        if (w->x < 0) w->x = 0;
        if (w->y < 0) w->y = 0;
    }
}

/* Follow a resize of the hosting window.
 *
 * The backing store is sized to the window's content area, so the window
 * manager replaces it on every resize and the pointer taken at open time
 * stops being what it composites. Re-acquire it, or everything drawn from
 * here on lands in a surface nobody reads and the session freezes on its
 * last pre-resize frame. */
static void output_follow_resize(void)
{
    if (!g_out.hosted || g_out.win == 0u) return;

    uint32_t w = 0u, h = 0u;
    if (window_poll_resize(g_out.win, &w, &h) <= 0) return;

    uint32_t bw = 0u, bh = 0u;
    uint32_t *pixels = window_get_backing_store(g_out.win, &bw, &bh);
    if (!pixels || bw == 0u || bh == 0u) {
        wlc_log("[wl] lost the backing store across a resize\n");
        return;
    }
    if (pixels == g_out.canvas && bw == g_out.w && bh == g_out.h) return;

    g_out.canvas = pixels;
    g_out.w = bw;
    g_out.h = bh;
    (void)window_set_surface_opaque(g_out.win, false);
    reclamp_windows();
    g_dirty = true;
    wlc_logf_u32("[wl] output resized, width=", bw);
}

/* "panel" / "hosted" force a backend; anything else picks whichever fits
 * what is running. */
#define MODE_AUTO   0
#define MODE_PANEL  1
#define MODE_HOSTED 2

static int read_mode(void)
{
    char arg[64];
    memset(arg, 0, sizeof(arg));
    if (process_get_launch_argument(arg, (uint32_t)sizeof(arg)) < 0) return MODE_AUTO;
    if (strstr(arg, "panel"))  return MODE_PANEL;
    if (strstr(arg, "hosted")) return MODE_HOSTED;
    return MODE_AUTO;
}

void _start(void)
{
    wlc_log("[wl] compositor starting\n");

    int mode = read_mode();
    bool opened = false;

    if (mode == MODE_HOSTED) {
        /* Asked for the WM explicitly: give it a moment to come up. */
        for (uint32_t waited = 0; waited < 5000u && window_get_wm_pid() < 0;
             waited += 100u) {
            sleep_ms(100);
        }
    }

    bool wm_running = (mode != MODE_PANEL) && (window_get_wm_pid() >= 0);
    if (wm_running) opened = output_open_hosted();
    if (!opened && mode != MODE_HOSTED) opened = output_open_panel();
    if (!opened) {
        wlc_log("[wl] no usable output, giving up\n");
        process_exit(1);
    }

    wlc_client_init_slots();
    memset(g_windows, 0, sizeof(g_windows));
    g_zcount = 0;

    int32_t ls = u_socket();
    if (ls < 0) { wlc_log("[wl] u_socket FAILED\n"); process_exit(1); }
    if (u_bind(ls, WL_SOCK_PATH) < 0) { wlc_log("[wl] u_bind FAILED\n"); process_exit(1); }
    if (u_listen(ls) < 0) { wlc_log("[wl] u_listen FAILED\n"); process_exit(1); }
    wlc_log("[wl] listening on " WL_SOCK_PATH "\n");

    /* Paint once up front so the session is visible before any client
     * connects -- in panel mode this is the whole screen. */
    g_dirty = true;
    uint32_t last_frame = 0u;

    for (;;) {
        bool busy = false;

        int32_t c = u_accept(ls);
        while (c >= 0) {
            if (wlc_client_accept(c)) busy = true;
            c = u_accept(ls);
        }

        for (uint32_t i = 0; i < WLC_MAX_CLIENTS; i++) {
            if (g_clients[i].fd < 0) continue;
            if (wlc_client_pump(&g_clients[i])) busy = true;
        }

        if (input_poll()) busy = true;

        output_follow_resize();

        /* Outlive the window manager. The scene is held in this process --
         * every surface's pixels were copied out at commit -- so when the
         * WM exits there is nothing to rebuild: take the panel and carry on
         * with the same clients still connected. */
        if (g_out.hosted && window_get_wm_pid() < 0) {
            wlc_log("[wl] window manager gone, taking over the panel\n");
            g_out.hosted = false;
            if (output_open_panel()) {
                reclamp_windows();
                g_dirty = true;
                busy = true;
            } else {
                wlc_log("[wl] panel takeover failed, exiting\n");
                process_exit(1);
            }
        }

        uint32_t now = wlc_now_ms();
        if (now - last_frame >= FRAME_MS) {
            if (g_dirty) {
                compose();
                output_flush();
                g_dirty = false;
                busy = true;
            }
            if (wlc_frames_done()) busy = true;
            last_frame = now;
        }

        if (!busy) sleep_ms(IDLE_NAP_MS);
    }
}
