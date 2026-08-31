#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/syscalls.h>
#include "Syscalls.h"
#include "API/File.h"
#include "API/WM_Protocol.h"
#include "API/Process.h"
#include "API/IPC.h"
#include "API/Graphics.h"
#include "API/Input.h"
#include "API/Window.h"
#include "API/Serial.h"
#include "API/SystemInfo.h"
#include "API/Audio.h"
#include "API/Socket.h"
#include "API/PnP.h"
#include "API/WiFi.h"

static uint32_t g_current_window_id = 0;
static uint32_t g_wm_request_id = 0;
static int32_t g_cached_wm_pid = -1;

#define MAX_INPUT_QUEUE             64
#define MAX_DEFERRED_IPC_QUEUE      64
#define IPC_DRAIN_MAX               4
#define WM_RPC_TIMEOUT_MS         5000ULL
#define WM_SEND_RETRY_TIMEOUT_MS  1000ULL

static input_keyboard_event_t g_kbd_queue[MAX_INPUT_QUEUE];
static uint32_t g_kbd_head = 0, g_kbd_tail = 0, g_kbd_count = 0;

static input_mouse_event_t g_mouse_queue[MAX_INPUT_QUEUE];
static uint32_t g_mouse_head = 0, g_mouse_tail = 0, g_mouse_count = 0;

static ipc_message_t g_deferred_ipc[MAX_DEFERRED_IPC_QUEUE];
static uint32_t g_deferred_head;
static uint32_t g_deferred_tail;
static uint32_t g_deferred_count;

typedef struct {
    window_id_t window_id;
    int32_t handle;
    uint32_t *address;
    uint32_t size_bytes;
} window_backing_mapping_t;

static window_backing_mapping_t g_window_backing_mappings[256];

extern uint64_t syscall0(uint64_t num);
extern uint64_t syscall1(uint64_t num, uint64_t arg1);
extern uint64_t syscall2(uint64_t num, uint64_t arg1, uint64_t arg2);
extern uint64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3);
extern uint64_t syscall4(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4);
extern uint64_t syscall5(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);

static int32_t os_errno_from_i32_status(int32_t value);

static int32_t ipc_receive_raw(ipc_message_t *out_message)
{
    return os_errno_from_i32_status((int32_t)syscall1(
        SYSCALL_IPC_RECEIVE_MESSAGE, (uint64_t)out_message));
}

static int32_t deferred_ipc_push(const ipc_message_t *message)
{
    if (message == NULL || g_deferred_count >= MAX_DEFERRED_IPC_QUEUE) {
        os_set_errno(message == NULL ? EINVAL : ENOBUFS);
        return -1;
    }
    g_deferred_ipc[g_deferred_head] = *message;
    g_deferred_head = (g_deferred_head + 1u) % MAX_DEFERRED_IPC_QUEUE;
    ++g_deferred_count;
    return 0;
}

static int32_t deferred_ipc_pop(ipc_message_t *out_message)
{
    if (out_message == NULL || g_deferred_count == 0u) return -1;
    *out_message = g_deferred_ipc[g_deferred_tail];
    g_deferred_tail = (g_deferred_tail + 1u) % MAX_DEFERRED_IPC_QUEUE;
    --g_deferred_count;
    return 0;
}

static bool handle_input_message(const ipc_message_t *msg)
{
    if (msg == NULL || msg->size < sizeof(wm_msg_header_t)) return false;
    const wm_msg_header_t *hdr = (const wm_msg_header_t *)msg->data;
    if (hdr->type == WM_KEYBOARD_EVENT) {
        if (msg->size < sizeof(wm_msg_header_t) + sizeof(input_keyboard_event_t))
            return true;
        if (g_kbd_count < MAX_INPUT_QUEUE) {
            memcpy(&g_kbd_queue[g_kbd_head],
                   msg->data + sizeof(wm_msg_header_t),
                   sizeof(input_keyboard_event_t));
            g_kbd_head = (g_kbd_head + 1u) % MAX_INPUT_QUEUE;
            ++g_kbd_count;
        }
        return true;
    }
    if (hdr->type == WM_MOUSE_EVENT) {
        if (msg->size < sizeof(wm_msg_header_t) + sizeof(input_mouse_event_t))
            return true;
        if (g_mouse_count < MAX_INPUT_QUEUE) {
            memcpy(&g_mouse_queue[g_mouse_head],
                   msg->data + sizeof(wm_msg_header_t),
                   sizeof(input_mouse_event_t));
            g_mouse_head = (g_mouse_head + 1u) % MAX_INPUT_QUEUE;
            ++g_mouse_count;
        }
        return true;
    }
    return false;
}

static void preserve_unhandled_message(const ipc_message_t *message)
{
    if (!handle_input_message(message)) (void)deferred_ipc_push(message);
}

static bool deadline_expired(uint64_t deadline_ms)
{
    return get_uptime_ms() >= deadline_ms;
}

int os_get_errno(void)
{
    return os_errno;
}

void os_set_errno(int value)
{
    os_errno = (value < 0) ? -value : value;
}

void os_clear_errno(void)
{
    os_errno = 0;
}

int os_status_is_error(int64_t status_code)
{
    return status_code < 0;
}

int os_status_to_errno(int64_t status_code)
{
    if (status_code >= 0) {
        return 0;
    }

    switch (status_code) {
        case OS_STATUS_INVALID_ARG:
            return EINVAL;
        case OS_STATUS_NOT_FOUND:
            return ENOENT;
        case OS_STATUS_ACCESS_DENIED:
            return EACCES;
        case OS_STATUS_LIMIT_REACHED:
            return EMFILE;
        case OS_STATUS_IO_ERROR:
            return EIO;
        case OS_STATUS_FAULT:
            return EFAULT;
        case OS_STATUS_NOT_SUPPORTED:
            return ENOTSUP;
        case OS_STATUS_INTERNAL:
            return 255;
        default:
            if (status_code <= -4096LL) {
                return 255;
            }
            return (int)(-status_code);
    }
}

static int32_t os_errno_from_i32_status(int32_t value)
{
    if (os_status_is_error((int64_t)value)) {
        os_set_errno(os_status_to_errno((int64_t)value));
    } else {
        os_clear_errno();
    }
    return value;
}

static int64_t os_errno_from_i64_status(int64_t value)
{
    if (os_status_is_error(value)) {
        os_set_errno(os_status_to_errno(value));
    } else {
        os_clear_errno();
    }
    return value;
}

size_t os_strnlen(const char *str, size_t max_len)
{
    return strnlen(str, max_len);
}

int os_strcpy_s(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || src == NULL || dst_size == 0) {
        return -1;
    }
    if (strlcpy(dst, src, dst_size) >= dst_size) {
        dst[0] = '\0';
        return -1;
    }
    return 0;
}

int os_strcat_s(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || src == NULL || dst_size == 0) {
        return -1;
    }

    return strlcat(dst, src, dst_size) < dst_size ? 0 : -1;
}


__attribute__((unused)) int32_t file_open(const char *path, uint64_t flags)
{
    return os_errno_from_i32_status((int32_t)syscall2(SYSCALL_FILE_OPEN, (uint64_t)path, flags));
}

__attribute__((unused)) int32_t file_creat(const char *path)
{
    return os_errno_from_i32_status((int32_t)syscall1(SYSCALL_FILE_CREAT, (uint64_t)path));
}

__attribute__((unused)) int64_t file_read(int32_t fd, void *buffer, uint64_t len)
{
    return os_errno_from_i64_status((int64_t)syscall3(SYSCALL_FILE_READ, (uint64_t)fd, (uint64_t)buffer, len));
}

__attribute__((unused)) int64_t file_write(int32_t fd, const void *buffer, uint64_t len)
{
    return os_errno_from_i64_status((int64_t)syscall3(SYSCALL_FILE_WRITE, (uint64_t)fd, (uint64_t)buffer, len));
}

__attribute__((unused)) int32_t file_close(int32_t fd)
{
    return os_errno_from_i32_status((int32_t)syscall1(SYSCALL_FILE_CLOSE, (uint64_t)fd));
}

__attribute__((unused)) int32_t file_mkdir(const char *path)
{
    return os_errno_from_i32_status((int32_t)syscall1(SYSCALL_FILE_MKDIR, (uint64_t)path));
}

__attribute__((unused)) int32_t file_opendir(const char *path)
{
    return os_errno_from_i32_status((int32_t)syscall1(SYSCALL_FILE_OPENDIR, (uint64_t)path));
}

