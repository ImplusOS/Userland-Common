#include "WM_Icons.h"
#include "../Compositor/WM_Raster.h"
#include "../Core/WM_Assets.h"

#include <stddef.h>

/*
 * Icons are external Material Design assets, pre-rendered to anti-aliased
 * white RGBA PNGs by Tools/fetch_icons.sh into Resource/Icons/md/. They are
 * loaded once and tinted at blit time (the source is white, so the tint is
 * just the requested RGB with alpha = source_coverage * tint_alpha).
 */

#define ICON_DIR "/Userland/com.ImplusOS.windowmanager/Resource/Icons/md/"

static const char *const k_icon_file[] = {
    [WM_ICON_MENU]          = "menu",
    [WM_ICON_APPS]          = "apps",
    [WM_ICON_SEARCH]        = "search",
    [WM_ICON_WIFI]          = "wifi",
    [WM_ICON_WIFI_OFF]      = "wifi_off",
    [WM_ICON_VOLUME]        = "volume_up",
    [WM_ICON_VOLUME_MUTE]   = "volume_off",
    [WM_ICON_IME]           = "keyboard",
    [WM_ICON_NOTIFICATIONS] = "notifications",
    [WM_ICON_SETTINGS]      = "settings",
    [WM_ICON_POWER]         = "power_settings_new",
    [WM_ICON_RESTART]       = "restart_alt",
    [WM_ICON_FOLDER]        = "folder",
    [WM_ICON_DOC]           = "description",
    [WM_ICON_PERSON]        = "person",
    [WM_ICON_CLOSE]         = "close",
    [WM_ICON_MINIMIZE]      = "remove",
    [WM_ICON_MAXIMIZE]      = "crop_square",
    [WM_ICON_RESTORE]       = "filter_none",
    [WM_ICON_CHEVRON_UP]    = "expand_less",
    [WM_ICON_TERMINAL]      = "terminal",
    [WM_ICON_EDIT]          = "edit",
};
#define ICON_COUNT ((int)(sizeof(k_icon_file) / sizeof(k_icon_file[0])))

typedef struct { uint32_t *px; uint32_t w, h; int tried; } icon_cache_t;
static icon_cache_t g_cache[ICON_COUNT];
static icon_cache_t g_cursor;

static const icon_cache_t *icon_get(wm_icon_kind_t kind)
{
    if ((int)kind < 0 || (int)kind >= ICON_COUNT || !k_icon_file[kind]) return NULL;
    icon_cache_t *c = &g_cache[kind];
    if (!c->tried) {
        c->tried = 1;
        char path[160];
        const char *dir = ICON_DIR;
        char *p = path;
        while (*dir) *p++ = *dir++;
        const char *n = k_icon_file[kind];
        while (*n) *p++ = *n++;
        *p++ = '.'; *p++ = 'p'; *p++ = 'n'; *p++ = 'g'; *p = '\0';
        c->px = wm_assets_load_png(path, &c->w, &c->h);
    }
    return c->px ? c : NULL;
}

/* bilinear sample of the source alpha channel; u_fp,v_fp are .16 in [0,65536) */
static uint32_t sample_a(const icon_cache_t *ic, uint32_t u_fp, uint32_t v_fp)
{
    uint32_t fx = (uint32_t)(((uint64_t)u_fp * (ic->w - 1u)) >> 0);   /* pixel .16 */
    uint32_t fy = (uint32_t)(((uint64_t)v_fp * (ic->h - 1u)) >> 0);
    uint32_t x0 = fx >> 16, y0 = fy >> 16;
    uint32_t x1 = x0 + 1u < ic->w ? x0 + 1u : x0;
    uint32_t y1 = y0 + 1u < ic->h ? y0 + 1u : y0;
    uint32_t tx = (fx >> 8) & 0xFFu, ty = (fy >> 8) & 0xFFu;
    uint32_t a00 = ic->px[y0 * ic->w + x0] >> 24;
    uint32_t a10 = ic->px[y0 * ic->w + x1] >> 24;
    uint32_t a01 = ic->px[y1 * ic->w + x0] >> 24;
    uint32_t a11 = ic->px[y1 * ic->w + x1] >> 24;
    uint32_t top = a00 * (256u - tx) + a10 * tx;
    uint32_t bot = a01 * (256u - tx) + a11 * tx;
    return (top * (256u - ty) + bot * ty) >> 16;
}

