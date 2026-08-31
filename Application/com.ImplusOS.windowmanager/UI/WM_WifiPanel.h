#pragma once

#include "../Core/WM_State.h"

/*
 * WM_WifiPanel.c -- the Wi-Fi settings panel opened from the taskbar's
 * network tray icon (WM_TASKBAR_HIT_NETWORK, see WM_Taskbar.c). Follows
 * the same rect/draw/hit_test shape as WM_StartMenu.c and
 * WM_Notification.c's notification center: this file only draws and
 * reports what was clicked; WM_Input.c performs the actual
 * wifi_scan_start()/wifi_connect()/wifi_disconnect() syscalls and owns
 * state->wifi_panel's open/closed lifecycle.
 */

typedef enum {
    WM_WIFI_ACTION_NONE = 0,
    WM_WIFI_ACTION_SELECT,      /* index = row clicked (secured network -> password entry) */
    WM_WIFI_ACTION_CONNECT_OPEN,/* index = row clicked (open network -> connect immediately) */
    WM_WIFI_ACTION_CONNECT_PSK, /* confirm button in password entry sub-view */
    WM_WIFI_ACTION_BACK,        /* leave password entry sub-view */
    WM_WIFI_ACTION_SCAN,
    WM_WIFI_ACTION_DISCONNECT,
} wm_wifi_action_kind_t;

typedef struct {
    wm_wifi_action_kind_t kind;
    uint32_t index;
} wm_wifi_action_t;

wm_rect_t wm_wifi_panel_rect(const wm_state_t *state);
void wm_wifi_panel_draw(wm_state_t *state, wm_canvas_t *canvas);
wm_wifi_action_t wm_wifi_panel_hit_test(wm_state_t *state, int32_t x, int32_t y);
bool wm_wifi_panel_contains(const wm_state_t *state, int32_t x, int32_t y);
bool wm_wifi_panel_scroll(wm_state_t *state, int32_t rows);

bool wm_wifi_panel_input_char(wm_state_t *state, char ch);
bool wm_wifi_panel_input_backspace(wm_state_t *state);

/* Called once per main-loop tick from WM_Main.c; internally rate-limits
 * itself (no-op most calls) so it's cheap to call unconditionally.
 * Refreshes status/scan-results while the panel is open, and clears
 * scan_active once the driver's scan window (see AX900.c) has elapsed.
 * Returns true if it changed anything the panel needs redrawn for. */
bool wm_wifi_panel_poll(wm_state_t *state, uint64_t now_ms);