__attribute__((unused)) int32_t file_readdir(int32_t dir_handle, file_dirent_t *out_entry)
{
    return os_errno_from_i32_status((int32_t)syscall2(SYSCALL_FILE_READDIR, (uint64_t)dir_handle, (uint64_t)out_entry));
}

__attribute__((unused)) int32_t file_closedir(int32_t dir_handle)
{
    return os_errno_from_i32_status((int32_t)syscall1(SYSCALL_FILE_CLOSEDIR, (uint64_t)dir_handle));
}

__attribute__((unused)) int32_t file_unlink(const char *path)
{
    return os_errno_from_i32_status((int32_t)syscall1(SYSCALL_FILE_UNLINK, (uint64_t)path));
}

__attribute__((unused)) int32_t file_rename(const char *old_path,
                                             const char *new_path)
{
    return os_errno_from_i32_status((int32_t)syscall2(
        SYSCALL_RENAME, (uint64_t)old_path, (uint64_t)new_path));
}

void *os_mmap(uint64_t length, uint64_t flags)
{
    int64_t raw = (int64_t)syscall2(SYSCALL_USER_MMAP, length, flags);
    if (raw <= 0 && length != 0ULL) {
        os_set_errno(raw < 0 ? os_status_to_errno(raw) : ENOMEM);
        return NULL;
    }
    os_clear_errno();
    return (void *)(uintptr_t)(uint64_t)raw;
}

int32_t os_shared_memory_create(uint32_t size)
{
    return os_errno_from_i32_status(
        (int32_t)syscall1(SYSCALL_SHM_CREATE, size));
}

int32_t os_shared_memory_grant(int32_t handle, int32_t pid)
{
    return os_errno_from_i32_status(
        (int32_t)syscall2(SYSCALL_SHM_GRANT,
                          (uint64_t)(int64_t)handle,
                          (uint64_t)(int64_t)pid));
}

void *os_shared_memory_map(int32_t handle)
{
    void *address = (void *)(uintptr_t)syscall1(
        SYSCALL_SHM_MAP, (uint64_t)(int64_t)handle);
    if (!address) os_set_errno(ENOMEM);
    else os_clear_errno();
    return address;
}

int32_t os_shared_memory_unmap(int32_t handle, void *address)
{
    return os_errno_from_i32_status(
        (int32_t)syscall2(SYSCALL_SHM_UNMAP,
                          (uint64_t)(int64_t)handle,
                          (uint64_t)(uintptr_t)address));
}

int32_t os_shared_memory_close(int32_t handle)
{
    return os_errno_from_i32_status(
        (int32_t)syscall1(SYSCALL_SHM_CLOSE,
                          (uint64_t)(int64_t)handle));
}

int32_t os_memfd_shm_handle(int32_t fd)
{
    return (int32_t)syscall1(SYSCALL_MEMFD_SHM_HANDLE,
                             (uint64_t)(int64_t)fd);
}

signal_handler_t os_signal(int32_t signum, signal_handler_t handler)
{
    uint64_t raw = syscall2(SYSCALL_PROCESS_SIGNAL,
                            (uint64_t)signum,
                            (uint64_t)(uintptr_t)handler);
    if (os_status_is_error((int64_t)raw)) {
        os_set_errno(os_status_to_errno((int64_t)raw));
        return (signal_handler_t)0;
    }
    os_clear_errno();
    return (signal_handler_t)(uintptr_t)raw;
}

__attribute__((unused)) int64_t file_seek(int32_t fd, int64_t offset, int32_t whence)
{
    return os_errno_from_i64_status((int64_t)syscall3(SYSCALL_FILE_SEEK,
                                                      (uint64_t)fd,
                                                      (uint64_t)offset,
                                                      (uint64_t)whence));
}

void serial_write_char(char c)
{
    (void)syscall1(SYSCALL_SERIAL_PUTCHAR, (uint64_t)(uint8_t)c);
}

void serial_write_string(const char *str)
{
    (void)syscall1(SYSCALL_SERIAL_PUTS, (uint64_t)str);
}

void serial_write_uint64(uint64_t value)
{
    (void)syscall1(SYSCALL_SERIAL_WRITE_U64, (uint64_t)value);
}

void serial_write_uint32(uint32_t value)
{
    (void)syscall1(SYSCALL_SERIAL_WRITE_U32, (uint64_t)value);
}

void serial_write_uint16(uint16_t value)
{
    (void)syscall1(SYSCALL_SERIAL_WRITE_U16, (uint64_t)value);
}

void process_yield(void)
{
    (void)syscall0(SYSCALL_PROCESS_YIELD);
}

int32_t input_read_keyboard(input_keyboard_event_t *event_out)
{
    return os_errno_from_i32_status((int32_t)syscall1(SYSCALL_INPUT_READ_KEYBOARD, (uint64_t)event_out));
}

int32_t input_read_mouse(input_mouse_event_t *event_out)
{
    return os_errno_from_i32_status((int32_t)syscall1(SYSCALL_INPUT_READ_MOUSE, (uint64_t)event_out));
}

int32_t ipc_send_message(int32_t target_pid, const void *message, uint32_t size)
{
    if (size > IPC_MESSAGE_MAX_SIZE) {
        os_set_errno(EINVAL);
        return -1;
    }
    return os_errno_from_i32_status((int32_t)syscall3(SYSCALL_IPC_SEND_MESSAGE,
                                                      (uint64_t)target_pid,
                                                      (uint64_t)message,
                                                      (uint64_t)size));
}

int32_t ipc_receive_message(ipc_message_t *out_message)
{
    if (out_message == NULL) {
        os_set_errno(EINVAL);
        return -1;
    }
    if (deferred_ipc_pop(out_message) == 0) {
        os_clear_errno();
        return 0;
    }
    return ipc_receive_raw(out_message);
}

static int32_t pnp_send_request(uint16_t opcode)
{
    pnp_request_t request;
    pnp_request_init(&request, opcode);
    return ipc_send_message(PNP_NOTIFICATION_ENDPOINT_PID,
                            &request,
                            (uint32_t)sizeof(request));
}

int32_t pnp_subscribe(void)
{
    return pnp_send_request(PNP_OP_SUBSCRIBE);
}

int32_t pnp_unsubscribe(void)
{
    return pnp_send_request(PNP_OP_UNSUBSCRIBE);
}

int32_t pnp_drain(void)
{
    return pnp_send_request(PNP_OP_DRAIN);
}

int32_t process_get_current_pid(void)
{
    return os_errno_from_i32_status((int32_t)syscall0(SYSCALL_PROCESS_GET_PID));
}

int32_t process_spawn(const char *path)
{
    return os_errno_from_i32_status((int32_t)syscall1(SYSCALL_PROCESS_SPAWN_ELF, (uint64_t)path));
}

int32_t process_spawn_with_arg(const char *path, const char *argument)
{
    if (!path || !argument) {
        os_set_errno(EINVAL);
        return -1;
    }
    return os_errno_from_i32_status((int32_t)syscall2(
        SYSCALL_PROCESS_SPAWN_ELF_ARG, (uint64_t)path,
        (uint64_t)argument));
}

int32_t process_get_launch_argument(char *buffer, uint32_t capacity)
{
    if (!buffer || capacity == 0u) {
        os_set_errno(EINVAL);
        return -1;
    }
    return os_errno_from_i32_status((int32_t)syscall2(
        SYSCALL_PROCESS_GET_LAUNCH_ARG, (uint64_t)buffer,
        (uint64_t)capacity));
}

int32_t window_register_service(void)
{
    int32_t result = os_errno_from_i32_status(
        (int32_t)syscall0(SYSCALL_WINDOW_REGISTER_SERVICE));
    if (result == 0) g_cached_wm_pid = process_get_current_pid();
    return result;
}

int32_t window_get_wm_pid(void)
{
    int32_t pid = (int32_t)syscall0(SYSCALL_WINDOW_GET_WM_PID);
    if (pid < 0) {
        g_cached_wm_pid = -1;
        os_set_errno(EFAULT);
        return -1;
    }
    g_cached_wm_pid = pid;
    os_clear_errno();
    return pid;
}

window_id_t window_create(uint32_t width, uint32_t height, const char *title)
{
    return window_create_ex(100, 100, width, height, 0xFF000000, title);
}

