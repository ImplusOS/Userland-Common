#pragma once

#include "../Core/WM_State.h"
#include <stdbool.h>

void wm_input_handle_keyboard(wm_state_t *state, const ipc_message_t *message);
void wm_input_handle_mouse(wm_state_t *state, const ipc_message_t *message);

/* Drain the kernel's raw HID event queues directly (SYSCALL_INPUT_READ_*).
 * The WM owns cursor position/clamping and keyboard routing entirely in
 * userland now; the kernel only hands out raw relative-motion mouse deltas
 * and discrete key events, gated to whichever pid holds input ownership
 * (see window_register_service()). */
bool wm_input_poll_raw_mouse(wm_state_t *state);
bool wm_input_poll_raw_keyboard(wm_state_t *state);
