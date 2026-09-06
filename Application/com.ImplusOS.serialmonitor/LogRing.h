#pragma once

/*
 * LogRing -- the capture side of the serial monitor, with no UI and no
 * syscalls in it, so the host harness in Tests/ can drive it directly.
 *
 * Bytes arrive from /dev/kmsg in whatever chunks the read returns: a chunk
 * can end mid-line and a line can span several chunks, so the split is
 * stateful (`partial`). Completed lines go into a fixed ring; when it is
 * full the oldest line falls off the back, which is the same thing the
 * kernel's own log ring does to a slow reader.
 */

#include <stdint.h>
#include <stdbool.h>

#define LOG_LINE_MAX   192u    /* a longer line wraps onto the next one */
#define LOG_MAX_LINES 1024u    /* ~192 KiB of captured backlog */

typedef struct {
    char     lines[LOG_MAX_LINES][LOG_LINE_MAX];
    uint32_t head;             /* index of the oldest live line */
    uint32_t count;            /* lines currently held */
    uint64_t captured;         /* lines ever captured, including evicted ones */
    char     partial[LOG_LINE_MAX];
    uint32_t partial_len;
} log_ring_t;

/* Drop everything, including a half-received line. */
void logring_reset(log_ring_t *r);

/* Absorb `len` bytes of raw device output. */
void logring_feed(log_ring_t *r, const char *data, uint32_t len);

/* Line `i` counting from the oldest (0 <= i < r->count), or "" out of range. */
const char *logring_line(const log_ring_t *r, uint32_t i);

/* Case-insensitive substring test; an empty or NULL needle matches anything. */
bool logring_match(const char *line, const char *needle);

/*
 * Renders the newest `max_lines` lines matching BOTH needles into `out` as
 * newline-terminated text, and stores how many were written in `*out_shown`
 * (may be NULL). Returns the number of bytes written, excluding the
 * terminating NUL. Truncates rather than overflowing `out`.
 *
 * Two needles because there are two independent filters: what the user typed,
 * and what the chosen view selects (the "apps" view is the "[app]" lines).
 * Either may be NULL or empty, which matches everything.
 */
uint32_t logring_render(const log_ring_t *r, const char *needle_a,
                        const char *needle_b, uint32_t max_lines, char *out,
                        uint32_t out_size, uint32_t *out_shown);