window_id_t window_create_ex(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                             uint32_t bg_color, const char *title)
{
    struct {
        wm_msg_header_t hdr;
        uint32_t width, height;
        uint32_t x, y;
        uint32_t bg_color;
        char title[64];
    } __attribute__((aligned(16))) cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = WM_CREATE_WINDOW;
    cmd.hdr.request_id = ++g_wm_request_id;
    cmd.width = width;
    cmd.height = height;
    cmd.x = x;
    cmd.y = y;
    cmd.bg_color = bg_color;
    
    if (title) {
        os_strcpy_s(cmd.title, sizeof(cmd.title), title);
    }

    int32_t wm_pid = window_get_wm_pid();
    if (ipc_send_message(wm_pid, &cmd, sizeof(cmd)) < 0) {
        return 0;
    }

    ipc_message_t resp __attribute__((aligned(16)));
    uint64_t deadline = get_uptime_ms() + WM_RPC_TIMEOUT_MS;
    while (!deadline_expired(deadline)) {
        if (ipc_receive_raw(&resp) == 0) {
            wm_msg_header_t *hdr = (wm_msg_header_t *)resp.data;
            if (resp.size >= sizeof(*hdr) &&
                hdr->type == WM_WINDOW_CREATED &&
                hdr->request_id == cmd.hdr.request_id) {
                return hdr->window_id;
            }
            preserve_unhandled_message(&resp);
        }
        process_yield();
    }
    os_set_errno(ETIMEDOUT);
    return 0;
}

void window_destroy(window_id_t wid)
{
    window_release_backing_store(wid);
    wm_msg_header_t hdr;
    hdr.type = WM_DESTROY_WINDOW;
    hdr.window_id = wid;
    ipc_send_message(window_get_wm_pid(), &hdr, sizeof(hdr));
}

void window_set_rect(window_id_t wid, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    struct {
        wm_msg_header_t hdr;
        uint32_t x, y, w, h;
    } cmd;
    cmd.hdr.type = WM_SET_WINDOW_RECT;
    cmd.hdr.window_id = wid;
    cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
    ipc_send_message(window_get_wm_pid(), &cmd, sizeof(cmd));
}

void window_show(window_id_t wid)
{
    wm_msg_header_t hdr;
    hdr.type = WM_SHOW_WINDOW;
    hdr.window_id = wid;
    ipc_send_message(window_get_wm_pid(), &hdr, sizeof(hdr));
}

void window_hide(window_id_t wid)
{
    wm_msg_header_t hdr;
    hdr.type = WM_HIDE_WINDOW;
    hdr.window_id = wid;
    ipc_send_message(window_get_wm_pid(), &hdr, sizeof(hdr));
}

void window_raise(window_id_t wid)
{
    wm_msg_header_t hdr;
    hdr.type = WM_RAISE_WINDOW;
    hdr.window_id = wid;
    ipc_send_message(window_get_wm_pid(), &hdr, sizeof(hdr));
}

void window_lower(window_id_t wid)
{
    wm_msg_header_t hdr;
    hdr.type = WM_LOWER_WINDOW;
    hdr.window_id = wid;
    ipc_send_message(window_get_wm_pid(), &hdr, sizeof(hdr));
}

void window_set_focus(window_id_t wid)
{
    wm_msg_header_t hdr;
    hdr.type = WM_SET_FOCUS;
    hdr.window_id = wid;
    ipc_send_message(window_get_wm_pid(), &hdr, sizeof(hdr));
}

int32_t window_get_rect(window_id_t wid, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h)
{
    if (wid == 0 || x == NULL || y == NULL || w == NULL || h == NULL) {
        os_set_errno(EINVAL);
        return WM_STATUS_INVALID_ARG;
    }

    struct {
        wm_msg_header_t hdr;
    } cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = WM_GET_WINDOW_RECT;
    cmd.hdr.request_id = ++g_wm_request_id;
    cmd.hdr.window_id = wid;

    int32_t wm_pid = window_get_wm_pid();
    if (wm_pid < 0) {
        return -1;
    }
    if (ipc_send_message(wm_pid, &cmd, sizeof(cmd)) < 0) {
        return -1;
    }

    ipc_message_t resp __attribute__((aligned(16)));
    uint64_t deadline = get_uptime_ms() + WM_RPC_TIMEOUT_MS;
    while (!deadline_expired(deadline)) {
        if (ipc_receive_raw(&resp) == 0) {
            if (resp.size >= sizeof(wm_msg_header_t) + sizeof(int32_t) + (sizeof(uint32_t) * 4U)) {
                struct {
                    wm_msg_header_t hdr;
                    int32_t status;
                    uint32_t x, y, w, h;
                } *reply = (void *)resp.data;

                if (reply->hdr.type == WM_GET_WINDOW_RECT &&
                    reply->hdr.request_id == cmd.hdr.request_id) {
                    if (reply->status != WM_STATUS_OK) {
                        return os_errno_from_i32_status(reply->status);
                    }
                    *x = reply->x;
                    *y = reply->y;
                    *w = reply->w;
                    *h = reply->h;
                    os_clear_errno();
                    return WM_STATUS_OK;
                }
            }
            preserve_unhandled_message(&resp);
        }
        process_yield();
    }
    os_set_errno(ETIMEDOUT);
    return -1;
}

window_id_t window_get_focus(void)
{
    struct {
        wm_msg_header_t hdr;
    } cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = WM_GET_FOCUS;
    cmd.hdr.request_id = ++g_wm_request_id;
    cmd.hdr.window_id = 0;

    int32_t wm_pid = window_get_wm_pid();
    if (wm_pid < 0) {
        return 0;
    }
    if (ipc_send_message(wm_pid, &cmd, sizeof(cmd)) < 0) {
        return 0;
    }

    ipc_message_t resp __attribute__((aligned(16)));
    uint64_t deadline = get_uptime_ms() + WM_RPC_TIMEOUT_MS;
    while (!deadline_expired(deadline)) {
        if (ipc_receive_raw(&resp) == 0) {
            if (resp.size >= sizeof(wm_msg_header_t) + sizeof(int32_t) + sizeof(uint32_t)) {
                struct {
                    wm_msg_header_t hdr;
                    int32_t status;
                    uint32_t focused_window_id;
                } *reply = (void *)resp.data;

                if (reply->hdr.type == WM_GET_FOCUS &&
                    reply->hdr.request_id == cmd.hdr.request_id) {
                    if (reply->status != WM_STATUS_OK) {
                        (void)os_errno_from_i32_status(reply->status);
                        return 0;
                    }
                    os_clear_errno();
                    return reply->focused_window_id;
                }
            }
            preserve_unhandled_message(&resp);
        }
        process_yield();
    }
    os_set_errno(ETIMEDOUT);
    return 0;
}

int32_t window_subscribe_keyboard(window_id_t wid)
{
    struct {
        wm_msg_header_t hdr;
        uint32_t input_types;
    } cmd;
    cmd.hdr.type = WM_SUBSCRIBE_INPUT;
    cmd.hdr.window_id = wid;
    cmd.input_types = 1;
    return ipc_send_message(window_get_wm_pid(), &cmd, sizeof(cmd));
}

int32_t window_subscribe_mouse(window_id_t wid)
{
    struct {
        wm_msg_header_t hdr;
        uint32_t input_types;
    } cmd;
    cmd.hdr.type = WM_SUBSCRIBE_INPUT;
    cmd.hdr.window_id = wid;
    cmd.input_types = 2;
    return ipc_send_message(window_get_wm_pid(), &cmd, sizeof(cmd));
}

void window_set_system(window_id_t wid, bool is_system)
{
    struct {
        wm_msg_header_t hdr;
        bool is_system_flag;
    } cmd;
    cmd.hdr.type = WM_SET_WINDOW_SYSTEM;
    cmd.hdr.window_id = wid;
    cmd.is_system_flag = is_system;
    ipc_send_message(window_get_wm_pid(), &cmd, sizeof(cmd));
}

int32_t window_unsubscribe_input(window_id_t wid)
{
    struct {
        wm_msg_header_t hdr;
        uint32_t input_types;
    } cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = WM_UNSUBSCRIBE_INPUT;
    cmd.hdr.window_id = wid;
    cmd.input_types = 0;
    return ipc_send_message(window_get_wm_pid(), &cmd, sizeof(cmd));
}

void window_set_bg_color(window_id_t wid, uint32_t color)
{
    uint32_t win_x = 0;
    uint32_t win_y = 0;
    uint32_t win_w = 0;
    uint32_t win_h = 0;
    if (window_get_rect(wid, &win_x, &win_y, &win_w, &win_h) < 0) {
        return;
    }
    (void)win_x;
    (void)win_y;

    struct {
        wm_msg_header_t hdr;
        uint32_t x, y, w, h;
        uint32_t color_value;
    } cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = WM_DRAW_RECT;
    cmd.hdr.window_id = wid;
    cmd.x = 0;
    cmd.y = 0;
    cmd.w = win_w;
    cmd.h = win_h;
    cmd.color_value = color;
    (void)ipc_send_message(window_get_wm_pid(), &cmd, sizeof(cmd));
}

