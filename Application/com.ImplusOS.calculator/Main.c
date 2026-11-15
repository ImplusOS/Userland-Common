/*
 * com.ImplusOS.calculator -- a small expression calculator.
 *
 *   +----------------------------------------------+
 *   | 12 + 3 * (4 - 1)                            |   <- editable
 *   +----------------------------------------------+
 *   |  C  BS  (  )  |  7 8 9 /  |  4 5 6 *  | ... |
 *   |  =                                             |
 *   +----------------------------------------------+
 *   | 12 + 3 * (4 - 1) = 21                        |
 *
 * The line being edited is the expression itself: type it, press Enter or
 * "=", and the result replaces it while the full sum is kept on the status
 * line, so a chain of calculations stays readable. The keypad is only the
 * other way to write the same characters -- everything accepts the keyboard
 * directly, because the expression box keeps the focus.
 *
 * Grammar (recursive descent, in parse_* below):
 *
 *   expr   := term (('+' | '-') term)*
 *   term   := factor (('*' | '/' | '%') factor)*
 *   factor := ('+' | '-') factor | '(' expr ')' | number
 *
 * `%` is the remainder operator (fmod), not percent: a percent that has to
 * know what it is a percent *of* cannot be evaluated left to right, and a
 * calculator that silently guesses is worse than one that does not offer it.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "UI.h"
#include "Serial.h"

static ui_app_t    *g_app;
static ui_widget_t *w_expr, *w_history, *w_status;

/* ------------------------------------------------------------- parser */

typedef struct {
    const char *p;
    const char *err;      /* NULL while the expression is still plausible */
} parser_t;

static void skip_ws(parser_t *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t') ps->p++;
}

static double parse_expr(parser_t *ps);

static double parse_factor(parser_t *ps)
{
    skip_ws(ps);
    if (ps->err) return 0.0;

    if (*ps->p == '(') {
        ps->p++;
        double v = parse_expr(ps);
        skip_ws(ps);
        if (ps->err) return 0.0;
        if (*ps->p != ')') { ps->err = "閉じ括弧がありません"; return 0.0; }
        ps->p++;
        return v;
    }
    if (*ps->p == '+' || *ps->p == '-') {
        char op = *ps->p;
        ps->p++;
        double v = parse_factor(ps);
        return op == '-' ? -v : v;
    }

    char *end = NULL;
    double v = strtod(ps->p, &end);
    if (end == ps->p) { ps->err = "数値が必要です"; return 0.0; }
    ps->p = end;
    return v;
}

static double parse_term(parser_t *ps)
{
    double v = parse_factor(ps);
    for (;;) {
        skip_ws(ps);
        if (ps->err) return 0.0;
        char op = *ps->p;
        if (op != '*' && op != '/' && op != '%') break;
        ps->p++;
        double r = parse_factor(ps);
        if (ps->err) return 0.0;
        if (op == '*') {
            v *= r;
        } else if (r == 0.0) {
            ps->err = "0 で割ることはできません";
            return 0.0;
        } else if (op == '/') {
            v /= r;
        } else {
            v = fmod(v, r);
        }
    }
    return v;
}

static double parse_expr(parser_t *ps)
{
    double v = parse_term(ps);
    for (;;) {
        skip_ws(ps);
        if (ps->err) return 0.0;
        char op = *ps->p;
        if (op != '+' && op != '-') break;
        ps->p++;
        double r = parse_term(ps);
        if (ps->err) return 0.0;
        v = (op == '+') ? v + r : v - r;
    }
    return v;
}

/* ------------------------------------------------------------- display */

static void note(const char *text)
{
    ui_set_text(w_status, text);
    ui_redraw();
}

/* An integral result reads better as 21 than as 21.000000000, and a
 * fractional one keeps ten digits, which is as much of 1/3 as anyone
 * meant to type. The range test has to come first: casting a double that
 * does not fit in a long long is undefined, so `&&` order is load-bearing. */
static void format_result(double v, char *out, size_t cap)
{
    if (v > -1e15 && v < 1e15 && v == (double)(long long)v)
        snprintf(out, cap, "%lld", (long long)v);
    else
        snprintf(out, cap, "%.10g", v);
}

/* Checked before anything is formatted. This libc's %g normalises a value
 * by dividing it down to one digit, and infinity divided by ten is still
 * infinity -- printing an out-of-range result would spin forever rather
 * than show it, so the range is refused here where it can be reported. */
static bool out_of_range(double v)
{
    return __builtin_isnan(v) || __builtin_isinf(v);
}

static bool evaluate(void)
{
    const char *text = ui_get_text(w_expr);
    parser_t ps = { text, NULL };

    skip_ws(&ps);
    if (*ps.p == '\0') { note("式を入力してください"); return false; }

    double v = parse_expr(&ps);
    if (!ps.err) {
        skip_ws(&ps);
        if (*ps.p != '\0') ps.err = "式の終わりまで読めません";
    }
    if (ps.err) { note(ps.err); return false; }

    if (out_of_range(v)) {
        char line[256];
        snprintf(line, sizeof(line), "%s = 範囲外です", text);
        note(line);
        return false;
    }

    char result[64];
    format_result(v, result, sizeof(result));

    /* The expression goes to the status line and the box takes the answer,
     * so the next key can carry on from it: "12 + 3" -> 15, then "+ 1"
     * makes 16. */
    char line[256];
    snprintf(line, sizeof(line), "%s = %s", text, result);
    ui_set_text(w_history, line);
    ui_set_text(w_expr, result);
    ui_redraw();
    return true;
}

