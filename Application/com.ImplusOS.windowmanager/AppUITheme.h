#pragma once

#include <stdint.h>

/*
 * ImplusOS Application UI Theme -- Material Design 3
 *
 * The same colour roles the window manager draws itself with, frozen at the
 * M3 baseline seed (#6750A4) so an application can match the shell without
 * linking the shell's palette generator.
 *
 * These are the roles, not a list of nice colours: pick the one whose name
 * says what the thing is (a card is a surface container, the text on it is
 * on-surface, its divider is outline-variant), and light and dark stay
 * consistent for free.
 *
 * The window manager's own copy is generated at runtime from the seed in
 * its .theme file (Userland/API/Source/Material.c), so a desktop retinted
 * away from
 * the baseline will drift from these constants. An application that wants
 * to follow a retint should ask the shell rather than compile these in.
 *
 * Colour format: 0xAARRGGBB.
 */

/* ------------------------------------------------------- light scheme */

/* -- Primary ---------------------------------------------------*/
#define THEME_PRIMARY                            0xFF6750A4
#define THEME_ON_PRIMARY                         0xFFFFFFFF
#define THEME_PRIMARY_CONTAINER                  0xFFE9DDFF
#define THEME_ON_PRIMARY_CONTAINER               0xFF150D56

/* -- Secondary -------------------------------------------------*/
#define THEME_SECONDARY                          0xFF635A74
#define THEME_ON_SECONDARY                       0xFFFFFFFF
#define THEME_SECONDARY_CONTAINER                0xFFE9DEFB
#define THEME_ON_SECONDARY_CONTAINER             0xFF1F182D

/* -- Tertiary --------------------------------------------------*/
#define THEME_TERTIARY                           0xFF844F5C
#define THEME_ON_TERTIARY                        0xFFFFFFFF
#define THEME_TERTIARY_CONTAINER                 0xFFFFD9E1
#define THEME_ON_TERTIARY_CONTAINER              0xFF380A1A

/* -- Error -----------------------------------------------------*/
#define THEME_ERROR                              0xFFB3261E
#define THEME_ON_ERROR                           0xFFFFFFFF
#define THEME_ERROR_CONTAINER                    0xFFF9DEDC
#define THEME_ON_ERROR_CONTAINER                 0xFF410E0B

/* -- Surface ---------------------------------------------------*/
#define THEME_SURFACE                            0xFFFBF8FF
#define THEME_ON_SURFACE                         0xFF1D1B20
#define THEME_SURFACE_VARIANT                    0xFFE6E0EF
#define THEME_ON_SURFACE_VARIANT                 0xFF494551
#define THEME_SURFACE_DIM                        0xFFDCD9E0
#define THEME_SURFACE_BRIGHT                     0xFFFBF8FF

/* -- Surface containers -- the elevation ramp ------------------*/
#define THEME_SURFACE_CONTAINER_LOWEST           0xFFFFFFFF
#define THEME_SURFACE_CONTAINER_LOW              0xFFF5F2FA
#define THEME_SURFACE_CONTAINER                  0xFFEFEDF4
#define THEME_SURFACE_CONTAINER_HIGH             0xFFEAE7EE
#define THEME_SURFACE_CONTAINER_HIGHEST          0xFFE4E1E9

/* -- Outline ---------------------------------------------------*/
#define THEME_OUTLINE                            0xFF7A7582
#define THEME_OUTLINE_VARIANT                    0xFFC9C4D3

/* -- Inverse, for content shown against the opposite scheme ----*/
#define THEME_INVERSE_SURFACE                    0xFF322F35
#define THEME_INVERSE_ON_SURFACE                 0xFFF2EFF7
#define THEME_INVERSE_PRIMARY                    0xFFD2BBFF

/* -- Shadow and scrim ------------------------------------------*/
#define THEME_SHADOW                             0xFF000000
#define THEME_SCRIM                              0xFF000000

/* -------------------------------------------------------- dark scheme */

/* -- Primary ---------------------------------------------------*/
#define THEME_DARK_PRIMARY                       0xFFD2BBFF
#define THEME_DARK_ON_PRIMARY                    0xFF32226F
#define THEME_DARK_PRIMARY_CONTAINER             0xFF4C3889
#define THEME_DARK_ON_PRIMARY_CONTAINER          0xFFE9DDFF

