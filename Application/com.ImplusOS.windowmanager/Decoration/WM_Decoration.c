#include "WM_Decoration.h"

#include "../Compositor/WM_Raster.h"
#include "../Font/WM_Font.h"
#include "../SceneGraph/WM_Node.h"

#include <stdlib.h>
#include <string.h>

static uint32_t apply_opacity(uint32_t color, uint8_t opacity)
{
    uint32_t alpha = ((color >> 24u) * opacity) / 255u;
    return (color & 0x00FFFFFFu) | (alpha << 24u);
}

static wm_rect_t visual_frame(const wm_state_t *state, const wm_window_t *window)
{
    uint32_t title      = window->is_system ? 0u : state->theme.title_height;
    uint32_t total_h    = window->frame.h + title;
    uint32_t width      = (uint32_t)((float)window->frame.w * window->visual_scale);
    uint32_t height     = (uint32_t)((float)total_h     * window->visual_scale);
    return (wm_rect_t){
        window->frame.x + (int32_t)((window->frame.w - width)  / 2u),
        window->frame.y + (int32_t)((total_h       - height) / 2u) +
            (int32_t)window->visual_offset_y,
        width, height
    };
}

static bool shadow_pixel_visible(uint32_t x, uint32_t y,
                                  uint32_t width, uint32_t height,
                                  uint32_t radius)
{
    if (radius == 0u) return true;
    radius = wm_min_u32(radius, wm_min_u32(width/2u, height/2u));
    uint32_t dx2 = x < radius ?
        radius*2u-(x*2u+1u) :
        (x >= width-radius ? x*2u+1u-(width-radius)*2u : 0u);
    uint32_t dy2 = y < radius ?
        radius*2u-(y*2u+1u) :
        (y >= height-radius ? y*2u+1u-(height-radius)*2u : 0u);
    if (!dx2 || !dy2) return true;
    return dx2*dx2 + dy2*dy2 <= radius*radius*4u;
}

static bool build_shadow_cache(wm_window_t *window,
                                uint32_t frame_w, uint32_t frame_h,
                                uint32_t radius, uint32_t shadow_size,
                                uint8_t theme_alpha)
{
    if (window->shadow_mask &&
        window->shadow_cache_frame_width  == frame_w &&
        window->shadow_cache_frame_height == frame_h &&
        window->shadow_cache_radius       == radius  &&
        window->shadow_cache_size         == shadow_size &&
        window->shadow_cache_alpha        == theme_alpha) return true;

    uint64_t w64 = (uint64_t)frame_w + shadow_size * 2u;
    uint64_t h64 = (uint64_t)frame_h + shadow_size * 3u + 6u;
    if (!w64 || !h64 || w64>UINT32_MAX || h64>UINT32_MAX || w64*h64>SIZE_MAX) return false;
    uint32_t W = (uint32_t)w64, H = (uint32_t)h64;
    uint32_t needed = W * H;

    /* Interactive resize drags call this up to ~60x/sec with a slightly
     * different size on nearly every frame; only grow the buffer when the
     * new size actually exceeds what's already allocated instead of
     * free+malloc-ing every time. */
    uint8_t *mask = window->shadow_mask;
    if (needed > window->shadow_mask_capacity) {
        uint8_t *grown = (uint8_t *)realloc(window->shadow_mask, needed);
        if (!grown) return false;
        mask = grown;
        window->shadow_mask_capacity = needed;
    }
    memset(mask, 0, needed);

    for (uint32_t layer = shadow_size; layer > 0u; --layer) {
        uint32_t strength = ((shadow_size - layer + 1u) * 40u) /
                            (shadow_size == 0u ? 1u : shadow_size);
        uint32_t alpha = (strength * theme_alpha) / 255u;
        uint32_t rx = shadow_size - layer;
        uint32_t ry = shadow_size - layer / 2u + 5u;
        uint32_t rw = frame_w + layer * 2u;
        uint32_t rh = frame_h + layer * 2u;
        uint32_t rr = radius + layer;
        for (uint32_t y = 0; y < rh && ry+y < H; ++y)
            for (uint32_t x = 0; x < rw && rx+x < W; ++x) {
                if (!shadow_pixel_visible(x,y,rw,rh,rr)) continue;
                uint8_t *px = &mask[(ry+y)*W + rx+x];
                *px = (uint8_t)(alpha + ((uint32_t)*px*(255u-alpha)+127u)/255u);
            }
    }
    window->shadow_mask              = mask;
    window->shadow_mask_width        = W;
    window->shadow_mask_height       = H;
    window->shadow_cache_frame_width  = frame_w;
    window->shadow_cache_frame_height = frame_h;
    window->shadow_cache_radius       = radius;
    window->shadow_cache_size         = shadow_size;
    window->shadow_cache_alpha        = theme_alpha;
    return true;
}

