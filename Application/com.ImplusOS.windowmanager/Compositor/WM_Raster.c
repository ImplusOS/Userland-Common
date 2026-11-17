#include "WM_Raster.h"

#include <stdlib.h>
#include <string.h>

uint32_t wm_color_blend(uint32_t background, uint32_t foreground)
{
    uint32_t alpha = foreground >> 24u;
    if (alpha == 255u) return foreground;
    if (alpha == 0u) return background;
    uint32_t inverse = 255u - alpha;
    uint32_t red = ((((foreground >> 16u) & 0xFFu) * alpha) +
                    (((background >> 16u) & 0xFFu) * inverse) + 127u) / 255u;
    uint32_t green = ((((foreground >> 8u) & 0xFFu) * alpha) +
                      (((background >> 8u) & 0xFFu) * inverse) + 127u) / 255u;
    uint32_t blue = (((foreground & 0xFFu) * alpha) +
                     ((background & 0xFFu) * inverse) + 127u) / 255u;
    /* Porter-Duff "over" with a premultiplied destination. On an opaque
     * destination (the CPU-composited desktop) this is the familiar
     * straight-alpha mix and the result stays opaque. On a GPU layer --
     * cleared to transparent black -- the same colour arithmetic yields the
     * premultiplied colour and this alpha the layer's coverage, which is
     * exactly what the GPU's ONE / ONE_MINUS_SRC_ALPHA blend expects. */
    uint32_t out_alpha = alpha +
        (((background >> 24u) * inverse) + 127u) / 255u;
    return (out_alpha << 24u) | (red << 16u) | (green << 8u) | blue;
}

void wm_canvas_init(wm_canvas_t *canvas, uint32_t *pixels,
                    uint32_t width, uint32_t height, uint32_t stride)
{
    if (!canvas) return;
    canvas->pixels = pixels;
    canvas->width = width;
    canvas->height = height;
    canvas->stride = stride;
    canvas->clip = (wm_rect_t){0, 0, width, height};
}

void wm_canvas_set_clip(wm_canvas_t *canvas, wm_rect_t clip)
{
    if (!canvas) return;
    canvas->clip = wm_rect_intersection(clip,
        (wm_rect_t){0, 0, canvas->width, canvas->height});
}

void wm_canvas_put(wm_canvas_t *canvas, int32_t x, int32_t y, uint32_t color)
{
    if (!canvas || !canvas->pixels || x < canvas->clip.x || y < canvas->clip.y) return;
    int64_t clip_x1 = (int64_t)canvas->clip.x + (int64_t)canvas->clip.w;
    int64_t clip_y1 = (int64_t)canvas->clip.y + (int64_t)canvas->clip.h;
    if ((int64_t)x >= clip_x1 || (int64_t)y >= clip_y1 ||
        x < 0 || y < 0 || (uint32_t)x >= canvas->width || (uint32_t)y >= canvas->height) return;
    uint32_t *pixel = &canvas->pixels[(uint32_t)y * canvas->stride + (uint32_t)x];
    *pixel = wm_color_blend(*pixel, color);
}

void wm_canvas_fill(wm_canvas_t *canvas, wm_rect_t rect, uint32_t color)
{
    if (!canvas || !canvas->pixels) return;
    rect = wm_rect_intersection(rect, canvas->clip);
    if (rect.w == 0u || rect.h == 0u) return;
    uint32_t alpha = color >> 24u;
    if (alpha == 0u) return;
    if (alpha == 255u) {
        uint32_t *first = &canvas->pixels[
            (uint32_t)rect.y * canvas->stride + (uint32_t)rect.x];
        for (uint32_t col = 0; col < rect.w; ++col) first[col] = color;
        for (uint32_t row = 1u; row < rect.h; ++row) {
            uint32_t *destination = &canvas->pixels[
                (uint32_t)(rect.y + (int32_t)row) * canvas->stride +
                (uint32_t)rect.x];
            memcpy(destination, first, (size_t)rect.w * sizeof(uint32_t));
        }
        return;
    }
    for (uint32_t row = 0; row < rect.h; ++row) {
        uint32_t *destination = &canvas->pixels[
            (uint32_t)(rect.y + (int32_t)row) * canvas->stride + (uint32_t)rect.x];
        for (uint32_t col = 0; col < rect.w; ++col)
            destination[col] = wm_color_blend(destination[col], color);
    }
}

