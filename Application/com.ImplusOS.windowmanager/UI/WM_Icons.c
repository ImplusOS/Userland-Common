#include "WM_Icons.h"
#include "../Compositor/WM_Raster.h"

/* ---- fixed-point drawing on a 0..1000 grid mapped into `box` ---- */

static inline int32_t mapx(wm_rect_t b, int32_t n) { return b.x + (int32_t)((int64_t)n * (int32_t)b.w / 1000); }
static inline int32_t mapy(wm_rect_t b, int32_t n) { return b.y + (int32_t)((int64_t)n * (int32_t)b.h / 1000); }
static inline int32_t maps(wm_rect_t b, int32_t n) {
    int32_t d = (int32_t)(b.w < b.h ? b.w : b.h);
    int32_t v = (int32_t)((int64_t)n * d / 1000);
    return v < 1 ? 1 : v;
}

static void nrect(wm_canvas_t *c, wm_rect_t b, int32_t x, int32_t y,
                  int32_t w, int32_t h, int32_t rad, uint32_t col)
{
    int32_t px = mapx(b, x), py = mapy(b, y);
    int32_t pw = mapx(b, x + w) - px, ph = mapy(b, y + h) - py;
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;
    wm_rect_t r = {px, py, (uint32_t)pw, (uint32_t)ph};
    if (rad > 0) wm_canvas_fill_rounded(c, r, (uint32_t)maps(b, rad), col);
    else wm_canvas_fill(c, r, col);
}

static void ndisc(wm_canvas_t *c, wm_rect_t b, int32_t cx, int32_t cy,
                  int32_t rr, uint32_t col)
{
    int32_t pcx = mapx(b, cx), pcy = mapy(b, cy), pr = maps(b, rr);
    for (int32_t dy = -pr; dy <= pr; ++dy)
        for (int32_t dx = -pr; dx <= pr; ++dx)
            if (dx * dx + dy * dy <= pr * pr)
                wm_canvas_put(c, pcx + dx, pcy + dy, col);
}

static void nring(wm_canvas_t *c, wm_rect_t b, int32_t cx, int32_t cy,
                  int32_t rr, int32_t th, uint32_t col)
{
    int32_t pcx = mapx(b, cx), pcy = mapy(b, cy), pr = maps(b, rr), pt = maps(b, th);
    int32_t inner = pr - pt; if (inner < 0) inner = 0;
    for (int32_t dy = -pr; dy <= pr; ++dy)
        for (int32_t dx = -pr; dx <= pr; ++dx) {
            int32_t d2 = dx * dx + dy * dy;
            if (d2 <= pr * pr && d2 >= inner * inner)
                wm_canvas_put(c, pcx + dx, pcy + dy, col);
        }
}

/* thick line via disc stamping */
static void nline(wm_canvas_t *c, wm_rect_t b, int32_t x0, int32_t y0,
                  int32_t x1, int32_t y1, int32_t th, uint32_t col)
{
    int32_t px0 = mapx(b, x0), py0 = mapy(b, y0);
    int32_t px1 = mapx(b, x1), py1 = mapy(b, y1);
    int32_t pt = maps(b, th) / 2; if (pt < 0) pt = 0;
    int32_t dx = px1 - px0, dy = py1 - py0;
    int32_t steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
                    ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    if (steps == 0) steps = 1;
    for (int32_t i = 0; i <= steps; ++i) {
        int32_t x = px0 + dx * i / steps;
        int32_t y = py0 + dy * i / steps;
        for (int32_t oy = -pt; oy <= pt; ++oy)
            for (int32_t ox = -pt; ox <= pt; ++ox)
                if (ox * ox + oy * oy <= (pt + 1) * (pt + 1))
                    wm_canvas_put(c, x + ox, y + oy, col);
    }
}

/* three stacked Wi-Fi arcs approximated by nested rings clipped to the top */
static void wifi_arcs(wm_canvas_t *c, wm_rect_t b, uint32_t col, bool connected)
{
    /* dot */
    ndisc(c, b, 500, 760, 55, col);
    int32_t radii[3] = {230, 380, 530};
    int32_t n = connected ? 3 : 1;
    for (int32_t k = 0; k < 3; ++k) {
        uint32_t cc = (k < n) ? col : ((col & 0x00FFFFFFu) | 0x55000000u);
        int32_t pcx = mapx(b, 500), pcy = mapy(b, 760), pr = maps(b, radii[k]);
        int32_t pt = maps(b, 70);
        for (int32_t dy = -pr; dy <= 0; ++dy)
            for (int32_t dx = -pr; dx <= pr; ++dx) {
                int32_t d2 = dx * dx + dy * dy;
                if (d2 <= pr * pr && d2 >= (pr - pt) * (pr - pt))
                    wm_canvas_put(c, pcx + dx, pcy + dy, cc);
            }
    }
}