static void draw_shadow(wm_state_t *state, wm_canvas_t *canvas,
                         wm_window_t *window, wm_rect_t frame, uint8_t opacity)
{
    uint32_t size = state->theme.shadow_size;
    if (!size) return;
    uint8_t theme_alpha = (uint8_t)(state->theme.shadow >> 24u);
    if (!build_shadow_cache(window, frame.w, frame.h,
                             state->theme.corner_radius, size, theme_alpha)) {
        for (uint32_t layer = size; layer > 0u; --layer) {
            uint32_t strength = ((size-layer+1u)*40u)/(size==0u?1u:size);
            strength = (strength*opacity)/255u;
            wm_rect_t sh = {frame.x-(int32_t)layer, frame.y-(int32_t)(layer/2u)+5,
                            frame.w+layer*2u, frame.h+layer*2u};
            wm_canvas_fill_rounded(canvas, sh, state->theme.corner_radius+layer,
                apply_opacity(state->theme.shadow, (uint8_t)strength));
        }
        return;
    }
    uint32_t rgb = state->theme.shadow & 0x00FFFFFFu;
    int32_t ox = frame.x - (int32_t)size;
    int32_t oy = frame.y - (int32_t)size;
    for (uint32_t y = 0; y < window->shadow_mask_height; ++y)
        for (uint32_t x = 0; x < window->shadow_mask_width; ++x) {
            uint32_t ma = window->shadow_mask[y*window->shadow_mask_width+x];
            if (!ma) continue;
            uint32_t alpha = (ma*(uint32_t)opacity+127u)/255u;
            wm_canvas_put(canvas, ox+(int32_t)x, oy+(int32_t)y, rgb|(alpha<<24u));
        }
}

#define BTN_SZ   24u
#define BTN_PAD  3u
#define BTN_MARGIN 6u

static bool is_unscaled(const wm_window_t *window)
{
    return window->visual_scale > 0.999f && window->visual_scale < 1.001f;
}

static bool rect_contains_rect(wm_rect_t outer, wm_rect_t inner)
{
    return inner.w != 0u && inner.h != 0u &&
           inner.x >= outer.x && inner.y >= outer.y &&
           (int64_t)inner.x + inner.w <= (int64_t)outer.x + outer.w &&
           (int64_t)inner.y + inner.h <= (int64_t)outer.y + outer.h;
}