void window_clear(window_id_t wid)
{
    if (wid == 0) {
        return;
    }

    wm_msg_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.type = WM_CLEAR_WINDOW;
    hdr.window_id = wid;
    (void)ipc_send_message(window_get_wm_pid(), &hdr, sizeof(hdr));
}

int32_t graphics_init(uint32_t window_id)
{
    g_current_window_id = window_id;
    return 0;
}

__attribute__((optimize("O2"))) void draw_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (g_current_window_id == 0) {
        (void)syscall3(SYSCALL_DISPLAY_DRAW_PIXEL,
                       (uint64_t)x,
                       (uint64_t)y,
                       (uint64_t)color);
        return;
    }

    struct {
        wm_msg_header_t hdr;
        uint32_t x, y;
        uint32_t color;
    } cmd;
    cmd.hdr.type = WM_DRAW_PIXEL;
    cmd.hdr.window_id = g_current_window_id;
    cmd.x = x; cmd.y = y; cmd.color = color;
    ipc_send_message(window_get_wm_pid(), &cmd, sizeof(cmd));
}

__attribute__((optimize("O2"))) void draw_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    if (g_current_window_id == 0) {
        (void)syscall5(SYSCALL_DISPLAY_FILL_RECT,
                       (uint64_t)x,
                       (uint64_t)y,
                       (uint64_t)w,
                       (uint64_t)h,
                       (uint64_t)color);
        return;
    }

    struct {
        wm_msg_header_t hdr;
        uint32_t x, y, w, h;
        uint32_t color;
    } cmd;
    cmd.hdr.type = WM_DRAW_RECT;
    cmd.hdr.window_id = g_current_window_id;
    cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h; cmd.color = color;
    ipc_send_message(window_get_wm_pid(), &cmd, sizeof(cmd));
}

uint32_t get_pixel(uint32_t x, uint32_t y)
{
    return (uint32_t)syscall2(SYSCALL_DISPLAY_GET_PIXEL, (uint64_t)x, (uint64_t)y);
}

__attribute__((optimize("O2"))) void draw_present(void)
{
    if (g_current_window_id == 0) {
        (void)syscall0(SYSCALL_DISPLAY_PRESENT);
        return;
    }

    wm_msg_header_t hdr;
    hdr.type = WM_UPDATE_COMPLETE;
    hdr.window_id = g_current_window_id;
    ipc_send_message(window_get_wm_pid(), &hdr, sizeof(hdr));
}

__attribute__((optimize("O2"))) void draw_present_rects(const display_rect_t *rects, uint32_t count)
{
    if (g_current_window_id == 0) {
        if (rects != NULL && count != 0u) {
            (void)syscall2(SYSCALL_DISPLAY_PRESENT_RECTS,
                           (uint64_t)(uintptr_t)rects,
                           (uint64_t)count);
        } else {
            (void)syscall0(SYSCALL_DISPLAY_PRESENT);
        }
        return;
    }

    wm_msg_header_t hdr;
    hdr.type = WM_UPDATE_COMPLETE;
    hdr.window_id = g_current_window_id;
    ipc_send_message(window_get_wm_pid(), &hdr, sizeof(hdr));
}

uint32_t get_display_width(void)
{
    return (uint32_t)syscall0(SYSCALL_GET_DISPLAY_WIDTH);
}

uint32_t get_display_height(void)
{
    return (uint32_t)syscall0(SYSCALL_GET_DISPLAY_HEIGHT);
}

void *sys_get_display_framebuffer(void)
{
    return (void *)(uintptr_t)syscall0(SYSCALL_GET_DISPLAY_FRAMEBUFFER);
}

int64_t display_get_topology(display_topology_t *out_topology)
{
    if (out_topology == NULL) {
        return -22;
    }
    return os_errno_from_i64_status((int64_t)syscall1(
        SYSCALL_DISPLAY_GET_TOPOLOGY,
        (uint64_t)(uintptr_t)out_topology));
}

int64_t display_get_monitor_info(uint32_t monitor_index,
                                 display_monitor_info_t *out_info)
{
    if (out_info == NULL) {
        return -22;
    }
    return os_errno_from_i64_status((int64_t)syscall2(
        SYSCALL_DISPLAY_GET_MONITOR_INFO,
        (uint64_t)monitor_index,
        (uint64_t)(uintptr_t)out_info));
}

int64_t display_get_monitor_mode_info(uint32_t monitor_index,
                                      uint32_t mode_index,
                                      display_mode_info_t *out_info)
{
    if (out_info == NULL) {
        return -22;
    }
    return os_errno_from_i64_status((int64_t)syscall3(
        SYSCALL_DISPLAY_GET_MONITOR_MODE_INFO,
        (uint64_t)monitor_index,
        (uint64_t)mode_index,
        (uint64_t)(uintptr_t)out_info));
}

int64_t display_set_monitor_mode(uint32_t monitor_index, uint32_t mode_index)
{
    return os_errno_from_i64_status((int64_t)syscall2(
        SYSCALL_DISPLAY_SET_MONITOR_MODE,
        (uint64_t)monitor_index,
        (uint64_t)mode_index));
}

int32_t window_input_keyboard_poll(input_keyboard_event_t *out)
{
    if (out == NULL) {
        os_set_errno(EINVAL);
        return -1;
    }
    ipc_message_t msg;
    uint32_t drain_count = 0;
    while (drain_count < IPC_DRAIN_MAX && ipc_receive_raw(&msg) == 0) {
        preserve_unhandled_message(&msg);
        ++drain_count;
    }
    
    if (g_kbd_count > 0) {
        *out = g_kbd_queue[g_kbd_tail];
        g_kbd_tail = (g_kbd_tail + 1) % MAX_INPUT_QUEUE;
        g_kbd_count--;
        return 1;
    }
    return 0;
}

int32_t window_input_mouse_poll(input_mouse_event_t *out)
{
    if (out == NULL) {
        os_set_errno(EINVAL);
        return -1;
    }
    ipc_message_t msg;
    uint32_t drain_count = 0;
    while (drain_count < IPC_DRAIN_MAX && ipc_receive_raw(&msg) == 0) {
        preserve_unhandled_message(&msg);
        ++drain_count;
    }

    if (g_mouse_count > 0) {
        *out = g_mouse_queue[g_mouse_tail];
        g_mouse_tail = (g_mouse_tail + 1) % MAX_INPUT_QUEUE;
        g_mouse_count--;
        return 1;
    }
    return 0;
}

int32_t window_input_keyboard_wait(input_keyboard_event_t *out)
{
    while (1) {
        if (window_input_keyboard_poll(out) > 0) return 1;
        process_yield();
    }
}

int32_t window_input_mouse_wait(input_mouse_event_t *out)
{
    while (1) {
        if (window_input_mouse_poll(out) > 0) return 1;
        process_yield();
    }
}

int32_t window_input_keyboard_pending(void)
{
    ipc_message_t msg;
    uint32_t drain_count = 0;
    while (drain_count < IPC_DRAIN_MAX && ipc_receive_raw(&msg) == 0) {
        preserve_unhandled_message(&msg);
        ++drain_count;
    }
    return (int32_t)g_kbd_count;
}

int32_t window_input_mouse_pending(void)
{
    ipc_message_t msg;
    uint32_t drain_count = 0;
    while (drain_count < IPC_DRAIN_MAX && ipc_receive_raw(&msg) == 0) {
        preserve_unhandled_message(&msg);
        ++drain_count;
    }
    return (int32_t)g_mouse_count;
}