void wm_icon_draw(wm_canvas_t *canvas, wm_rect_t box,
                  wm_icon_kind_t kind, uint32_t argb)
{
    if (!canvas || box.w == 0u || box.h == 0u) return;
    uint32_t col = argb | (argb >> 24 ? 0u : 0xFF000000u);

    switch (kind) {
    case WM_ICON_MENU:
        nrect(canvas, box, 150, 270, 700, 90, 45, col);
        nrect(canvas, box, 150, 455, 700, 90, 45, col);
        nrect(canvas, box, 150, 640, 700, 90, 45, col);
        break;
    case WM_ICON_APPS:
        for (int r = 0; r < 3; ++r)
            for (int cc = 0; cc < 3; ++cc)
                ndisc(canvas, box, 250 + cc * 250, 250 + r * 250, 95, col);
        break;
    case WM_ICON_SEARCH:
        nring(canvas, box, 430, 430, 300, 90, col);
        nline(canvas, box, 640, 640, 860, 860, 110, col);
        break;
    case WM_ICON_WIFI:      wifi_arcs(canvas, box, col, true);  break;
    case WM_ICON_WIFI_OFF:  wifi_arcs(canvas, box, col, false);
        nline(canvas, box, 180, 180, 820, 820, 90, col);        break;
    case WM_ICON_VOLUME:
        nrect(canvas, box, 150, 380, 180, 240, 40, col);
        nline(canvas, box, 330, 380, 520, 220, 0, col);
        nline(canvas, box, 330, 620, 520, 780, 0, col);
        nrect(canvas, box, 470, 220, 60, 560, 30, col);
        nring(canvas, box, 620, 500, 230, 80, col);
        nring(canvas, box, 640, 500, 380, 80, col);
        break;
    case WM_ICON_VOLUME_MUTE:
        nrect(canvas, box, 150, 380, 180, 240, 40, col);
        nrect(canvas, box, 470, 220, 60, 560, 30, col);
        nline(canvas, box, 330, 380, 520, 220, 0, col);
        nline(canvas, box, 330, 620, 520, 780, 0, col);
        nline(canvas, box, 640, 360, 900, 640, 90, col);
        nline(canvas, box, 900, 360, 640, 640, 90, col);
        break;
    case WM_ICON_IME:
        nring(canvas, box, 500, 500, 430, 80, col);       /* body outline via rounded rect */
        nrect(canvas, box, 120, 300, 760, 400, 90, col);
        /* keys punched out darker are hard without bg; draw key dots lighter */
        {
            uint32_t k = (col & 0x00FFFFFFu) | 0x66000000u;
            for (int i = 0; i < 4; ++i) ndisc(canvas, box, 230 + i * 180, 430, 45, k);
            for (int i = 0; i < 4; ++i) ndisc(canvas, box, 230 + i * 180, 570, 45, k);
            nrect(canvas, box, 330, 545, 340, 60, 30, k);
        }
        break;
    case WM_ICON_NOTIFICATIONS:
        nrect(canvas, box, 300, 220, 400, 120, 60, col);
        nring(canvas, box, 500, 520, 360, 90, col);
        nrect(canvas, box, 180, 700, 640, 90, 45, col);
        ndisc(canvas, box, 500, 850, 90, col);
        break;
    case WM_ICON_SETTINGS:
        nring(canvas, box, 500, 500, 380, 110, col);
        ndisc(canvas, box, 500, 500, 150, col);
        for (int i = 0; i < 8; ++i) {
            static const int sx[8] = {500,720,820,720,500,280,180,280};
            static const int sy[8] = {180,280,500,720,820,720,500,280};
            ndisc(canvas, box, sx[i], sy[i], 100, col);
        }
        break;
    case WM_ICON_POWER:
        nring(canvas, box, 500, 560, 340, 100, col);
        nrect(canvas, box, 450, 150, 100, 380, 50, col);
        break;
    case WM_ICON_RESTART:
        nring(canvas, box, 500, 500, 340, 100, col);
        nrect(canvas, box, 500, 500, 260, 100, 0, (col & 0x00FFFFFFu)); /* erase-ish no-op */
        ndisc(canvas, box, 780, 300, 120, col);
        nline(canvas, box, 720, 180, 820, 420, 90, col);
        break;
    case WM_ICON_FOLDER:
        nrect(canvas, box, 120, 300, 340, 160, 50, col);
        nrect(canvas, box, 120, 380, 760, 460, 70, col);
        break;
    case WM_ICON_DOC:
        nrect(canvas, box, 240, 140, 420, 720, 60, col);
        nrect(canvas, box, 560, 140, 200, 220, 40, (col & 0x00FFFFFFu) | 0x99000000u);
        break;
    case WM_ICON_PERSON:
        ndisc(canvas, box, 500, 340, 200, col);
        nrect(canvas, box, 220, 620, 560, 300, 150, col);
        break;
    case WM_ICON_CLOSE:
        nline(canvas, box, 220, 220, 780, 780, 110, col);
        nline(canvas, box, 780, 220, 220, 780, 110, col);
        break;
    case WM_ICON_MINIMIZE:
        nrect(canvas, box, 200, 720, 600, 90, 45, col);
        break;
    case WM_ICON_MAXIMIZE:
        nring(canvas, box, 500, 500, 340, 90, col);
        break;
    case WM_ICON_RESTORE:
        nring(canvas, box, 440, 560, 280, 90, col);
        nline(canvas, box, 560, 300, 760, 300, 90, col);
        nline(canvas, box, 760, 300, 760, 500, 90, col);
        break;
    case WM_ICON_CHEVRON_UP:
        nline(canvas, box, 220, 620, 500, 340, 110, col);
        nline(canvas, box, 500, 340, 780, 620, 110, col);
        break;
    case WM_ICON_TERMINAL:
        nrect(canvas, box, 120, 200, 760, 600, 70, col);
        nline(canvas, box, 260, 380, 440, 500, 80, (col & 0x00FFFFFFu) | 0xCC000000u);
        nline(canvas, box, 440, 500, 260, 620, 80, (col & 0x00FFFFFFu) | 0xCC000000u);
        break;
    case WM_ICON_EDIT:
        nline(canvas, box, 200, 800, 700, 300, 130, col);
        nrect(canvas, box, 660, 200, 160, 160, 40, col);
        nline(canvas, box, 200, 800, 320, 800, 90, col);
        break;
    default:
        break;
    }
}

