/*
 * com.ImplusOS.loginui -- the graphical login screen.
 *
 * Spawned by the init process (Userland.ELF) after the userland services are
 * up and before the window manager. It:
 *   - paints the window manager's wallpaper as the backdrop,
 *   - lists the known users (icon + name) at the bottom-left, each row
 *     clickable, with no panel behind the section,
 *   - shows a create-user screen when the user database is empty,
 *   - on a correct password, records the session in tmpfs and starts the
 *     window-manager session, then idles as the session leader.
 *
 * The credential store is entirely userland: /var/System/users.db (writable
 * tmpfs), one "name:salt_hex:hash_hex" record per line, hash =
 * PBKDF2-HMAC-SHA256 via Library/Crypto. There is no kernel user model; the
 * chosen uid is derived from the record position (1000 + index) and written
 * to /run/session.uid for other userland components to read.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "Graphics.h"
#include "Input.h"
#include "File.h"
#include "Process.h"
#include "Serial.h"
#include "Window.h"

#include "Crypto/Crypto.h"
#include "Unicode/UTF8/UTF8.h"

/* ------------------------------------------------------------- stb glue */

static void *ui_realloc_sized(void *p, size_t oldsz, size_t newsz) {
    if (newsz == 0) { if (p) free(p); return NULL; }
    void *q = malloc(newsz);
    if (q && p) { memcpy(q, p, oldsz < newsz ? oldsz : newsz); free(p); }
    return q;
}

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_malloc(x, u) ((void)(u), malloc(x))
#define STBTT_free(x, u)   ((void)(u), free(x))
#define STBTT_fmod(x, y)   fmod(x, y)
#include "Header/stb_truetype.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wshadow"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_PNG
#define STBI_MALLOC(sz)             malloc(sz)
#define STBI_REALLOC(p, ns)         ui_realloc_sized(p, 0, ns)
#define STBI_REALLOC_SIZED(p, o, n) ui_realloc_sized(p, o, n)
#define STBI_FREE(p)                free(p)
#include "Header/stb_image.h"
#pragma GCC diagnostic pop

/* ------------------------------------------------------------- resources */

#define FONT_PATH        "/Userland/com.ImplusOS.windowmanager/Resource/Fonts/NotoSansJP-Regular.ttf"
#define FONT_PATH_ALT    "/BootManager/Resource/Fonts/NotoSansJP-Regular.ttf"
#define WALLPAPER_PATH   "/Userland/com.ImplusOS.windowmanager/Resource/Background.png"

#define USER_DB_DIR      "/var/System"
#define USER_DB_FILE     "/var/System/users.db"
#define SESSION_DIR      "/run"
#define SESSION_USER     "/run/session.user"
#define SESSION_UID      "/run/session.uid"
#define USER_DB_MAX_BYTES (256u * 1024u)

#define WM_PATH   "/Userland/com.ImplusOS.windowmanager/com.ImplusOS.windowmanager.ELF"
#define SYSNOTIF  "/Userland/com.ImplusOS.sysnotif/com.ImplusOS.sysnotif.ELF"

#define MAX_USERS 32u

/* ------------------------------------------------------------- globals */

static uint32_t *g_fb;
static int       g_w, g_h;

static uint8_t        *g_font_buf;
static stbtt_fontinfo  g_font;
static bool            g_font_ok;

static uint32_t *g_wallpaper;      /* pre-composited backdrop, g_w*g_h */

typedef struct { char name[64]; uint32_t uid; } login_user_t;
static login_user_t g_users[MAX_USERS];
static uint32_t     g_user_count;

/* ------------------------------------------------------------- pixels */

static inline uint32_t blend(uint32_t src, uint32_t dst, uint8_t a) {
    uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    uint32_t r = (sr * a + dr * (255u - a)) / 255u;
    uint32_t g = (sg * a + dg * (255u - a)) / 255u;
    uint32_t b = (sb * a + db * (255u - a)) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static inline void px(int x, int y, uint32_t color, uint8_t a) {
    if (x < 0 || y < 0 || x >= g_w || y >= g_h) return;
    uint32_t *d = &g_fb[y * g_w + x];
    *d = (a == 255) ? (0xFF000000u | color) : blend(color, *d, a);
}

static void fill_rect(int x, int y, int w, int h, uint32_t color, uint8_t a) {
    for (int j = y; j < y + h; ++j) {
        if (j < 0 || j >= g_h) continue;
        for (int i = x; i < x + w; ++i) {
            if (i < 0 || i >= g_w) continue;
            uint32_t *d = &g_fb[j * g_w + i];
            *d = (a == 255) ? (0xFF000000u | color) : blend(color, *d, a);
        }
    }
}

static void fill_round_rect(int x, int y, int w, int h, int rad, uint32_t color, uint8_t a) {
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    int rsq = rad * rad;
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            int cx = i, cy = j, dxv = 0, dyv = 0;
            if (i < rad && j < rad)              { dxv = rad - 1 - i; dyv = rad - 1 - j; }
            else if (i >= w - rad && j < rad)    { dxv = i - (w - rad); dyv = rad - 1 - j; }
            else if (i < rad && j >= h - rad)    { dxv = rad - 1 - i; dyv = j - (h - rad); }
            else if (i >= w - rad && j >= h - rad){ dxv = i - (w - rad); dyv = j - (h - rad); }
            else { px(x + cx, y + cy, color, a); continue; }
            if (dxv * dxv + dyv * dyv <= rsq) px(x + cx, y + cy, color, a);
        }
    }
}

