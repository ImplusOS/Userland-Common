/*
 * Host-compiled checks for the Material Design 3 token module.
 *
 * The interesting claim this file makes is that the tonal palettes really
 * are M3's: the generator works in LCh(ab) rather than HCT, so the tones it
 * produces are compared against the published baseline scheme with a
 * per-channel tolerance instead of for equality.
 */

#include "Material.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t channel(uint32_t argb, uint32_t shift)
{
    return (argb >> shift) & 0xFFu;
}

static void expect_near(const char *what, uint32_t got, uint32_t want,
                        uint32_t tolerance)
{
    uint32_t worst = 0u;
    for (uint32_t shift = 0u; shift <= 16u; shift += 8u) {
        uint32_t a = channel(got, shift);
        uint32_t b = channel(want, shift);
        uint32_t delta = a > b ? a - b : b - a;
        if (delta > worst) worst = delta;
    }
    if (worst > tolerance) {
        printf("%s: got #%06X, want #%06X (off by %u, tolerance %u)\n",
               what, got & 0x00FFFFFFu, want & 0x00FFFFFFu, worst, tolerance);
        abort();
    }
    assert((got >> 24u) == 0xFFu);
}

/* The M3 baseline light scheme, from the specification. */
static void test_baseline_light_scheme(void)
{
    m3_scheme_t scheme;
    m3_scheme_init(&scheme, M3_SEED_BASELINE, false);

    expect_near("primary",                scheme.primary,                0x6750A4u, 2u);
    expect_near("on_primary",             scheme.on_primary,             0xFFFFFFu, 0u);
    expect_near("primary_container",      scheme.primary_container,      0xEADDFFu, 4u);
    expect_near("on_primary_container",   scheme.on_primary_container,   0x21005Du, 14u);
    expect_near("secondary",              scheme.secondary,              0x625B71u, 3u);
    expect_near("secondary_container",    scheme.secondary_container,    0xE8DEF8u, 4u);
    expect_near("tertiary",               scheme.tertiary,               0x7D5260u, 8u);
    expect_near("tertiary_container",     scheme.tertiary_container,     0xFFD8E4u, 8u);
    expect_near("error",                  scheme.error,                  0xB3261Eu, 0u);
    expect_near("error_container",        scheme.error_container,        0xF9DEDCu, 0u);
    expect_near("surface",                scheme.surface,                0xFEF7FFu, 4u);
    expect_near("on_surface",             scheme.on_surface,             0x1D1B20u, 2u);
    expect_near("surface_variant",        scheme.surface_variant,        0xE7E0ECu, 4u);
    expect_near("on_surface_variant",     scheme.on_surface_variant,     0x49454Fu, 2u);
    expect_near("outline",                scheme.outline,                0x79747Eu, 4u);
    expect_near("outline_variant",        scheme.outline_variant,        0xCAC4D0u, 4u);
    expect_near("inverse_primary",        scheme.inverse_primary,        0xD0BCFFu, 4u);
    expect_near("shadow",                 scheme.shadow,                 0x000000u, 2u);

    /* The container ramp has to stay ordered, or elevation reads backwards. */
    assert(channel(scheme.surface_container_lowest, 8u) >
           channel(scheme.surface_container_low, 8u));
    assert(channel(scheme.surface_container_low, 8u) >
           channel(scheme.surface_container, 8u));
    assert(channel(scheme.surface_container, 8u) >
           channel(scheme.surface_container_high, 8u));
    assert(channel(scheme.surface_container_high, 8u) >
           channel(scheme.surface_container_highest, 8u));
    assert(!m3_scheme_is_dark(&scheme));
}

static void test_baseline_dark_scheme(void)
{
    m3_scheme_t scheme;
    m3_scheme_init(&scheme, M3_SEED_BASELINE, true);

    expect_near("dark primary",              scheme.primary,              0xD0BCFFu, 4u);
    expect_near("dark on_primary",           scheme.on_primary,           0x381E72u, 12u);
    expect_near("dark primary_container",    scheme.primary_container,    0x4F378Bu, 12u);
    expect_near("dark on_primary_container", scheme.on_primary_container, 0xEADDFFu, 4u);
    expect_near("dark error",                scheme.error,                0xF2B8B5u, 0u);
    expect_near("dark surface",              scheme.surface,              0x141218u, 4u);
    expect_near("dark on_surface",           scheme.on_surface,           0xE6E0E9u, 4u);
    expect_near("dark outline",              scheme.outline,              0x938F99u, 4u);

    /* Dark containers climb with elevation instead of falling. */
    assert(channel(scheme.surface_container_lowest, 8u) <
           channel(scheme.surface_container_low, 8u));
    assert(channel(scheme.surface_container_high, 8u) <
           channel(scheme.surface_container_highest, 8u));
    assert(m3_scheme_is_dark(&scheme));
}

/* A seed with no hue at all yields M3's monochrome scheme rather than
 * having a hue invented for it -- while a seed that merely looks grey but
 * carries a trace of one (#1C1B1F, a hair of violet) keeps that hue and has
 * its chroma raised to the primary palette's floor, which is what HCT does
 * too. */
