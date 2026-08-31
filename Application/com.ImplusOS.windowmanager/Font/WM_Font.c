#include "WM_Font.h"

#include "../Compositor/WM_Raster.h"
#include "../../../../Userland/API/Source/File.h"

#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H

#define WM_GLYPH_CACHE_SIZE 4096u
#define WM_GLYPH_HASH_SIZE  8192u
#define WM_GLYPH_NONE       UINT32_MAX
#define WM_GLYPH_HASH_EMPTY (-1)
#define WM_GLYPH_HASH_TOMBSTONE (-2)

typedef struct {
    int codepoint;
    uint32_t size_x100;
    int width;
    int height;
    int x_offset;
    int y_offset;
    int advance;
    uint8_t *bitmap;
    bool valid;
    uint32_t lru_previous;
    uint32_t lru_next;
} wm_glyph_t;

typedef struct {
    FT_Library library;
    FT_Face face;
    uint8_t *buffer;
    uint32_t buffer_size;
    wm_glyph_t glyphs[WM_GLYPH_CACHE_SIZE];
    int32_t hash_slots[WM_GLYPH_HASH_SIZE];
    uint32_t lru_head;
    uint32_t lru_tail;
    uint32_t next_unused;
} wm_font_impl_t;

static int utf8_decode(const char **cursor)
{
    const uint8_t *text = (const uint8_t *)*cursor;
    uint8_t first = *text++;
    if (first == 0u) return 0;
    if (first < 0x80u) {
        *cursor = (const char *)text;
        return first;
    }
    uint32_t value;
    uint32_t remaining;
    if ((first & 0xE0u) == 0xC0u) {
        value = first & 0x1Fu;
        remaining = 1u;
    } else if ((first & 0xF0u) == 0xE0u) {
        value = first & 0x0Fu;
        remaining = 2u;
    } else if ((first & 0xF8u) == 0xF0u) {
        value = first & 0x07u;
        remaining = 3u;
    } else {
        *cursor = (const char *)text;
        return '?';
    }
    for (uint32_t i = 0; i < remaining; ++i) {
        uint8_t next = *text;
        if ((next & 0xC0u) != 0x80u) {
            *cursor = (const char *)text;
            return '?';
        }
        value = (value << 6u) | (next & 0x3Fu);
        ++text;
    }
    *cursor = (const char *)text;
    return value <= 0x10FFFFu ? (int)value : '?';
}

static uint32_t glyph_hash(int codepoint, uint32_t size_x100)
{
    return ((uint32_t)codepoint * 2654435761u) ^
           (size_x100 * 2246822519u);
}

static void glyph_lru_remove(wm_font_impl_t *impl, uint32_t index)
{
    wm_glyph_t *glyph = &impl->glyphs[index];
    if (glyph->lru_previous != WM_GLYPH_NONE)
        impl->glyphs[glyph->lru_previous].lru_next = glyph->lru_next;
    else
        impl->lru_head = glyph->lru_next;
    if (glyph->lru_next != WM_GLYPH_NONE)
        impl->glyphs[glyph->lru_next].lru_previous = glyph->lru_previous;
    else
        impl->lru_tail = glyph->lru_previous;
    glyph->lru_previous = WM_GLYPH_NONE;
    glyph->lru_next = WM_GLYPH_NONE;
}

static void glyph_lru_make_recent(wm_font_impl_t *impl, uint32_t index)
{
    wm_glyph_t *glyph = &impl->glyphs[index];
    if (impl->lru_head == index) return;
    if (glyph->valid &&
        (glyph->lru_previous != WM_GLYPH_NONE ||
         glyph->lru_next != WM_GLYPH_NONE ||
         impl->lru_tail == index))
        glyph_lru_remove(impl, index);
    glyph->lru_previous = WM_GLYPH_NONE;
    glyph->lru_next = impl->lru_head;
    if (impl->lru_head != WM_GLYPH_NONE)
        impl->glyphs[impl->lru_head].lru_previous = index;
    else
        impl->lru_tail = index;
    impl->lru_head = index;
}

