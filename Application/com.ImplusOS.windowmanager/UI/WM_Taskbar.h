#pragma once

#include "../Core/WM_State.h"

typedef enum {
    WM_TASKBAR_HIT_NONE = 0,
    WM_TASKBAR_HIT_LAUNCHER,
    WM_TASKBAR_HIT_WINDOW,
    WM_TASKBAR_HIT_CLOCK,
    WM_TASKBAR_HIT_NOTIFICATION,
    WM_TASKBAR_HIT_NETWORK,
    WM_TASKBAR_HIT_AUDIO,
    WM_TASKBAR_HIT_IME,
    WM_TASKBAR_HIT_PIN
} wm_taskbar_hit_kind_t;

typedef struct {
    wm_taskbar_hit_kind_t kind;
    uint32_t window_id;   /* WM_TASKBAR_HIT_WINDOW */
    uint32_t index;       /* WM_TASKBAR_HIT_PIN -> assets.apps[] index */
} wm_taskbar_hit_t;

#define NTP_REFRESH_MS 3600000u

/* how many apps.list entries are pinned to the taskbar after the Start button */
#define WM_TASKBAR_MAX_PINS 6u

wm_rect_t wm_taskbar_rect(const wm_state_t *state);
wm_rect_t wm_taskbar_clock_rect(const wm_state_t *state);
void wm_taskbar_draw(wm_state_t *state, wm_canvas_t *canvas);
wm_taskbar_hit_t wm_taskbar_hit_test(wm_state_t *state, int32_t x, int32_t y);
bool wm_taskbar_update_clock(wm_state_t *state);
void wm_taskbar_start_ntp(wm_state_t *state);
void wm_taskbar_poll_ntp(wm_state_t *state);