/* Per-corner radii, in the order M3 writes a shape: top-left, top-right,
 * bottom-right, bottom-left. A component with three round corners and one
 * square one -- a title bar, a menu anchored to an edge -- is an M3 shape
 * like any other, and used to have to be faked by painting a rounded rect
 * and then a square patch over part of it. */
typedef struct {
    uint32_t top_left;
    uint32_t top_right;
    uint32_t bottom_right;
    uint32_t bottom_left;
} wm_corners_t;

static uint32_t corner_radius_at(const wm_corners_t *corners,
                                 bool left, bool top)
{
    if (top) return left ? corners->top_left : corners->top_right;
    return left ? corners->bottom_left : corners->bottom_right;
}

static uint8_t rounded_pixel_coverage(uint32_t x, uint32_t y,
                                      uint32_t width, uint32_t height,
                                      const wm_corners_t *corners)
{
    if (width == 0u || height == 0u) return 255u;
    uint32_t limit = wm_min_u32(width / 2u, height / 2u);

    bool left = x < width / 2u;
    bool top = y < height / 2u;
    uint32_t radius = wm_min_u32(corner_radius_at(corners, left, top), limit);
    if (radius == 0u) return 255u;

    bool corner_x = left ? x < radius : x >= width - radius;
    bool corner_y = top ? y < radius : y >= height - radius;
    if (!corner_x || !corner_y) return 255u;

    int32_t center_x16 = left ? (int32_t)radius * 16 :
        (int32_t)(width - radius) * 16;
    int32_t center_y16 = top ? (int32_t)radius * 16 :
        (int32_t)(height - radius) * 16;
    int32_t radius16 = (int32_t)radius * 16;
    int64_t radius_squared = (int64_t)radius16 * (int64_t)radius16;
    static const int32_t sample_offsets[4] = {2, 6, 10, 14};
    uint32_t hits = 0u;
    for (uint32_t sy = 0u; sy < 4u; ++sy) {
        int32_t sample_y16 = (int32_t)y * 16 + sample_offsets[sy];
        int32_t dy = sample_y16 - center_y16;
        for (uint32_t sx = 0u; sx < 4u; ++sx) {
            int32_t sample_x16 = (int32_t)x * 16 + sample_offsets[sx];
            int32_t dx = sample_x16 - center_x16;
            if ((int64_t)dx * (int64_t)dx +
                (int64_t)dy * (int64_t)dy <= radius_squared) ++hits;
        }
    }
    return (uint8_t)((hits * 255u + 8u) / 16u);
}

static void wm_canvas_put_coverage(wm_canvas_t *canvas, int32_t x, int32_t y,
                                   uint32_t color, uint8_t coverage)
{
    if (coverage == 0u) return;
    if (coverage < 255u) {
        uint32_t alpha = (((color >> 24u) * (uint32_t)coverage) + 127u) / 255u;
        color = (color & 0x00FFFFFFu) | (alpha << 24u);
    }
    wm_canvas_put(canvas, x, y, color);
}

