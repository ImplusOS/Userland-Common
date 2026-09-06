#pragma once

/*
 * com.ImplusOS.waylandcompositor -- a self-contained Wayland display server.
 *
 * It owns everything a GTK3 client needs: the wire protocol, a surface
 * scene with its own stacking and focus, a seat with a real pointer and
 * keyboard, and an output. Nothing here depends on
 * com.ImplusOS.windowmanager being alive -- the WM is one of two possible
 * output/input backends, not a prerequisite:
 *
 *   panel  -- no WM: claim the raw HID stream and scan out to the panel
 *             framebuffer directly. This is the standalone session.
 *   hosted -- a WM is running: composite into one WM window's backing store
 *             and take input from the WM's routing, so a Wayland session and
 *             the native desktop can share the screen.
 *
 * The Wayland side is identical in both; only Output/Input differ.
 * See Docs/Others/TODO_GTK3_Wayland_LinuxABI.md (G3).
 */

#include <stdint.h>
#include <stdbool.h>

#include "Window.h"

#define WLC_MAX_CLIENTS   4u
#define WLC_MAX_ID        0x4000u    /* client-allocated ids live below this */
#define WLC_MAX_WINDOWS   32u
#define WLC_RX_CAP        (128u * 1024u)
#define WLC_TX_CAP        (16u * 1024u)
#define WLC_MAX_RX_FDS    16u

/* ---- object types ---------------------------------------------------- */
enum {
    O_NONE = 0, O_DISPLAY, O_REGISTRY, O_CALLBACK, O_COMPOSITOR, O_SHM,
    O_SHM_POOL, O_BUFFER, O_SURFACE, O_REGION, O_XDG_WM_BASE, O_XDG_SURFACE,
    O_XDG_TOPLEVEL, O_XDG_POPUP, O_POSITIONER, O_SEAT, O_POINTER, O_KEYBOARD,
    O_OUTPUT, O_SUBCOMPOSITOR, O_SUBSURFACE, O_DDM, O_DATA_DEVICE,
    O_DATA_SOURCE
};

typedef struct {
    uint8_t  type;

    /* wl_shm_pool / wl_buffer: an index into the pool-mapping table, not an
     * object id. GTK destroys a pool as soon as it has carved a buffer out
     * of it, so the mapping has to outlive the object that made it. */
    int32_t  pool_index;

    /* wl_buffer */
    int32_t  b_pool;                 /* pool-mapping index */
    uint32_t b_off, b_w, b_h, b_stride, b_fmt;

    /* wl_surface */
    uint32_t s_pending_buf, s_current_buf, s_frame_cb, s_xdg;
    int32_t  s_window;            /* index into the scene, or -1 */

    /* xdg_surface */
    uint32_t x_surface, x_role;   /* role = toplevel or popup object id */

    /* xdg_toplevel / xdg_popup: the xdg_surface that owns it */
    uint32_t r_xdg;

    /* xdg_positioner */
    int32_t  p_ax, p_ay, p_ox, p_oy;
    uint32_t p_aw, p_ah, p_w, p_h;
    uint32_t p_anchor, p_gravity;
} wlc_obj_t;

/* ---- a connected client --------------------------------------------- */
typedef struct wlc_client {
    int32_t    fd;                /* -1 when the slot is free */
    wlc_obj_t *obj;               /* WLC_MAX_ID entries */
    uint8_t   *rx;
    uint32_t   rxlen;
    int32_t    rx_fds[WLC_MAX_RX_FDS];
    uint32_t   fd_head, fd_tail;

    uint32_t   seat_version;
    uint32_t   pointer;           /* bound wl_pointer id, 0 if none */
    uint32_t   keyboard;          /* bound wl_keyboard id, 0 if none */
    uint32_t   output;            /* bound wl_output id, 0 if none */
    bool       keymap_sent;
    bool       broken;            /* stream desynced; drop at the next pump */
} wlc_client_t;

/* ---- a mapped surface with a shell role ------------------------------ */
typedef struct {
    bool          used;
    bool          mapped;
    bool          popup;
    wlc_client_t *client;
    uint32_t      surface;        /* wl_surface object id */
    uint32_t      xdg_surface;
    uint32_t      role;           /* xdg_toplevel / xdg_popup object id */
    uint32_t      parent_surface; /* popups only */
    int32_t       x, y;           /* top-left in output coordinates */
    uint32_t      w, h;           /* from the committed buffer */

    /* Committed pixels, copied out of the client's wl_shm buffer at commit
     * so the buffer can be released straight away and the scene can be
     * recomposited at any time -- including after the client has gone. */
    uint32_t     *pixels;
    uint32_t      pix_cap;        /* pixels allocated, in uint32_t units */
} wlc_window_t;

/* ---- the output ------------------------------------------------------ */
typedef struct {
    uint32_t   *canvas;           /* what everything is composited into */
    uint32_t    w, h;
    bool        hosted;           /* true: canvas is a WM backing store */
    window_id_t win;              /* hosted only */
    uint32_t   *fb;               /* panel only: mapped scanout */
    uint32_t    fb_stride;        /* panel only, in pixels */
} wlc_output_t;

/* ---- shared state ---------------------------------------------------- */
extern wlc_client_t g_clients[WLC_MAX_CLIENTS];
extern wlc_window_t g_windows[WLC_MAX_WINDOWS];
extern int32_t      g_zorder[WLC_MAX_WINDOWS];   /* bottom .. top */
extern uint32_t     g_zcount;
extern wlc_output_t g_out;
extern bool         g_dirty;

/* ---- Compositor.c ---------------------------------------------------- */
void wlc_log(const char *s);
void wlc_logf_u32(const char *prefix, uint32_t value);
void wlc_log_u32(const char *prefix, uint32_t value);
uint32_t wlc_now_ms(void);

int32_t wlc_window_alloc(wlc_client_t *c, uint32_t surface, uint32_t xdg_surface);
void    wlc_window_free(int32_t index);
void    wlc_window_raise(int32_t index);
void    wlc_window_place(int32_t index);
int32_t wlc_window_at(int32_t x, int32_t y, int32_t *out_local_x, int32_t *out_local_y);
int32_t wlc_window_focus_candidate(void);
void    wlc_damage_all(void);

/* ---- Wayland.c ------------------------------------------------------- */
void wlc_client_init_slots(void);
bool wlc_client_accept(int32_t fd);
void wlc_client_drop(wlc_client_t *c);
bool wlc_client_pump(wlc_client_t *c);
bool wlc_frames_done(void);

/* seat event senders, called by the input backends */
void wlc_seat_pointer_motion(int32_t x, int32_t y);
void wlc_seat_pointer_button(uint32_t button, bool pressed);
void wlc_seat_pointer_axis(int32_t steps);
void wlc_seat_pointer_leave_all(void);
void wlc_seat_key(uint16_t evdev_code, bool pressed, uint32_t modifiers);
void wlc_seat_refresh_focus(void);