static int32_t glyph_hash_find(const wm_font_impl_t *impl,
                               int codepoint, uint32_t size_x100)
{
    uint32_t slot = glyph_hash(codepoint, size_x100) % WM_GLYPH_HASH_SIZE;
    for (uint32_t probe = 0; probe < WM_GLYPH_HASH_SIZE; ++probe) {
        int32_t index = impl->hash_slots[slot];
        if (index == WM_GLYPH_HASH_EMPTY) return -1;
        if (index >= 0) {
            const wm_glyph_t *glyph = &impl->glyphs[(uint32_t)index];
            if (glyph->valid && glyph->codepoint == codepoint &&
                glyph->size_x100 == size_x100)
                return index;
        }
        slot = (slot + 1u) % WM_GLYPH_HASH_SIZE;
    }
    return -1;
}

static void glyph_hash_remove(wm_font_impl_t *impl,
                              int codepoint, uint32_t size_x100)
{
    uint32_t slot = glyph_hash(codepoint, size_x100) % WM_GLYPH_HASH_SIZE;
    for (uint32_t probe = 0; probe < WM_GLYPH_HASH_SIZE; ++probe) {
        int32_t index = impl->hash_slots[slot];
        if (index == WM_GLYPH_HASH_EMPTY) return;
        if (index >= 0) {
            wm_glyph_t *glyph = &impl->glyphs[(uint32_t)index];
            if (glyph->valid && glyph->codepoint == codepoint &&
                glyph->size_x100 == size_x100) {
                impl->hash_slots[slot] = WM_GLYPH_HASH_TOMBSTONE;
                return;
            }
        }
        slot = (slot + 1u) % WM_GLYPH_HASH_SIZE;
    }
}

static bool glyph_hash_insert(wm_font_impl_t *impl, uint32_t glyph_index)
{
    const wm_glyph_t *glyph = &impl->glyphs[glyph_index];
    uint32_t slot = glyph_hash(glyph->codepoint, glyph->size_x100) %
                    WM_GLYPH_HASH_SIZE;
    uint32_t tombstone = WM_GLYPH_NONE;
    for (uint32_t probe = 0; probe < WM_GLYPH_HASH_SIZE; ++probe) {
        int32_t value = impl->hash_slots[slot];
        if (value == WM_GLYPH_HASH_TOMBSTONE && tombstone == WM_GLYPH_NONE)
            tombstone = slot;
        if (value == WM_GLYPH_HASH_EMPTY) {
            impl->hash_slots[tombstone != WM_GLYPH_NONE ? tombstone : slot] =
                (int32_t)glyph_index;
            return true;
        }
        slot = (slot + 1u) % WM_GLYPH_HASH_SIZE;
    }
    if (tombstone != WM_GLYPH_NONE) {
        impl->hash_slots[tombstone] = (int32_t)glyph_index;
        return true;
    }
    return false;
}