static void fill_shape(wm_canvas_t *canvas, wm_rect_t rect,
                       const wm_corners_t *corners, uint32_t color)
{
    if (!canvas || !canvas->pixels || rect.w == 0u || rect.h == 0u) return;
    uint32_t limit = wm_min_u32(rect.w / 2u, rect.h / 2u);
    uint32_t widest = corners->top_left;
    if (corners->top_right > widest) widest = corners->top_right;
    if (corners->bottom_right > widest) widest = corners->bottom_right;
    if (corners->bottom_left > widest) widest = corners->bottom_left;
    widest = wm_min_u32(widest, limit);
    if (widest == 0u) {
        wm_canvas_fill(canvas, rect, color);
        return;
    }
    wm_rect_t visible = wm_rect_intersection(rect, canvas->clip);
    if (visible.w == 0u || visible.h == 0u) return;

    for (uint32_t row = 0u; row < visible.h; ++row) {
        int32_t py = visible.y + (int32_t)row;
        uint32_t local_y = (uint32_t)(py - rect.y);
        if (local_y >= widest && local_y < rect.h - widest) {
            wm_canvas_fill(canvas, (wm_rect_t){visible.x, py, visible.w, 1u}, color);
            continue;
        }
        /* A corner row: only the arcs at either end need coverage; the run
         * between them is solid and goes down as one span. */
        bool top = local_y < rect.h / 2u;
        uint32_t left_r = wm_min_u32(corner_radius_at(corners, true, top), limit);
        uint32_t right_r = wm_min_u32(corner_radius_at(corners, false, top), limit);
        int32_t vx0 = visible.x;
        int32_t vx1 = visible.x + (int32_t)visible.w;
        int32_t mid0 = rect.x + (int32_t)left_r;
        int32_t mid1 = rect.x + (int32_t)(rect.w - right_r);
        int32_t left_end = mid0 < vx1 ? mid0 : vx1;
        for (int32_t px = vx0; px < left_end; ++px) {
            uint8_t coverage = rounded_pixel_coverage((uint32_t)(px - rect.x),
                                                      local_y, rect.w, rect.h,
                                                      corners);
            wm_canvas_put_coverage(canvas, px, py, color, coverage);
        }
        int32_t a = mid0 > vx0 ? mid0 : vx0;
        int32_t b = mid1 < vx1 ? mid1 : vx1;
        if (b > a)
            wm_canvas_fill(canvas, (wm_rect_t){a, py, (uint32_t)(b - a), 1u}, color);
        int32_t right_start = mid1 > vx0 ? mid1 : vx0;
        if (right_start < left_end) right_start = left_end;
        for (int32_t px = right_start; px < vx1; ++px) {
            uint8_t coverage = rounded_pixel_coverage((uint32_t)(px - rect.x),
                                                      local_y, rect.w, rect.h,
                                                      corners);
            wm_canvas_put_coverage(canvas, px, py, color, coverage);
        }
    }
}

void wm_canvas_fill_rounded(wm_canvas_t *canvas, wm_rect_t rect,
                            uint32_t radius, uint32_t color)
{
    wm_corners_t corners = {radius, radius, radius, radius};
    fill_shape(canvas, rect, &corners, color);
}

void wm_canvas_fill_shape(wm_canvas_t *canvas, wm_rect_t rect,
                          uint32_t top_left, uint32_t top_right,
                          uint32_t bottom_right, uint32_t bottom_left,
                          uint32_t color)
{
    wm_corners_t corners = {top_left, top_right, bottom_right, bottom_left};
    fill_shape(canvas, rect, &corners, color);
}