static void draw_title_buttons(wm_state_t *state, wm_canvas_t *canvas,
                                const wm_window_t *window, wm_rect_t frame,
                                uint8_t opacity)
{
    int32_t right = frame.x + (int32_t)frame.w - (int32_t)BTN_MARGIN;
    int32_t by    = frame.y + ((int32_t)state->theme.title_height - (int32_t)BTN_SZ) / 2;

    wm_rect_t close_btn = {right - (int32_t)BTN_SZ, by, BTN_SZ, BTN_SZ};
    wm_rect_t max_btn   = {right - (int32_t)(BTN_SZ*2u+BTN_PAD), by, BTN_SZ, BTN_SZ};
    wm_rect_t min_btn   = {right - (int32_t)(BTN_SZ*3u+BTN_PAD*2u), by, BTN_SZ, BTN_SZ};

    uint32_t r2 = BTN_SZ / 2u;

    uint32_t close_bg = window->hover_close ?
        apply_opacity(state->theme.danger, opacity) :
        apply_opacity(state->theme.surface_hover, opacity);
    if (window->hover_close || window->has_focus)
        wm_canvas_fill_rounded(canvas, close_btn, r2, close_bg);
    {
        int32_t cx = close_btn.x + (int32_t)BTN_SZ/2;
        int32_t cy = close_btn.y + (int32_t)BTN_SZ/2;
        uint32_t xc = window->hover_close ?
            apply_opacity(0xFFFFFFFFu, opacity) :
            apply_opacity(state->theme.text_dim, opacity);
        wm_canvas_line(canvas, cx-4, cy-4, cx+4, cy+4, xc);
        wm_canvas_line(canvas, cx+4, cy-4, cx-4, cy+4, xc);
    }

    if (window->hover_maximize)
        wm_canvas_fill_rounded(canvas, max_btn, r2,
            apply_opacity(state->theme.surface_hover, opacity));
    {
        uint32_t mc = apply_opacity(
            window->hover_maximize ? state->theme.text : state->theme.text_dim, opacity);
        int32_t mx = max_btn.x + (int32_t)BTN_SZ/2 - 4;
        int32_t my = max_btn.y + (int32_t)BTN_SZ/2 - 4;
        if (window->maximized) {
            wm_canvas_fill(canvas,(wm_rect_t){mx+2,my,6u,1u},mc);
            wm_canvas_fill(canvas,(wm_rect_t){mx+2,my+5,6u,1u},mc);
            wm_canvas_fill(canvas,(wm_rect_t){mx+2,my,1u,6u},mc);
            wm_canvas_fill(canvas,(wm_rect_t){mx+7,my,1u,6u},mc);
            wm_canvas_fill(canvas,(wm_rect_t){mx,my+2,6u,1u},mc);
            wm_canvas_fill(canvas,(wm_rect_t){mx,my+7,6u,1u},mc);
            wm_canvas_fill(canvas,(wm_rect_t){mx,my+2,1u,6u},mc);
            wm_canvas_fill(canvas,(wm_rect_t){mx+5,my+2,1u,6u},mc);
        } else {
            wm_canvas_fill(canvas,(wm_rect_t){mx,my,8u,1u},mc);
            wm_canvas_fill(canvas,(wm_rect_t){mx,my+7,8u,1u},mc);
            wm_canvas_fill(canvas,(wm_rect_t){mx,my,1u,8u},mc);
            wm_canvas_fill(canvas,(wm_rect_t){mx+7,my,1u,8u},mc);
        }
    }

    if (window->hover_minimize)
        wm_canvas_fill_rounded(canvas, min_btn, r2,
            apply_opacity(state->theme.surface_hover, opacity));
    {
        uint32_t mc = apply_opacity(
            window->hover_minimize ? state->theme.text : state->theme.text_dim, opacity);
        int32_t lx = min_btn.x + (int32_t)BTN_SZ/2 - 4;
        int32_t ly = min_btn.y + (int32_t)BTN_SZ/2 + 1;
        wm_canvas_fill(canvas, (wm_rect_t){lx, ly, 9u, 2u}, mc);
    }
}

static void draw_window_icon(wm_state_t *state, wm_canvas_t *canvas,
                              const wm_window_t *window, wm_rect_t frame,
                              uint8_t opacity)
{
    uint32_t isz = 18u;
    int32_t x = frame.x + 10;
    int32_t y = frame.y + (int32_t)((state->theme.title_height - isz) / 2u);
    if (window->has_icon)
        wm_canvas_blit_scaled(canvas, (wm_rect_t){x,y,isz,isz},
                              window->icon, 32u,32u, opacity, 4u);
    else
        wm_canvas_draw_icon(canvas, (wm_rect_t){x,y,isz,isz},
                            &state->assets.system_icons.window,
                            opacity, 3u, state->theme.text_dim);
}