static void stroke_round_rect(int x, int y, int w, int h, int rad, uint32_t color, uint8_t a) {
    for (int i = 0; i < w; ++i) { px(x + i, y, color, a); px(x + i, y + h - 1, color, a); }
    for (int j = 0; j < h; ++j) { px(x, y + j, color, a); px(x + w - 1, y + j, color, a); }
    (void)rad;
}

static void fill_circle(int cx, int cy, int r, uint32_t color, uint8_t a) {
    for (int j = -r; j <= r; ++j)
        for (int i = -r; i <= r; ++i)
            if (i * i + j * j <= r * r) px(cx + i, cy + j, color, a);
}

/* ------------------------------------------------------------- text */

static bool font_load(void) {
    const char *paths[] = { FONT_PATH, FONT_PATH_ALT };
    for (unsigned k = 0; k < 2; ++k) {
        file_stat_t st;
        if (file_stat(paths[k], &st) < 0 || !st.exists || st.is_dir || st.size == 0 ||
            st.size > 64u * 1024u * 1024u)
            continue;
        int32_t fd = file_open(paths[k], 0);
        if (fd < 0) continue;
        uint8_t *buf = malloc(st.size);
        if (!buf) { file_close(fd); continue; }
        uint32_t got = 0;
        while (got < st.size) {
            int64_t n = file_read(fd, buf + got, st.size - got);
            if (n <= 0) break;
            got += (uint32_t)n;
        }
        file_close(fd);
        if (got != st.size) { free(buf); continue; }
        int off = stbtt_GetFontOffsetForIndex(buf, 0);
        if (off < 0 || !stbtt_InitFont(&g_font, buf, off)) { free(buf); continue; }
        g_font_buf = buf;
        g_font_ok = true;
        return true;
    }
    return false;
}

static int text_width(const char *s, float scale) {
    if (!g_font_ok || !s) return 0;
    const char *p = s, *e = s + strlen(s);
    int w = 0, has_prev = 0;
    utf8_codepoint_t prev = 0;
    while (p < e) {
        utf8_codepoint_t cp;
        if (utf8_next(&p, e, &cp) != 0) continue;
        if (has_prev) w += (int)(stbtt_GetCodepointKernAdvance(&g_font, (int)prev, (int)cp) * scale);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, (int)cp, &adv, &lsb);
        w += (int)(adv * scale);
        prev = cp; has_prev = 1;
    }
    return w;
}

static void draw_glyph(int x, int y, int cp, float scale, uint32_t color) {
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&g_font, cp, scale, scale, &x0, &y0, &x1, &y1);
    int bw = x1 - x0, bh = y1 - y0;
    if (bw <= 0 || bh <= 0) return;
    uint8_t *bmp = malloc((size_t)(bw * bh));
    if (!bmp) return;
    stbtt_MakeCodepointBitmap(&g_font, bmp, bw, bh, bw, scale, scale, cp);
    for (int j = 0; j < bh; ++j)
        for (int i = 0; i < bw; ++i) {
            uint8_t av = bmp[j * bw + i];
            if (av) px(x + x0 + i, y + y0 + j, color, av);
        }
    free(bmp);
}

static void draw_text(int x, int y, const char *s, float scale, uint32_t color) {
    if (!g_font_ok || !s) return;
    const char *p = s, *e = s + strlen(s);
    int pen = x, has_prev = 0;
    utf8_codepoint_t prev = 0;
    while (p < e) {
        utf8_codepoint_t cp;
        if (utf8_next(&p, e, &cp) != 0) continue;
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, (int)cp, &adv, &lsb);
        if (cp == ' ') { pen += (int)(adv * scale); prev = 0; has_prev = 0; continue; }
        if (has_prev) pen += (int)(stbtt_GetCodepointKernAdvance(&g_font, (int)prev, (int)cp) * scale);
        draw_glyph(pen, y, (int)cp, scale, color);
        pen += (int)(adv * scale);
        prev = cp; has_prev = 1;
    }
}

static float scale_for(float px_height) {
    return g_font_ok ? stbtt_ScaleForPixelHeight(&g_font, px_height) : 0.0f;
}

/* baseline helper: draw text whose visual top sits at y */
static void draw_text_top(int x, int y, const char *s, float px_height, uint32_t color) {
    if (!g_font_ok) return;
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&g_font, &asc, &desc, &gap);
    float sc = scale_for(px_height);
    draw_text(x, y + (int)(asc * sc), s, sc, color);
}

/* ------------------------------------------------------------- wallpaper */

