#pragma once

#include "../Core/WM_State.h"

bool wm_font_init(wm_font_t *font, const char *path);
void wm_font_destroy(wm_font_t *font);
uint32_t wm_font_measure(wm_font_t *font, const char *text, float pixel_height);
void wm_font_draw(wm_font_t *font, wm_canvas_t *canvas,
                  int32_t x, int32_t y, const char *text,
                  uint32_t color, float pixel_height, uint32_t max_width);
