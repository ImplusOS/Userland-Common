#include "WM_Damage.h"

#include <string.h>

static bool rect_valid(wm_rect_t rect)
{
    return rect.w != 0u && rect.h != 0u;
}

static bool rect_touches(wm_rect_t a, wm_rect_t b)
{
    int64_t ar = (int64_t)a.x + (int64_t)a.w;
    int64_t ab = (int64_t)a.y + (int64_t)a.h;
    int64_t br = (int64_t)b.x + (int64_t)b.w;
    int64_t bb = (int64_t)b.y + (int64_t)b.h;
    return (int64_t)a.x <= br + 2 && (int64_t)b.x <= ar + 2 &&
           (int64_t)a.y <= bb + 2 && (int64_t)b.y <= ab + 2;
}

static void region_compact_to_union(wm_region_t *region, wm_rect_t rect)
{
    if (!region) return;
    wm_rect_t merged = rect;
    for (uint32_t i = 0; i < region->count; ++i)
        merged = wm_rect_union(merged, region->rects[i]);
    region->rects[0] = merged;
    region->count = 1u;
}

void wm_region_reset(wm_region_t *region)
{
    if (!region) return;
    memset(region, 0, sizeof(*region));
}

bool wm_region_is_empty(const wm_region_t *region)
{
    return !region || (!region->full && region->count == 0u);
}

wm_rect_t wm_rect_intersection(wm_rect_t a, wm_rect_t b)
{
    int32_t x0 = wm_max_i32(a.x, b.x);
    int32_t y0 = wm_max_i32(a.y, b.y);
    int64_t a_x1 = (int64_t)a.x + (int64_t)a.w;
    int64_t a_y1 = (int64_t)a.y + (int64_t)a.h;
    int64_t b_x1 = (int64_t)b.x + (int64_t)b.w;
    int64_t b_y1 = (int64_t)b.y + (int64_t)b.h;
    int64_t x1 = a_x1 < b_x1 ? a_x1 : b_x1;
    int64_t y1 = a_y1 < b_y1 ? a_y1 : b_y1;
    if (x1 <= x0 || y1 <= y0) return (wm_rect_t){0, 0, 0, 0};
    return (wm_rect_t){x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0)};
}

bool wm_rect_intersects(wm_rect_t a, wm_rect_t b)
{
    return rect_valid(wm_rect_intersection(a, b));
}

wm_rect_t wm_rect_union(wm_rect_t a, wm_rect_t b)
{
    if (!rect_valid(a)) return b;
    if (!rect_valid(b)) return a;
    int32_t x0 = wm_min_i32(a.x, b.x);
    int32_t y0 = wm_min_i32(a.y, b.y);
    int64_t a_x1 = (int64_t)a.x + (int64_t)a.w;
    int64_t a_y1 = (int64_t)a.y + (int64_t)a.h;
    int64_t b_x1 = (int64_t)b.x + (int64_t)b.w;
    int64_t b_y1 = (int64_t)b.y + (int64_t)b.h;
    int64_t x1 = a_x1 > b_x1 ? a_x1 : b_x1;
    int64_t y1 = a_y1 > b_y1 ? a_y1 : b_y1;
    return (wm_rect_t){x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0)};
}

void wm_region_add_full(wm_region_t *region)
{
    if (!region) return;
    region->full = true;
    region->count = 0u;
}

void wm_region_add(wm_region_t *region, wm_rect_t rect, wm_rect_t bounds)
{
    if (!region || region->full) return;
    rect = wm_rect_intersection(rect, bounds);
    if (!rect_valid(rect)) return;

    for (uint32_t i = 0; i < region->count; ++i) {
        if (rect_touches(region->rects[i], rect)) {
            region->rects[i] = wm_rect_union(region->rects[i], rect);
            for (uint32_t j = 0; j < region->count; ++j) {
                if (j == i || !rect_touches(region->rects[i], region->rects[j])) continue;
                region->rects[i] = wm_rect_union(region->rects[i], region->rects[j]);
                region->rects[j] = region->rects[region->count - 1u];
                --region->count;
                if (j < i) --i;
                j = 0u;
            }
            return;
        }
    }

    if (region->count >= WM_MAX_DAMAGE_RECTS) {
        region_compact_to_union(region, rect);
        return;
    }
    region->rects[region->count++] = rect;
}

void wm_region_union(wm_region_t *dst, const wm_region_t *src, wm_rect_t bounds)
{
    if (!dst || !src || dst->full) return;
    if (src->full) {
        wm_region_add_full(dst);
        return;
    }
    for (uint32_t i = 0; i < src->count; ++i)
        wm_region_add(dst, src->rects[i], bounds);
}