/* -- Secondary -------------------------------------------------*/
#define THEME_DARK_SECONDARY                     0xFFCCC2DF
#define THEME_DARK_ON_SECONDARY                  0xFF342D43
#define THEME_DARK_SECONDARY_CONTAINER           0xFF4B435B
#define THEME_DARK_ON_SECONDARY_CONTAINER        0xFFE9DEFB

/* -- Tertiary --------------------------------------------------*/
#define THEME_DARK_TERTIARY                      0xFFF3B6C3
#define THEME_DARK_ON_TERTIARY                   0xFF50212E
#define THEME_DARK_TERTIARY_CONTAINER            0xFF6A3744
#define THEME_DARK_ON_TERTIARY_CONTAINER         0xFFFFD9E1

/* -- Error -----------------------------------------------------*/
#define THEME_DARK_ERROR                         0xFFF2B8B5
#define THEME_DARK_ON_ERROR                      0xFF601410
#define THEME_DARK_ERROR_CONTAINER               0xFF8C1D18
#define THEME_DARK_ON_ERROR_CONTAINER            0xFFF9DEDC

/* -- Surface ---------------------------------------------------*/
#define THEME_DARK_SURFACE                       0xFF151218
#define THEME_DARK_ON_SURFACE                    0xFFE4E1E9
#define THEME_DARK_SURFACE_VARIANT               0xFF494551
#define THEME_DARK_ON_SURFACE_VARIANT            0xFFC9C4D3
#define THEME_DARK_SURFACE_DIM                   0xFF151218
#define THEME_DARK_SURFACE_BRIGHT                0xFF3A383E

/* -- Surface containers -- the elevation ramp ------------------*/
#define THEME_DARK_SURFACE_CONTAINER_LOWEST      0xFF100D14
#define THEME_DARK_SURFACE_CONTAINER_LOW         0xFF1D1B20
#define THEME_DARK_SURFACE_CONTAINER             0xFF211F24
#define THEME_DARK_SURFACE_CONTAINER_HIGH        0xFF2B292F
#define THEME_DARK_SURFACE_CONTAINER_HIGHEST     0xFF36343A

/* -- Outline ---------------------------------------------------*/
#define THEME_DARK_OUTLINE                       0xFF938E9C
#define THEME_DARK_OUTLINE_VARIANT               0xFF494551

/* -- Inverse, for content shown against the opposite scheme ----*/
#define THEME_DARK_INVERSE_SURFACE               0xFFE4E1E9
#define THEME_DARK_INVERSE_ON_SURFACE            0xFF322F35
#define THEME_DARK_INVERSE_PRIMARY               0xFF6750A4

/* -- Shadow and scrim ------------------------------------------*/
#define THEME_DARK_SHADOW                        0xFF000000
#define THEME_DARK_SCRIM                         0xFF000000

/* --------------------------------------------- state layer opacities */

/* M3 lays the "on" colour over a component's container at these
 * percentages to show what the pointer or the keyboard is doing to it. */
#define THEME_STATE_HOVER_PERCENT    8
#define THEME_STATE_FOCUS_PERCENT   10
#define THEME_STATE_PRESSED_PERCENT 10
#define THEME_STATE_DRAGGED_PERCENT 16

/* ---------------------------------------------------------- shape scale */

#define THEME_SHAPE_NONE         0
#define THEME_SHAPE_EXTRA_SMALL  4
#define THEME_SHAPE_SMALL        8
#define THEME_SHAPE_MEDIUM      12
#define THEME_SHAPE_LARGE       16
#define THEME_SHAPE_EXTRA_LARGE 28

/* ----------------------------------------------------------- type scale */

/* The M3 type scale, in pixels. */
#define THEME_TYPE_HEADLINE_MEDIUM 28
#define THEME_TYPE_HEADLINE_SMALL  24
#define THEME_TYPE_TITLE_LARGE     22
#define THEME_TYPE_TITLE_MEDIUM    16
#define THEME_TYPE_TITLE_SMALL     14
#define THEME_TYPE_BODY_LARGE      16
#define THEME_TYPE_BODY_MEDIUM     14
#define THEME_TYPE_BODY_SMALL      12
#define THEME_TYPE_LABEL_LARGE     14
#define THEME_TYPE_LABEL_MEDIUM    12
#define THEME_TYPE_LABEL_SMALL     11