void wm_icon_draw_cursor(wm_canvas_t *canvas, int32_t x, int32_t y,
                         wm_cursor_style_t style, uint32_t fill, uint32_t edge)
{
    if (!canvas) return;

    if (style == WM_CURSOR_DEFAULT) {
        /* Material-style filled arrow pointer, 18px tall. */
        static const signed char pts[19] = {1,2,3,4,5,6,7,8,9,10,11,12,7,4,3,2,2,1,0};
        /* outline pass */
        for (int32_t r = 0; r < 19; ++r) {
            int32_t span = pts[r];
            for (int32_t cx = -1; cx <= span + 1; ++cx)
                wm_canvas_put(canvas, x + cx, y + r, edge);
            wm_canvas_put(canvas, x + span + 1, y + r, edge);
        }
        /* fill pass */
        for (int32_t r = 1; r < 17; ++r) {
            int32_t span = pts[r];
            for (int32_t cx = 1; cx < span; ++cx)
                wm_canvas_put(canvas, x + cx, y + r, fill);
        }
        /* tail */
        for (int32_t r = 12; r < 20; ++r) {
            for (int32_t cx = 6; cx <= 9; ++cx)
                wm_canvas_put(canvas, x + cx, y + r, fill);
            wm_canvas_put(canvas, x + 5, y + r, edge);
            wm_canvas_put(canvas, x + 10, y + r, edge);
        }
        return;
    }

    int32_t cx = x + 7, cy = y + 7;
    bool hz = style == WM_CURSOR_RESIZE_HORIZONTAL;
    bool vt = style == WM_CURSOR_RESIZE_VERTICAL;
    bool nwse = style == WM_CURSOR_RESIZE_DIAGONAL_NW_SE;
    for (int32_t off = -1; off <= 1; ++off) {
        uint32_t lc = off == 0 ? fill : edge;
        if (hz)        wm_canvas_line(canvas, cx - 7, cy + off, cx + 7, cy + off, lc);
        else if (vt)   wm_canvas_line(canvas, cx + off, cy - 7, cx + off, cy + 7, lc);
        else if (nwse) wm_canvas_line(canvas, cx - 6 + off, cy - 6, cx + 6 + off, cy + 6, lc);
        else           wm_canvas_line(canvas, cx + 6 + off, cy - 6, cx - 6 + off, cy + 6, lc);
    }
}
