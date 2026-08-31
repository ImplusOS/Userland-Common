#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "API/Process.h"
#include "API/Serial.h"
#include "API/Memory.h"
#include "API/Graphics.h"
#include "API/File.h"
#include "API/Window.h"
#include "API/Input.h"
#include "API/SystemInfo.h"
#include "Service/service_client.h"
#include "Unicode/UTF8/UTF8.h"
#include "Crypto/Crypto.h"
#include <Identifier/Identifier.h>


#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_malloc(x,u)  ((void)(u),malloc(x))
#define STBTT_free(x,u)    ((void)(u),free(x))
#define STBTT_fmod(x,y)    fmod(x,y)
#include "Header/stb_truetype.h"

static void* ul_realloc_sized(void* p, size_t oldsz, size_t newsz) {
    if (newsz == 0) { if (p) free(p); return NULL; }
    void* q = malloc(newsz);
    if (q && p) { memcpy(q, p, oldsz < newsz ? oldsz : newsz); free(p); }
    return q;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wextra"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_PNG
#define STBI_MALLOC(sz)              malloc(sz)
#define STBI_REALLOC(p,newsz)        ul_realloc_sized(p, 0, newsz)
#define STBI_REALLOC_SIZED(p,os,ns)  ul_realloc_sized(p, os, ns)
#define STBI_FREE(p)                 free(p)
#include "Header/stb_image.h"
#pragma GCC diagnostic pop

static uint32_t *g_fb_snapshot = NULL;
static uint32_t g_fb_snapshot_pixels = 0;
static uint32_t *g_bg_cache = NULL;
static int g_bg_cache_width = 0;
static int g_bg_cache_height = 0;

static void serial_write_i32(int32_t value)
{
    if (value < 0) {
        serial_write_string("-");
        serial_write_uint32((uint32_t)(-value));
        return;
    }
    serial_write_uint32((uint32_t)value);
}

static int32_t spawn_with_fallbacks(const char *const *paths, uint32_t path_count) {
    if (!paths || path_count == 0) return -1;
    for (uint32_t i = 0; i < path_count; ++i) {
        const char *path = paths[i];
        if (path && path[0] != '\0') {
            int32_t pid = process_spawn(path);
            if (pid >= 0) return pid;
        }
    }
    return -1;
}

static uint8_t *g_font_buffer = NULL;
static stbtt_fontinfo g_font;
static int g_font_loaded = 0;

static int load_font(const char *path) {
    file_stat_t st;
    if (file_stat(path, &st) < 0 || !st.exists || st.is_dir ||
        st.size == 0 || st.size > 64u * 1024u * 1024u) {
        return -1;
    }

    int32_t fd = file_open(path, 0);
    if (fd < 0) return -1;

    uint8_t *font_buffer = (uint8_t *)malloc((size_t)st.size);
    if (!font_buffer) {
        file_close(fd);
        return -1;
    }

    uint32_t total = 0;
    while (total < st.size) {
        int64_t n = file_read(fd, font_buffer + total, st.size - total);
        if (n <= 0) {
            file_close(fd);
            free(font_buffer);
            return -1;
        }
        total += (uint32_t)n;
    }

    file_close(fd);

    int offset = stbtt_GetFontOffsetForIndex(font_buffer, 0);
    stbtt_fontinfo font;
    if (offset < 0 || !stbtt_InitFont(&font, font_buffer, offset)) {
        free(font_buffer);
        return -1;
    }

    free(g_font_buffer);
    g_font_buffer = font_buffer;
    g_font = font;
    g_font_loaded = 1;
    return 0;
}

static int load_default_font(void) {
    static const char *const paths[] = {
        "/Userland/com.ImplusOS.windowmanager/Resource/Fonts/NotoSansJP-Regular.ttf",
        "/BootManager/Resource/Fonts/NotoSansJP-Regular.ttf",
    };

    for (uint32_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        if (load_font(paths[i]) == 0) {
            return 0;
        }
    }

    return -1;
}

#define WP_PATH "/Userland/com.ImplusOS.windowmanager/Resource/Background.png"

static void draw_background(void) {
    uint32_t *fb = (uint32_t *)sys_get_display_framebuffer();
    int width  = (int)get_display_width();
    int height = (int)get_display_height();
    if (!fb || width <= 0 || height <= 0) return;

    if (g_bg_cache && g_bg_cache_width == width && g_bg_cache_height == height) {
        memcpy(fb, g_bg_cache, (size_t)width * (size_t)height * sizeof(uint32_t));
        return;
    }

    for (int y = 0; y < height; ++y) {
        uint32_t *row = &fb[y * width];
        float ty = (float)y / (float)height;
        for (int x = 0; x < width; ++x) {
            float tx = (float)x / (float)width;
            float t  = (tx + ty) * 0.5f;
            uint8_t r = (uint8_t)(20.0f + (6.0f  - 20.0f) * t);
            uint8_t g = (uint8_t)(16.0f + (24.0f - 16.0f) * t);
            uint8_t b = (uint8_t)(44.0f + (36.0f - 44.0f) * t);
            float dx = tx - 0.5f, dy = ty - 0.5f;
            float glow = 1.0f - (dx*dx + dy*dy) * 2.0f;
            if (glow > 0.0f) {
                r = (uint8_t)(r + (30.0f  - r) * glow * 0.4f);
                g = (uint8_t)(g + (80.0f  - g) * glow * 0.4f);
                b = (uint8_t)(b + (180.0f - b) * glow * 0.4f);
            }
            row[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }

    bool loaded_img = false;
    int32_t fd = file_open(WP_PATH, 0);
    if (fd >= 0) {
        file_stat_t st;
        if (file_stat(WP_PATH, &st) >= 0 && st.size > 0) {
            uint8_t *file_buf = (uint8_t *)malloc((size_t)st.size);
            if (file_buf) {
                int64_t total = 0;
                while (total < st.size) {
                    int64_t n = file_read(fd, file_buf + total, st.size - total);
                    if (n <= 0) break;
                    total += n;
                }
                if (total == st.size) {
                    int iw = 0, ih = 0, ic = 0;
                    uint8_t *img = stbi_load_from_memory(file_buf, (int)total, &iw, &ih, &ic, 4);
                    if (img && iw > 0 && ih > 0) {
                        uint32_t snum, sden;
                        if ((uint64_t)width * ih > (uint64_t)height * iw) {
                            snum = (uint32_t)width;  sden = (uint32_t)iw;
                        } else {
                            snum = (uint32_t)height; sden = (uint32_t)ih;
                        }
                        uint32_t sw = (uint32_t)iw * snum / sden;
                        uint32_t sh = (uint32_t)ih * snum / sden;
                        int32_t ox = (int32_t)((sw > (uint32_t)width)  ? (sw - (uint32_t)width)  / 2 : 0);
                        int32_t oy = (int32_t)((sh > (uint32_t)height) ? (sh - (uint32_t)height) / 2 : 0);

                        for (int y = 0; y < height; ++y) {
                            uint32_t *row = &fb[y * width];
                            uint32_t sy = (uint32_t)(((int32_t)y + oy) * (int32_t)sden / (int32_t)snum);
                            if (sy >= (uint32_t)ih) sy = (uint32_t)ih - 1;
                            const uint8_t *src_row = img + sy * (uint32_t)iw * 4;
                            for (int x = 0; x < width; ++x) {
                                uint32_t sx = (uint32_t)(((int32_t)x + ox) * (int32_t)sden / (int32_t)snum);
                                if (sx >= (uint32_t)iw) sx = (uint32_t)iw - 1;
                                const uint8_t *p = src_row + sx * 4;
                                uint8_t r = (uint8_t)((uint32_t)p[0] * 45u / 100u);
                                uint8_t g = (uint8_t)((uint32_t)p[1] * 45u / 100u);
                                uint8_t b = (uint8_t)((uint32_t)p[2] * 45u / 100u);
                                row[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                            }
                        }
                        STBI_FREE(img);
                        loaded_img = true;
                    }
                }
                free(file_buf);
            }
        }
        file_close(fd);
    }

    if (loaded_img) {
        uint32_t *tmp = (uint32_t *)malloc((size_t)width * sizeof(uint32_t));
        if (tmp) {
            int rad = 6;
            for (int y = 0; y < height; ++y) {
                uint32_t *row = &fb[y * width];
                uint32_t sr = 0, sg = 0, sb2 = 0, cnt = 0;
                int rs = 0, re = rad < (width - 1) ? rad : (width - 1);
                for (int i = rs; i <= re; ++i) {
                    sr += (row[i]>>16)&0xFF; sg += (row[i]>>8)&0xFF; sb2 += row[i]&0xFF; cnt++;
                }
                for (int x = 0; x < width; ++x) {
                    tmp[x] = 0xFF000000u|((sr/cnt)<<16)|((sg/cnt)<<8)|(sb2/cnt);
                    int add = x + 1 + rad, sub = x - rad;
                    if (add < width)  { sr += (row[add]>>16)&0xFF; sg += (row[add]>>8)&0xFF; sb2 += row[add]&0xFF; cnt++; }
                    if (sub >= 0)     { sr -= (row[sub]>>16)&0xFF; sg -= (row[sub]>>8)&0xFF; sb2 -= row[sub]&0xFF; cnt--; }
                }
                memcpy(row, tmp, (size_t)width * sizeof(uint32_t));
            }
            for (int x = 0; x < width; ++x) {
                uint32_t sr2 = 0, sg2 = 0, sb3 = 0, cnt2 = 0;
                int rs = 0, re = rad < (height - 1) ? rad : (height - 1);
                for (int i = rs; i <= re; ++i) {
                    uint32_t c = fb[i * width + x];
                    sr2 += (c>>16)&0xFF; sg2 += (c>>8)&0xFF; sb3 += c&0xFF; cnt2++;
                }
                for (int y = 0; y < height; ++y) {
                    tmp[y] = 0xFF000000u|((sr2/cnt2)<<16)|((sg2/cnt2)<<8)|(sb3/cnt2);
                    int add = y + 1 + rad, sub = y - rad;
                    if (add < height) { uint32_t c = fb[add*width+x]; sr2+=(c>>16)&0xFF; sg2+=(c>>8)&0xFF; sb3+=c&0xFF; cnt2++; }
                    if (sub >= 0)     { uint32_t c = fb[sub*width+x]; sr2-=(c>>16)&0xFF; sg2-=(c>>8)&0xFF; sb3-=c&0xFF; cnt2--; }
                }
                for (int y = 0; y < height; ++y) fb[y * width + x] = tmp[y];
            }
            free(tmp);
        }
    }

    if (g_bg_cache) {
        free(g_bg_cache);
    }
    g_bg_cache = (uint32_t *)malloc((size_t)width * (size_t)height * sizeof(uint32_t));
    if (g_bg_cache) {
        g_bg_cache_width = width;
        g_bg_cache_height = height;
        memcpy(g_bg_cache, fb, (size_t)width * (size_t)height * sizeof(uint32_t));
    }
}

static void draw_gradient_background(uint32_t top_color, uint32_t bottom_color) {
    (void)top_color;
    (void)bottom_color;
    draw_background();
}

static void draw_translucent_card(uint32_t *fb, int width, int height, int cx, int cy, int cw, int ch, uint32_t bg_color, uint8_t bg_alpha, uint32_t border_color, int radius) {
    uint8_t bg_r = (bg_color >> 16) & 0xFF, bg_g = (bg_color >> 8) & 0xFF, bg_b = bg_color & 0xFF;
    uint8_t b_r = (border_color >> 16) & 0xFF, b_g = (border_color >> 8) & 0xFF, b_b = border_color & 0xFF;
    uint8_t b_a = (border_color >> 24) & 0xFF;
    if (b_a == 0) b_a = 255;

    int rad_sq = radius * radius;
    int inner_rad_sq = (radius - 1) * (radius - 1);

    for (int y = cy; y < cy + ch; ++y) {
        if (y < 0 || y >= height) continue;
        uint32_t *row = &fb[y * width];
        int dy = y - cy, br_y = cy + ch - 1 - y;
        for (int x = cx; x < cx + cw; ++x) {
            if (x < 0 || x >= width) continue;
            int dx = x - cx, br_x = cx + cw - 1 - x;
            bool is_corner = false;
            if (dx < radius && dy < radius) {
                int rx = radius - 1 - dx;
                int ry = radius - 1 - dy;
                int dist_sq = rx * rx + ry * ry;
                if (dist_sq > rad_sq) continue;
                is_corner = (dist_sq > inner_rad_sq);
            } else if (br_x < radius && dy < radius) {
                int rx = radius - 1 - br_x;
                int ry = radius - 1 - dy;
                int dist_sq = rx * rx + ry * ry;
                if (dist_sq > rad_sq) continue;
                is_corner = (dist_sq > inner_rad_sq);
            } else if (dx < radius && br_y < radius) {
                int rx = radius - 1 - dx;
                int ry = radius - 1 - br_y;
                int dist_sq = rx * rx + ry * ry;
                if (dist_sq > rad_sq) continue;
                is_corner = (dist_sq > inner_rad_sq);
            } else if (br_x < radius && br_y < radius) {
                int rx = radius - 1 - br_x;
                int ry = radius - 1 - br_y;
                int dist_sq = rx * rx + ry * ry;
                if (dist_sq > rad_sq) continue;
                is_corner = (dist_sq > inner_rad_sq);
            }

            bool is_edge = (dx == 0 || dy == 0 || br_x == 0 || br_y == 0);
            bool border = is_corner || (is_edge && dx >= radius && br_x >= radius && dy >= radius && br_y >= radius);
            uint32_t src = row[x];
            uint8_t src_r = (src >> 16) & 0xFF, src_g = (src >> 8) & 0xFF, src_b = src & 0xFF;

            if (border) {
                uint8_t r = (uint8_t)((b_r * b_a + src_r * (255 - b_a)) >> 8);
                uint8_t g = (uint8_t)((b_g * b_a + src_g * (255 - b_a)) >> 8);
                uint8_t b = (uint8_t)((b_b * b_a + src_b * (255 - b_a)) >> 8);
                row[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            } else {
                uint8_t r = (uint8_t)((bg_r * bg_alpha + src_r * (255 - bg_alpha)) >> 8);
                uint8_t g = (uint8_t)((bg_g * bg_alpha + src_g * (255 - bg_alpha)) >> 8);
                uint8_t b = (uint8_t)((bg_b * bg_alpha + src_b * (255 - bg_alpha)) >> 8);
                row[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }
        }
    }
}

static void draw_avatar_icon(uint32_t *fb, int width, int height, int cx, int cy) {
    int r = 30;
    int r_sq = r * r;
    int edge_r_sq = (r - 1) * (r - 1);
    for (int y = cy - r; y <= cy + r; ++y) {
        if (y < 0 || y >= height) continue;
        uint32_t *row = &fb[y * width];
        int dy = y - cy;
        int dy_sq = dy * dy;
        for (int x = cx - r; x <= cx + r; ++x) {
            if (x < 0 || x >= width) continue;
            int dx = x - cx;
            int dist_sq = dx * dx + dy_sq;
            if (dist_sq <= r_sq) {
                uint8_t cr = 0x47, cg = 0x55, cb = 0x69;
                int hdx = dx, hdy = dy + 6;
                int bdx = dx, bdy = dy - 22;
                if (hdx * hdx + hdy * hdy <= 9 * 9) {
                    cr = 0xE2; cg = 0xE8; cb = 0xF0;
                } else if ((bdx * bdx * 3 + bdy * bdy * 6) <= 800 && dy > 0) {
                    cr = 0xE2; cg = 0xE8; cb = 0xF0;
                }
                if (dist_sq >= edge_r_sq) {
                    uint32_t src = row[x];
                    cr = (uint8_t)((cr + ((src >> 16) & 0xFF)) >> 1);
                    cg = (uint8_t)((cg + ((src >> 8) & 0xFF)) >> 1);
                    cb = (uint8_t)((cb + (src & 0xFF)) >> 1);
                }
                row[x] = ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | cb;
            }
        }
    }
}

static void draw_password_dots(uint32_t *fb, int width, int height, int start_x, int cy, int count) {
    int r = 4, gap = 12;
    for (int i = 0; i < count; ++i) {
        int cx = start_x + i * gap;
        for (int y = cy - r; y <= cy + r; ++y) {
            if (y < 0 || y >= height) continue;
            uint32_t *row = &fb[y * width];
            int dy = y - cy;
            for (int x = cx - r; x <= cx + r; ++x) {
                if (x < 0 || x >= width) continue;
                int dx = x - cx;
                if (dx * dx + dy * dy <= r * r) row[x] = 0xFFFFFFFF;
            }
        }
    }
}

static inline uint32_t blend(uint32_t src, uint32_t dst, uint8_t alpha) {
    uint32_t sr = (src >> 16) & 0xFF;
    uint32_t sg = (src >> 8) & 0xFF;
    uint32_t sb = src & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >> 8) & 0xFF;
    uint32_t db = dst & 0xFF;
    uint32_t r = (sr * alpha + dr * (255 - alpha)) / 255;
    uint32_t g = (sg * alpha + dg * (255 - alpha)) / 255;
    uint32_t b = (sb * alpha + db * (255 - alpha)) / 255;
    return (r << 16) | (g << 8) | b;
}

static void draw_char(int x, int y, utf8_codepoint_t cp,
                      float scale, uint32_t color) {
    if (!g_font_loaded) return;

    int c_x1, c_y1, c_x2, c_y2;

    stbtt_GetCodepointBitmapBox(
        &g_font,
        cp,
        scale,
        scale,
        &c_x1,
        &c_y1,
        &c_x2,
        &c_y2
    );

    int width  = c_x2 - c_x1;
    int height = c_y2 - c_y1;

    if (width <= 0 || height <= 0)
        return;

    uint8_t *bitmap = malloc((size_t)(width * height));
    if (!bitmap)
        return;

    stbtt_MakeCodepointBitmap(
        &g_font,
        bitmap,
        width,
        height,
        width,
        scale,
        scale,
        cp
    );

    uint32_t *fb = (uint32_t*)sys_get_display_framebuffer();
    if (!fb) {
        free(bitmap);
        return;
    }
    int screen_w = (int)get_display_width();
    int screen_h = (int)get_display_height();
    if (screen_w <= 0 || screen_h <= 0) {
        free(bitmap);
        return;
    }

    for (int py = 0; py < height; ++py) {
        for (int px = 0; px < width; ++px) {

            uint8_t alpha = bitmap[py * width + px];
            if (!alpha) continue;

            int sx = x + c_x1 + px;
            int sy = y + c_y1 + py;

            if (sx < 0 || sy < 0 ||
                sx >= screen_w || sy >= screen_h)
                continue;

            uint32_t dst = fb[sy * screen_w + sx];

            fb[sy * screen_w + sx] =
                blend(color, dst, alpha);
        }
    }

    free(bitmap);
}

static int get_text_width(const char *text, float scale) {
    const char *p = text, *end = text + strlen(text);
    int width = 0, has_prev = 0;
    utf8_codepoint_t prev = 0;
    while (p < end) {
        utf8_codepoint_t cp;
        if (utf8_next(&p, end, &cp) != 0) continue;
        if (has_prev) {
            width += (int)(stbtt_GetCodepointKernAdvance(&g_font, prev, cp) * scale);
        }
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&g_font, cp, &advance, &lsb);
        width += (int)(advance * scale);
        prev = cp;
        has_prev = 1;
    }
    return width;
}

static void draw_text(int x, int y, const char *text, float scale, uint32_t color) {
    if (!g_font_loaded || !text) return;

    const char *p = text, *end = text + strlen(text);
    int pen_x = x, has_prev = 0;
    utf8_codepoint_t prev = 0;

    while (p < end) {
        utf8_codepoint_t cp;

        if (utf8_next(&p, end, &cp) != 0)
            continue;

        if (cp == ' ') {
            int advance, lsb;
            stbtt_GetCodepointHMetrics(&g_font, ' ', &advance, &lsb);
            pen_x += (int)(advance * scale);
            prev = 0;
            has_prev = 0;
            continue;
        }

        if (has_prev) {
            pen_x += (int)(stbtt_GetCodepointKernAdvance(&g_font, prev, cp) * scale);
        }

        draw_char(pen_x, y, cp, scale, color);

        int advance, lsb;
        stbtt_GetCodepointHMetrics(&g_font, cp, &advance, &lsb);
        pen_x += (int)(advance * scale);

        prev = cp;
        has_prev = 1;
    }
}

static void draw_text_centered(const char *text, int ypos, float pixel_height, uint32_t color) {
    if (!g_font_loaded || !text) return;
    float scale = stbtt_ScaleForPixelHeight(&g_font, pixel_height);
    int width = get_text_width(text, scale);
    draw_text((get_display_width() - width) / 2, (get_display_height() / 2) + ypos, text, scale, color);
}

static void draw_login_screen(const char *title, const char *prompt, const char *value, bool hidden, const char *status) {
    uint32_t *fb = (uint32_t *)sys_get_display_framebuffer();
    if (!fb) return;
    int width = (int)get_display_width(), height = (int)get_display_height();
    draw_background();

    int card_w = 460, card_h = 320;
    int card_x = (width - card_w) / 2, card_y = (height - card_h) / 2;
    draw_translucent_card(fb, width, height, card_x, card_y, card_w, card_h, 0x0F172A, 220, 0x25FFFFFF, 12);
    draw_avatar_icon(fb, width, height, card_x + card_w / 2, card_y + 55);

    if (g_font_loaded) {
        float title_scale = stbtt_ScaleForPixelHeight(&g_font, 22.0f);
        float prompt_scale = stbtt_ScaleForPixelHeight(&g_font, 14.0f);
        float text_scale = stbtt_ScaleForPixelHeight(&g_font, 16.0f);
        float hint_scale = stbtt_ScaleForPixelHeight(&g_font, 11.0f);

        int title_w = get_text_width(title, title_scale);
        draw_text(card_x + (card_w - title_w) / 2, card_y + 110, title, title_scale, 0xFFFFFF);

        int prompt_w = get_text_width(prompt, prompt_scale);
        draw_text(card_x + (card_w - prompt_w) / 2, card_y + 148, prompt, prompt_scale, 0x94A3B8);

        int input_w = 340, input_h = 38;
        int input_x = card_x + (card_w - input_w) / 2, input_y = card_y + 172;
        draw_translucent_card(fb, width, height, input_x, input_y, input_w, input_h, 0x090D16, 200, 0xFF3B82F6, 6);

        if (hidden) {
            draw_password_dots(fb, width, height, input_x + 14, input_y + input_h / 2, (int)strlen(value));
        } else {
            draw_text(input_x + 14, input_y + (input_h - 16) / 2, value, text_scale, 0xFFFFFF);
            int val_w = get_text_width(value, text_scale);
            int cursor_x = input_x + 14 + val_w;
            if (cursor_x < input_x + input_w - 14) {
                draw_fill_rect((uint32_t)cursor_x, (uint32_t)(input_y + 10), 2, 18, 0xFFFFFFFF);
            }
        }

        if (status && status[0]) {
            int status_w = get_text_width(status, prompt_scale);
            draw_text(card_x + (card_w - status_w) / 2, card_y + 225, status, prompt_scale, 0xEF4444);
        }

        int btn_w = 140, btn_h = 32;
        int btn_x = card_x + (card_w - btn_w) / 2, btn_y = card_y + 258;
        draw_translucent_card(fb, width, height, btn_x, btn_y, btn_w, btn_h, 0x0078D4, 255, 0x0078D4, 6);
        int btn_text_w = get_text_width("確定 [Enter]", hint_scale);
        draw_text(btn_x + (btn_w - btn_text_w) / 2, btn_y + (btn_h - 11) / 2, "確定 [Enter]", hint_scale, 0xFFFFFF);
    }
    draw_present();
}

#define USER_DB_FILE "/Userland/users.db"
#define USER_DB_MAX_BYTES (1024u * 1024u)
#define KEY_STATE_WORDS (65536u / 64u)

static bool key_state_test(const uint64_t key_state[KEY_STATE_WORDS],
                           uint16_t keycode) {
    return (key_state[keycode >> 6] & (1ULL << (keycode & 63u))) != 0u;
}

static void key_state_set(uint64_t key_state[KEY_STATE_WORDS],
                          uint16_t keycode,
                          bool pressed) {
    uint64_t mask = 1ULL << (keycode & 63u);
    if (pressed) {
        key_state[keycode >> 6] |= mask;
    } else {
        key_state[keycode >> 6] &= ~mask;
    }
}

static int prompt_user_input(const char *title, const char *prompt,
                             char *out, size_t out_size, bool hidden) {
    size_t pos = 0;
    out[0] = '\0';

    draw_login_screen(title, prompt, out, hidden, "");

    uint64_t key_down[KEY_STATE_WORDS];
    memset(key_down, 0, sizeof(key_down));

    while (1) {
        input_keyboard_event_t ev;

        if (input_read_keyboard(&ev) < 0) {
            sleep_ms(10u);
            continue;
        }

        if (!ev.pressed) {
            key_state_set(key_down, ev.keycode, false);
            continue;
        }

        if (key_state_test(key_down, ev.keycode)) {
            continue;
        }

        key_state_set(key_down, ev.keycode, true);

        if (ev.ascii == '\r' || ev.ascii == '\n') {
            return 1;
        }

        if (ev.ascii == 8 || ev.ascii == 127) {
            if (pos > 0) {
                out[--pos] = '\0';
                draw_login_screen(title, prompt, out, hidden, "");
            }
            continue;
        }

        if (ev.ascii >= 32 &&
            ev.ascii < 127 &&
            pos + 1 < out_size) {

            out[pos++] = (char)ev.ascii;
            out[pos] = '\0';

            draw_login_screen(title, prompt, out, hidden, "");
        }
    }
}

static bool user_db_exists(void) {
    file_stat_t stat;
    return (file_stat(USER_DB_FILE, &stat) >= 0) && (stat.exists != 0);
}

static bool parse_user_record(const char *line, char *username, size_t username_size, char *salt, size_t salt_size, char *hash, size_t hash_size) {
    const char *first_colon = strchr(line, ':');
    if (!first_colon) return false;
    const char *second_colon = strchr(first_colon + 1, ':');
    if (!second_colon) return false;

    size_t name_len = (size_t)(first_colon - line);
    size_t salt_len = (size_t)(second_colon - first_colon - 1);
    size_t hash_len = strlen(second_colon + 1);
    while (hash_len > 0 &&
           (second_colon[hash_len] == '\n' ||
            second_colon[hash_len] == '\r')) {
        hash_len -= 1;
    }
    if (name_len >= username_size || salt_len >= salt_size || hash_len >= hash_size) return false;

    memcpy(username, line, name_len);
    username[name_len] = '\0';
    memcpy(salt, first_colon + 1, salt_len);
    salt[salt_len] = '\0';
    memcpy(hash, second_colon + 1, hash_len);
    hash[hash_len] = '\0';
    return true;
}

static char *user_db_read_all(size_t *size_out) {
    file_stat_t stat;
    if (size_out) *size_out = 0;
    if (file_stat(USER_DB_FILE, &stat) < 0 || !stat.exists || stat.is_dir ||
        stat.size == 0u || stat.size > USER_DB_MAX_BYTES) {
        return NULL;
    }

    int32_t fd = file_open(USER_DB_FILE, 0);
    if (fd < 0) return NULL;

    char *buffer = (char *)malloc((size_t)stat.size + 1u);
    if (!buffer) {
        file_close(fd);
        return NULL;
    }

    size_t total = 0;
    while (total < stat.size) {
        int64_t n = file_read(fd, buffer + total, (uint64_t)stat.size - total);
        if (n <= 0) {
            free(buffer);
            file_close(fd);
            return NULL;
        }
        total += (size_t)n;
    }
    file_close(fd);
    buffer[total] = '\0';
    if (size_out) *size_out = total;
    return buffer;
}

static bool user_db_lookup(const char *username, char *salt, size_t salt_size, char *hash, size_t hash_size) {
    char *buffer = user_db_read_all(NULL);
    if (!buffer) return false;

    char *line = buffer;
    while (*line) {
        char *newline = strchr(line, '\n');
        if (newline) *newline = '\0';

        char record_user[33], record_salt[65], record_hash[65];
        if (parse_user_record(line, record_user, sizeof(record_user), record_salt, sizeof(record_salt), record_hash, sizeof(record_hash))) {
            if (strcmp(record_user, username) == 0) {
                os_strcpy_s(salt, salt_size, record_salt);
                os_strcpy_s(hash, hash_size, record_hash);
                free(buffer);
                return true;
            }
        }
        if (!newline) break;
        line = newline + 1;
    }
    free(buffer);
    return false;
}

static bool user_db_add(const char *username, const char *salt, const char *hash) {
    char existing_salt[65], existing_hash[65];
    if (user_db_lookup(username,
                       existing_salt,
                       sizeof(existing_salt),
                       existing_hash,
                       sizeof(existing_hash))) {
        return false;
    }

    int32_t fd = file_open(USER_DB_FILE, 1);
    if (fd < 0) {
        fd = file_creat(USER_DB_FILE);
        if (fd < 0) return false;
    }
    file_seek(fd, 0, 2);
    char line[128];
    int len = snprintf(line, sizeof(line), "%s:%s:%s\n", username, salt, hash);
    if (len <= 0 || (size_t)len >= sizeof(line) || file_write(fd, line, (uint64_t)len) != len) {
        file_close(fd);
        return false;
    }
    file_close(fd);
    return true;
}

static bool user_db_upgrade_hash(const char *username, const char *new_hash) {
    size_t buffer_size = 0;
    char *buffer = user_db_read_all(&buffer_size);
    if (!buffer || strlen(new_hash) != 64u) {
        free(buffer);
        return false;
    }

    char *line = buffer;
    while ((size_t)(line - buffer) < buffer_size && *line) {
        char *newline = strchr(line, '\n');
        if (newline) *newline = '\0';

        char record_user[33], record_salt[65], record_hash[65];
        if (parse_user_record(line,
                              record_user,
                              sizeof(record_user),
                              record_salt,
                              sizeof(record_salt),
                              record_hash,
                              sizeof(record_hash)) &&
            strcmp(record_user, username) == 0 &&
            strlen(record_hash) == 64u) {
            char *first_colon = strchr(line, ':');
            char *second_colon = first_colon ? strchr(first_colon + 1, ':') : NULL;
            if (!second_colon) break;

            int32_t fd = file_open(USER_DB_FILE, 1);
            if (fd < 0) break;
            int64_t offset = (int64_t)((second_colon + 1) - buffer);
            bool ok = file_seek(fd, offset, 0) == offset &&
                      file_write(fd, new_hash, 64u) == 64;
            file_close(fd);
            free(buffer);
            return ok;
        }

        if (!newline) break;
        line = newline + 1;
    }

    free(buffer);
    return false;
}

static bool secure_hash_equal(const char *left, const char *right) {
    uint8_t difference = 0u;
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    if (left_len != right_len) return false;
    for (size_t i = 0; i < left_len; ++i) {
        difference |= (uint8_t)(left[i] ^ right[i]);
    }
    return difference == 0u;
}

static void login_failure_backoff(uint32_t failure_count,
                                  const char *username,
                                  const char *message) {
    uint32_t exponent = failure_count < 5u ? failure_count : 5u;
    uint32_t delay_seconds = 1u << exponent;
    char status[128];
    snprintf(status,
             sizeof(status),
             "%s %u秒後に再試行できます。",
             message,
             delay_seconds);
    draw_login_screen("ログイン",
                      "ユーザー名を入力してください",
                      username,
                      false,
                      status);
    sleep_ms((uint64_t)delay_seconds * 1000u);
}

static bool user_login_create_user(char *username_out, size_t username_out_size) {
    char username[33], password[129], confirm[129], status[128];
    while (1) {
        status[0] = '\0';
        if (!prompt_user_input("新規ユーザー登録", "ユーザー名を入力してください", username, sizeof(username), false)) return false;
        if (username[0] == '\0' || strchr(username, ':') || strchr(username, '\n') || strchr(username, '\r')) {
            os_strcpy_s(status, sizeof(status), "ユーザー名に無効な文字が含まれています。");
            draw_login_screen("新規ユーザー登録", "ユーザー名を入力してください", username, false, status);
            continue;
        }
        if (user_db_lookup(username, (char[65]){0}, 65, (char[65]){0}, 65)) {
            os_strcpy_s(status, sizeof(status), "このユーザー名は既に使われています。");
            draw_login_screen("新規ユーザー登録", "ユーザー名を入力してください", username, false, status);
            continue;
        }
        if (!prompt_user_input("新規ユーザー登録", "パスワードを入力してください", password, sizeof(password), true)) return false;
        if (!prompt_user_input("新規ユーザー登録", "パスワードを再入力してください", confirm, sizeof(confirm), true)) return false;
        if (strcmp(password, confirm) != 0) {
            os_strcpy_s(status, sizeof(status), "パスワードが一致しません。もう一度入力してください。");
            draw_login_screen("新規ユーザー登録", "パスワードを入力してください", password, true, status);
            continue;
        }

        uint8_t salt_bytes[16];
        char salt_hex[33], hash_hex[65];
        crypto_generate_salt_bytes(salt_bytes, sizeof(salt_bytes));
        crypto_hex_encode(salt_bytes, sizeof(salt_bytes), salt_hex);
        if (crypto_hash_password_hex(password, salt_hex, hash_hex) < 0) {
            os_strcpy_s(status, sizeof(status), "パスワード処理に失敗しました。");
            draw_login_screen("新規ユーザー登録", "ユーザー名を入力してください", username, false, status);
            continue;
        }

        if (!user_db_add(username, salt_hex, hash_hex)) {
            os_strcpy_s(status, sizeof(status), "ユーザー登録に失敗しました。");
            draw_login_screen("新規ユーザー登録", "ユーザー名を入力してください", username, false, status);
            continue;
        }
        os_strcpy_s(username_out, username_out_size, username);
        return true;
    }
}

static bool user_login_authenticate(char *username_out, size_t username_out_size) {
    char username[33], password[129], salt[65], stored_hash[65], computed_hash[65];
    uint32_t failure_count = 0u;
    while (1) {
        if (!prompt_user_input("ログイン", "ユーザー名を入力してください", username, sizeof(username), false)) return false;
        if (!user_db_lookup(username, salt, sizeof(salt), stored_hash, sizeof(stored_hash))) {
            failure_count++;
            login_failure_backoff(failure_count,
                                  username,
                                  "ユーザー名またはパスワードが違います。");
            continue;
        }
        if (!prompt_user_input("ログイン", "パスワードを入力してください", password, sizeof(password), true)) return false;

        bool authenticated =
            crypto_hash_password_hex(password, salt, computed_hash) == 0 &&
            secure_hash_equal(stored_hash, computed_hash);
        if (!authenticated &&
            crypto_hash_password_hex_legacy(password, salt, computed_hash) == 0 &&
            secure_hash_equal(stored_hash, computed_hash)) {
            authenticated =
                crypto_hash_password_hex(password, salt, computed_hash) == 0;
            if (authenticated) {
                (void)user_db_upgrade_hash(username, computed_hash);
            }
        }

        memset(password, 0, sizeof(password));
        if (authenticated) {
            os_strcpy_s(username_out, username_out_size, username);
            return true;
        }
        failure_count++;
        login_failure_backoff(failure_count,
                              username,
                              "ユーザー名またはパスワードが違います。");
    }
}

static bool run_user_login_flow(char *current_username, size_t current_username_size) {
    if (!user_db_exists()) return user_login_create_user(current_username, current_username_size);
    return user_login_authenticate(current_username, current_username_size);
}

static void fade_in(uint32_t duration_ms, uint32_t steps) {
    uint32_t width  = get_display_width(), height = get_display_height();
    uint32_t *fb = (uint32_t *)sys_get_display_framebuffer();
    if (!fb) return;
    uint32_t pixels = width * height;

    if (!g_fb_snapshot || g_fb_snapshot_pixels != pixels) {
        free(g_fb_snapshot);
        g_fb_snapshot = (uint32_t *)malloc(pixels * sizeof(uint32_t));
        if (!g_fb_snapshot) return;
        g_fb_snapshot_pixels = pixels;
    }
    memcpy(g_fb_snapshot, fb, pixels * sizeof(uint32_t));
    uint32_t delay = duration_ms / steps;

    for (uint32_t step = 0; step <= steps; ++step) {
        float t = (float)step / (float)steps;
        for (uint32_t i = 0; i < pixels; ++i) {
            uint32_t src = g_fb_snapshot[i];
            uint8_t r = (uint8_t)(((src >> 16) & 0xFF) * t);
            uint8_t g = (uint8_t)(((src >> 8) & 0xFF) * t);
            uint8_t b = (uint8_t)((src & 0xFF) * t);
            fb[i] = (r << 16) | (g << 8) | b;
        }
        draw_present();
        sleep_ms(delay);
    }
}

#define BOOT_COUNT_FILE "/Userland/boot_count.txt"

static int read_boot_count(void) {
    int32_t fd = file_open(BOOT_COUNT_FILE, 0);
    if (fd < 0) return 0;
    char buf[32] = {0};
    int64_t n = file_read(fd, buf, sizeof(buf) - 1);
    file_close(fd);
    if (n <= 0) return 0;
    int count = 0;
    for (int i = 0; buf[i]; ++i) {
        if (buf[i] >= '0' && buf[i] <= '9') count = (count * 10) + (buf[i] - '0');
    }
    return count;
}

static void write_boot_count(int count) {
    int32_t fd = file_creat(BOOT_COUNT_FILE);
    if (fd < 0) return;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", count);
    if (len > 0) file_write(fd, buf, (uint64_t)len);
    file_close(fd);
}

static void serial_write_dec(uint32_t value)
{
    char buf[11];
    int i = 10;
    buf[i] = '\0';
    if (value == 0) { serial_write_char('0'); return; }
    while (value > 0) { buf[--i] = '0' + (char)(value % 10u); value /= 10u; }
    serial_write_string(&buf[i]);
}

static void measure_and_print_cpu_usage(void) {
    system_cpu_usage_t snap1, snap2;
    if (os_get_cpu_usage(&snap1) < 0) return;
    sleep_ms(1000);
    if (os_get_cpu_usage(&snap2) < 0) return;

    uint64_t wall_delta = snap2.wall_ns - snap1.wall_ns;
    if (wall_delta == 0) return;

    serial_write_string("[CPU] per-core usage over 1s:\n");
    uint32_t count = snap2.cpu_count;
    if (count == 0) count = 1;
    for (uint32_t i = 0; i < count && i < OS_CPU_USAGE_MAX_CORES; ++i) {
        uint64_t idle_delta = snap2.idle_ns[i] - snap1.idle_ns[i];
        uint32_t busy_pct = (uint32_t)((idle_delta * 100ULL) / wall_delta);
        uint32_t used_pct = (busy_pct <= 100u) ? (100u - busy_pct) : 0u;

        serial_write_string("  CPU");
        serial_write_dec(i);
        serial_write_string(": ");
        serial_write_dec(used_pct);
        serial_write_string("%\n");
    }
}

int Enable_Login = false;

void _start(void) {
    int boot_count = read_boot_count();
    bool first_boot = (boot_count == 0);
    boot_count++;
    write_boot_count(boot_count);

    draw_background();
    load_default_font();

    if (first_boot) {
        draw_text_centered("はじめまして。サービスを開始中です。", 0, 42.0f, 0xFFFFFF);
    } else {
        draw_text_centered("おかえりなさい。サービスを開始中です。", 0, 42.0f, 0xFFFFFF);
    }

    uuid_t id;

    uuid_generate_v4(&id);

    char buf[UUID_STR_BUF_SIZE];
    uuid_to_string(&id, buf);

    char current_user[128];

    snprintf(
        current_user,
        sizeof(current_user),
        "Nameless User %s",
        buf
    );

    if (Enable_Login) {
        current_user[0] = '\0';

        if (!run_user_login_flow(current_user, sizeof(current_user))) {
            draw_text_centered("ログインに失敗しました。再起動してください。", 80, 20.0f, 0xFF8080);
            draw_present();

            while (1) sleep_ms(1000u);
        }
    }

    char welcome_text[128];
    snprintf(welcome_text, sizeof(welcome_text), "ようこそ、%s さん。システムを起動しています...", current_user);
    draw_text_centered(welcome_text, 80, 20.0f, 0xFFFFFF);

    char boot_msg[128];
    snprintf(boot_msg, sizeof(boot_msg), "今回は、%d回目の起動です。", boot_count);
    draw_text_centered(boot_msg, 100, 25.0f, 0xFFFFFF);

    draw_present();
    fade_in(240, 12);

    /* Pull in the Userland services (POSIX, network stack, ...) listed in
     * /Userland/Service/services.list. Each is a hot-loadable .so and can
     * be dropped again at runtime via service_unload(). */
    service_load_all();

    static const char *const wm_paths[] = {
        "/Userland/com.ImplusOS.windowmanager/com.ImplusOS.windowmanager.ELF",
    };
    int32_t wm_spawn_pid = spawn_with_fallbacks(wm_paths, 1);
    process_set_priority(wm_spawn_pid, 3);
    process_yield();

    int32_t wm_pid = -1;
    for (int i = 0; i < 250; i++) {
        wm_pid = window_get_wm_pid();
        if (wm_pid >= 0) break;
        sleep_ms(20);
    }

    if (wm_pid >= 0) {
        static const char *const sysnotif_paths[] = {
            "/Userland/com.ImplusOS.sysnotif/com.ImplusOS.sysnotif.ELF",
        };
        spawn_with_fallbacks(sysnotif_paths, 1);
    }

    if (wm_pid >= 0) {
        static const char *const doom_paths[] = {
            "/Userland/Doom/Doom.ELF",
        };
        spawn_with_fallbacks(doom_paths, 1);
    }

    if (g_bg_cache) {
        free(g_bg_cache);
        g_bg_cache = NULL;
    }
    free(g_fb_snapshot);
    g_fb_snapshot = NULL;
    g_fb_snapshot_pixels = 0;
    free(g_font_buffer);
    g_font_buffer = NULL;
    g_font_loaded = 0;

    while (1) {
        sleep_ms(1000u);
    }
}