int32_t window_set_layout_xml(window_id_t wid, const char *xml_str, uint32_t xml_len)
{
    if (wid == 0 || !xml_str) {
        os_set_errno(EINVAL);
        return -1;
    }
    
    struct {
        uint32_t type;
        uint32_t request_id;
        uint32_t window_id;
        uint32_t total_size;
    } start_cmd;
    
    start_cmd.type = WM_SET_LAYOUT_XML_START;
    start_cmd.request_id = ++g_wm_request_id;
    start_cmd.window_id = wid;
    start_cmd.total_size = xml_len;
    
    int32_t wm_pid = window_get_wm_pid();
    int32_t res = ipc_send_message(wm_pid, &start_cmd, sizeof(start_cmd));

    uint64_t retry_deadline = get_uptime_ms() + WM_SEND_RETRY_TIMEOUT_MS;
    while (res == OS_STATUS_LIMIT_REACHED && !deadline_expired(retry_deadline)) {
        process_yield();
        res = ipc_send_message(wm_pid, &start_cmd, sizeof(start_cmd));
    }
    if (res < 0) return res;
    
    uint32_t offset = 0;
    while (offset < xml_len) {
        char chunk_msg[IPC_MESSAGE_MAX_SIZE];
        wm_msg_header_t *hdr = (wm_msg_header_t *)chunk_msg;
        hdr->type = WM_SET_LAYOUT_XML_CHUNK;
        hdr->request_id = ++g_wm_request_id;
        hdr->window_id = wid;
        
        uint32_t chunk_capacity = IPC_MESSAGE_MAX_SIZE - sizeof(wm_msg_header_t);
        uint32_t copy_size = (xml_len - offset < chunk_capacity) ? (xml_len - offset) : chunk_capacity;
        
        memcpy(chunk_msg + sizeof(wm_msg_header_t), xml_str + offset, copy_size);
        
        res = ipc_send_message(wm_pid, chunk_msg, sizeof(wm_msg_header_t) + copy_size);
        retry_deadline = get_uptime_ms() + WM_SEND_RETRY_TIMEOUT_MS;
        while (res == OS_STATUS_LIMIT_REACHED && !deadline_expired(retry_deadline)) {
            process_yield();
            res = ipc_send_message(wm_pid, chunk_msg, sizeof(wm_msg_header_t) + copy_size);
        }
        if (res < 0) return res;
        
        offset += copy_size;
    }
    
    wm_msg_header_t end_cmd;
    end_cmd.type = WM_SET_LAYOUT_XML_END;
    end_cmd.request_id = ++g_wm_request_id;
    end_cmd.window_id = wid;
    
    res = ipc_send_message(wm_pid, &end_cmd, sizeof(end_cmd));
    retry_deadline = get_uptime_ms() + WM_SEND_RETRY_TIMEOUT_MS;
    while (res == OS_STATUS_LIMIT_REACHED && !deadline_expired(retry_deadline)) {
        process_yield();
        res = ipc_send_message(wm_pid, &end_cmd, sizeof(end_cmd));
    }
    return res;
}

int32_t window_load_layout(window_id_t wid, const char *xml_path)
{
    int32_t fd = file_open(xml_path, 0);
    if (fd < 0) return fd;
    
    char *xml_buf = malloc(4096);
    if (!xml_buf) {
        file_close(fd);
        return -1;
    }
    
    int64_t bytes = file_read(fd, xml_buf, 4095);
    file_close(fd);
    
    if (bytes <= 0) {
        free(xml_buf);
        return -1;
    }
    xml_buf[bytes] = '\0';
    
    int32_t res = window_set_layout_xml(wid, xml_buf, (uint32_t)bytes);
    free(xml_buf);
    return res;
}

void window_draw_text(window_id_t wid, uint32_t x, uint32_t y, const char *text, uint32_t color, float font_size)
{
    struct {
        wm_msg_header_t hdr;
        uint32_t x, y;
        uint32_t color;
        float font_size;
        char text[128];
    } cmd;

    if (!text || wid == 0) return;

    cmd.hdr.type = WM_DRAW_TEXT;
    cmd.hdr.window_id = wid;
    cmd.x = x;
    cmd.y = y;
    cmd.color = color;
    cmd.font_size = font_size;
    os_strcpy_s(cmd.text, sizeof(cmd.text), text);

    ipc_send_message(window_get_wm_pid(), &cmd, sizeof(cmd));
}

void window_show_notification(const char *title, const char *message)
{
    struct {
        wm_msg_header_t hdr;
        char title[64];
        char message[128];
    } cmd;

    if (!title || !message) return;

    cmd.hdr.type = WM_SHOW_NOTIFICATION;
    cmd.hdr.window_id = 0;
    os_strcpy_s(cmd.title, sizeof(cmd.title), title);
    os_strcpy_s(cmd.message, sizeof(cmd.message), message);

    ipc_send_message(window_get_wm_pid(), &cmd, sizeof(cmd));
}

void window_show_dialog(uint32_t type, const char *title, const char *message)
{
    struct {
        wm_msg_header_t hdr;
        uint32_t type;
        char title[64];
        char message[128];
    } cmd;

    if (!title || !message) return;

    cmd.hdr.type = WM_SHOW_DIALOG;
    cmd.hdr.window_id = 0;
    cmd.type = type;
    os_strcpy_s(cmd.title, sizeof(cmd.title), title);
    os_strcpy_s(cmd.message, sizeof(cmd.message), message);

    ipc_send_message(window_get_wm_pid(), &cmd, sizeof(cmd));
}

void window_show_info(const char *title, const char *message)
{
    window_show_dialog(0u, title, message);
}

void window_show_warning(const char *title, const char *message)
{
    window_show_dialog(1u, title, message);
}

void window_show_error(const char *title, const char *message)
{
    window_show_dialog(2u, title, message);
}

uint32_t window_get_capabilities(void)
{
    wm_msg_header_t command;
    memset(&command, 0, sizeof(command));
    command.type = WM_GET_CAPABILITIES;
    command.request_id = ++g_wm_request_id;
    int32_t wm_pid = window_get_wm_pid();
    if (wm_pid < 0 || ipc_send_message(wm_pid, &command, sizeof(command)) < 0) return 0;

    ipc_message_t response __attribute__((aligned(16)));
    uint64_t deadline = get_uptime_ms() + WM_RPC_TIMEOUT_MS;
    while (!deadline_expired(deadline)) {
        if (ipc_receive_raw(&response) == 0) {
            if (response.size >= sizeof(wm_msg_header_t) + sizeof(int32_t) + sizeof(uint32_t)) {
                struct {
                    wm_msg_header_t header;
                    int32_t status;
                    uint32_t capabilities;
                } *reply = (void *)response.data;
                if (reply->header.type == WM_GET_CAPABILITIES &&
                    reply->header.request_id == command.request_id) {
                    return reply->status == WM_STATUS_OK ? reply->capabilities : 0u;
                }
            }
            preserve_unhandled_message(&response);
        }
        process_yield();
    }
    os_set_errno(ETIMEDOUT);
    return 0;
}

static uint32_t *window_get_backing_store_internal(window_id_t wid, uint32_t *out_w, uint32_t *out_h, int32_t wm_pid)
{
    wm_msg_header_t request;
    memset(&request, 0, sizeof(request));
    request.type = WM_GET_BACKING_STORE;
    request.request_id = ++g_wm_request_id;
    request.window_id = wid;

    if (wm_pid < 0 ||
        ipc_send_message(wm_pid, &request, sizeof(request)) < 0) {
        return NULL;
    }

    ipc_message_t incoming __attribute__((aligned(16)));
    uint64_t deadline = get_uptime_ms() + WM_RPC_TIMEOUT_MS;
    while (!deadline_expired(deadline)) {
        if (ipc_receive_raw(&incoming) == 0) {
            if (incoming.size >= sizeof(wm_backing_store_response_t)) {
                const wm_backing_store_response_t *response =
                    (const void *)incoming.data;
                if (response->header.type == WM_BACKING_STORE_READY &&
                    response->header.request_id == request.request_id) {
                    if (response->status != WM_STATUS_OK ||
                        response->shared_memory_handle <= 0 ||
                        response->width == 0u || response->height == 0u ||
                        response->size_bytes <
                            (uint64_t)response->width * response->height *
                            sizeof(uint32_t)) {
                        if (response->status != WM_STATUS_OK)
                            (void)os_errno_from_i32_status(response->status);
                        else
                            os_set_errno(EINVAL);
                        return NULL;
                    }

                    window_backing_mapping_t *free_slot = NULL;
                    for (uint32_t i = 0u;
                         i < sizeof(g_window_backing_mappings) /
                             sizeof(g_window_backing_mappings[0]);
                         ++i) {
                        window_backing_mapping_t *mapping =
                            &g_window_backing_mappings[i];
                        if (mapping->window_id == wid) {
                            if (mapping->handle ==
                                response->shared_memory_handle) {
                                if (out_w) *out_w = response->width;
                                if (out_h) *out_h = response->height;
                                os_clear_errno();
                                return mapping->address;
                            }
                            (void)os_shared_memory_unmap(
                                mapping->handle, mapping->address);
                            memset(mapping, 0, sizeof(*mapping));
                            free_slot = mapping;
                            break;
                        }
                        if (!free_slot && mapping->window_id == 0u)
                            free_slot = mapping;
                    }
                    if (!free_slot) {
                        os_set_errno(ENOBUFS);
                        return NULL;
                    }

                    uint32_t *address = (uint32_t *)os_shared_memory_map(
                        response->shared_memory_handle);
                    if (!address) return NULL;
                    free_slot->window_id = wid;
                    free_slot->handle = response->shared_memory_handle;
                    free_slot->address = address;
                    free_slot->size_bytes = response->size_bytes;
                    if (out_w) *out_w = response->width;
                    if (out_h) *out_h = response->height;
                    os_clear_errno();
                    return address;
                }
            }
            preserve_unhandled_message(&incoming);
        }
        process_yield();
    }
    os_set_errno(ETIMEDOUT);
    return NULL;
}