/* ------------------------------------------------------------- keypad */

/* The toolkit drops the focus when any button is pressed, which would mean one
 * click on the keypad and the keyboard stops reaching the expression box.
 * Every keypad action therefore hands the focus straight back. */
static void focus_expr(void)
{
    ui_focus(w_expr);
}

static void expr_append(const char *s)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s%s", ui_get_text(w_expr), s);
    ui_set_text(w_expr, buf);
    focus_expr();
    ui_redraw();
}

static void on_backspace(ui_widget_t *w)
{
    (void)w;
    const char *s = ui_get_text(w_expr);
    size_t n = strlen(s);
    if (n == 0) { focus_expr(); return; }
    char buf[512];
    memcpy(buf, s, n - 1u);
    buf[n - 1u] = '\0';
    ui_set_text(w_expr, buf);
    focus_expr();
    ui_redraw();
}

static void on_key_press(ui_widget_t *w)
{
    expr_append((const char *)w->user);
}

static void on_clear(ui_widget_t *w)
{
    (void)w;
    ui_set_text(w_expr, "");
    ui_set_text(w_history, "");
    focus_expr();
    note("クリアしました");
}

static void on_equals(ui_widget_t *w)
{
    (void)w;
    (void)evaluate();
    focus_expr();
}

/* "C" and "BS" on the edit row are actions rather than characters, so they
 * do not go through on_key_press. */
static void on_edit_key(ui_widget_t *w)
{
    const char *key = (const char *)w->user;
    if (key[0] == 'C' && key[1] == '\0') on_clear(w);
    else on_backspace(w);
}

/* Enter reaches this through the textbox's on_action; '=' would otherwise
 * just be typed into the box, so the app takes it first. */
static bool on_key(const input_keyboard_event_t *ev)
{
    if ((ev->modifiers & INPUT_KBD_MOD_CTRL) != 0u) return false;
    if (ev->ascii == '=') { (void)evaluate(); return true; }
    return false;
}

/* One keypad row: every button takes the same share of the width, which is
 * what makes the columns line up without measuring anything.
 *
 * The height has to be requested on the *row*, not on its buttons: a
 * container measures as 0 unless it sets min_h, and a 0-height row hands
 * its buttons a 0-height box, which draws five rows of labels on top of
 * one another. */
#define KEYPAD_ROW_H 52

static void keypad_row(ui_widget_t *parent, const char *const *keys, uint32_t n,
                       ui_cb_t cb)
{
    ui_widget_t *row = ui_row(parent, 0, 6);
    row->grow = 0; row->min_h = KEYPAD_ROW_H;
    for (uint32_t i = 0; i < n; ++i) {
        ui_widget_t *b = ui_button(row, keys[i], UI_ICON_NONE,
                                   cb, (void *)(uintptr_t)keys[i]);
        b->grow = 1;
        b->min_h = KEYPAD_ROW_H;
    }
}

int _start(void);
int _start(void)
{
    serial_write_string("[calculator] start\n");

    g_app = ui_create(460, 660, "Calculator");
    if (!g_app) {
        serial_write_string("[calculator] no window\n");
        for (;;) {}
    }

    ui_widget_t *root = g_app->root;

    ui_widget_t *header = ui_bar(root, 64);
    header->pad = 16; header->gap = 8;
    ui_widget_t *title = ui_title(header, "計算機");
    title->grow = 1;

    ui_widget_t *body = ui_col(root, 12, 10);
    body->grow = 1;

    w_expr = ui_textbox(body, "");
    w_expr->grow = 0;
    w_expr->min_h = 64;
    w_expr->on_action = on_equals;

    w_history = ui_caption(body, "");
    w_history->grow = 0; w_history->min_h = 22;

    ui_widget_t *pad = ui_col(body, 0, 6);
    pad->grow = 1; pad->min_h = 0;

    /* Keypad, in the order every calculator has it: clear and the editing
     * keys on top, digits in a 3x3 block with the operators down the right
     * edge, "%" beside the last digit row, "=" full width underneath. */
    static const char *const row_edit[] = { "C",  "BS", "(",  ")" };
    static const char *const row_789[]  = { "7",  "8",  "9",  "/" };
    static const char *const row_456[]  = { "4",  "5",  "6",  "*" };
    static const char *const row_123[]  = { "1",  "2",  "3",  "-" };
    static const char *const row_0dot[] = { "0",  ".",  "%",  "+" };

    keypad_row(pad, row_edit, 4, on_edit_key);
    keypad_row(pad, row_789,  4, on_key_press);
    keypad_row(pad, row_456,  4, on_key_press);
    keypad_row(pad, row_123,  4, on_key_press);
    keypad_row(pad, row_0dot, 4, on_key_press);

    ui_widget_t *eq = ui_button(pad, "=", UI_ICON_NONE, on_equals, NULL);
    eq->primary = true;
    eq->grow = 0; eq->min_h = 56;

    ui_widget_t *sb = ui_bar(root, 32);
    sb->pad = 12; sb->gap = 0;
    w_status = ui_caption(sb, "Enter か = で計算します");
    w_status->grow = 1;

    g_app->on_key = on_key;
    /* The expression box is what the window is for, so it starts focused:
     * typing a digit before clicking anything has to work. */
    ui_focus(w_expr);

    ui_run();
}
