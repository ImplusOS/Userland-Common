#pragma once

#include "WM_State.h"

void wm_event_queue_init(wm_event_queue_t *queue);
bool wm_event_queue_push(wm_event_queue_t *queue, const ipc_message_t *message);
bool wm_event_queue_push_coalesced(wm_event_queue_t *queue,
                                   const ipc_message_t *message);
bool wm_event_queue_pop(wm_event_queue_t *queue, ipc_message_t *message);
