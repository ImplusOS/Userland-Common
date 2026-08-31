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
    return 0xFF000000u | (red << 16u) | (green << 8u) | blue;
}

uint32_t wm_color_lerp(uint32_t a, uint32_t b, uint32_t numerator, uint32_t denominator)
{
    if (denominator == 0u || numerator == 0u) return a;
    if (numerator >= denominator) return b;
    uint64_t inverse = (uint64_t)denominator - numerator;
    uint64_t round = denominator / 2u;
    uint32_t alpha = (uint32_t)((((uint64_t)(a >> 24u) * inverse) +
                                 ((uint64_t)(b >> 24u) * numerator) + round) /
                                denominator);
    uint32_t red = (uint32_t)((((uint64_t)((a >> 16u) & 0xFFu) * inverse) +
                               ((uint64_t)((b >> 16u) & 0xFFu) * numerator) + round) /
                              denominator);
    uint32_t green = (uint32_t)((((uint64_t)((a >> 8u) & 0xFFu) * inverse) +
                                 ((uint64_t)((b >> 8u) & 0xFFu) * numerator) + round) /
                                denominator);
    uint32_t blue = (uint32_t)((((uint64_t)(a & 0xFFu) * inverse) +
                                ((uint64_t)(b & 0xFFu) * numerator) + round) /
                               denominator);
    return (alpha << 24u) | (red << 16u) | (green << 8u) | blue;
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

static uint8_t rounded_pixel_coverage(uint32_t x, uint32_t y,
                                      uint32_t width, uint32_t height,
                                      uint32_t radius)
{
    if (radius == 0u || width == 0u || height == 0u) return 255u;
    radius = wm_min_u32(radius, wm_min_u32(width / 2u, height / 2u));
    if (radius == 0u) return 255u;

    bool corner_x = x < radius || x >= width - radius;
    bool corner_y = y < radius || y >= height - radius;
    if (!corner_x || !corner_y) return 255u;

    int32_t center_x16 = x < radius ? (int32_t)radius * 16 :
        (int32_t)(width - radius) * 16;
    int32_t center_y16 = y < radius ? (int32_t)radius * 16 :
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

void wm_canvas_fill_rounded(wm_canvas_t *canvas, wm_rect_t rect,
                            uint32_t radius, uint32_t color)
{
    if (!canvas || !canvas->pixels || rect.w == 0u || rect.h == 0u) return;
    radius = wm_min_u32(radius, wm_min_u32(rect.w / 2u, rect.h / 2u));
    if (radius == 0u) {
        wm_canvas_fill(canvas, rect, color);
        return;
    }
    wm_rect_t visible = wm_rect_intersection(rect, canvas->clip);
    if (visible.w == 0u || visible.h == 0u) return;

    for (uint32_t row = 0u; row < visible.h; ++row) {
        int32_t py = visible.y + (int32_t)row;
        uint32_t local_y = (uint32_t)(py - rect.y);
        if (local_y >= radius && local_y < rect.h - radius) {
            wm_canvas_fill(canvas, (wm_rect_t){visible.x, py, visible.w, 1u}, color);
            continue;
        }
        for (uint32_t col = 0u; col < visible.w; ++col) {
            int32_t px = visible.x + (int32_t)col;
            uint32_t local_x = (uint32_t)(px - rect.x);
            uint8_t coverage = rounded_pixel_coverage(local_x, local_y,
                                                      rect.w, rect.h, radius);
            wm_canvas_put_coverage(canvas, px, py, color, coverage);
        }
    }
}

void wm_canvas_gradient_vertical(wm_canvas_t *canvas, wm_rect_t rect,
                                 uint32_t top, uint32_t bottom)
{
    if (!canvas || rect.h == 0u) return;
    wm_rect_t visible = wm_rect_intersection(rect, canvas->clip);
    for (uint32_t row = 0; row < visible.h; ++row) {
        uint32_t local_y = (uint32_t)(visible.y + (int32_t)row - rect.y);
        uint32_t color = wm_color_lerp(top, bottom, local_y, rect.h > 1u ? rect.h - 1u : 1u);
        wm_canvas_fill(canvas, (wm_rect_t){visible.x, visible.y + (int32_t)row, visible.w, 1u}, color);
    }
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
                destination.w, destination.h, corner_radius);
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

void wm_canvas_draw_icon(wm_canvas_t *canvas, wm_rect_t destination,
                         const wm_icon_image_t *icon, uint8_t opacity,
                         uint32_t corner_radius, uint32_t fallback_color)
{
    if (!canvas || destination.w == 0u || destination.h == 0u || opacity == 0u) return;
    if (icon && icon->pixels && icon->width != 0u && icon->height != 0u) {
        wm_canvas_blit_scaled(canvas, destination, icon->pixels,
                              icon->width, icon->height, opacity, corner_radius);
        return;
    }
    uint32_t alpha = ((fallback_color >> 24u) * (uint32_t)opacity + 127u) / 255u;
    wm_canvas_fill(canvas, destination, (fallback_color & 0x00FFFFFFu) | (alpha << 24u));
}

