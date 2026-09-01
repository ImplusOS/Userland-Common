#pragma once

#include "../Core/WM_State.h"

/*
 * Procedural Material Design 3 style iconography for the shell (taskbar
 * tray, Start button, Start menu rail, window controls, desktop). Drawn
 * from primitives on a notional 24x24 grid scaled into the destination
 * box, so they stay crisp at any size and need no bitmap assets.
 */
typedef enum {
    WM_ICON_MENU = 0,        /* hamburger */
    WM_ICON_APPS,            /* 3x3 dots -- Start button */
    WM_ICON_SEARCH,
    WM_ICON_WIFI,            /* connected */
    WM_ICON_WIFI_OFF,
    WM_ICON_VOLUME,
    WM_ICON_VOLUME_MUTE,
    WM_ICON_IME,             /* keyboard / input-method */
    WM_ICON_NOTIFICATIONS,   /* bell */
    WM_ICON_SETTINGS,        /* gear */
    WM_ICON_POWER,
    WM_ICON_RESTART,
    WM_ICON_FOLDER,
    WM_ICON_DOC,
    WM_ICON_PERSON,
    WM_ICON_CLOSE,
    WM_ICON_MINIMIZE,
    WM_ICON_MAXIMIZE,
    WM_ICON_RESTORE,
    WM_ICON_CHEVRON_UP,
    WM_ICON_TERMINAL,
    WM_ICON_EDIT,
} wm_icon_kind_t;

/* Draw `kind` centred in `box`, tinted `argb` (alpha respected). */
void wm_icon_draw(wm_canvas_t *canvas, wm_rect_t box,
                  wm_icon_kind_t kind, uint32_t argb);

/* Material-style pointer / resize cursors, top-left anchored at (x,y). */
void wm_icon_draw_cursor(wm_canvas_t *canvas, int32_t x, int32_t y,
                         wm_cursor_style_t style, uint32_t fill, uint32_t edge);
