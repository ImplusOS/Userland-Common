#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "IPC.h"
#include "PnP.h"
#include "Process.h"
#include "Window.h"

#define SYSNOTIF_DRAIN_INTERVAL_MS 1000ULL
#define SYSNOTIF_IDLE_SLEEP_MS       50ULL

static const char *class_name(uint32_t device_class)
{
    switch (device_class) {
    case PNP_CLASS_DRIVER:
        return "driver";
    case PNP_CLASS_PCI_DEVICE:
        return "PCI device";
    case PNP_CLASS_DISPLAY:
        return "display";
    case PNP_CLASS_MONITOR:
        return "monitor";
    case PNP_CLASS_INPUT:
        return "input device";
    case PNP_CLASS_KEYBOARD:
        return "keyboard";
    case PNP_CLASS_MOUSE:
        return "mouse";
    case PNP_CLASS_USB_DEVICE:
        return "USB device";
    case PNP_CLASS_STORAGE:
        return "storage";
    default:
        return "device";
    }
}

static const char *action_name(uint16_t action)
{
    switch (action) {
    case PNP_EVENT_DRIVER_READY:
        return "Ready";
    case PNP_EVENT_DEVICE_ADDED:
        return "Detected";
    case PNP_EVENT_DEVICE_REMOVED:
        return "Removed";
    case PNP_EVENT_DEVICE_CHANGED:
        return "Changed";
    default:
        return "Updated";
    }
}

static const char *event_device_name(const pnp_event_t *event)
{
    return event->device[0] != '\0' ?
           event->device :
           class_name(event->device_class);
}

static int pnp_message_valid(const ipc_message_t *message)
{
    if (message == NULL ||
        message->sender_pid != PNP_NOTIFICATION_ENDPOINT_PID ||
        message->size != sizeof(pnp_event_t)) {
        return 0;
    }

    const pnp_event_t *event = (const pnp_event_t *)message->data;
    return event->magic == PNP_IPC_MAGIC &&
           event->version == PNP_IPC_VERSION;
}

static void format_pci_message(const pnp_event_t *event,
                               char *body,
                               uint32_t body_size)
{
    uint32_t bus = (event->location0 >> 16u) & 0xFFu;
    uint32_t device = (event->location0 >> 8u) & 0xFFu;
    uint32_t function = event->location0 & 0xFFu;
    snprintf(body,
             body_size,
             "%s PCI device %04X:%04X at %u:%u.%u",
             action_name(event->action),
             (unsigned int)event->vendor_id,
             (unsigned int)event->device_id,
             bus,
             device,
             function);
}

static void format_usb_message(const pnp_event_t *event,
                               char *body,
                               uint32_t body_size)
{
    uint32_t addr = (event->location0 >> 16u) & 0xFFu;
    uint32_t interface = (event->location0 >> 8u) & 0xFFu;
    uint32_t endpoint = event->location0 & 0xFFu;
    snprintf(body,
             body_size,
             "%s %s %04X:%04X on USB addr %u iface %u ep %u",
             action_name(event->action),
             event_device_name(event),
             (unsigned int)event->vendor_id,
             (unsigned int)event->device_id,
             addr,
             interface,
             endpoint);
}

static void format_ps2_message(const pnp_event_t *event,
                               char *body,
                               uint32_t body_size)
{
    snprintf(body,
             body_size,
             "%s %s on PS/2 port %u",
             action_name(event->action),
             event_device_name(event),
             event->location0);
}

static void format_display_message(const pnp_event_t *event,
                                   char *body,
                                   uint32_t body_size)
{
    if (event->device_class == PNP_CLASS_MONITOR) {
        uint32_t scanout = (event->location0 >> 16u) & 0xFFFFu;
        uint32_t monitor = event->location0 & 0xFFFFu;
        uint32_t width = (event->location1 >> 16u) & 0xFFFFu;
        uint32_t height = event->location1 & 0xFFFFu;
        snprintf(body,
                 body_size,
                 "%s monitor %u scanout %u at %ux%u",
                 action_name(event->action),
                 monitor,
                 scanout,
                 width,
                 height);
        return;
    }

    snprintf(body,
             body_size,
             "%s %s at %ux%u",
             action_name(event->action),
             event_device_name(event),
             event->location0,
             event->location1);
}

static void format_generic_message(const pnp_event_t *event,
                                   char *body,
                                   uint32_t body_size)
{
    if (event->driver[0] != '\0') {
        snprintf(body,
                 body_size,
                 "%s %s via %s",
                 action_name(event->action),
                 event_device_name(event),
                 event->driver);
        return;
    }

    snprintf(body,
             body_size,
             "%s %s",
             action_name(event->action),
             event_device_name(event));
}

static void show_pnp_notification(const pnp_event_t *event)
{
    char body[128];
    body[0] = '\0';

    if (event->action == PNP_EVENT_DRIVER_READY) {
        format_generic_message(event, body, (uint32_t)sizeof(body));
    } else {
        switch (event->bus) {
        case PNP_BUS_PCI:
            format_pci_message(event, body, (uint32_t)sizeof(body));
            break;
        case PNP_BUS_USB:
            format_usb_message(event, body, (uint32_t)sizeof(body));
            break;
        case PNP_BUS_PS2:
            format_ps2_message(event, body, (uint32_t)sizeof(body));
            break;
        case PNP_BUS_DISPLAY:
            format_display_message(event, body, (uint32_t)sizeof(body));
            break;
        default:
            format_generic_message(event, body, (uint32_t)sizeof(body));
            break;
        }
    }

    body[sizeof(body) - 1u] = '\0';
    window_show_notification("Plug and Play", body);
}

static void wait_for_window_manager(void)
{
    while (window_get_wm_pid() < 0) {
        sleep_ms(SYSNOTIF_IDLE_SLEEP_MS);
    }
}

void _start(void)
{
    wait_for_window_manager();

    while (pnp_subscribe() < 0) {
        sleep_ms(250u);
    }

    uint64_t next_drain_ms = get_uptime_ms() + SYSNOTIF_DRAIN_INTERVAL_MS;
    for (;;) {
        ipc_message_t message;
        if (ipc_receive_message(&message) == 0) {
            if (pnp_message_valid(&message)) {
                const pnp_event_t *event = (const pnp_event_t *)message.data;
                show_pnp_notification(event);
            }
            continue;
        }

        uint64_t now_ms = get_uptime_ms();
        if (now_ms >= next_drain_ms) {
            (void)pnp_drain();
            next_drain_ms = now_ms + SYSNOTIF_DRAIN_INTERVAL_MS;
        }
        sleep_ms(SYSNOTIF_IDLE_SLEEP_MS);
    }
}
