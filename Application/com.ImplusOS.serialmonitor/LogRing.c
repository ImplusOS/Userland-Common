/* LogRing -- see LogRing.h. */

#include "LogRing.h"

#include <string.h>

void logring_reset(log_ring_t *r)
{
    if (!r) return;
    r->head = r->count = r->partial_len = 0u;
    r->captured = 0u;
    r->partial[0] = '\0';
}

static void ring_push(log_ring_t *r, const char *line)
{
    uint32_t slot;
    if (r->count == LOG_MAX_LINES) {
        slot = r->head;                              /* evict the oldest */
        r->head = (r->head + 1u) % LOG_MAX_LINES;
    } else {
        slot = (r->head + r->count) % LOG_MAX_LINES;
        r->count++;
    }
    strncpy(r->lines[slot], line, LOG_LINE_MAX - 1u);
    r->lines[slot][LOG_LINE_MAX - 1u] = '\0';
    r->captured++;
}

static void commit(log_ring_t *r)
{
    /* Some paths pad a line out with spaces; that is not worth a column. */
    while (r->partial_len > 0u && r->partial[r->partial_len - 1u] == ' ') {
        r->partial_len--;
    }
    r->partial[r->partial_len] = '\0';
    ring_push(r, r->partial);
    r->partial_len = 0u;
    r->partial[0] = '\0';
}

void logring_feed(log_ring_t *r, const char *data, uint32_t len)
{
    if (!r || !data) return;

    for (uint32_t i = 0; i < len; ++i) {
        char c = data[i];
        if (c == '\n') { commit(r); continue; }
        /* Dropped, not turned into a glyph: the kernel writes "\r\n" on some
         * paths, and a lone CR only ever means "redraw this line" to a
         * terminal -- neither is something this pane can show. */
        if (c == '\r') continue;
        if (c == '\t') c = ' ';
        /* The log carries the odd control byte (a panic dump, a stray NUL from
         * a driver). Keep the pane printable instead of feeding the renderer
         * bytes its font has no glyph for. */
        if (c < 32 || c > 126) c = '.';
        if (r->partial_len + 1u >= LOG_LINE_MAX) {
            commit(r);                               /* wrap, never truncate */
        }
        r->partial[r->partial_len++] = c;
    }
}

const char *logring_line(const log_ring_t *r, uint32_t i)
{
    if (!r || i >= r->count) return "";
    return r->lines[(r->head + i) % LOG_MAX_LINES];
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

bool logring_match(const char *line, const char *needle)
{
    if (!needle || !needle[0]) return true;
    if (!line) return false;
    for (const char *s = line; *s; ++s) {
        const char *a = s, *b = needle;
        while (*b && lower(*a) == lower(*b)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}

uint32_t logring_render(const log_ring_t *r, const char *needle_a,
                        const char *needle_b, uint32_t max_lines, char *out,
                        uint32_t out_size, uint32_t *out_shown)
{
    if (out_shown) *out_shown = 0u;
    if (!out || out_size == 0u) return 0u;
    out[0] = '\0';
    if (!r) return 0u;

    /* Count the matches first: only the newest `max_lines` are rendered, so
     * the pane stays a screenful-plus-scrollback rather than the whole ring
     * (the paint pass walks the buffer every frame). */
    uint32_t matched = 0u;
    for (uint32_t i = 0; i < r->count; ++i) {
        const char *line = logring_line(r, i);
        if (logring_match(line, needle_a) && logring_match(line, needle_b)) {
            matched++;
        }
    }
    uint32_t skip = (matched > max_lines) ? matched - max_lines : 0u;

    uint32_t used = 0u, seen = 0u, shown = 0u;
    for (uint32_t i = 0; i < r->count; ++i) {
        const char *line = logring_line(r, i);
        if (!logring_match(line, needle_a) || !logring_match(line, needle_b)) {
            continue;
        }
        if (seen++ < skip) continue;
        uint32_t len = (uint32_t)strlen(line);
        if (used + len + 2u > out_size) break;       /* +1 '\n', +1 NUL */
        memcpy(out + used, line, len);
        used += len;
        out[used++] = '\n';
        shown++;
    }
    out[used] = '\0';
    if (out_shown) *out_shown = shown;
    return used;
}