uint32_t *window_get_backing_store(window_id_t wid, uint32_t *out_w, uint32_t *out_h)
{
    if (out_w) *out_w = 0u;
    if (out_h) *out_h = 0u;
    if (wid == 0u) {
        os_set_errno(EINVAL);
        return NULL;
    }

    int32_t wm_pid = window_get_wm_pid();
    if (wm_pid < 0) {
        return NULL;
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        uint32_t *pixels = window_get_backing_store_internal(wid, out_w, out_h, wm_pid);
        if (pixels) return pixels;

        if (os_get_errno() != ETIMEDOUT) {
            break;
        }
    }

    return NULL;
}

void window_release_backing_store(window_id_t wid)
{
    if (wid == 0u) return;
    for (uint32_t i = 0u;
         i < sizeof(g_window_backing_mappings) /
             sizeof(g_window_backing_mappings[0]);
         ++i) {
        window_backing_mapping_t *mapping = &g_window_backing_mappings[i];
        if (mapping->window_id != wid) continue;
        (void)os_shared_memory_unmap(mapping->handle, mapping->address);
        memset(mapping, 0, sizeof(*mapping));
        return;
    }
}

void window_damage(window_id_t wid, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    struct {
        wm_msg_header_t header;
        uint32_t x, y, w, h;
    } command;
    memset(&command, 0, sizeof(command));
    command.header.type = WM_DAMAGE;
    command.header.window_id = wid;
    command.x = x;
    command.y = y;
    command.w = w;
    command.h = h;
    (void)ipc_send_message(window_get_wm_pid(), &command, sizeof(command));
}

void window_begin_transaction(window_id_t wid)
{
    wm_msg_header_t command;
    memset(&command, 0, sizeof(command));
    command.type = WM_BEGIN_TRANSACTION;
    command.window_id = wid;
    (void)ipc_send_message(window_get_wm_pid(), &command, sizeof(command));
}

void window_end_transaction(window_id_t wid)
{
    wm_msg_header_t command;
    memset(&command, 0, sizeof(command));
    command.type = WM_END_TRANSACTION;
    command.window_id = wid;
    (void)ipc_send_message(window_get_wm_pid(), &command, sizeof(command));
}

int32_t window_set_icon_path(window_id_t wid, const char *path)
{
    if (wid == 0u || !path) return WM_STATUS_INVALID_ARG;
    struct {
        wm_msg_header_t header;
        char path[192];
    } command;
    memset(&command, 0, sizeof(command));
    command.header.type = WM_SET_WINDOW_ICON_PATH;
    command.header.window_id = wid;
    os_strcpy_s(command.path, sizeof(command.path), path);
    return ipc_send_message(window_get_wm_pid(), &command, sizeof(command));
}

int32_t window_set_surface_opaque(window_id_t wid, bool opaque)
{
    if (wid == 0u) return WM_STATUS_INVALID_ARG;
    struct {
        wm_msg_header_t header;
        bool opaque;
    } command;
    memset(&command, 0, sizeof(command));
    command.header.type = WM_SET_WINDOW_SURFACE_OPAQUE;
    command.header.window_id = wid;
    command.opaque = opaque;
    return ipc_send_message(window_get_wm_pid(), &command, sizeof(command));
}
bool udp_send(uint32_t dst_ipv4_addr,
              uint16_t src_port,
              uint16_t dst_port,
              const void *payload,
              uint16_t payload_len)
{
    uint64_t arg2 = ((uint64_t)src_port << 16) | dst_port;
    uint64_t result = syscall4(SYSCALL_UDP_SEND, 
                               (uint64_t)dst_ipv4_addr, 
                               arg2, 
                               (uint64_t)(uintptr_t)payload, 
                               (uint64_t)payload_len);
    return result != 0;
}

int32_t udp_bind_port(uint16_t port)
{
    return (int32_t)syscall1(SYSCALL_UDP_BIND, (uint64_t)port);
}

int32_t udp_unbind_port(uint16_t port)
{
    return (int32_t)syscall1(SYSCALL_UDP_UNBIND, (uint64_t)port);
}

int32_t udp_recv(uint16_t port, void *buf, uint32_t buf_len)
{
    return (int32_t)syscall3(SYSCALL_UDP_RECV,
                             (uint64_t)port,
                             (uint64_t)(uintptr_t)buf,
                             (uint64_t)buf_len);
}

bool wifi_scan_start(void)
{
    return syscall0(SYSCALL_WIFI_SCAN_START) != 0u;
}

uint32_t wifi_get_scan_results(wifi_scan_result_t *out, uint32_t max_count)
{
    if (max_count > WIFI_MAX_SCAN_RESULTS) {
        max_count = WIFI_MAX_SCAN_RESULTS;
    }
    return (uint32_t)syscall2(SYSCALL_WIFI_GET_SCAN_RESULTS,
                              (uint64_t)(uintptr_t)out,
                              (uint64_t)max_count);
}

bool wifi_connect(const char *ssid, const char *psk)
{
    return syscall2(SYSCALL_WIFI_CONNECT,
                    (uint64_t)(uintptr_t)ssid,
                    (uint64_t)(uintptr_t)psk) != 0u;
}

void wifi_disconnect(void)
{
    (void)syscall0(SYSCALL_WIFI_DISCONNECT);
}

void wifi_get_status(wifi_status_t *out_status)
{
    (void)syscall1(SYSCALL_WIFI_GET_STATUS, (uint64_t)(uintptr_t)out_status);
}

uint32_t net_get_dhcp_dns_server(void)
{
    return (uint32_t)syscall0(SYSCALL_NET_GET_DHCP_DNS);
}

int32_t tcp_connect(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port)
{
    uint64_t arg2 = ((uint64_t)remote_port << 16) | (uint64_t)local_port;
    return (int32_t)syscall2(SYSCALL_TCP_CONNECT, (uint64_t)remote_ip, arg2);
}

int32_t tcp_listen(uint16_t port)
{
    return (int32_t)syscall1(SYSCALL_TCP_LISTEN, (uint64_t)port);
}

int32_t tcp_accept(int32_t listen_conn_id)
{
    return (int32_t)syscall1(SYSCALL_TCP_ACCEPT, (uint64_t)(int64_t)listen_conn_id);
}

int32_t tcp_send(int32_t conn_id, const void *data, uint16_t len)
{
    return (int32_t)syscall3(SYSCALL_TCP_SEND,
                             (uint64_t)(int64_t)conn_id,
                             (uint64_t)(uintptr_t)data,
                             (uint64_t)len);
}

int32_t tcp_recv(int32_t conn_id, void *buf, uint16_t buf_len)
{
    return (int32_t)syscall3(SYSCALL_TCP_RECV,
                             (uint64_t)(int64_t)conn_id,
                             (uint64_t)(uintptr_t)buf,
                             (uint64_t)buf_len);
}

int32_t tcp_close(int32_t conn_id)
{
    return (int32_t)syscall1(SYSCALL_TCP_CLOSE, (uint64_t)(int64_t)conn_id);
}

int32_t tcp_get_state(int32_t conn_id)
{
    return (int32_t)syscall1(SYSCALL_TCP_GET_STATE, (uint64_t)(int64_t)conn_id);
}

