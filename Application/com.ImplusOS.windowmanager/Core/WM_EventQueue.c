#include "WM_EventQueue.h"
#include "../../../../Userland/API/WM_Protocol.h"

#include <string.h>

void wm_event_queue_init(wm_event_queue_t *queue)
{
    if (queue) {
        memset(queue, 0, sizeof(*queue));
        queue->last_mouse_index = -1;
    }
}

bool wm_event_queue_push(wm_event_queue_t *queue, const ipc_message_t *message)
{
    if (!queue || !message) return false;
    if (queue->count == WM_EVENT_QUEUE_SIZE) {
        ++queue->dropped;
        return false;
    }
    queue->messages[queue->head] = *message;
    queue->head = (queue->head + 1u) % WM_EVENT_QUEUE_SIZE;
    ++queue->count;
    return true;
}

static bool message_has_type(const ipc_message_t *message, uint32_t type)
{
    if (!message || message->size < sizeof(wm_msg_header_t)) return false;
    const wm_msg_header_t *header = (const wm_msg_header_t *)message->data;
    return header->type == type;
}

bool wm_event_queue_push_coalesced(wm_event_queue_t *queue,
                                   const ipc_message_t *message)
{
    if (!queue || !message) return false;
    if (message_has_type(message, WM_MOUSE_EVENT)) {
        if (queue->last_mouse_index >= 0) {
            uint32_t mi = (uint32_t)queue->last_mouse_index;
            uint32_t t = queue->tail;
            uint32_t dist = (mi >= t) ? (mi - t)
                                      : (WM_EVENT_QUEUE_SIZE - t + mi);
            bool in_range = (dist < queue->count);
            if (in_range && message_has_type(&queue->messages[mi], WM_MOUSE_EVENT)) {
                queue->messages[mi] = *message;
                return true;
            }
            queue->last_mouse_index = -1;
        }
        for (uint32_t i = 0; i < queue->count; ++i) {
            uint32_t index = (queue->tail + i) % WM_EVENT_QUEUE_SIZE;
            if (message_has_type(&queue->messages[index], WM_MOUSE_EVENT)) {
                queue->messages[index] = *message;
                queue->last_mouse_index = (int32_t)index;
                return true;
            }
        }
        bool result = wm_event_queue_push(queue, message);
        if (result) {
            queue->last_mouse_index = (int32_t)((queue->head + WM_EVENT_QUEUE_SIZE - 1) % WM_EVENT_QUEUE_SIZE);
        }
        return result;
    }
    queue->last_mouse_index = -1;
    return wm_event_queue_push(queue, message);
}

bool wm_event_queue_pop(wm_event_queue_t *queue, ipc_message_t *message)
{
    if (!queue || !message || queue->count == 0u) return false;
    *message = queue->messages[queue->tail];
    if (queue->last_mouse_index == (int32_t)queue->tail)
        queue->last_mouse_index = -1;
    queue->tail = (queue->tail + 1u) % WM_EVENT_QUEUE_SIZE;
    --queue->count;
    return true;
}