static void test_monochrome_seed(void)
{
    m3_scheme_t scheme;
    m3_scheme_init(&scheme, 0xFF808080u, false);
    for (uint32_t shift = 0u; shift <= 16u; shift += 8u) {
        uint32_t reference = channel(scheme.primary, 8u);
        uint32_t value = channel(scheme.primary, shift);
        uint32_t delta = value > reference ? value - reference : reference - value;
        assert(delta <= 1u);
    }
    /* The error palette is fixed, so it keeps its colour regardless. */
    expect_near("mono error", scheme.error, 0xB3261Eu, 0u);

    m3_scheme_t tinted;
    m3_scheme_init(&tinted, 0xFF1C1B1Fu, false);
    assert(channel(tinted.primary, 16u) != channel(tinted.primary, 8u));
}

static void test_tone_ramp_is_monotonic(void)
{
    uint32_t previous = 0u;
    for (uint32_t tone = 0u; tone <= 100u; tone += 5u) {
        uint32_t value = m3_tone(M3_SEED_BASELINE,
                                    M3_PALETTE_NEUTRAL, tone);
        uint32_t green = channel(value, 8u);
        assert(green >= previous);
        previous = green;
    }
    assert(channel(m3_tone(M3_SEED_BASELINE, M3_PALETTE_NEUTRAL, 0u), 8u) == 0u);
    assert(channel(m3_tone(M3_SEED_BASELINE, M3_PALETTE_NEUTRAL, 100u), 8u) == 255u);
    /* Out-of-range tones clamp rather than reading off the end. */
    assert(m3_tone(M3_SEED_BASELINE, M3_PALETTE_PRIMARY, 500u) ==
           m3_tone(M3_SEED_BASELINE, M3_PALETTE_PRIMARY, 100u));
}

static void test_state_layers(void)
{
    /* 8% of white over black is 20/255, which is what the hover layer on a
     * dark container has to come to. */
    uint32_t hovered = m3_state_layer(0xFF000000u, 0xFFFFFFFFu,
                                         M3_STATE_HOVER);
    assert(channel(hovered, 0u) == 20u);
    assert((hovered >> 24u) == 0xFFu);
    /* A zero-opacity layer is the container untouched. */
    assert(m3_state_layer(0xFF123456u, 0xFFFFFFFFu, 0u) == 0xFF123456u);
    /* A full layer is the "on" colour, with the container's alpha kept. */
    assert(m3_state_layer(0xFF000000u, 0xFFFFFFFFu, 100u) == 0xFFFFFFFFu);

    /* The translucent form carries the opacity in the alpha channel. */
    uint32_t overlay = m3_state_overlay(0xFF102030u, M3_STATE_PRESSED);
    assert((overlay & 0x00FFFFFFu) == 0x102030u);
    assert((overlay >> 24u) == 26u);

    assert(m3_alpha(0xFF203040u, 50u) == 0x80203040u);
    assert(m3_alpha(0xFF203040u, 0u) == 0x00203040u);
}

static void test_elevation(void)
{
    m3_scheme_t light;
    m3_scheme_init(&light, M3_SEED_BASELINE, false);
    uint32_t flat = m3_elevated(&light, light.surface, M3_ELEVATION_0);
    uint32_t raised = m3_elevated(&light, light.surface, M3_ELEVATION_3);
    assert(flat == light.surface);
    assert(raised != light.surface);
    /* Tint pulls the surface toward the primary, so it gets darker. */
    assert(channel(raised, 8u) < channel(flat, 8u));

    /* The dark scheme carries elevation in the container tones instead. */
    m3_scheme_t dark;
    m3_scheme_init(&dark, M3_SEED_BASELINE, true);
    assert(m3_elevated(&dark, dark.surface, M3_ELEVATION_3) == dark.surface);

    assert(m3_shadow_spread(M3_ELEVATION_0) == 0u);
    assert(m3_shadow_spread(M3_ELEVATION_3) >
           m3_shadow_spread(M3_ELEVATION_1));
    assert(m3_shadow_offset_y(M3_ELEVATION_3) >
           m3_shadow_offset_y(M3_ELEVATION_1));
}

static void test_shape_scale(void)
{
    /* A stadium is half the shorter side. */
    assert(m3_shape_radius(M3_SHAPE_FULL, 100u, 40u) == 20u);
    /* A fixed token is itself, until the box is too small to hold it. */
    assert(m3_shape_radius(M3_SHAPE_MEDIUM, 100u, 40u) == M3_SHAPE_MEDIUM);
    assert(m3_shape_radius(M3_SHAPE_EXTRA_LARGE, 100u, 20u) == 10u);
    assert(m3_shape_radius(M3_SHAPE_NONE, 100u, 40u) == 0u);
}

static void test_typescale(void)
{
    m3_typescale_t type;
    m3_typescale_init(&type, 1.0f);
    assert(type.body_medium == 14.0f);
    assert(type.label_small == 11.0f);
    assert(type.title_large == 22.0f);
    assert(type.headline_small > type.title_large);

    m3_typescale_init(&type, 1.5f);
    assert(type.body_medium == 21.0f);

    /* A nonsense scale falls back to the specified sizes rather than
     * producing a shell with unreadable text. */
    m3_typescale_init(&type, 0.0f);
    assert(type.body_medium == 14.0f);
    m3_typescale_init(&type, 99.0f);
    assert(type.body_medium == 14.0f);
}

int main(void)
{
    test_baseline_light_scheme();
    test_baseline_dark_scheme();
    test_monochrome_seed();
    test_tone_ramp_is_monotonic();
    test_state_layers();
    test_elevation();
    test_shape_scale();
    test_typescale();
    printf("MaterialTest: all checks passed\n");
    return 0;
}