#define SYSCALL_PROCESS_WAITPID    110ULL
#define SYSCALL_PROCESS_GETPPID    111ULL
#define SYSCALL_PROCESS_EXIT_STATUS 112ULL
#define SYSCALL_SLEEP_MS           113ULL
#define SYSCALL_FILE_STAT_NUM      114ULL
#define SYSCALL_GET_UPTIME_MS_NUM  119ULL
#define SYSCALL_GET_PROC_COUNT_NUM 121ULL
#define SYSCALL_GET_PROC_INFO_NUM  122ULL
#define SYSCALL_GET_PROC_PERF_INFO_NUM 125ULL
#define SYSCALL_GET_BOOT_PROFILE_COUNT_NUM 126ULL
#define SYSCALL_GET_BOOT_PROFILE_ENTRY_NUM 127ULL
#define SYSCALL_GET_RTC_TIME       140ULL
#define SYSCALL_GET_TOTAL_MEMORY_NUM 253ULL
#define SYSCALL_GET_USED_MEMORY_NUM  254ULL
#define SYSCALL_TKILL_NUM            186ULL

int32_t process_kill(int32_t pid)
{
    return (int32_t)syscall1(SYSCALL_TKILL_NUM, (uint64_t)(int64_t)pid);
}

#define SYSCALL_SET_PROCESS_PRIORITY_NUM 236ULL

int32_t process_set_priority(int32_t pid, uint32_t priority)
{
    return (int32_t)syscall2(SYSCALL_SET_PROCESS_PRIORITY_NUM,
                             (uint64_t)(int64_t)pid,
                             (uint64_t)priority);
}

uint64_t get_total_memory(void)
{
    return syscall0(SYSCALL_GET_TOTAL_MEMORY_NUM);
}

uint64_t get_used_memory(void)
{
    return syscall0(SYSCALL_GET_USED_MEMORY_NUM);
}

int32_t sys_get_rtc_time(rtc_time_t *time)
{
    return (int32_t)syscall1(SYSCALL_GET_RTC_TIME, (uint64_t)time);
}

int32_t process_waitpid(int32_t pid, int32_t *status_out, int32_t options)
{
    return (int32_t)syscall3(SYSCALL_PROCESS_WAITPID,
                             (uint64_t)(int64_t)pid,
                             (uint64_t)(uintptr_t)status_out,
                             (uint64_t)(int64_t)options);
}

int32_t process_getppid(void)
{
    return (int32_t)syscall0(SYSCALL_PROCESS_GETPPID);
}

void process_exit(int32_t status)
{
    (void)syscall1(SYSCALL_PROCESS_EXIT_STATUS, (uint64_t)(int64_t)status);
    while (1) { sleep_ms(1000u); }
}

void system_shutdown(void)
{
    (void)syscall0(SYSCALL_SYSTEM_SHUTDOWN);
    while (1) { sleep_ms(1000u); }
}

void system_shutdown_broadcast(void)
{
    (void)syscall0(SYSCALL_SYSTEM_SHUTDOWN_BROADCAST);
}

void system_reboot(void)
{
    (void)syscall0(SYSCALL_SYSTEM_REBOOT);
    while (1) { sleep_ms(1000u); }
}

void sleep_ms(uint64_t ms)
{
    (void)syscall1(SYSCALL_SLEEP_MS, ms);
}

uint64_t get_uptime_ms(void)
{
    return syscall0(SYSCALL_GET_UPTIME_MS_NUM);
}

int32_t file_stat(const char *path, file_stat_t *stat_out)
{
    return os_errno_from_i32_status(
        (int32_t)syscall2(SYSCALL_FILE_STAT_NUM,
                          (uint64_t)(uintptr_t)path,
                          (uint64_t)(uintptr_t)stat_out));
}

int32_t get_process_count(void)
{
    return (int32_t)syscall0(SYSCALL_GET_PROC_COUNT_NUM);
}

int32_t get_process_info(int32_t pid, process_info_t *info_out)
{
    return os_errno_from_i32_status(
        (int32_t)syscall2(SYSCALL_GET_PROC_INFO_NUM,
                          (uint64_t)(int64_t)pid,
                          (uint64_t)(uintptr_t)info_out));
}

int32_t get_process_perf_info(int32_t pid, process_perf_info_t *info_out)
{
    return os_errno_from_i32_status(
        (int32_t)syscall2(SYSCALL_GET_PROC_PERF_INFO_NUM,
                          (uint64_t)(int64_t)pid,
                          (uint64_t)(uintptr_t)info_out));
}

int32_t get_boot_profile_count(void)
{
    return (int32_t)syscall0(SYSCALL_GET_BOOT_PROFILE_COUNT_NUM);
}

int32_t get_boot_profile_entry(int32_t index, boot_profile_entry_t *entry_out)
{
    return os_errno_from_i32_status(
        (int32_t)syscall2(SYSCALL_GET_BOOT_PROFILE_ENTRY_NUM,
                          (uint64_t)(int64_t)index,
                          (uint64_t)(uintptr_t)entry_out));
}

#define SYSCALL_FILE_PIPE_NUM  115ULL
#define SYSCALL_FILE_DUP_NUM   116ULL
#define SYSCALL_FILE_DUP2_NUM  117ULL

int32_t file_pipe(int32_t fds[2])
{
    return os_errno_from_i32_status(
        (int32_t)syscall1(SYSCALL_FILE_PIPE_NUM,
                          (uint64_t)(uintptr_t)fds));
}

int32_t file_dup(int32_t oldfd)
{
    return (int32_t)syscall1(SYSCALL_FILE_DUP_NUM, (uint64_t)(int64_t)oldfd);
}

int32_t file_dup2(int32_t oldfd, int32_t newfd)
{
    return (int32_t)syscall2(SYSCALL_FILE_DUP2_NUM,
                             (uint64_t)(int64_t)oldfd,
                             (uint64_t)(int64_t)newfd);
}

int32_t socket_create(int32_t type)
{
    return (int32_t)syscall1(SYSCALL_SOCKET_CREATE, (uint64_t)(int64_t)type);
}

int32_t socket_connect(int32_t sockfd, uint32_t ip, uint16_t port)
{
    return (int32_t)syscall3(SYSCALL_SOCKET_CONNECT,
                             (uint64_t)(int64_t)sockfd,
                             (uint64_t)ip,
                             (uint64_t)port);
}

int32_t socket_bind(int32_t sockfd, uint16_t port)
{
    return (int32_t)syscall2(SYSCALL_SOCKET_BIND,
                             (uint64_t)(int64_t)sockfd, (uint64_t)port);
}

int32_t socket_listen(int32_t sockfd)
{
    return (int32_t)syscall1(SYSCALL_SOCKET_LISTEN, (uint64_t)(int64_t)sockfd);
}

int32_t socket_listen_with_backlog(int32_t sockfd, int32_t backlog)
{
    return (int32_t)syscall2(SYSCALL_SOCKET_LISTEN_EX,
                             (uint64_t)(int64_t)sockfd,
                             (uint64_t)(int64_t)backlog);
}

int32_t socket_accept(int32_t sockfd)
{
    return (int32_t)syscall1(SYSCALL_SOCKET_ACCEPT, (uint64_t)(int64_t)sockfd);
}

int32_t socket_send(int32_t sockfd, const void *data, uint32_t len)
{
    return (int32_t)syscall3(SYSCALL_SOCKET_SEND,
                             (uint64_t)(int64_t)sockfd,
                             (uint64_t)(uintptr_t)data,
                             (uint64_t)len);
}

int32_t socket_recv(int32_t sockfd, void *buf, uint32_t buf_len)
{
    return (int32_t)syscall3(SYSCALL_SOCKET_RECV,
                             (uint64_t)(int64_t)sockfd,
                             (uint64_t)(uintptr_t)buf,
                             (uint64_t)buf_len);
}

int32_t socket_close(int32_t sockfd)
{
    return (int32_t)syscall1(SYSCALL_SOCKET_CLOSE, (uint64_t)(int64_t)sockfd);
}

int32_t socket_get_info(int32_t sockfd, socket_info_t *info_out)
{
    return (int32_t)syscall2(SYSCALL_SOCKET_GET_INFO,
                             (uint64_t)(int64_t)sockfd,
                             (uint64_t)(uintptr_t)info_out);
}

int32_t socket_set_option(int32_t sockfd, int32_t level,
                          int32_t option, int32_t value)
{
    return (int32_t)syscall4(SYSCALL_SOCKET_SET_OPTION,
                             (uint64_t)(int64_t)sockfd,
                             (uint64_t)(int64_t)level,
                             (uint64_t)(int64_t)option,
                             (uint64_t)(int64_t)value);
}