void wm_canvas_stroke_rounded(wm_canvas_t *canvas, wm_rect_t rect,
                              uint32_t radius, uint32_t thickness,
                              uint32_t color)
{
    if (!canvas || thickness == 0u || rect.w == 0u || rect.h == 0u) return;
    if (thickness * 2u >= rect.w || thickness * 2u >= rect.h) {
        wm_canvas_fill_rounded(canvas, rect, radius, color);
        return;
    }
    /* An M3 outline is a ring: paint the outer shape, then knock the inner
     * one back out. Doing it on a scratch row would be cheaper, but the
     * shell strokes a handful of small components per frame, and this keeps
     * the antialiasing identical to every other shape on screen. */
    wm_rect_t visible = wm_rect_intersection(rect, canvas->clip);
    if (visible.w == 0u || visible.h == 0u) return;

    wm_rect_t inner = {rect.x + (int32_t)thickness, rect.y + (int32_t)thickness,
                       rect.w - thickness * 2u, rect.h - thickness * 2u};
    uint32_t inner_radius = radius > thickness ? radius - thickness : 0u;
    wm_corners_t outer_corners = {radius, radius, radius, radius};
    wm_corners_t inner_corners = {inner_radius, inner_radius,
                                  inner_radius, inner_radius};

    /* Away from the corners a row is either all ring (above / below the
     * hole) or ring only at its two ends; those runs are known without
     * sampling, so only the corner arcs are computed pixel by pixel. */
    uint32_t outer_r = wm_min_u32(radius, wm_min_u32(rect.w / 2u, rect.h / 2u));
    uint32_t hole_r = wm_min_u32(inner_radius,
                                 wm_min_u32(inner.w / 2u, inner.h / 2u));
    int32_t vx1 = visible.x + (int32_t)visible.w;
    for (uint32_t row = 0u; row < visible.h; ++row) {
        int32_t py = visible.y + (int32_t)row;
        bool in_hole_rows = py >= inner.y && py < inner.y + (int32_t)inner.h;
        int32_t skip0, skip1;      /* a run settled without sampling */
        if (in_hole_rows) {
            skip0 = inner.x + (int32_t)hole_r;
            skip1 = inner.x + (int32_t)(inner.w - hole_r);
        } else {
            skip0 = rect.x + (int32_t)outer_r;
            skip1 = rect.x + (int32_t)(rect.w - outer_r);
            int32_t a = skip0 > visible.x ? skip0 : visible.x;
            int32_t b = skip1 < vx1 ? skip1 : vx1;
            if (b > a)
                wm_canvas_fill(canvas, (wm_rect_t){a, py, (uint32_t)(b - a), 1u},
                               color);
        }
        for (int32_t px = visible.x; px < vx1; ++px) {
            if (px >= skip0 && px < skip1) { px = skip1 - 1; continue; }
            uint8_t outer = rounded_pixel_coverage((uint32_t)(px - rect.x),
                                                   (uint32_t)(py - rect.y),
                                                   rect.w, rect.h,
                                                   &outer_corners);
            if (outer == 0u) continue;
            uint8_t hole = 0u;
            if (px >= inner.x && py >= inner.y &&
                px < inner.x + (int32_t)inner.w &&
                py < inner.y + (int32_t)inner.h) {
                hole = rounded_pixel_coverage((uint32_t)(px - inner.x),
                                              (uint32_t)(py - inner.y),
                                              inner.w, inner.h,
                                              &inner_corners);
            }
            if (hole >= outer) continue;
            wm_canvas_put_coverage(canvas, px, py, color,
                                   (uint8_t)(outer - hole));
        }
    }
}

void wm_canvas_shadow(wm_canvas_t *canvas, wm_rect_t rect, uint32_t radius,
                      uint32_t spread, uint32_t shadow_color, uint8_t opacity)
{
    if (!canvas || spread == 0u || opacity == 0u) return;
    int32_t offset_y = (int32_t)(spread * 2u / 5u);
    uint32_t base_alpha = shadow_color >> 24u;
    if (base_alpha == 0u) return;

    /* A soft ramp under the component, densest at its edge. */
    for (uint32_t layer = spread; layer > 0u; --layer) {
        uint32_t strength = ((spread - layer + 1u) * 40u) / spread;
        strength = (strength * base_alpha) / 255u;
        strength = (strength * (uint32_t)opacity) / 255u;
        if (strength == 0u) continue;
        wm_rect_t halo = {rect.x - (int32_t)layer,
                          rect.y - (int32_t)(layer / 2u) + offset_y,
                          rect.w + layer * 2u, rect.h + layer * 2u};
        wm_canvas_fill_rounded(canvas, halo, radius + layer,
            (shadow_color & 0x00FFFFFFu) | ((strength & 0xFFu) << 24u));
    }
}

/* [x0, x1) x [y0, y1) as a rect, empty when inverted. */
static wm_rect_t span_rect(int64_t x0, int64_t y0, int64_t x1, int64_t y1)
{
    if (x1 <= x0 || y1 <= y0) return (wm_rect_t){0, 0, 0u, 0u};
    return (wm_rect_t){(int32_t)x0, (int32_t)y0,
                       (uint32_t)(x1 - x0), (uint32_t)(y1 - y0)};
}

