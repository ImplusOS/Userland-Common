/*
 * Host harness for the serial monitor's capture ring (LogRing.c).
 *
 * LogRing has no UI and no syscalls in it precisely so this can build with the
 * host gcc and exercise the parts that are awkward to see through a QEMU
 * boot: a line split across two reads, a line longer than the buffer, the
 * ring wrapping, and which lines a filtered view ends up showing.
 *
 *   ./Tests/run.sh
 */
#include <stdio.h>
#include <string.h>

#include "../LogRing.h"

static int g_fail;

static void check(const char *what, int ok)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) g_fail++;
}

static void feed_str(log_ring_t *r, const char *s)
{
    logring_feed(r, s, (uint32_t)strlen(s));
}

static log_ring_t g_ring;

int main(void)
{
    log_ring_t *r = &g_ring;

    /* 1. Whole lines in one chunk. */
    logring_reset(r);
    feed_str(r, "alpha\nbravo\n");
    check("two complete lines are captured", r->count == 2);
    check("oldest line first", strcmp(logring_line(r, 0), "alpha") == 0);
    check("newest line last", strcmp(logring_line(r, 1), "bravo") == 0);

    /* 2. A line split across reads is one line, not two. The device hands
     *    back whatever is in the ring when the read happens, so this is the
     *    normal case, not the edge case. */
    logring_reset(r);
    feed_str(r, "[ahci] port 0 ");
    check("no line until the newline arrives", r->count == 0);
    feed_str(r, "link up\n");
    check("split line is rejoined",
          r->count == 1 && strcmp(logring_line(r, 0), "[ahci] port 0 link up") == 0);

    /* 3. CRLF and trailing padding do not become visible columns. */
    logring_reset(r);
    feed_str(r, "crlf line\r\n");
    check("trailing CR is stripped", strcmp(logring_line(r, 0), "crlf line") == 0);
    logring_reset(r);
    feed_str(r, "padded   \n");
    check("trailing spaces are stripped", strcmp(logring_line(r, 0), "padded") == 0);

    /* 4. Control bytes are made printable rather than handed to the font. */
    logring_reset(r);
    feed_str(r, "a\tb\x01\x7f" "c\n");
    check("tab becomes a space, control bytes become dots",
          strcmp(logring_line(r, 0), "a b..c") == 0);

    /* 5. An over-long line wraps onto the next one; nothing is dropped. */
    logring_reset(r);
    {
        char big[LOG_LINE_MAX * 2];
        for (size_t i = 0; i < sizeof(big) - 1; ++i) big[i] = 'x';
        big[sizeof(big) - 1] = '\0';
        feed_str(r, big);
        feed_str(r, "\n");
        size_t total = 0;
        for (uint32_t i = 0; i < r->count; ++i) total += strlen(logring_line(r, i));
        check("over-long line wraps instead of truncating",
              r->count == 3 && total == sizeof(big) - 1);
    }

    /* 6. The ring evicts the oldest line and keeps counting. */
    logring_reset(r);
    for (uint32_t i = 0; i < LOG_MAX_LINES + 10u; ++i) {
        char line[32];
        snprintf(line, sizeof(line), "line %u\n", i);
        feed_str(r, line);
    }
    check("ring holds at most LOG_MAX_LINES", r->count == LOG_MAX_LINES);
    check("captured counts evicted lines too",
          r->captured == LOG_MAX_LINES + 10u);
    check("oldest survivor is the 11th line",
          strcmp(logring_line(r, 0), "line 10") == 0);
    check("newest is the last fed",
          strcmp(logring_line(r, r->count - 1), "line 1033") == 0);

    /* 7. Filtering is a case-insensitive substring test. */
    check("empty filter matches", logring_match("anything", ""));
    check("NULL filter matches", logring_match("anything", NULL));
    check("substring matches", logring_match("[ahci] port 0", "ahci"));
    check("case is ignored", logring_match("[AHCI] port 0", "ahci"));
    check("non-match is rejected", !logring_match("[ahci] port 0", "nvme"));
    check("prefix of a longer needle does not match",
          !logring_match("ahc", "ahci"));

    /* 8. The view renders the newest matching lines, capped. */
    logring_reset(r);
    feed_str(r, "[pci] one\n[ahci] two\n[pci] three\n[nvme] four\n");
    {
        char out[4096];
        uint32_t shown = 0;
        uint32_t n = logring_render(r, "pci", NULL, 10, out, sizeof(out), &shown);
        check("filter selects only matching lines",
              shown == 2 && strcmp(out, "[pci] one\n[pci] three\n") == 0);
        check("returned length matches the text", n == strlen(out));

        n = logring_render(r, "", NULL, 2, out, sizeof(out), &shown);
        check("cap keeps the newest lines, not the oldest",
              shown == 2 && strcmp(out, "[pci] three\n[nvme] four\n") == 0);
        (void)n;

        /* A buffer too small must truncate on a line boundary and stay
         * NUL-terminated -- the pane renders whatever it gets. */
        char small[12];
        n = logring_render(r, "", NULL, 10, small, sizeof(small), &shown);
        check("small buffer truncates safely",
              n <= sizeof(small) - 1 && small[n] == '\0' &&
              (n == 0 || small[n - 1] == '\n'));

        n = logring_render(r, "zzz", NULL, 10, out, sizeof(out), &shown);
        check("a filter matching nothing renders an empty pane",
              shown == 0 && n == 0 && out[0] == '\0');

        /* Both needles have to match: the view filter ("apps" = the [app]
         * lines) narrows what the typed filter already selected. */
        logring_reset(r);
        feed_str(r, "[app] xterm exec\n[pci] 8086\n[app] xterm exit\n");
        n = logring_render(r, NULL, "[app]", 10, out, sizeof(out), &shown);
        check("view filter alone selects its lines", shown == 2);
        n = logring_render(r, "exit", "[app]", 10, out, sizeof(out), &shown);
        check("both filters are ANDed",
              shown == 1 && strcmp(out, "[app] xterm exit\n") == 0);
        n = logring_render(r, "8086", "[app]", 10, out, sizeof(out), &shown);
        check("a line matching only one filter is dropped", shown == 0);
        (void)n;
    }

    /* 9. Reset really clears, including a half-received line. */
    logring_reset(r);
    feed_str(r, "half a li");
    logring_reset(r);
    feed_str(r, "ne\n");
    check("reset drops the partial line",
          r->count == 1 && strcmp(logring_line(r, 0), "ne") == 0);

    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail ? 1 : 0;
}