static wm_glyph_t *glyph_get(wm_font_impl_t *impl, int codepoint, float pixel_height)
{
    uint32_t size_x100 = (uint32_t)(pixel_height * 100.0f + 0.5f);
    int32_t existing = glyph_hash_find(impl, codepoint, size_x100);
    if (existing >= 0) {
        glyph_lru_make_recent(impl, (uint32_t)existing);
        return &impl->glyphs[(uint32_t)existing];
    }

    uint32_t candidate_index;
    if (impl->next_unused < WM_GLYPH_CACHE_SIZE) {
        candidate_index = impl->next_unused++;
    } else {
        candidate_index = impl->lru_tail;
        if (candidate_index == WM_GLYPH_NONE) return NULL;
    }
    wm_glyph_t *candidate = &impl->glyphs[candidate_index];

    if (candidate->valid) {
        glyph_hash_remove(impl, candidate->codepoint, candidate->size_x100);
        glyph_lru_remove(impl, candidate_index);
    }
    free(candidate->bitmap);
    memset(candidate, 0, sizeof(*candidate));
    candidate->lru_previous = WM_GLYPH_NONE;
    candidate->lru_next = WM_GLYPH_NONE;

    FT_Set_Pixel_Sizes(impl->face, 0, (FT_UInt)(pixel_height + 0.5f));

    FT_UInt glyph_index = FT_Get_Char_Index(impl->face, (FT_ULong)codepoint);
    FT_Error err = FT_Load_Glyph(impl->face, glyph_index, FT_LOAD_DEFAULT);
    if (err) {
        candidate->codepoint = codepoint;
        candidate->size_x100 = size_x100;
        candidate->valid = true;
        if (!glyph_hash_insert(impl, candidate_index)) {
            candidate->valid = false;
            return NULL;
        }
        glyph_lru_make_recent(impl, candidate_index);
        return candidate;
    }

    FT_GlyphSlot slot = impl->face->glyph;

    candidate->width = (int)slot->bitmap.width;
    candidate->height = (int)slot->bitmap.rows;
    candidate->x_offset = (int)slot->bitmap_left;
    candidate->y_offset = -(int)slot->bitmap_top;
    candidate->advance = (int)(slot->advance.x >> 6);
    candidate->codepoint = codepoint;
    candidate->size_x100 = size_x100;
    candidate->valid = true;

    if (candidate->width > 0 && candidate->height > 0 &&
        candidate->width <= 192 && candidate->height <= 192) {
        FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL);
        size_t bytes = (size_t)candidate->width * (size_t)candidate->height;
        candidate->bitmap = (uint8_t *)malloc(bytes);
        if (candidate->bitmap) {
            for (int row = 0; row < candidate->height; ++row) {
                memcpy(candidate->bitmap + (size_t)row * (size_t)candidate->width,
                       slot->bitmap.buffer + (size_t)row * (size_t)slot->bitmap.pitch,
                       (size_t)candidate->width);
            }
        }
    }
    if (!glyph_hash_insert(impl, candidate_index)) {
        free(candidate->bitmap);
        memset(candidate, 0, sizeof(*candidate));
        candidate->lru_previous = WM_GLYPH_NONE;
        candidate->lru_next = WM_GLYPH_NONE;
        return NULL;
    }
    glyph_lru_make_recent(impl, candidate_index);
    return candidate;
}

bool wm_font_init(wm_font_t *font, const char *path)
{
    if (!font || !path) return false;
    memset(font, 0, sizeof(*font));
    file_stat_t stat;
    if (file_stat(path, &stat) < 0 || stat.size == 0u) return false;
    int32_t fd = file_open(path, 0);
    if (fd < 0) return false;

    wm_font_impl_t *impl = (wm_font_impl_t *)malloc(sizeof(*impl));
    if (!impl) {
        file_close(fd);
        return false;
    }
    memset(impl, 0, sizeof(*impl));
    for (uint32_t i = 0; i < WM_GLYPH_HASH_SIZE; ++i)
        impl->hash_slots[i] = WM_GLYPH_HASH_EMPTY;
    impl->lru_head = WM_GLYPH_NONE;
    impl->lru_tail = WM_GLYPH_NONE;
    for (uint32_t i = 0; i < WM_GLYPH_CACHE_SIZE; ++i) {
        impl->glyphs[i].lru_previous = WM_GLYPH_NONE;
        impl->glyphs[i].lru_next = WM_GLYPH_NONE;
    }
    impl->buffer = (uint8_t *)malloc(stat.size);
    if (!impl->buffer) {
        free(impl);
        file_close(fd);
        return false;
    }
    int64_t bytes = file_read(fd, impl->buffer, stat.size);
    file_close(fd);
    if (bytes != (int64_t)stat.size) {
        free(impl->buffer);
        free(impl);
        return false;
    }

    FT_Error err = FT_Init_FreeType(&impl->library);
    if (err) {
        free(impl->buffer);
        free(impl);
        return false;
    }

    err = FT_New_Memory_Face(impl->library,
                              (const FT_Byte *)impl->buffer,
                              (FT_Long)stat.size,
                              0, &impl->face);
    if (err || !impl->face) {
        FT_Done_FreeType(impl->library);
        free(impl->buffer);
        free(impl);
        return false;
    }

    impl->buffer_size = stat.size;
    font->impl = impl;
    font->loaded = true;
    return true;
}

