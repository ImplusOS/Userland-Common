#pragma once

#include <stdint.h>

/*
 * ImplusOS Application UI Theme — Catppuccin Mocha
 *
 * Unified color palette for all com.ImplusOS.* applications.
 * Based on the Catppuccin Mocha theme (https://catppuccin.com).
 *
 * Usage: #include this header in your app's main .c file.
 *
 * Color format: 0xAARRGGBB (32-bit ARGB)
 */

/* ── Base ─────────────────────────────────────────────────────────── */
#define THEME_BASE        0xFF1E1E2E
#define THEME_MANTLE      0xFF181825
#define THEME_CRUST       0xFF11111B

/* ── Surface ──────────────────────────────────────────────────────── */
#define THEME_SURFACE0    0xFF313244
#define THEME_SURFACE1    0xFF45475A
#define THEME_SURFACE2    0xFF585B70
#define THEME_OVERLAY0    0xFF6C7086
#define THEME_OVERLAY1    0xFF7F849C
#define THEME_OVERLAY2    0xFF9399B2

/* ── Text ─────────────────────────────────────────────────────────── */
#define THEME_TEXT         0xFFCDD6F4
#define THEME_SUBTEXT1     0xFFBAC2DE
#define THEME_SUBTEXT0     0xFFA6ADC8

/* ── Accent ───────────────────────────────────────────────────────── */
#define THEME_BLUE        0xFF89B4FA
#define THEME_LAVENDER    0xFFB4BEFE
#define THEME_SAPPHIRE    0xFF74C7EC
#define THEME_SKY         0xFF89DCEB
#define THEME_TEAL        0xFF94E2D5
#define THEME_GREEN       0xFFA6E3A1
#define THEME_YELLOW      0xFFF9E2AF
#define THEME_PEACH       0xFFFAB387
#define THEME_MAROON      0xFFEBA0AC
#define THEME_RED         0xFFF38BA8
#define THEME_MAUVE       0xFFCBA6F7
#define THEME_PINK        0xFFF5C2E7
#define THEME_FLAMINGO    0xFFF2CDCD
#define THEME_ROSEWATER   0xFFF5E0DC

/* ── Semantic shortcuts ───────────────────────────────────────────── */
#define THEME_BG          THEME_BASE
#define THEME_FG          THEME_TEXT
#define THEME_DIM         THEME_OVERLAY0
#define THEME_ACCENT      THEME_BLUE
#define THEME_HIGHLIGHT   THEME_SURFACE1
#define THEME_BORDER      THEME_SURFACE0
#define THEME_SELECTED    THEME_SURFACE1
#define THEME_WARN        THEME_YELLOW
#define THEME_GOOD        THEME_GREEN
#define THEME_BAD         THEME_RED
