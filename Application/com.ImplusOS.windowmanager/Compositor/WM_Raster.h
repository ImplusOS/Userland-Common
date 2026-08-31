#pragma once

#include "../Core/WM_State.h"
#include "WM_Damage.h"

uint32_t wm_color_blend(uint32_t background, uint32_t foreground);
uint32_t wm_color_lerp(uint32_t a, uint32_t b, uint32_t numerator, uint32_t denominator);

void wm_canvas_init(wm_canvas_t *canvas, uint32_t *pixels,
                    uint32_t width, uint32_t height, uint32_t stride);
void wm_canvas_set_clip(wm_canvas_t *canvas, wm_rect_t clip);
void wm_canvas_put(wm_canvas_t *canvas, int32_t x, int32_t y, uint32_t color);
void wm_canvas_fill(wm_canvas_t *canvas, wm_rect_t rect, uint32_t color);
void wm_canvas_fill_rounded(wm_canvas_t *canvas, wm_rect_t rect,
                            uint32_t radius, uint32_t color);
void wm_canvas_gradient_vertical(wm_canvas_t *canvas, wm_rect_t rect,
                                 uint32_t top, uint32_t bottom);
void wm_canvas_line(wm_canvas_t *canvas, int32_t x0, int32_t y0,
                    int32_t x1, int32_t y1, uint32_t color);
void wm_canvas_blit(wm_canvas_t *canvas, wm_rect_t destination,
                    const uint32_t *source, uint32_t source_width,
                    uint32_t source_height, uint32_t source_x,
                    uint32_t source_y, uint8_t opacity, bool force_opaque);
void wm_canvas_blit_scaled(wm_canvas_t *canvas, wm_rect_t destination,
                           const uint32_t *source, uint32_t source_width,
                           uint32_t source_height, uint8_t opacity,
                           uint32_t corner_radius);
void wm_canvas_draw_icon(wm_canvas_t *canvas, wm_rect_t destination,
                         const wm_icon_image_t *icon, uint8_t opacity,
                         uint32_t corner_radius, uint32_t fallback_color);