void wm_canvas_shadow_behind_opaque(wm_canvas_t *canvas, wm_rect_t rect,
                                    uint32_t radius, uint32_t spread,
                                    uint32_t shadow_color, uint8_t opacity)
{
    if (!canvas || spread == 0u || opacity == 0u) return;
    if (rect.w == 0u || rect.h == 0u) {
        wm_canvas_shadow(canvas, rect, radius, spread, shadow_color, opacity);
        return;
    }
    /* The shape about to be painted over `rect` hides the shadow under
     * its rows clear of the corners (full width) and under its columns
     * clear of the corners (full height). Paint every layer of the ramp
     * only around that cross: five bands, the corner squares kept. */
    uint32_t r = wm_min_u32(radius, wm_min_u32(rect.w / 2u, rect.h / 2u));
    wm_rect_t old = canvas->clip;
    if (old.w == 0u || old.h == 0u) return;
    int64_t cx0 = old.x, cy0 = old.y;
    int64_t cx1 = (int64_t)old.x + old.w, cy1 = (int64_t)old.y + old.h;
    int64_t x0 = rect.x, y0 = rect.y;
    int64_t x1 = (int64_t)rect.x + rect.w, y1 = (int64_t)rect.y + rect.h;
    int64_t ri = (int64_t)r;
    wm_rect_t bands[8] = {
        span_rect(cx0, cy0, cx1, y0),                       /* above */
        span_rect(cx0, y0, x0 + ri, y0 + ri),               /* top corners */
        span_rect(x1 - ri, y0, cx1, y0 + ri),
        span_rect(cx0, y0 + ri, x0, y1 - ri),               /* beside */
        span_rect(x1, y0 + ri, cx1, y1 - ri),
        span_rect(cx0, y1 - ri, x0 + ri, y1),               /* bottom corners */
        span_rect(x1 - ri, y1 - ri, cx1, y1),
        span_rect(cx0, y1, cx1, cy1),                       /* below */
    };
    for (uint32_t i = 0u; i < 8u; ++i) {
        wm_rect_t band = wm_rect_intersection(bands[i], old);
        if (band.w == 0u || band.h == 0u) continue;
        canvas->clip = band;
        wm_canvas_shadow(canvas, rect, radius, spread, shadow_color, opacity);
    }
    canvas->clip = old;
}

void wm_canvas_fill_bordered(wm_canvas_t *canvas, wm_rect_t rect,
                             uint32_t radius, uint32_t border, uint32_t fill)
{
    if (!canvas || rect.w == 0u || rect.h == 0u) return;
    wm_rect_t inner = {rect.x + 1, rect.y + 1,
                       rect.w > 2u ? rect.w - 2u : 0u,
                       rect.h > 2u ? rect.h - 2u : 0u};
    uint32_t inner_radius = radius > 0u ? radius - 1u : 0u;
    uint32_t r = wm_min_u32(radius, wm_min_u32(rect.w / 2u, rect.h / 2u));
    uint32_t band = r + 1u;
    /* Only an opaque fill hides the border shape under it; anything else
     * keeps the plain paint-then-cover order. */
    if ((fill >> 24u) != 255u || (border >> 24u) != 255u ||
        inner.w == 0u || inner.h == 0u || band * 2u >= rect.h) {
        wm_canvas_fill_rounded(canvas, rect, radius, border);
        wm_canvas_fill_rounded(canvas, inner, inner_radius, fill);
        return;
    }
    /* The border shape only shows in the corner bands at the top and the
     * bottom and in the 1px columns between them. */
    wm_rect_t old = canvas->clip;
    canvas->clip = wm_rect_intersection(old, (wm_rect_t){rect.x, rect.y, rect.w, band});
    wm_canvas_fill_rounded(canvas, rect, radius, border);
    canvas->clip = wm_rect_intersection(old,
        (wm_rect_t){rect.x, rect.y + (int32_t)(rect.h - band), rect.w, band});
    wm_canvas_fill_rounded(canvas, rect, radius, border);
    canvas->clip = old;
    uint32_t mid_h = rect.h - band * 2u;
    wm_canvas_fill(canvas, (wm_rect_t){rect.x, rect.y + (int32_t)band, 1u, mid_h}, border);
    wm_canvas_fill(canvas, (wm_rect_t){rect.x + (int32_t)rect.w - 1,
                                       rect.y + (int32_t)band, 1u, mid_h}, border);
    wm_canvas_fill_rounded(canvas, inner, inner_radius, fill);
}