void wm_font_destroy(wm_font_t *font)
{
    if (!font || !font->impl) return;
    wm_font_impl_t *impl = (wm_font_impl_t *)font->impl;
    for (uint32_t i = 0; i < WM_GLYPH_CACHE_SIZE; ++i)
        if (impl->glyphs[i].bitmap) free(impl->glyphs[i].bitmap);
    if (impl->face) FT_Done_Face(impl->face);
    if (impl->library) FT_Done_FreeType(impl->library);
    free(impl->buffer);
    free(impl);
    font->impl = NULL;
    font->loaded = false;
}

uint32_t wm_font_measure(wm_font_t *font, const char *text, float pixel_height)
{
    if (!font || !font->loaded || !text || pixel_height <= 0.0f) return 0u;
    wm_font_impl_t *impl = (wm_font_impl_t *)font->impl;
    int32_t width = 0;
    FT_UInt previous = 0;
    FT_Set_Pixel_Sizes(impl->face, 0, (FT_UInt)(pixel_height + 0.5f));
    while (*text) {
        int codepoint = utf8_decode(&text);
        FT_UInt current = FT_Get_Char_Index(impl->face, (FT_ULong)codepoint);
        if (previous && current) {
            FT_Vector kern;
            FT_Get_Kerning(impl->face, previous, current, FT_KERNING_DEFAULT, &kern);
            width += (int)(kern.x >> 6);
        }
        FT_Load_Glyph(impl->face, current, FT_LOAD_DEFAULT);
        width += (int)(impl->face->glyph->advance.x >> 6);
        previous = current;
    }
    return width > 0 ? (uint32_t)width : 0u;
}

void wm_font_draw(wm_font_t *font, wm_canvas_t *canvas,
                  int32_t x, int32_t y, const char *text,
                  uint32_t color, float pixel_height, uint32_t max_width)
{
    if (!font || !font->loaded || !canvas || !text || pixel_height <= 0.0f) return;
    wm_font_impl_t *impl = (wm_font_impl_t *)font->impl;
    FT_Set_Pixel_Sizes(impl->face, 0, (FT_UInt)(pixel_height + 0.5f));
    int32_t baseline = y + (int)(impl->face->size->metrics.ascender >> 6);
    int32_t cursor_x = x;
    FT_UInt previous = 0;
    uint32_t red = (color >> 16u) & 0xFFu;
    uint32_t green = (color >> 8u) & 0xFFu;
    uint32_t blue = color & 0xFFu;
    uint32_t color_alpha = color >> 24u;

    while (*text) {
        int codepoint = utf8_decode(&text);
        FT_UInt current = FT_Get_Char_Index(impl->face, (FT_ULong)codepoint);
        if (previous && current) {
            FT_Vector kern;
            FT_Get_Kerning(impl->face, previous, current, FT_KERNING_DEFAULT, &kern);
            cursor_x += (int)(kern.x >> 6);
        }
        wm_glyph_t *glyph = glyph_get(impl, codepoint, pixel_height);
        if (!glyph) { previous = current; continue; }
        if (max_width != 0u && cursor_x + glyph->advance - x > (int32_t)max_width) break;
        if (glyph->bitmap) {
            for (int row = 0; row < glyph->height; ++row) {
                for (int col = 0; col < glyph->width; ++col) {
                    uint32_t coverage = glyph->bitmap[row * glyph->width + col];
                    uint32_t alpha = (coverage * color_alpha) / 255u;
                    if (alpha == 0u) continue;
                    int32_t px = cursor_x + glyph->x_offset + col;
                    if (max_width != 0u && px >= x + (int32_t)max_width) continue;
                    wm_canvas_put(canvas, px, baseline + glyph->y_offset + row,
                        (alpha << 24u) | (red << 16u) | (green << 8u) | blue);
                }
            }
        }
        cursor_x += glyph->advance;
        previous = current;
    }
}
