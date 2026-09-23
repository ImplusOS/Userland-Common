#include "WM_Decoration.h"

#include "../Compositor/WM_Raster.h"
#include "../Font/WM_Font.h"
#include "../SceneGraph/WM_Node.h"
#include "../UI/WM_Icons.h"

#include <stdlib.h>
#include <string.h>

/*
 * The window frame, in Material Design 3 terms: an M3 "large" shape filled
 * with a surface container, outlined in outline-variant, carrying the key
 * light of elevation level 3, and topped by a title bar whose tone says
 * whether the window has focus. The controls on it are M3 icon buttons --
 * a 32px stadium of state layer under a Material Symbols glyph -- with the
 * close button reaching for the error roles the moment it is the one under
 * the pointer.
 */

/* Elevation of a window at rest. M3 does not raise a surface on focus, so
 * neither does this: focus is carried by the title bar's tone and the text
 * on it, and the shadow mask below stays valid across a focus change
 * instead of being rebuilt for every click. */
#define WM_DECOR_ELEVATION M3_ELEVATION_3

/* Opacity of the key light. The layered build-up below multiplies this
 * down per ring, so it is the ceiling rather than what any pixel gets. */
#define WM_DECOR_SHADOW_PERCENT 25u

/* Icon button metrics, and the glyph inside one. */
#define BTN_SZ   WM_TITLE_BUTTON_SIZE
#define BTN_GLYPH 16u
#define WM_DECOR_ICON_SIZE 16u

static uint32_t apply_opacity(uint32_t color, uint8_t opacity)
{
    uint32_t alpha = ((color >> 24u) * opacity) / 255u;
    return (color & 0x00FFFFFFu) | (alpha << 24u);
}

/* Width the row of controls occupies, margin included. */
static uint32_t title_controls_width(void)
{
    return (BTN_SZ + WM_TITLE_BUTTON_GAP) * WM_TITLE_BUTTON_COUNT +
           WM_TITLE_BUTTON_MARGIN;
}

bool wm_decoration_has_title_buttons(uint32_t frame_width)
{
    /* Only the window icon has to survive beside them: the title text has
     * its own, wider check, and a window that cannot show its title is
     * still one the user has to be able to close. */
    return frame_width >= title_controls_width() + WM_DECOR_ICON_SIZE + 16u;
}

wm_rect_t wm_decoration_title_button_rect(const wm_state_t *state,
                                          wm_rect_t frame,
                                          wm_title_button_t which)
{
    if (!state || which >= WM_TITLE_BUTTON_COUNT)
        return (wm_rect_t){0, 0, 0, 0};
    uint32_t step = BTN_SZ + WM_TITLE_BUTTON_GAP;
    int32_t right = frame.x + (int32_t)frame.w - (int32_t)WM_TITLE_BUTTON_MARGIN;
    int32_t x = right - (int32_t)BTN_SZ - (int32_t)((uint32_t)which * step);
    int32_t y = frame.y +
        ((int32_t)state->theme.title_height - (int32_t)BTN_SZ) / 2;
    return (wm_rect_t){x, y, BTN_SZ, BTN_SZ};
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

/* The same ramp wm_canvas_elevation_shadow() paints, baked into an 8-bit
 * mask so a window that is only moving does not repeat the work every
 * frame. The mask carries no vertical offset; draw_shadow() applies the
 * level's offset when it places it, which keeps the cache valid. */
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
        uint32_t ry = shadow_size - layer / 2u;
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
    uint32_t key_light = m3_alpha(state->theme.color.shadow,
                                     WM_DECOR_SHADOW_PERCENT);
    uint8_t theme_alpha = (uint8_t)(key_light >> 24u);
    int32_t offset_y = m3_shadow_offset_y(WM_DECOR_ELEVATION);
    if (!build_shadow_cache(window, frame.w, frame.h,
                             state->theme.corner_radius, size, theme_alpha)) {
        wm_canvas_elevation_shadow(canvas, frame, state->theme.corner_radius,
                                   WM_DECOR_ELEVATION, key_light, opacity);
        return;
    }
    uint32_t rgb = key_light & 0x00FFFFFFu;
    int32_t ox = frame.x - (int32_t)size;
    int32_t oy = frame.y - (int32_t)size + offset_y;
    for (uint32_t y = 0; y < window->shadow_mask_height; ++y)
        for (uint32_t x = 0; x < window->shadow_mask_width; ++x) {
            uint32_t ma = window->shadow_mask[y*window->shadow_mask_width+x];
            if (!ma) continue;
            uint32_t alpha = (ma*(uint32_t)opacity+127u)/255u;
            wm_canvas_put(canvas, ox+(int32_t)x, oy+(int32_t)y, rgb|(alpha<<24u));
        }
}

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