static void blit_tinted(wm_canvas_t *canvas, wm_rect_t box,
                        const icon_cache_t *ic, uint32_t argb)
{
    if (!ic || box.w == 0u || box.h == 0u) return;
    wm_rect_t vis = wm_rect_intersection(box, canvas->clip);
    if (vis.w == 0u || vis.h == 0u) return;
    uint32_t tint_rgb = argb & 0x00FFFFFFu;
    uint32_t tint_a = (argb >> 24) ? (argb >> 24) : 255u;
    uint32_t dnx = box.w > 1u ? box.w - 1u : 1u;
    uint32_t dny = box.h > 1u ? box.h - 1u : 1u;
    for (uint32_t ry = 0; ry < vis.h; ++ry) {
        int32_t py = vis.y + (int32_t)ry;
        uint32_t ly = (uint32_t)(py - box.y);
        uint32_t v_fp = (ly << 16) / dny;
        for (uint32_t rx = 0; rx < vis.w; ++rx) {
            int32_t px = vis.x + (int32_t)rx;
            uint32_t lx = (uint32_t)(px - box.x);
            uint32_t u_fp = (lx << 16) / dnx;
            uint32_t sa = sample_a(ic, u_fp, v_fp);
            if (sa == 0u) continue;
            uint32_t a = (sa * tint_a) / 255u;
            if (a == 0u) continue;
            wm_canvas_put(canvas, px, py, (a << 24) | tint_rgb);
        }
    }
}

void wm_icon_draw(wm_canvas_t *canvas, wm_rect_t box,
                  wm_icon_kind_t kind, uint32_t argb)
{
    if (!canvas) return;
    const icon_cache_t *ic = icon_get(kind);
    if (!ic) return;
    blit_tinted(canvas, box, ic, argb);
}

void wm_icon_draw_cursor(wm_canvas_t *canvas, int32_t x, int32_t y,
                         wm_cursor_style_t style, uint32_t fill, uint32_t edge)
{
    if (!canvas) return;

    if (style == WM_CURSOR_DEFAULT) {
        if (!g_cursor.tried) {
            g_cursor.tried = 1;
            g_cursor.px = wm_assets_load_png(ICON_DIR "cursor.png",
                                             &g_cursor.w, &g_cursor.h);
        }
        if (g_cursor.px) {
            uint32_t h = 22u;
            uint32_t w = g_cursor.h ? (g_cursor.w * h / g_cursor.h) : h;
            /* dark halo, then white body */
            static const int8_t off[8][2] =
                { {-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},{0,1},{1,1} };
            for (int i = 0; i < 8; ++i)
                blit_tinted(canvas,
                    (wm_rect_t){x + off[i][0], y + off[i][1], w, h}, &g_cursor, edge);
            blit_tinted(canvas, (wm_rect_t){x, y, w, h}, &g_cursor, fill);
            return;
        }
        /* asset missing -> simple triangle fallback */
        for (int32_t r = 0; r < 18; ++r)
            for (int32_t c = 0; c <= r && c < 12; ++c)
                wm_canvas_put(canvas, x + c, y + r, (c == r) ? edge : fill);
        return;
    }

    int32_t cx = x + 7, cy = y + 7;
    bool hz = style == WM_CURSOR_RESIZE_HORIZONTAL;
    bool vt = style == WM_CURSOR_RESIZE_VERTICAL;
    bool nwse = style == WM_CURSOR_RESIZE_DIAGONAL_NW_SE;
    for (int32_t o = -1; o <= 1; ++o) {
        uint32_t lc = o == 0 ? fill : edge;
        if (hz)        wm_canvas_line(canvas, cx - 7, cy + o, cx + 7, cy + o, lc);
        else if (vt)   wm_canvas_line(canvas, cx + o, cy - 7, cx + o, cy + 7, lc);
        else if (nwse) wm_canvas_line(canvas, cx - 6 + o, cy - 6, cx + 6 + o, cy + 6, lc);
        else           wm_canvas_line(canvas, cx + 6 + o, cy - 6, cx - 6 + o, cy + 6, lc);
    }
}