void wm_canvas_popup_surface(wm_canvas_t *canvas, wm_rect_t rect,
                             uint32_t radius, uint32_t spread,
                             uint32_t shadow_color, uint8_t opacity,
                             uint32_t border, uint32_t fill)
{
    if (!canvas || rect.w == 0u || rect.h == 0u) return;
    if ((fill >> 24u) == 255u && (border >> 24u) == 255u)
        wm_canvas_shadow_behind_opaque(canvas, rect, radius, spread,
                                       shadow_color, opacity);
    else
        wm_canvas_shadow(canvas, rect, radius, spread, shadow_color, opacity);
    wm_canvas_fill_bordered(canvas, rect, radius, border, fill);
}

void wm_canvas_line(wm_canvas_t *canvas, int32_t x0, int32_t y0,
                    int32_t x1, int32_t y1, uint32_t color)
{
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t dy_abs = y1 > y0 ? y1 - y0 : y0 - y1;
    int32_t dy = -dy_abs;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t error = dx + dy;
    for (;;) {
        wm_canvas_put(canvas, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int32_t twice = error * 2;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static uint32_t interpolate_channel(uint32_t a, uint32_t b, uint32_t fraction)
{
    return (a * (256u - fraction) + b * fraction + 128u) >> 8u;
}

static uint32_t bilinear_sample(const uint32_t *source, uint32_t source_width,
                                uint32_t source_height, uint32_t x_fp,
                                uint32_t y_fp)
{
    uint32_t x0 = x_fp >> 8u;
    uint32_t y0 = y_fp >> 8u;
    uint32_t x1 = wm_min_u32(x0 + 1u, source_width - 1u);
    uint32_t y1 = wm_min_u32(y0 + 1u, source_height - 1u);
    uint32_t fx = x_fp & 0xFFu;
    uint32_t fy = y_fp & 0xFFu;
    uint32_t c00 = source[y0 * source_width + x0];
    uint32_t c10 = source[y0 * source_width + x1];
    uint32_t c01 = source[y1 * source_width + x0];
    uint32_t c11 = source[y1 * source_width + x1];

    uint32_t a0 = interpolate_channel(c00 >> 24u, c10 >> 24u, fx);
    uint32_t r0 = interpolate_channel((c00 >> 16u) & 0xFFu,
                                      (c10 >> 16u) & 0xFFu, fx);
    uint32_t g0 = interpolate_channel((c00 >> 8u) & 0xFFu,
                                      (c10 >> 8u) & 0xFFu, fx);
    uint32_t b0 = interpolate_channel(c00 & 0xFFu, c10 & 0xFFu, fx);
    uint32_t a1 = interpolate_channel(c01 >> 24u, c11 >> 24u, fx);
    uint32_t r1 = interpolate_channel((c01 >> 16u) & 0xFFu,
                                      (c11 >> 16u) & 0xFFu, fx);
    uint32_t g1 = interpolate_channel((c01 >> 8u) & 0xFFu,
                                      (c11 >> 8u) & 0xFFu, fx);
    uint32_t b1 = interpolate_channel(c01 & 0xFFu, c11 & 0xFFu, fx);

    uint32_t alpha = interpolate_channel(a0, a1, fy);
    uint32_t red = interpolate_channel(r0, r1, fy);
    uint32_t green = interpolate_channel(g0, g1, fy);
    uint32_t blue = interpolate_channel(b0, b1, fy);
    return (alpha << 24u) | (red << 16u) | (green << 8u) | blue;
}

/* Reused across calls instead of malloc/free-ing every blit -- this path
 * runs every animation frame (window show/hide/minimize/restore) whenever
 * the theme uses square corners, so a fresh heap round-trip per call adds
 * up. Grows on demand, capped by screen width, and lives for the process's
 * lifetime. */
static uint32_t *g_x_lut = NULL;
static uint32_t g_x_lut_capacity = 0u;

static uint32_t *acquire_x_lut(uint32_t count)
{
    if (count > g_x_lut_capacity) {
        uint32_t *grown = (uint32_t *)realloc(g_x_lut, (size_t)count * sizeof(uint32_t));
        if (!grown) return NULL;
        g_x_lut = grown;
        g_x_lut_capacity = count;
    }
    return g_x_lut;
}

static bool wm_canvas_blit_scaled_fast(wm_canvas_t *canvas,
                                       wm_rect_t destination,
                                       const uint32_t *source,
                                       uint32_t source_width,
                                       uint32_t source_height)
{
    if (!canvas || !canvas->pixels || !source ||
        destination.w == 0u || destination.h == 0u ||
        source_width == 0u || source_height == 0u) {
        return false;
    }

    wm_rect_t visible = wm_rect_intersection(destination, canvas->clip);
    if (visible.w == 0u || visible.h == 0u) {
        return true;
    }

    uint32_t *x_lut = acquire_x_lut(visible.w);
    if (!x_lut) {
        return false;
    }

    uint32_t denom_x = destination.w > 1u ? destination.w - 1u : 1u;
    uint32_t denom_y = destination.h > 1u ? destination.h - 1u : 1u;
    uint32_t max_source_x = source_width > 1u ? source_width - 1u : 0u;
    uint32_t max_source_y = source_height > 1u ? source_height - 1u : 0u;
    uint32_t first_local_x = (uint32_t)(visible.x - destination.x);

    for (uint32_t col = 0u; col < visible.w; ++col) {
        uint32_t local_x = first_local_x + col;
        x_lut[col] = (uint32_t)(((uint64_t)local_x *
                                 max_source_x * 256u) / denom_x);
    }

    uint32_t first_local_y = (uint32_t)(visible.y - destination.y);
    for (uint32_t row = 0u; row < visible.h; ++row) {
        uint32_t local_y = first_local_y + row;
        uint32_t source_y_fp = (uint32_t)(((uint64_t)local_y *
                                           max_source_y * 256u) / denom_y);
        uint32_t *dst = &canvas->pixels[
            (uint32_t)(visible.y + (int32_t)row) * canvas->stride +
            (uint32_t)visible.x];
        for (uint32_t col = 0u; col < visible.w; ++col) {
            uint32_t color = bilinear_sample(source, source_width,
                                             source_height, x_lut[col],
                                             source_y_fp);
            uint32_t alpha = color >> 24u;
            if (alpha == 255u) {
                dst[col] = color;
            } else if (alpha != 0u) {
                dst[col] = wm_color_blend(dst[col], color);
            }
        }
    }

    return true;
}

void wm_canvas_blit(wm_canvas_t *canvas, wm_rect_t destination,
                    const uint32_t *source, uint32_t source_width,
                    uint32_t source_height, uint32_t source_x,
                    uint32_t source_y, uint8_t opacity, bool force_opaque)
{
    if (!canvas || !canvas->pixels || !source || destination.w == 0u ||
        destination.h == 0u || source_width == 0u || source_height == 0u ||
        source_x >= source_width || source_y >= source_height ||
        opacity == 0u) return;

    uint32_t max_width = source_width - source_x;
    uint32_t max_height = source_height - source_y;
    if (destination.w > max_width) destination.w = max_width;
    if (destination.h > max_height) destination.h = max_height;
    wm_rect_t visible = wm_rect_intersection(destination, canvas->clip);
    if (visible.w == 0u || visible.h == 0u) return;

    uint32_t start_x = source_x + (uint32_t)(visible.x - destination.x);
    uint32_t start_y = source_y + (uint32_t)(visible.y - destination.y);
    for (uint32_t row = 0u; row < visible.h; ++row) {
        const uint32_t *src = &source[(start_y + row) * source_width + start_x];
        uint32_t *dst = &canvas->pixels[
            (uint32_t)(visible.y + (int32_t)row) * canvas->stride +
            (uint32_t)visible.x];
        if (force_opaque && opacity == 255u) {
            memcpy(dst, src, (size_t)visible.w * sizeof(uint32_t));
            continue;
        }
        if (opacity == 255u) {
            bool opaque = true;
            for (uint32_t col = 0u; col < visible.w; ++col) {
                if ((src[col] >> 24u) != 255u) {
                    opaque = false;
                    break;
                }
            }
            if (opaque) {
                memcpy(dst, src, (size_t)visible.w * sizeof(uint32_t));
                continue;
            }
        }

        for (uint32_t col = 0u; col < visible.w; ++col) {
            uint32_t color = src[col];
            uint32_t alpha = force_opaque ? 255u : color >> 24u;
            if (opacity != 255u)
                alpha = (alpha * (uint32_t)opacity + 127u) / 255u;
            if (alpha == 0u) continue;
            color = (color & 0x00FFFFFFu) | (alpha << 24u);
            dst[col] = alpha == 255u ? color : wm_color_blend(dst[col], color);
        }
    }
}

void wm_canvas_blit_scaled(wm_canvas_t *canvas, wm_rect_t destination,
                           const uint32_t *source, uint32_t source_width,
                           uint32_t source_height, uint8_t opacity,
                           uint32_t corner_radius)
{
    wm_corners_t corners = {corner_radius, corner_radius,
                            corner_radius, corner_radius};
    if (!canvas || !source || destination.w == 0u || destination.h == 0u ||
        source_width == 0u || source_height == 0u || opacity == 0u) return;

    if (opacity == 255u && corner_radius == 0u &&
        wm_canvas_blit_scaled_fast(canvas, destination, source,
                                   source_width, source_height)) {
        return;
    }

    wm_rect_t visible = wm_rect_intersection(destination, canvas->clip);
    if (visible.w == 0u || visible.h == 0u) return;
    corner_radius = wm_min_u32(corner_radius,
        wm_min_u32(destination.w / 2u, destination.h / 2u));

    uint32_t denom_x = destination.w > 1u ? destination.w - 1u : 1u;
    uint32_t denom_y = destination.h > 1u ? destination.h - 1u : 1u;
    uint32_t max_source_x = source_width > 1u ? source_width - 1u : 0u;
    uint32_t max_source_y = source_height > 1u ? source_height - 1u : 0u;
    for (uint32_t row = 0u; row < visible.h; ++row) {
        int32_t py = visible.y + (int32_t)row;
        uint32_t local_y = (uint32_t)(py - destination.y);
        uint32_t source_y_fp = (uint32_t)(((uint64_t)local_y * max_source_y * 256u) /
                                          denom_y);
        for (uint32_t col = 0u; col < visible.w; ++col) {
            int32_t px = visible.x + (int32_t)col;
            uint32_t local_x = (uint32_t)(px - destination.x);
            uint8_t coverage = rounded_pixel_coverage(local_x, local_y,
                destination.w, destination.h, &corners);
            if (coverage == 0u) continue;
            uint32_t source_x_fp = (uint32_t)(((uint64_t)local_x * max_source_x * 256u) /
                                              denom_x);
            uint32_t color = bilinear_sample(source, source_width, source_height,
                                             source_x_fp, source_y_fp);
            uint32_t alpha = ((color >> 24u) * (uint32_t)opacity + 127u) / 255u;
            alpha = (alpha * (uint32_t)coverage + 127u) / 255u;
            if (alpha == 0u) continue;
            color = (color & 0x00FFFFFFu) | (alpha << 24u);
            wm_canvas_put(canvas, px, py, color);
        }
    }
}