/* One M3 icon button: a full-shape state layer under a Material Symbols
 * glyph. `hover_container` is the tone the layer resolves to, already mixed
 * against whatever the button sits on. */
static void draw_icon_button(wm_canvas_t *canvas, wm_rect_t box,
                             wm_icon_kind_t glyph, bool hovered,
                             uint32_t hover_container, uint32_t glyph_color,
                             uint32_t hover_glyph_color, uint8_t opacity)
{
    if (hovered) {
        wm_canvas_fill_rounded(canvas, box,
            m3_shape_radius(M3_SHAPE_FULL, box.w, box.h),
            apply_opacity(hover_container, opacity));
    }
    wm_rect_t inner = {box.x + (int32_t)(box.w - BTN_GLYPH) / 2,
                       box.y + (int32_t)(box.h - BTN_GLYPH) / 2,
                       BTN_GLYPH, BTN_GLYPH};
    wm_icon_draw(canvas, inner, glyph,
                 apply_opacity(hovered ? hover_glyph_color : glyph_color,
                               opacity));
}

static void draw_title_buttons(wm_state_t *state, wm_canvas_t *canvas,
                                const wm_window_t *window, wm_rect_t frame,
                                uint32_t title_container, uint8_t opacity)
{
    const m3_scheme_t *m3 = &state->theme.color;
    uint32_t state_layer = m3_state_layer(title_container, m3->on_surface,
                                             M3_STATE_HOVER);

    draw_icon_button(canvas,
        wm_decoration_title_button_rect(state, frame, WM_TITLE_BUTTON_MINIMIZE),
        WM_ICON_MINIMIZE, window->hover_minimize,
        state_layer, m3->on_surface_variant, m3->on_surface, opacity);

    draw_icon_button(canvas,
        wm_decoration_title_button_rect(state, frame, WM_TITLE_BUTTON_MAXIMIZE),
        window->maximized ? WM_ICON_RESTORE : WM_ICON_MAXIMIZE,
        window->hover_maximize,
        state_layer, m3->on_surface_variant, m3->on_surface, opacity);

    /* M3 reserves the error roles for destructive intent, and closing a
     * window is the one thing on this bar that throws work away. */
    draw_icon_button(canvas,
        wm_decoration_title_button_rect(state, frame, WM_TITLE_BUTTON_CLOSE),
        WM_ICON_CLOSE, window->hover_close,
        m3->error_container, m3->on_surface_variant, m3->on_error_container,
        opacity);
}

static void draw_window_icon(wm_state_t *state, wm_canvas_t *canvas,
                              const wm_window_t *window, wm_rect_t frame,
                              uint8_t opacity)
{
    uint32_t isz = WM_DECOR_ICON_SIZE;
    int32_t x = frame.x + 9;
    int32_t y = frame.y + (int32_t)((state->theme.title_height - isz) / 2u);
    if (window->has_icon)
        wm_canvas_blit_scaled(canvas, (wm_rect_t){x,y,isz,isz},
                              window->icon, 32u,32u, opacity,
                              M3_SHAPE_EXTRA_SMALL);
    else
        wm_icon_draw(canvas, (wm_rect_t){x,y,isz,isz}, WM_ICON_DOC,
                     apply_opacity(state->theme.color.on_surface_variant,
                                   opacity));
}