static void compose_wallpaper(void) {
    g_wallpaper = malloc((size_t)g_w * (size_t)g_h * 4);
    if (!g_wallpaper) return;

    /* default: deep vertical gradient */
    for (int y = 0; y < g_h; ++y) {
        float t = (float)y / (float)g_h;
        uint8_t r = (uint8_t)(18 + (10 - 18) * t);
        uint8_t g = (uint8_t)(20 + (28 - 20) * t);
        uint8_t b = (uint8_t)(40 + (58 - 40) * t);
        uint32_t c = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        for (int x = 0; x < g_w; ++x) g_wallpaper[y * g_w + x] = c;
    }

    file_stat_t st;
    if (file_stat(WALLPAPER_PATH, &st) < 0 || !st.exists || st.is_dir ||
        st.size == 0 || st.size > 32u * 1024u * 1024u)
        return;
    int32_t fd = file_open(WALLPAPER_PATH, 0);
    if (fd < 0) return;
    uint8_t *raw = malloc(st.size);
    if (!raw) { file_close(fd); return; }
    uint32_t got = 0;
    while (got < st.size) {
        int64_t n = file_read(fd, raw + got, st.size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    file_close(fd);
    if (got != st.size) { free(raw); return; }

    int iw, ih, comp;
    uint8_t *img = stbi_load_from_memory(raw, (int)st.size, &iw, &ih, &comp, 4);
    free(raw);
    if (!img) return;

    /* cover-fit + light dim so the foreground UI stays readable */
    uint32_t snum, sden;
    if ((uint64_t)iw * (uint64_t)g_h > (uint64_t)ih * (uint64_t)g_w) {
        snum = (uint32_t)g_h; sden = (uint32_t)ih;
    } else {
        snum = (uint32_t)g_w; sden = (uint32_t)iw;
    }
    uint32_t sw = (uint32_t)iw * snum / sden;
    uint32_t sh = (uint32_t)ih * snum / sden;
    int ox = (sw > (uint32_t)g_w) ? (int)((sw - (uint32_t)g_w) / 2) : 0;
    int oy = (sh > (uint32_t)g_h) ? (int)((sh - (uint32_t)g_h) / 2) : 0;

    for (int y = 0; y < g_h; ++y) {
        uint32_t sy = (uint32_t)(((int64_t)(y + oy) * sden) / snum);
        if (sy >= (uint32_t)ih) sy = (uint32_t)ih - 1;
        const uint8_t *srow = img + sy * (uint32_t)iw * 4;
        for (int x = 0; x < g_w; ++x) {
            uint32_t sx = (uint32_t)(((int64_t)(x + ox) * sden) / snum);
            if (sx >= (uint32_t)iw) sx = (uint32_t)iw - 1;
            const uint8_t *s = srow + sx * 4;
            uint8_t r = (uint8_t)((uint32_t)s[0] * 78u / 100u);
            uint8_t g = (uint8_t)((uint32_t)s[1] * 78u / 100u);
            uint8_t b = (uint8_t)((uint32_t)s[2] * 78u / 100u);
            g_wallpaper[y * g_w + x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
    stbi_image_free(img);
}

static void paint_backdrop(void) {
    if (g_wallpaper)
        memcpy(g_fb, g_wallpaper, (size_t)g_w * (size_t)g_h * 4);
    else
        fill_rect(0, 0, g_w, g_h, 0x101828, 255);
}

/* ------------------------------------------------------------- avatar */

/* Deterministic per-user accent so rows are visually distinct. */
static uint32_t accent_for(uint32_t idx) {
    static const uint32_t palette[] = {
        0x4F86F7, 0x36B37E, 0xF2994A, 0xBB6BD9,
        0xEB5757, 0x2D9CDB, 0xF2C94C, 0x56CCF2,
    };
    return palette[idx % (sizeof(palette) / sizeof(palette[0]))];
}

static void draw_avatar(int cx, int cy, int r, uint32_t accent, const char *name, float glyph_px) {
    fill_circle(cx, cy, r, accent, 255);
    fill_circle(cx, cy, r - 2, blend(accent, 0xFFFFFF, 40), 255);
    /* initial */
    if (g_font_ok && name && name[0]) {
        char init[5] = {0};
        const char *p = name, *e = name + strlen(name);
        utf8_codepoint_t cp;
        if (utf8_next(&p, e, &cp) == 0) {
            /* re-encode the first codepoint */
            size_t n = (size_t)(p - name);
            if (n > 4) n = 4;
            memcpy(init, name, n);
            float sc = scale_for(glyph_px);
            int tw = text_width(init, sc);
            int asc, desc, gap;
            stbtt_GetFontVMetrics(&g_font, &asc, &desc, &gap);
            int th = (int)((asc - desc) * sc);
            draw_text(cx - tw / 2, cy - th / 2 + (int)(asc * sc), init, sc, 0xFFFFFF);
        }
    }
}

/* ------------------------------------------------------------- user db */

static bool db_read_all(char **out, size_t *out_len) {
    *out = NULL; *out_len = 0;
    file_stat_t st;
    if (file_stat(USER_DB_FILE, &st) < 0 || !st.exists || st.is_dir || st.size == 0 ||
        st.size > USER_DB_MAX_BYTES)
        return false;
    int32_t fd = file_open(USER_DB_FILE, 0);
    if (fd < 0) return false;
    char *buf = malloc(st.size + 1u);
    if (!buf) { file_close(fd); return false; }
    uint32_t got = 0;
    while (got < st.size) {
        int64_t n = file_read(fd, buf + got, st.size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    file_close(fd);
    if (got != st.size) { free(buf); return false; }
    buf[st.size] = '\0';
    *out = buf; *out_len = st.size;
    return true;
}

static bool parse_record(const char *line, char *name, size_t name_sz,
                         char *salt, size_t salt_sz, char *hash, size_t hash_sz) {
    const char *c1 = strchr(line, ':');
    if (!c1) return false;
    const char *c2 = strchr(c1 + 1, ':');
    if (!c2) return false;
    size_t nl = (size_t)(c1 - line);
    size_t sl = (size_t)(c2 - c1 - 1);
    size_t hl = strlen(c2 + 1);
    while (hl > 0 && (c2[hl] == '\n' || c2[hl] == '\r')) hl--;
    if (nl >= name_sz || sl >= salt_sz || hl >= hash_sz) return false;
    memcpy(name, line, nl); name[nl] = '\0';
    memcpy(salt, c1 + 1, sl); salt[sl] = '\0';
    memcpy(hash, c2 + 1, hl); hash[hl] = '\0';
    return true;
}

static void db_load_users(void) {
    g_user_count = 0;
    char *buf; size_t len;
    if (!db_read_all(&buf, &len)) return;
    char *line = buf;
    while (*line && g_user_count < MAX_USERS) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char name[64], salt[96], hash[96];
        if (line[0] && parse_record(line, name, sizeof(name), salt, sizeof(salt),
                                    hash, sizeof(hash))) {
            strncpy(g_users[g_user_count].name, name, sizeof(g_users[0].name) - 1);
            g_users[g_user_count].name[sizeof(g_users[0].name) - 1] = '\0';
            g_users[g_user_count].uid = 1000u + g_user_count;
            g_user_count++;
        }
        if (!nl) break;
        line = nl + 1;
    }
    free(buf);
}

static bool db_lookup(const char *username, char *salt, size_t salt_sz,
                      char *hash, size_t hash_sz) {
    char *buf; size_t len;
    if (!db_read_all(&buf, &len)) return false;
    char *line = buf;
    bool found = false;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char name[64], s[96], h[96];
        if (line[0] && parse_record(line, name, sizeof(name), s, sizeof(s), h, sizeof(h)) &&
            strcmp(name, username) == 0) {
            strncpy(salt, s, salt_sz - 1); salt[salt_sz - 1] = '\0';
            strncpy(hash, h, hash_sz - 1); hash[hash_sz - 1] = '\0';
            found = true;
            break;
        }
        if (!nl) break;
        line = nl + 1;
    }
    free(buf);
    return found;
}

static bool db_add(const char *username, const char *salt_hex, const char *hash_hex) {
    (void)file_mkdir(USER_DB_DIR);
    int32_t fd = file_open(USER_DB_FILE, 1);
    if (fd < 0) fd = file_creat(USER_DB_FILE);
    if (fd < 0) return false;
    file_seek(fd, 0, 2);
    char line[192];
    int n = snprintf(line, sizeof(line), "%s:%s:%s\n", username, salt_hex, hash_hex);
    bool ok = (n > 0 && (size_t)n < sizeof(line) && file_write(fd, line, (uint64_t)n) == n);
    file_close(fd);
    return ok;
}

static bool hash_eq_ct(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return false;
    uint8_t d = 0;
    for (size_t i = 0; i < la; ++i) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static bool verify_password(const char *username, const char *password, uint32_t *uid_out) {
    char salt[96], stored[96], calc[96];
    if (!db_lookup(username, salt, sizeof(salt), stored, sizeof(stored))) return false;
    bool ok = crypto_hash_password_hex(password, salt, calc) == 0 && hash_eq_ct(stored, calc);
    if (!ok && crypto_hash_password_hex_legacy(password, salt, calc) == 0 && hash_eq_ct(stored, calc))
        ok = true;
    if (ok && uid_out) {
        for (uint32_t i = 0; i < g_user_count; ++i)
            if (strcmp(g_users[i].name, username) == 0) { *uid_out = g_users[i].uid; break; }
    }
    return ok;
}

static bool create_user(const char *username, const char *password, uint32_t *uid_out) {
    uint8_t salt_bytes[16];
    char salt_hex[33], hash_hex[65];
    crypto_generate_salt_bytes(salt_bytes, sizeof(salt_bytes));
    crypto_hex_encode(salt_bytes, sizeof(salt_bytes), salt_hex);
    if (crypto_hash_password_hex(password, salt_hex, hash_hex) < 0) return false;
    if (!db_add(username, salt_hex, hash_hex)) return false;
    db_load_users();
    if (uid_out) {
        *uid_out = 1000u;
        for (uint32_t i = 0; i < g_user_count; ++i)
            if (strcmp(g_users[i].name, username) == 0) { *uid_out = g_users[i].uid; break; }
    }
    return true;
}

/* ------------------------------------------------------------- session */

static void start_session(const char *username, uint32_t uid);

static bool read_small_file(const char *path, char *out, size_t out_sz) {
    file_stat_t st;
    if (file_stat(path, &st) < 0 || !st.exists || st.is_dir || st.size == 0 ||
        st.size >= out_sz)
        return false;
    int32_t fd = file_open(path, 0);
    if (fd < 0) return false;
    int64_t n = file_read(fd, out, out_sz - 1);
    file_close(fd);
    if (n <= 0) return false;
    out[n] = '\0';
    return true;
}

/*
 * Optional unattended login. Content is "name" (an existing user, password
 * skipped) or "name:password" (the account is created on first boot). The
 * writable copy under /var wins; a read-only seed can ship with the image.
 * Absent by default -> normal interactive login.
 */
static bool try_autologin(void) {
    char raw[192];
    if (!read_small_file("/var/System/autologin", raw, sizeof raw) &&
        !read_small_file("/Userland/com.ImplusOS.loginui/Resource/autologin.conf", raw, sizeof raw))
        return false;

    size_t n = strlen(raw);
    while (n > 0 && (raw[n - 1] == '\n' || raw[n - 1] == '\r' || raw[n - 1] == ' '))
        raw[--n] = '\0';
    if (raw[0] == '\0' || raw[0] == '#') return false;

    char name[64], pass[128];
    char *colon = strchr(raw, ':');
    if (colon) {
        size_t nl = (size_t)(colon - raw);
        if (nl == 0 || nl >= sizeof(name)) return false;
        memcpy(name, raw, nl); name[nl] = '\0';
        strncpy(pass, colon + 1, sizeof(pass) - 1); pass[sizeof(pass) - 1] = '\0';
    } else {
        strncpy(name, raw, sizeof(name) - 1); name[sizeof(name) - 1] = '\0';
        pass[0] = '\0';
    }

    uint32_t uid = 1000;
    bool user_exists = false;
    for (uint32_t i = 0; i < g_user_count; ++i)
        if (strcmp(g_users[i].name, name) == 0) { user_exists = true; uid = g_users[i].uid; break; }

    bool ok = false;
    if (user_exists) {
        ok = (pass[0] == '\0') || verify_password(name, pass, &uid);
    } else if (pass[0] != '\0') {
        ok = create_user(name, pass, &uid);
    }
    if (!ok) return false;

    serial_write_string("[loginui] autologin\n");
    start_session(name, uid); /* never returns */
    return true;
}

static void write_file_str(const char *path, const char *s) {
    int32_t fd = file_creat(path);
    if (fd < 0) fd = file_open(path, 1);
    if (fd < 0) return;
    file_write(fd, s, (uint64_t)strlen(s));
    file_close(fd);
}

static void start_session(const char *username, uint32_t uid) {
    (void)file_mkdir(SESSION_DIR);
    write_file_str(SESSION_USER, username);
    char n[16];
    snprintf(n, sizeof(n), "%u", uid);
    write_file_str(SESSION_UID, n);

    serial_write_string("[loginui] session for ");
    serial_write_string(username);
    serial_write_string(" uid=");
    serial_write_string(n);
    serial_write_string("\n");

    int32_t wm = process_spawn(WM_PATH);
    if (wm >= 0) process_set_priority(wm, 3);
    process_yield();
    { char b[48]; snprintf(b, sizeof(b), "[loginui] wm spawn -> %d\n", (int)wm);
      serial_write_string(b); }

    int32_t wm_pid = -1;
    for (int i = 0; i < 300; ++i) {
        wm_pid = window_get_wm_pid();
        if (wm_pid >= 0) break;
        sleep_ms(20);
    }
    { char b[48]; snprintf(b, sizeof(b), "[loginui] wm_pid=%d\n", (int)wm_pid);
      serial_write_string(b); }
    if (wm_pid >= 0) process_spawn(SYSNOTIF);

    for (;;) sleep_ms(1000);
}

/* ------------------------------------------------------------- input */

#define KEY_WORDS (65536u / 64u)
static uint64_t g_key_down[KEY_WORDS];
static bool key_edge(uint16_t kc, bool pressed) {
    uint64_t m = 1ULL << (kc & 63u);
    bool was = (g_key_down[kc >> 6] & m) != 0;
    if (pressed) g_key_down[kc >> 6] |= m; else g_key_down[kc >> 6] &= ~m;
    return pressed && !was;
}

typedef struct { int x, y; bool down, clicked; } mouse_t;
static void mouse_poll(mouse_t *m) {
    m->clicked = false;
    input_mouse_event_t ev;
    /* Raw mouse events are relative: the x/y fields are int16 deltas
     * (matches WindowManager's non-absolute path). Accumulate + clamp. */
    while (input_read_mouse(&ev) > 0) {
        m->x += (int)(int16_t)ev.x;
        m->y += (int)(int16_t)ev.y;
        if (m->x < 0) m->x = 0; else if (m->x > g_w - 1) m->x = g_w - 1;
        if (m->y < 0) m->y = 0; else if (m->y > g_h - 1) m->y = g_h - 1;
        bool now = (ev.buttons & INPUT_MOUSE_BTN_LEFT) != 0;
        if (now && !m->down) m->clicked = true;
        m->down = now;
    }
}

static void draw_cursor(int x, int y) {
    /* simple arrow: a filled triangle with a 1px dark outline */
    for (int j = 0; j < 18; ++j) {
        int span = 18 - j;
        if (j > 12) span = j - 6;
        if (span < 0) span = 0;
        for (int i = 0; i < span && i < 12; ++i) {
            px(x + i, y + j, 0xFFFFFF, 255);
        }
        px(x + span, y + j, 0x1A1A1A, 200);
    }
    for (int i = 0; i < 12; ++i) px(x + i, y, 0x1A1A1A, 200);
}

/* ------------------------------------------------------------- text field */

typedef struct {
    char   buf[128];
    size_t len;
    bool   hidden;
} field_t;

static void field_reset(field_t *f, bool hidden) { f->buf[0] = '\0'; f->len = 0; f->hidden = hidden; }

static void field_key(field_t *f, const input_keyboard_event_t *ev) {
    if (ev->ascii == 8 || ev->ascii == 127) {
        if (f->len > 0) f->buf[--f->len] = '\0';
    } else if (ev->ascii >= 32 && ev->ascii < 127 && f->len + 1 < sizeof(f->buf)) {
        f->buf[f->len++] = (char)ev->ascii;
        f->buf[f->len] = '\0';
    }
}

static void draw_field(int x, int y, int w, int h, const field_t *f, bool focus) {
    fill_round_rect(x, y, w, h, 8, 0x0B1220, 210);
    stroke_round_rect(x, y, w, h, 8, focus ? 0x4F86F7 : 0x33415A, focus ? 255 : 180);
    int tx = x + 14, ty = y + h / 2;
    if (f->hidden) {
        int dots = (int)f->len, dx = tx;
        for (int i = 0; i < dots && dx < x + w - 14; ++i, dx += 14)
            fill_circle(dx + 4, ty, 4, 0xE6EDF7, 255);
    } else {
        char tmp[130];
        strncpy(tmp, f->buf, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = '\0';
        draw_text_top(tx, y + (h - 15) / 2, tmp, 15.0f, 0xE6EDF7);
        if (focus) {
            int cw = text_width(tmp, scale_for(15.0f));
            fill_rect(tx + cw + 1, y + 8, 2, h - 16, 0x8AB4FF, 255);
        }
    }
}

/* ------------------------------------------------------------- screens */

typedef enum { SCR_PICK, SCR_PASSWORD, SCR_CREATE } screen_t;

int _start(void);

int _start(void) {
    graphics_init(0);
    g_fb = (uint32_t *)sys_get_display_framebuffer();
    g_w = (int)get_display_width();
    g_h = (int)get_display_height();
    if (!g_fb || g_w <= 0 || g_h <= 0) {
        serial_write_string("[loginui] no framebuffer\n");
        for (;;) sleep_ms(1000);
    }

    font_load();
    compose_wallpaper();
    db_load_users();

    {
        char dbg[64];
        snprintf(dbg, sizeof(dbg), "[loginui] up %dx%d users=%u font=%d\n",
                 g_w, g_h, g_user_count, (int)g_font_ok);
        serial_write_string(dbg);
    }

    if (try_autologin()) { /* never returns */ }

    screen_t scr = (g_user_count == 0) ? SCR_CREATE : SCR_PICK;
    int      sel = -1;                 /* selected user index for SCR_PASSWORD */
    field_t  fpw;   field_reset(&fpw, true);
    field_t  fname; field_reset(&fname, false);
    field_t  fpw2;  field_reset(&fpw2, true);
    int      create_focus = 0;         /* 0 name, 1 pw, 2 confirm */
    char     status[128]; status[0] = '\0';
    uint32_t fail_count = 0;
    uint64_t lock_until = 0;

    mouse_t mouse = { g_w / 2, g_h / 2, false, false };

    for (;;) {
        /* ---- input ---- */
        mouse_poll(&mouse);

        input_keyboard_event_t ev;
        while (input_read_keyboard(&ev) > 0) {
            if (!ev.pressed) { key_edge(ev.keycode, false); continue; }
            if (!key_edge(ev.keycode, true)) continue;

            if (scr == SCR_PASSWORD) {
                if (ev.ascii == 27) { scr = SCR_PICK; status[0] = '\0'; field_reset(&fpw, true); }
                else if (ev.ascii == '\r' || ev.ascii == '\n') {
                    if (get_uptime_ms() < lock_until) continue;
                    uint32_t uid = 1000;
                    if (verify_password(g_users[sel].name, fpw.buf, &uid)) {
                        paint_backdrop();
                        if (g_font_ok) {
                            char msg[96];
                            snprintf(msg, sizeof(msg), "ようこそ、%s さん", g_users[sel].name);
                            float sc = scale_for(26.0f);
                            draw_text((g_w - text_width(msg, sc)) / 2, g_h / 2, msg, sc, 0xFFFFFF);
                        }
                        draw_present();
                        start_session(g_users[sel].name, uid); /* never returns */
                    }
                    fail_count++;
                    uint32_t secs = 1u << (fail_count < 5 ? fail_count : 5);
                    lock_until = get_uptime_ms() + (uint64_t)secs * 1000u;
                    snprintf(status, sizeof(status), "パスワードが違います。%u 秒後に再試行できます。", secs);
                    field_reset(&fpw, true);
                } else field_key(&fpw, &ev);
            } else if (scr == SCR_CREATE) {
                if (ev.ascii == 27 && g_user_count > 0) { scr = SCR_PICK; status[0] = '\0'; }
                else if (ev.ascii == '\t') { create_focus = (create_focus + 1) % 3; }
                else if (ev.ascii == '\r' || ev.ascii == '\n') {
                    if (create_focus < 2) { create_focus++; }
                    else {
                        if (fname.len == 0 || strchr(fname.buf, ':') || strchr(fname.buf, '\n')) {
                            strncpy(status, "ユーザー名が無効です。", sizeof(status) - 1);
                        } else if (fpw.len == 0) {
                            strncpy(status, "パスワードを入力してください。", sizeof(status) - 1);
                        } else if (strcmp(fpw.buf, fpw2.buf) != 0) {
                            strncpy(status, "パスワードが一致しません。", sizeof(status) - 1);
                        } else {
                            uint32_t uid = 1000;
                            if (create_user(fname.buf, fpw.buf, &uid)) {
                                paint_backdrop();
                                draw_present();
                                start_session(fname.buf, uid); /* never returns */
                            }
                            strncpy(status, "ユーザー登録に失敗しました。", sizeof(status) - 1);
                        }
                    }
                } else {
                    field_t *tgt = create_focus == 0 ? &fname : (create_focus == 1 ? &fpw : &fpw2);
                    field_key(tgt, &ev);
                }
            } else { /* SCR_PICK */
                if (ev.ascii == '\r' || ev.ascii == '\n') {
                    if (g_user_count > 0) { sel = 0; scr = SCR_PASSWORD; field_reset(&fpw, true); }
                }
            }
        }

        /* ---- layout / hit-testing ---- */
        paint_backdrop();

        /* bottom-left user list (no panel behind the section) */
        const int row_h = 62, av_r = 22, list_x = 44;
        int list_bottom = g_h - 52;
        int hover_row = -1, add_hover = 0;

        if (scr == SCR_PICK || scr == SCR_PASSWORD) {
            for (uint32_t i = 0; i < g_user_count; ++i) {
                int ry = list_bottom - (int)(i + 1) * row_h;
                int rw = 300;
                bool hov = mouse.x >= list_x - 10 && mouse.x <= list_x - 10 + rw &&
                           mouse.y >= ry && mouse.y <= ry + row_h - 8;
                bool active = (scr == SCR_PASSWORD && (int)i == sel);
                if (hov || active) {
                    fill_round_rect(list_x - 10, ry, rw, row_h - 8, 12,
                                    active ? 0xFFFFFF : 0xFFFFFF, active ? 36 : 22);
                }
                draw_avatar(list_x + av_r, ry + (row_h - 8) / 2, av_r, accent_for(i),
                            g_users[i].name, 22.0f);
                if (g_font_ok)
                    draw_text_top(list_x + av_r * 2 + 16, ry + (row_h - 8) / 2 - 10,
                                  g_users[i].name, 17.0f, 0xF4F7FB);
                if (hov) hover_row = (int)i;
            }
            /* "add user" affordance under the list */
            int ay = list_bottom - (int)(g_user_count + 1) * row_h;
            bool ah = mouse.x >= list_x - 10 && mouse.x <= list_x - 10 + 300 &&
                      mouse.y >= ay && mouse.y <= ay + row_h - 8;
            if (ah) fill_round_rect(list_x - 10, ay, 300, row_h - 8, 12, 0xFFFFFF, 22);
            fill_circle(list_x + av_r, ay + (row_h - 8) / 2, av_r, 0x33415A, 255);
            if (g_font_ok) {
                float sc = scale_for(26.0f);
                draw_text(list_x + av_r - text_width("+", sc) / 2,
                          ay + (row_h - 8) / 2 + 9, "+", sc, 0xFFFFFF);
                draw_text_top(list_x + av_r * 2 + 16, ay + (row_h - 8) / 2 - 9,
                              "新規ユーザー", 16.0f, 0xC7D2E0);
            }
            add_hover = ah ? 1 : 0;
        }

        /* centred card for password / create */
        if (scr == SCR_PASSWORD || scr == SCR_CREATE) {
            int cw = 420, ch = (scr == SCR_CREATE) ? 400 : 300;
            int cx = (g_w - cw) / 2, cy = (g_h - ch) / 2;
            fill_round_rect(cx, cy, cw, ch, 18, 0x0E1526, 235);
            stroke_round_rect(cx, cy, cw, ch, 18, 0x2A3A55, 200);

            if (scr == SCR_PASSWORD) {
                draw_avatar(cx + cw / 2, cy + 66, 34, accent_for((uint32_t)sel),
                            g_users[sel].name, 30.0f);
                if (g_font_ok) {
                    float ns = scale_for(21.0f);
                    draw_text(cx + (cw - text_width(g_users[sel].name, ns)) / 2, cy + 138,
                              g_users[sel].name, ns, 0xFFFFFF);
                    draw_text_top(cx + 40, cy + 158, "パスワード", 12.0f, 0x8AA0BC);
                }
                draw_field(cx + 40, cy + 178, cw - 80, 40, &fpw, true);
                if (g_font_ok)
                    draw_text_top(cx + 40, cy + 232, "Enter でログイン / Esc で戻る", 11.0f, 0x6B7E99);
                if (status[0] && g_font_ok)
                    draw_text_top(cx + 40, cy + 256, status, 11.0f, 0xF08A8A);
            } else {
                if (g_font_ok) {
                    float ts = scale_for(20.0f);
                    draw_text(cx + (cw - text_width("ようこそ ImplusOS へ", ts)) / 2, cy + 46,
                              "ようこそ ImplusOS へ", ts, 0xFFFFFF);
                    draw_text_top(cx + 40, cy + 66, "最初のユーザーを作成します", 12.0f, 0x8AA0BC);

                    draw_text_top(cx + 40, cy + 104, "ユーザー名", 11.0f, 0x8AA0BC);
                }
                draw_field(cx + 40, cy + 122, cw - 80, 38, &fname, create_focus == 0);
                if (g_font_ok) draw_text_top(cx + 40, cy + 176, "パスワード", 11.0f, 0x8AA0BC);
                draw_field(cx + 40, cy + 194, cw - 80, 38, &fpw, create_focus == 1);
                if (g_font_ok) draw_text_top(cx + 40, cy + 248, "パスワード（確認）", 11.0f, 0x8AA0BC);
                draw_field(cx + 40, cy + 266, cw - 80, 38, &fpw2, create_focus == 2);

                int by = cy + 320;
                bool bhov = mouse.x >= cx + 40 && mouse.x <= cx + cw - 40 &&
                            mouse.y >= by && mouse.y <= by + 40;
                fill_round_rect(cx + 40, by, cw - 80, 40, 10, bhov ? 0x5B90F8 : 0x4F86F7, 255);
                if (g_font_ok)
                    draw_text(cx + (cw - text_width("作成してログイン", scale_for(14.0f))) / 2,
                              by + 26, "作成してログイン", scale_for(14.0f), 0xFFFFFF);
                if (status[0] && g_font_ok)
                    draw_text_top(cx + 40, cy + 366, status, 11.0f, 0xF08A8A);

                if (mouse.clicked && bhov) {
                    /* synthesize an Enter on the confirm field */
                    input_keyboard_event_t k = {0};
                    k.pressed = 1; k.ascii = '\r';
                    create_focus = 2;
                    if (fname.len == 0) strncpy(status, "ユーザー名が無効です。", sizeof(status) - 1);
                    else if (fpw.len == 0) strncpy(status, "パスワードを入力してください。", sizeof(status) - 1);
                    else if (strcmp(fpw.buf, fpw2.buf) != 0) strncpy(status, "パスワードが一致しません。", sizeof(status) - 1);
                    else {
                        uint32_t uid = 1000;
                        if (create_user(fname.buf, fpw.buf, &uid)) {
                            paint_backdrop(); draw_present();
                            start_session(fname.buf, uid);
                        }
                        strncpy(status, "ユーザー登録に失敗しました。", sizeof(status) - 1);
                    }
                    (void)k;
                }

                /* field focus by click */
                if (mouse.clicked) {
                    if (mouse.y >= cy + 122 && mouse.y < cy + 160) create_focus = 0;
                    else if (mouse.y >= cy + 194 && mouse.y < cy + 232) create_focus = 1;
                    else if (mouse.y >= cy + 266 && mouse.y < cy + 304) create_focus = 2;
                }
            }

            /* click outside the card (password screen) -> back to picker */
            if (scr == SCR_PASSWORD && mouse.clicked &&
                (mouse.x < cx || mouse.x > cx + cw || mouse.y < cy || mouse.y > cy + ch) &&
                !(mouse.x < list_x - 10 + 300 && mouse.x > list_x - 40)) {
                scr = SCR_PICK; status[0] = '\0'; field_reset(&fpw, true);
            }
        }

        /* clicks on the user list */
        if (mouse.clicked && (scr == SCR_PICK || scr == SCR_PASSWORD)) {
            if (hover_row >= 0) {
                sel = hover_row; scr = SCR_PASSWORD; status[0] = '\0';
                fail_count = 0; lock_until = 0; field_reset(&fpw, true);
            } else if (add_hover) {
                scr = SCR_CREATE; create_focus = 0; status[0] = '\0';
                field_reset(&fname, false); field_reset(&fpw, true); field_reset(&fpw2, true);
            }
        }

        /* header */
        if (g_font_ok && scr == SCR_PICK) {
            const char *h1 = "サインイン";
            draw_text(64, 96, h1, scale_for(30.0f), 0xFFFFFF);
            draw_text_top(64, 112, "ユーザーを選択してください", 13.0f, 0xC7D2E0);
        }

        draw_cursor(mouse.x, mouse.y);
        draw_present();
        sleep_ms(16);
    }
}
