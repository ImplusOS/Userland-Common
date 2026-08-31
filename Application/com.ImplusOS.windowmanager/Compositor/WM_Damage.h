#pragma once

#include "../Core/WM_State.h"

void wm_region_reset(wm_region_t *region);
bool wm_region_is_empty(const wm_region_t *region);
wm_rect_t wm_rect_intersection(wm_rect_t a, wm_rect_t b);
bool wm_rect_intersects(wm_rect_t a, wm_rect_t b);
wm_rect_t wm_rect_union(wm_rect_t a, wm_rect_t b);
void wm_region_add(wm_region_t *region, wm_rect_t rect, wm_rect_t bounds);
void wm_region_add_full(wm_region_t *region);
void wm_region_union(wm_region_t *dst, const wm_region_t *src, wm_rect_t bounds);