int32_t socket_get_option(int32_t sockfd, int32_t level,
                          int32_t option, int32_t *value_out)
{
    return (int32_t)syscall4(SYSCALL_SOCKET_GET_OPTION,
                             (uint64_t)(int64_t)sockfd,
                             (uint64_t)(int64_t)level,
                             (uint64_t)(int64_t)option,
                             (uint64_t)(uintptr_t)value_out);
}

int32_t socket_shutdown(int32_t sockfd, int32_t how)
{
    return (int32_t)syscall2(SYSCALL_SOCKET_SHUTDOWN,
                             (uint64_t)(int64_t)sockfd,
                             (uint64_t)(int64_t)how);
}

#define SYSCALL_KVM_OPEN_NUM   240ULL
#define SYSCALL_KVM_IOCTL_NUM  241ULL
#define SYSCALL_KVM_CLOSE_NUM  242ULL
#define SYSCALL_KVM_MMAP_NUM   243ULL

int32_t kvm_open(void)
{
    return (int32_t)syscall0(SYSCALL_KVM_OPEN_NUM);
}

int64_t kvm_ioctl(int32_t fd, uint64_t request, uint64_t arg)
{
    return (int64_t)syscall3(SYSCALL_KVM_IOCTL_NUM,
                             (uint64_t)(int64_t)fd,
                             request,
                             arg);
}

int32_t kvm_close(int32_t fd)
{
    return (int32_t)syscall1(SYSCALL_KVM_CLOSE_NUM, (uint64_t)(int64_t)fd);
}

void *kvm_mmap(int32_t fd, uint64_t offset, uint64_t size)
{
    return (void *)(uintptr_t)syscall3(SYSCALL_KVM_MMAP_NUM,
                                       (uint64_t)(int64_t)fd,
                                       offset,
                                       size);
}

#define SYSCALL_GET_CPU_INFO       200ULL
#define SYSCALL_GET_MEMORY_INFO   201ULL
#define SYSCALL_GET_VMEM_INFO     202ULL
#define SYSCALL_GET_DISK_INFO     203ULL
#define SYSCALL_GET_DEVICE_INFO   204ULL
#define SYSCALL_GET_GRAPHICS_INFO 205ULL
#define SYSCALL_GET_ARCH_INFO     206ULL
#define SYSCALL_GET_SYSTEM_INFO   207ULL
#define SYSCALL_GET_DISK_COUNT    216ULL
#define SYSCALL_RAW_BLOCK_READ    217ULL
#define SYSCALL_RAW_BLOCK_WRITE   218ULL
#define SYSCALL_GET_BOOT_FONT     219ULL
#define SYSCALL_GET_CPU_USAGE     235ULL

int64_t os_get_cpu_info(system_cpu_info_t *out_info)
{
    if (out_info == NULL) {
        return -22;
    }
    return syscall1(SYSCALL_GET_CPU_INFO, (uint64_t)(uintptr_t)out_info);
}

int64_t os_get_cpu_usage(system_cpu_usage_t *out_usage)
{
    if (out_usage == NULL) {
        return -22;
    }
    return syscall1(SYSCALL_GET_CPU_USAGE, (uint64_t)(uintptr_t)out_usage);
}

int64_t os_get_memory_info(system_memory_info_t *out_info)
{
    if (out_info == NULL) {
        return -22;
    }
    return syscall1(SYSCALL_GET_MEMORY_INFO, (uint64_t)(uintptr_t)out_info);
}

int64_t os_get_vmem_info(system_vmem_info_t *out_info)
{
    if (out_info == NULL) {
        return -22;
    }
    return syscall1(SYSCALL_GET_VMEM_INFO, (uint64_t)(uintptr_t)out_info);
}

int64_t os_get_disk_count(uint32_t *out_count)
{
    if (out_count == NULL) {
        return -22;
    }
    return syscall1(SYSCALL_GET_DISK_COUNT, (uint64_t)(uintptr_t)out_count);
}

int64_t os_get_disk_info(uint32_t index, system_disk_info_t *out_info)
{
    if (out_info == NULL) {
        return -22;
    }
    return syscall2(SYSCALL_GET_DISK_INFO, (uint64_t)index, (uint64_t)(uintptr_t)out_info);
}

int64_t os_raw_block_read(uint32_t disk_index, uint64_t lba, void *buffer, uint32_t sectors)
{
    if (buffer == NULL && sectors != 0) {
        return -22;
    }
    return syscall4(SYSCALL_RAW_BLOCK_READ,
                    (uint64_t)disk_index,
                    (uint64_t)lba,
                    (uint64_t)(uintptr_t)buffer,
                    (uint64_t)sectors);
}

int64_t os_raw_block_write(uint32_t disk_index, uint64_t lba, const void *buffer, uint32_t sectors)
{
    if (buffer == NULL && sectors != 0) {
        return -22;
    }
    return syscall4(SYSCALL_RAW_BLOCK_WRITE,
                    (uint64_t)disk_index,
                    (uint64_t)lba,
                    (uint64_t)(uintptr_t)buffer,
                    (uint64_t)sectors);
}

int64_t os_get_boot_font(void *buffer, uint64_t capacity)
{
    return syscall2(SYSCALL_GET_BOOT_FONT,
                    (uint64_t)(uintptr_t)buffer,
                    capacity);
}

int64_t os_get_device_count(uint32_t *out_count)
{
    if (out_count == NULL) {
        return -22;
    }
    
    int64_t result = syscall1(SYSCALL_GET_DEVICE_INFO, 0xFFFFFFFF);
    *out_count = (uint32_t)result;
    return 0;
}

int64_t os_get_device_info(uint32_t index, system_device_t *out_info)
{
    if (out_info == NULL) {
        return -22;
    }
    return syscall2(SYSCALL_GET_DEVICE_INFO, (uint64_t)index, (uint64_t)(uintptr_t)out_info);
}

int64_t os_get_graphics_info(system_graphics_info_t *out_info)
{
    if (out_info == NULL) {
        return -22;
    }
    return syscall1(SYSCALL_GET_GRAPHICS_INFO, (uint64_t)(uintptr_t)out_info);
}

int64_t os_get_arch_info(system_arch_info_t *out_info)
{
    if (out_info == NULL) {
        return -22;
    }
    return syscall1(SYSCALL_GET_ARCH_INFO, (uint64_t)(uintptr_t)out_info);
}

int64_t os_get_system_info(system_info_t *out_info)
{
    if (out_info == NULL) {
        return -22;
    }
    return syscall1(SYSCALL_GET_SYSTEM_INFO, (uint64_t)(uintptr_t)out_info);
}

int32_t os_audio_open(void)
{
    return os_errno_from_i32_status((int32_t)syscall0(SYSCALL_AUDIO_OPEN));
}

int32_t os_audio_get_info(os_audio_info_t *out_info)
{
    if (out_info == NULL) return -22;
    return os_errno_from_i32_status((int32_t)syscall1(
        SYSCALL_AUDIO_GET_INFO, (uint64_t)(uintptr_t)out_info));
}

int64_t os_audio_write(const void *pcm, uint64_t bytes)
{
    if (pcm == NULL || bytes == 0u || (bytes & 3u) != 0u) return -22;
    return os_errno_from_i64_status((int64_t)syscall2(
        SYSCALL_AUDIO_WRITE, (uint64_t)(uintptr_t)pcm, bytes));
}

int32_t os_audio_drain(uint32_t timeout_ms)
{
    return os_errno_from_i32_status((int32_t)syscall1(
        SYSCALL_AUDIO_DRAIN, timeout_ms));
}

int32_t os_audio_close(void)
{
    return os_errno_from_i32_status((int32_t)syscall0(SYSCALL_AUDIO_CLOSE));
}

int32_t get_process_debug_info(int32_t pid, process_debug_info_t *info_out)
{
    return os_errno_from_i32_status(
        (int32_t)syscall2(SYSCALL_GET_PROC_DEBUG_INFO,
                          (uint64_t)(int64_t)pid,
                          (uint64_t)(uintptr_t)info_out));
}

int32_t get_os_debug_info(os_debug_info_t *info_out)
{
    return os_errno_from_i32_status(
        (int32_t)syscall1(SYSCALL_GET_OS_DEBUG_INFO,
                          (uint64_t)(uintptr_t)info_out));
}

int32_t read_kernel_log(char *buf, uint32_t buf_size)
{
    if (buf == NULL || buf_size == 0u) {
        return -1;
    }
    return os_errno_from_i32_status(
        (int32_t)syscall2(SYSCALL_READ_KERNEL_LOG,
                          (uint64_t)(uintptr_t)buf,
                          (uint64_t)buf_size));
}