void wm_decoration_draw_window(wm_state_t *state, wm_canvas_t *canvas,
                                wm_window_t *window)
{
    if (!state || !canvas || !window || !window->visible ||
        window->minimized || window->visual_alpha <= 0.0f) return;
    uint8_t opacity = (uint8_t)(window->visual_alpha * 255.0f);
    wm_rect_t frame = visual_frame(state, window);
    if (!frame.w || !frame.h) return;
    wm_rect_t visual = wm_window_visual_bounds(state, window);
    if (!wm_rect_intersects(visual, canvas->clip)) return;
    wm_rect_t visual_clip = wm_rect_intersection(visual, canvas->clip);

    if (window->is_system) {
        if (is_unscaled(window) && state->theme.corner_radius == 0u) {
            wm_canvas_blit(canvas, frame, window->surface,
                           window->frame.w, window->frame.h, 0u, 0u,
                           opacity, window->surface_opaque);
        } else {
            wm_canvas_blit_scaled(canvas, frame, window->surface,
                                  window->frame.w, window->frame.h,
                                  opacity, state->theme.corner_radius);
        }
        return;
    }

    uint32_t title_h = (uint32_t)((float)state->theme.title_height *
                                   window->visual_scale);
    if (title_h > frame.h) title_h = frame.h;
    wm_rect_t content = {
        frame.x+1,
        frame.y+(int32_t)title_h+1,
        frame.w>2u?frame.w-2u:frame.w,
        frame.h>title_h+2u?frame.h-title_h-2u:0u
    };
    if (is_unscaled(window) && opacity == 255u &&
        rect_contains_rect(content, visual_clip)) {
        wm_canvas_blit(canvas, content, window->surface,
                       window->frame.w, window->frame.h, 0u, 0u,
                       opacity, window->surface_opaque);
        return;
    }

    draw_shadow(state, canvas, window, frame, opacity);

    wm_canvas_fill_rounded(canvas, frame, state->theme.corner_radius,
                           apply_opacity(state->theme.border, opacity));
    wm_rect_t inner = {frame.x+1, frame.y+1,
                       frame.w>2u?frame.w-2u:frame.w,
                       frame.h>2u?frame.h-2u:frame.h};
    uint32_t surf_alpha = ((uint32_t)opacity * 0xCCu) / 255u;
    uint32_t surf_tint = (state->theme.surface & 0x00FFFFFFu) | (surf_alpha << 24u);
    wm_canvas_fill_rounded(canvas, inner,
                           state->theme.corner_radius>0u?state->theme.corner_radius-1u:0u,
                           surf_tint);
                            
    wm_rect_t title_rect = {frame.x+1, frame.y+1,
                             frame.w>2u?frame.w-2u:frame.w, title_h};
    uint32_t title_color = window->has_focus ?
        state->theme.title_active : state->theme.title_inactive;
    uint32_t top_r = state->theme.corner_radius > 0u ?
        state->theme.corner_radius - 1u : 0u;
    uint32_t title_alpha_val = ((uint32_t)opacity * 0xCCu) / 255u;
    uint32_t title_tint = (title_color & 0x00FFFFFFu) | (title_alpha_val << 24u);
    wm_canvas_fill_rounded(canvas, title_rect, top_r, title_tint);
    if (title_rect.h > top_r) {
        wm_canvas_fill(canvas,
            (wm_rect_t){title_rect.x, title_rect.y+(int32_t)top_r,
                        title_rect.w, title_rect.h-top_r},
            title_tint);
    }
    if (window->has_focus && title_rect.w > 2u) {
        wm_canvas_fill_rounded(canvas,
            (wm_rect_t){title_rect.x, title_rect.y, title_rect.w, 2u},
            1u, apply_opacity(state->theme.text_dim, opacity));
    }
    wm_canvas_fill(canvas,
        (wm_rect_t){frame.x+1, frame.y+(int32_t)title_h,
                    frame.w>2u?frame.w-2u:frame.w, 1u},
        apply_opacity(state->theme.border, opacity));

    if (content.h) {
        if (is_unscaled(window)) {
            wm_canvas_blit(canvas, content, window->surface,
                           window->frame.w, window->frame.h, 0u, 0u,
                           opacity, window->surface_opaque);
        } else {
            wm_canvas_blit_scaled(canvas, content, window->surface,
                                  window->frame.w, window->frame.h, opacity,
                                  state->theme.corner_radius>2u?
                                      state->theme.corner_radius-2u:0u);
        }
    }

    draw_window_icon(state, canvas, window, frame, opacity);

    if (window->title[0] && frame.w > BTN_SZ*3u + BTN_PAD*2u + BTN_MARGIN + 80u) {
        uint32_t buttons_width = BTN_SZ*3u + BTN_PAD*2u + BTN_MARGIN;
        int32_t text_x  = frame.x + 34;
        uint32_t text_max = frame.w > (buttons_width + 40u) ?
            frame.w - buttons_width - 40u : 0u;
        int32_t text_y = frame.y +
            (int32_t)((state->theme.title_height - (uint32_t)state->theme.font_title) / 2u) - 1;
        wm_font_draw(&state->font, canvas, text_x, text_y,
                     window->title,
                     apply_opacity(window->has_focus ?
                         state->theme.text : state->theme.text_dim, opacity),
                     state->theme.font_title, text_max);
    }

    if (frame.w >= BTN_SZ*3u + BTN_PAD*2u + BTN_MARGIN + 60u)
        draw_title_buttons(state, canvas, window, frame, opacity);
}