void wm_decoration_draw_window(wm_state_t *state, wm_canvas_t *canvas,
                                wm_window_t *window)
{
    if (!state || !canvas || !window || !window->visible ||
        window->minimized || window->visual_alpha <= 0.0f) return;
    const m3_scheme_t *m3 = &state->theme.color;
    uint8_t opacity = (uint8_t)(window->visual_alpha * 255.0f);
    wm_rect_t frame = visual_frame(state, window);
    if (!frame.w || !frame.h) return;
    wm_rect_t visual = wm_window_visual_bounds(state, window);
    if (!wm_rect_intersects(visual, canvas->clip)) return;
    wm_rect_t visual_clip = wm_rect_intersection(visual, canvas->clip);

    /* Chrome, not an application: no frame, and no rounded corners either
     * -- the shape scale is for windows, and a session surface that runs to
     * the edges of the work area has no corners to soften. */
    if (window->is_system) {
        if (is_unscaled(window)) {
            wm_canvas_blit(canvas, frame, window->surface,
                           window->frame.w, window->frame.h, 0u, 0u,
                           opacity, window->surface_opaque);
        } else {
            wm_canvas_blit_scaled(canvas, frame, window->surface,
                                  window->frame.w, window->frame.h,
                                  opacity, 0u);
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

    uint32_t radius = m3_shape_radius(state->theme.corner_radius,
                                         frame.w, frame.h);

    /* Outline first, then the surface inset by a pixel inside it: the ring
     * M3 draws around a container, without a second pass over the body. */
    wm_canvas_fill_rounded(canvas, frame, radius,
                           apply_opacity(m3->outline_variant, opacity));
    wm_rect_t inner = {frame.x+1, frame.y+1,
                       frame.w>2u?frame.w-2u:frame.w,
                       frame.h>2u?frame.h-2u:frame.h};
    wm_canvas_fill_rounded(canvas, inner, radius > 0u ? radius - 1u : 0u,
                           apply_opacity(m3->surface_container_lowest, opacity));

    /* The title bar carries focus. An active window's bar is a container
     * lifted by the same elevation as its shadow; an inactive one sits a
     * step lower, which is the whole difference M3 asks for. */
    uint32_t title_container = window->has_focus
        ? m3_elevated(m3, m3->surface_container, M3_ELEVATION_2)
        : m3->surface_container_low;
    wm_rect_t title_rect = {frame.x+1, frame.y+1,
                             frame.w>2u?frame.w-2u:frame.w, title_h};
    uint32_t top_r = radius > 0u ? radius - 1u : 0u;
    wm_canvas_fill_shape(canvas, title_rect, top_r, top_r, 0u, 0u,
                         apply_opacity(title_container, opacity));
    wm_canvas_fill(canvas,
        (wm_rect_t){frame.x+1, frame.y+(int32_t)title_h,
                    frame.w>2u?frame.w-2u:frame.w, 1u},
        apply_opacity(m3->outline_variant, opacity));

    if (content.h) {
        if (is_unscaled(window)) {
            wm_canvas_blit(canvas, content, window->surface,
                           window->frame.w, window->frame.h, 0u, 0u,
                           opacity, window->surface_opaque);
        } else {
            wm_canvas_blit_scaled(canvas, content, window->surface,
                                  window->frame.w, window->frame.h, opacity,
                                  radius > 2u ? radius - 2u : 0u);
        }
    }

    draw_window_icon(state, canvas, window, frame, opacity);

    /* `reserved` is everything that is not the title: the icon and its
     * margins on the left, the controls on the right. Below about forty
     * pixels of what is left there is no room for a word, and
     * wm_font_draw() reads a zero limit as "no limit" -- so the title is
     * dropped rather than run under the buttons. */
    uint32_t reserved = title_controls_width() + WM_DECOR_ICON_SIZE + 26u;
    uint32_t text_max = frame.w > reserved + 40u ? frame.w - reserved : 0u;
    if (window->title[0] && text_max != 0u) {
        int32_t text_x = frame.x + 9 + (int32_t)WM_DECOR_ICON_SIZE + 8;
        float size = state->theme.type.title_small;
        int32_t text_y = frame.y +
            ((int32_t)state->theme.title_height - (int32_t)size) / 2 - 1;
        wm_font_draw(&state->font, canvas, text_x, text_y,
                     window->title,
                     apply_opacity(window->has_focus ?
                         m3->on_surface : m3->on_surface_variant, opacity),
                     size, text_max);
    }

    if (wm_decoration_has_title_buttons(frame.w))
        draw_title_buttons(state, canvas, window, frame, title_container, opacity);
}
