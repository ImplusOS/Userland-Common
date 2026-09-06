/*
 * Wayland wire protocol, by hand -- no libwayland.
 *
 * Interfaces served: wl_display, wl_registry, wl_callback, wl_compositor,
 * wl_region, wl_shm (+pool, +buffer), wl_surface, wl_seat (+pointer,
 * +keyboard), wl_output, wl_subcompositor (+subsurface),
 * wl_data_device_manager (+device, +source), xdg_wm_base, xdg_positioner,
 * xdg_surface, xdg_toplevel, xdg_popup.
 *
 * Object ids are client-allocated and always below WLC_MAX_ID, so each
 * client's object table is a flat array. Every client gets its own table:
 * more than one GTK application can be connected at a time.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "Compositor.h"
#include "File.h"
#include "Memory.h"
#include "Process.h"

extern uint64_t syscall1(uint64_t, uint64_t);
extern uint64_t syscall2(uint64_t, uint64_t, uint64_t);
extern uint64_t syscall3(uint64_t, uint64_t, uint64_t, uint64_t);

#define SYS_UNIX_SEND       225
#define SYS_UNIX_SENDMSG    227
#define SYS_UNIX_RECVMSG    228
#define SYS_UNIX_CLOSE      229

/* struct msghdr / iovec / cmsghdr, x86-64 glibc layout -- the kernel reads
 * these fields directly (Kernel/IPC/UnixSocket.c). */
struct msghdr_k {
    uint64_t name; uint32_t namelen; uint32_t _p0;
    uint64_t iov;  uint64_t iovlen;
    uint64_t control; uint64_t controllen;
    int32_t flags; uint32_t _p1;
};
struct iovec_k  { uint64_t base; uint64_t len; };
struct cmsghdr_k { uint32_t len; uint32_t _pad; int32_t level; int32_t type; };

static int32_t u_send(int32_t fd, const void *b, uint32_t n)
{
    return (int32_t)syscall3(SYS_UNIX_SEND, (uint64_t)fd,
                             (uint64_t)(uintptr_t)b, n);
}

/* Send bytes plus one SCM_RIGHTS fd. Only wl_keyboard.keymap needs this. */
static int32_t u_send_fd(int32_t fd, const void *b, uint32_t n, int32_t passfd)
{
    struct iovec_k iov = { (uint64_t)(uintptr_t)b, n };
    uint8_t cbuf[sizeof(struct cmsghdr_k) + sizeof(int32_t)];
    struct cmsghdr_k *c = (struct cmsghdr_k *)cbuf;
    struct msghdr_k msg;

    c->len = (uint32_t)sizeof(cbuf);
    c->_pad = 0;
    c->level = 1;          /* SOL_SOCKET */
    c->type = 1;           /* SCM_RIGHTS */
    memcpy(cbuf + sizeof(*c), &passfd, sizeof(passfd));

    memset(&msg, 0, sizeof(msg));
    msg.iov = (uint64_t)(uintptr_t)&iov;
    msg.iovlen = 1;
    msg.control = (uint64_t)(uintptr_t)cbuf;
    msg.controllen = sizeof(cbuf);
    return (int32_t)syscall2(SYS_UNIX_SENDMSG, (uint64_t)fd,
                             (uint64_t)(uintptr_t)&msg);
}

/* ------------------------------------------------------------------ */
/* message building                                                     */
/* ------------------------------------------------------------------ */

#define WLC_MSG_CAP 512u

typedef struct {
    uint8_t  b[WLC_MSG_CAP];
    uint32_t n;
    bool     overflow;
} wlc_msg_t;

static void w_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t r_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void m_begin(wlc_msg_t *m, uint32_t id, uint16_t op)
{
    m->n = 8;
    m->overflow = false;
    w_u32(m->b, id);
    w_u32(m->b + 4, (uint32_t)op);      /* size patched in m_send */
}

static void m_u32(wlc_msg_t *m, uint32_t v)
{
    if (m->n + 4u > WLC_MSG_CAP) { m->overflow = true; return; }
    w_u32(m->b + m->n, v);
    m->n += 4u;
}

static void m_i32(wlc_msg_t *m, int32_t v) { m_u32(m, (uint32_t)v); }

/* wl_fixed_t: 24.8 signed fixed point. */
static void m_fixed(wlc_msg_t *m, int32_t whole) { m_i32(m, whole * 256); }

static void m_str(wlc_msg_t *m, const char *s)
{
    uint32_t len = (uint32_t)strlen(s) + 1u;
    uint32_t pad = (len + 3u) & ~3u;
    if (m->n + 4u + pad > WLC_MSG_CAP) { m->overflow = true; return; }
    w_u32(m->b + m->n, len);
    m->n += 4u;
    memset(m->b + m->n, 0, pad);
    memcpy(m->b + m->n, s, len - 1u);
    m->n += pad;
}

static void m_array_empty(wlc_msg_t *m) { m_u32(m, 0u); }

/* A partial write would split an event in half and desynchronise the
 * client's parser for good, so keep pushing while the socket drains. The
 * wait is bounded in time rather than in iterations: a yield costs nothing
 * when nothing else is runnable, so a spin count is no protection at all.
 * If the budget does run out the stream is unusable, and the connection is
 * marked broken for the pump to close -- better than feeding the client
 * half an event. */
#define WLC_SEND_BUDGET_MS 250u

static void send_all(wlc_client_t *c, const uint8_t *b, uint32_t n)
{
    uint32_t off = 0u;
    uint32_t started = wlc_now_ms();
    uint32_t spins = 0u;

    while (off < n) {
        int32_t r = u_send(c->fd, b + off, n - off);
        if (r > 0) { off += (uint32_t)r; continue; }
        if (r != -11) break;                         /* hard error */
        if (wlc_now_ms() - started > WLC_SEND_BUDGET_MS) break;
        if (++spins < 64u) process_yield(); else sleep_ms(1);
    }
    if (off < n) {
        wlc_log("[wl] client stopped draining, dropping the connection\n");
        c->broken = true;
    }
}

static void m_send(wlc_msg_t *m, wlc_client_t *c)
{
    if (m->overflow || !c || c->fd < 0) return;
    w_u32(m->b + 4, (m->n << 16) | r_u32(m->b + 4));
    send_all(c, m->b, m->n);
}

static void m_send_fd(wlc_msg_t *m, wlc_client_t *c, int32_t passfd)
{
    if (m->overflow || !c || c->fd < 0) return;
    w_u32(m->b + 4, (m->n << 16) | r_u32(m->b + 4));
    (void)u_send_fd(c->fd, m->b, m->n, passfd);
}

/* ------------------------------------------------------------------ */
/* object table                                                         */
/* ------------------------------------------------------------------ */

wlc_client_t g_clients[WLC_MAX_CLIENTS];

static uint32_t g_serial = 1u;
static uint32_t next_serial(void) { return g_serial++; }

/* Which surface currently has pointer / keyboard focus, as a scene index. */
static int32_t g_pointer_focus = -1;
static int32_t g_keyboard_focus = -1;

static wlc_obj_t *obj(wlc_client_t *c, uint32_t id)
{
    if (!c || !c->obj || id == 0u || id >= WLC_MAX_ID) return NULL;
    return &c->obj[id];
}

static wlc_obj_t *obj_set(wlc_client_t *c, uint32_t id, uint8_t type)
{
    wlc_obj_t *o = obj(c, id);
    if (!o) return NULL;
    memset(o, 0, sizeof(*o));
    o->type = type;
    o->s_window = -1;
    o->pool_index = -1;
    o->b_pool = -1;
    return o;
}

static void wl_delete_id(wlc_client_t *c, uint32_t id)
{
    wlc_msg_t m;
    m_begin(&m, 1u, 1u);            /* wl_display.delete_id */
    m_u32(&m, id);
    m_send(&m, c);
}

/* Drop an object and tell the client its id is free again. */
static void obj_destroy(wlc_client_t *c, uint32_t id)
{
    wlc_obj_t *o = obj(c, id);
    if (!o || o->type == O_NONE) return;
    memset(o, 0, sizeof(*o));
    wl_delete_id(c, id);
}

/* ------------------------------------------------------------------ */
/* wl_shm pool mappings                                                 */
/* ------------------------------------------------------------------ */

/*
 * A pool's shared-memory mapping is reference counted separately from the
 * wl_shm_pool object, because GTK destroys the pool object the moment it
 * has created a buffer from it -- the pixels must stay mapped for as long
 * as any buffer still points at them. Kernel shared-memory objects are a
 * scarce global resource (256 of them), so this has to be exact: the last
 * unref unmaps, which is what lets the kernel free the pages.
 */
#define WLC_MAX_POOLS 64u

typedef struct {
    int32_t       handle;            /* kernel shm handle, -1 = free slot */
    uint8_t      *base;
    uint32_t      size;
    uint32_t      refs;
    wlc_client_t *owner;
} wlc_pool_t;

static wlc_pool_t g_pools[WLC_MAX_POOLS];

static int32_t pool_alloc(wlc_client_t *c, int32_t handle, uint8_t *base,
                          uint32_t size)
{
    for (uint32_t i = 0; i < WLC_MAX_POOLS; i++) {
        if (g_pools[i].handle >= 0) continue;
        g_pools[i].handle = handle;
        g_pools[i].base = base;
        g_pools[i].size = size;
        g_pools[i].refs = 1u;            /* the wl_shm_pool object */
        g_pools[i].owner = c;
        return (int32_t)i;
    }
    return -1;
}

static wlc_pool_t *pool_get(int32_t index)
{
    if (index < 0 || (uint32_t)index >= WLC_MAX_POOLS) return NULL;
    if (g_pools[index].handle < 0) return NULL;
    return &g_pools[index];
}

static void pool_ref(int32_t index)
{
    wlc_pool_t *p = pool_get(index);
    if (p) p->refs++;
}

static void pool_unref(int32_t index)
{
    wlc_pool_t *p = pool_get(index);
    if (!p) return;
    if (p->refs > 0u) p->refs--;
    if (p->refs == 0u) {
        (void)os_shared_memory_unmap(p->handle, p->base);
        memset(p, 0, sizeof(*p));
        p->handle = -1;
    }
}

static void pool_drop_client(wlc_client_t *c)
{
    for (uint32_t i = 0; i < WLC_MAX_POOLS; i++) {
        if (g_pools[i].handle >= 0 && g_pools[i].owner == c) {
            (void)os_shared_memory_unmap(g_pools[i].handle, g_pools[i].base);
            memset(&g_pools[i], 0, sizeof(g_pools[i]));
            g_pools[i].handle = -1;
        }
    }
}

static void pool_init_table(void)
{
    for (uint32_t i = 0; i < WLC_MAX_POOLS; i++) g_pools[i].handle = -1;
}

/* ------------------------------------------------------------------ */
/* client lifecycle                                                     */
/* ------------------------------------------------------------------ */

void wlc_client_init_slots(void)
{
    pool_init_table();
    for (uint32_t i = 0; i < WLC_MAX_CLIENTS; i++) {
        memset(&g_clients[i], 0, sizeof(g_clients[i]));
        g_clients[i].fd = -1;
    }
}

bool wlc_client_accept(int32_t fd)
{
    for (uint32_t i = 0; i < WLC_MAX_CLIENTS; i++) {
        wlc_client_t *c = &g_clients[i];
        if (c->fd >= 0) continue;

        if (!c->obj) c->obj = (wlc_obj_t *)malloc(sizeof(wlc_obj_t) * WLC_MAX_ID);
        if (!c->rx)  c->rx  = (uint8_t *)malloc(WLC_RX_CAP);
        if (!c->obj || !c->rx) {
            wlc_log("[wl] out of memory accepting client\n");
            (void)syscall1(SYS_UNIX_CLOSE, (uint64_t)fd);
            return false;
        }
        memset(c->obj, 0, sizeof(wlc_obj_t) * WLC_MAX_ID);
        c->fd = fd;
        c->rxlen = 0;
        c->fd_head = c->fd_tail = 0;
        c->seat_version = 0;
        c->pointer = c->keyboard = c->output = 0;
        c->keymap_sent = false;
        c->broken = false;
        c->obj[1].type = O_DISPLAY;
        c->obj[1].s_window = -1;
        wlc_log("[wl] client connected\n");
        return true;
    }
    wlc_log("[wl] too many clients, connection refused\n");
    (void)syscall1(SYS_UNIX_CLOSE, (uint64_t)fd);
    return false;
}

void wlc_client_drop(wlc_client_t *c)
{
    if (!c || c->fd < 0) return;

    for (uint32_t i = 0; i < WLC_MAX_WINDOWS; i++) {
        if (g_windows[i].used && g_windows[i].client == c) {
            wlc_window_free((int32_t)i);
        }
    }
    pool_drop_client(c);
    if (c->obj) memset(c->obj, 0, sizeof(wlc_obj_t) * WLC_MAX_ID);
    (void)syscall1(SYS_UNIX_CLOSE, (uint64_t)c->fd);
    c->fd = -1;
    c->rxlen = 0;
    c->fd_head = c->fd_tail = 0;
    c->pointer = c->keyboard = c->output = 0;
    if (g_pointer_focus >= 0 && !g_windows[g_pointer_focus].used) g_pointer_focus = -1;
    if (g_keyboard_focus >= 0 && !g_windows[g_keyboard_focus].used) g_keyboard_focus = -1;
    wlc_damage_all();
    wlc_log("[wl] client disconnected\n");
}

/* ------------------------------------------------------------------ */
/* registry                                                             */
/* ------------------------------------------------------------------ */

struct global { uint32_t name; const char *iface; uint32_t version; };

/* wl_seat is advertised at 5 deliberately. From version 5 a client batches
 * pointer events until wl_pointer.frame closes the group, which this
 * compositor sends; going higher would promise the high-resolution
 * axis_value120 scrolling of version 8, which it does not do. */
static const struct global G[] = {
    { 1, "wl_compositor",           4 },
    { 2, "wl_shm",                  1 },
    { 3, "wl_subcompositor",        1 },
    { 4, "xdg_wm_base",             3 },
    { 5, "wl_seat",                 5 },
    { 6, "wl_output",               3 },
    { 7, "wl_data_device_manager",  3 },
};
#define NGLOBAL (sizeof(G) / sizeof(G[0]))

static void send_shm_formats(wlc_client_t *c, uint32_t shm)
{
    wlc_msg_t m;
    m_begin(&m, shm, 0u); m_u32(&m, 0u); m_send(&m, c);   /* ARGB8888 */
    m_begin(&m, shm, 0u); m_u32(&m, 1u); m_send(&m, c);   /* XRGB8888 */
}

static void send_output_info(wlc_client_t *c, uint32_t out)
{
    wlc_msg_t m;

    m_begin(&m, out, 0u);            /* geometry */
    m_i32(&m, 0); m_i32(&m, 0);      /* x, y */
    m_i32(&m, (int32_t)(g_out.w / 4u));  /* physical mm, ~96 dpi */
    m_i32(&m, (int32_t)(g_out.h / 4u));
    m_i32(&m, 0);                    /* subpixel unknown */
    m_str(&m, "ImplusOS");
    m_str(&m, "Wayland");
    m_i32(&m, 0);                    /* transform normal */
    m_send(&m, c);

    m_begin(&m, out, 1u);            /* mode */
    m_u32(&m, 0x3u);                 /* current | preferred */
    m_i32(&m, (int32_t)g_out.w);
    m_i32(&m, (int32_t)g_out.h);
    m_i32(&m, 60000);
    m_send(&m, c);

    m_begin(&m, out, 3u); m_i32(&m, 1); m_send(&m, c);    /* scale */
    m_begin(&m, out, 2u); m_send(&m, c);                  /* done */
}

/*
 * wl_keyboard.keymap. The event carries an fd, so there is no way around
 * passing one: a client that gets none fails to demarshal and drops the
 * connection. Native processes have no memfd_create(), so the text goes into
 * a shared-memory object which SYSCALL_MEMFD_FROM_SHM wraps in a memfd for
 * SCM_RIGHTS.
 *
 * The map itself only names includes; libxkbcommon on the client resolves
 * them against /usr/share/X11/xkb, which Vendor/LinuxRuntime stages into the
 * image. That keeps a full US layout out of this binary.
 */
static const char k_keymap[] =
    "xkb_keymap {\n"
    "  xkb_keycodes { include \"evdev+aliases(qwerty)\" };\n"
    "  xkb_types { include \"complete\" };\n"
    "  xkb_compatibility { include \"complete\" };\n"
    "  xkb_symbols { include \"pc+us+inet(evdev)\" };\n"
    "};\n";

static int32_t g_keymap_shm = -1;

static void send_keymap(wlc_client_t *c, uint32_t kbd)
{
    uint32_t size = (uint32_t)sizeof(k_keymap);   /* includes the NUL */

    if (g_keymap_shm < 0) {
        int32_t h = os_shared_memory_create(size);
        if (h < 0) { wlc_log("[wl] keymap: shm create failed\n"); return; }
        void *p = os_shared_memory_map(h);
        if (!p) { wlc_log("[wl] keymap: shm map failed\n"); return; }
        memcpy(p, k_keymap, size);
        g_keymap_shm = h;
    }

    int32_t fd = os_memfd_from_shm(g_keymap_shm);
    if (fd < 0) { wlc_log("[wl] keymap: memfd wrap failed\n"); return; }

    wlc_msg_t m;
    m_begin(&m, kbd, 0u);            /* keymap(format, fd, size) */
    m_u32(&m, 1u);                   /* XKB_V1 */
    m_u32(&m, size);
    m_send_fd(&m, c, fd);
    /* sendmsg took its own reference for the receiver, so this end of the
     * fd is finished with. */
    (void)file_close(fd);
    c->keymap_sent = true;

    /* repeat_info is version 4+; the seat is advertised at 5. */
    m_begin(&m, kbd, 5u);
    m_i32(&m, 25);                   /* keys per second */
    m_i32(&m, 400);                  /* delay ms */
    m_send(&m, c);
}

/* ------------------------------------------------------------------ */
/* seat: focus + event delivery                                         */
/* ------------------------------------------------------------------ */

static void send_pointer_frame(wlc_client_t *c)
{
    if (!c || !c->pointer || c->seat_version < 5u) return;
    wlc_msg_t m;
    m_begin(&m, c->pointer, 5u);     /* wl_pointer.frame */
    m_send(&m, c);
}

static void pointer_leave(int32_t index)
{
    if (index < 0 || !g_windows[index].used) return;
    wlc_client_t *c = g_windows[index].client;
    if (!c || !c->pointer) return;
    wlc_msg_t m;
    m_begin(&m, c->pointer, 1u);     /* leave(serial, surface) */
    m_u32(&m, next_serial());
    m_u32(&m, g_windows[index].surface);
    m_send(&m, c);
    send_pointer_frame(c);
}

static void pointer_enter(int32_t index, int32_t lx, int32_t ly)
{
    if (index < 0 || !g_windows[index].used) return;
    wlc_client_t *c = g_windows[index].client;
    if (!c || !c->pointer) return;
    wlc_msg_t m;
    m_begin(&m, c->pointer, 0u);     /* enter(serial, surface, sx, sy) */
    m_u32(&m, next_serial());
    m_u32(&m, g_windows[index].surface);
    m_fixed(&m, lx);
    m_fixed(&m, ly);
    m_send(&m, c);
    send_pointer_frame(c);
}

static void keyboard_leave(int32_t index)
{
    if (index < 0 || !g_windows[index].used) return;
    wlc_client_t *c = g_windows[index].client;
    if (!c || !c->keyboard) return;
    wlc_msg_t m;
    m_begin(&m, c->keyboard, 2u);    /* leave(serial, surface) */
    m_u32(&m, next_serial());
    m_u32(&m, g_windows[index].surface);
    m_send(&m, c);
}

static void keyboard_enter(int32_t index)
{
    if (index < 0 || !g_windows[index].used) return;
    wlc_client_t *c = g_windows[index].client;
    if (!c || !c->keyboard) return;
    wlc_msg_t m;
    m_begin(&m, c->keyboard, 1u);    /* enter(serial, surface, keys[]) */
    m_u32(&m, next_serial());
    m_u32(&m, g_windows[index].surface);
    m_array_empty(&m);               /* nothing held down */
    m_send(&m, c);
}

void wlc_seat_refresh_focus(void)
{
    int32_t want = wlc_window_focus_candidate();
    if (want == g_keyboard_focus) return;
    keyboard_leave(g_keyboard_focus);
    g_keyboard_focus = want;
    keyboard_enter(g_keyboard_focus);
}

void wlc_seat_pointer_leave_all(void)
{
    pointer_leave(g_pointer_focus);
    g_pointer_focus = -1;
}

void wlc_seat_pointer_motion(int32_t x, int32_t y)
{
    int32_t lx = 0, ly = 0;
    int32_t index = wlc_window_at(x, y, &lx, &ly);

    if (index != g_pointer_focus) {
        pointer_leave(g_pointer_focus);
        g_pointer_focus = index;
        pointer_enter(index, lx, ly);
        return;                      /* enter already carries the position */
    }
    if (index < 0) return;

    wlc_client_t *c = g_windows[index].client;
    if (!c || !c->pointer) return;
    wlc_msg_t m;
    m_begin(&m, c->pointer, 2u);     /* motion(time, sx, sy) */
    m_u32(&m, wlc_now_ms());
    m_fixed(&m, lx);
    m_fixed(&m, ly);
    m_send(&m, c);
    send_pointer_frame(c);
}

void wlc_seat_pointer_button(uint32_t button, bool pressed)
{
    if (g_pointer_focus < 0 || !g_windows[g_pointer_focus].used) return;

    /* A press on a window makes it the keyboard focus, the way any
     * click-to-focus policy would. */
    if (pressed && !g_windows[g_pointer_focus].popup) {
        wlc_window_raise(g_pointer_focus);
        wlc_seat_refresh_focus();
    }

    wlc_client_t *c = g_windows[g_pointer_focus].client;
    if (!c || !c->pointer) return;
    wlc_msg_t m;
    m_begin(&m, c->pointer, 3u);     /* button(serial, time, button, state) */
    m_u32(&m, next_serial());
    m_u32(&m, wlc_now_ms());
    m_u32(&m, button);
    m_u32(&m, pressed ? 1u : 0u);
    m_send(&m, c);
    send_pointer_frame(c);
}

void wlc_seat_pointer_axis(int32_t steps)
{
    if (steps == 0 || g_pointer_focus < 0) return;
    if (!g_windows[g_pointer_focus].used) return;
    wlc_client_t *c = g_windows[g_pointer_focus].client;
    if (!c || !c->pointer) return;
    wlc_msg_t m;
    m_begin(&m, c->pointer, 4u);     /* axis(time, axis, value) */
    m_u32(&m, wlc_now_ms());
    m_u32(&m, 0u);                   /* vertical scroll */
    m_fixed(&m, steps * 10);
    m_send(&m, c);
    send_pointer_frame(c);
}

/* xkb modifier indices for the evdev/us keymap sent above. */
#define XKB_MOD_SHIFT   (1u << 0)
#define XKB_MOD_LOCK    (1u << 1)
#define XKB_MOD_CONTROL (1u << 2)
#define XKB_MOD_MOD1    (1u << 3)

void wlc_seat_key(uint16_t evdev_code, bool pressed, uint32_t modifiers)
{
    if (g_keyboard_focus < 0 || !g_windows[g_keyboard_focus].used) return;
    wlc_client_t *c = g_windows[g_keyboard_focus].client;
    if (!c || !c->keyboard) return;

    uint32_t depressed = 0u;
    if (modifiers & INPUT_KBD_MOD_SHIFT) depressed |= XKB_MOD_SHIFT;
    if (modifiers & INPUT_KBD_MOD_CTRL)  depressed |= XKB_MOD_CONTROL;
    if (modifiers & INPUT_KBD_MOD_ALT)   depressed |= XKB_MOD_MOD1;
    uint32_t locked = (modifiers & INPUT_KBD_MOD_CAPS) ? XKB_MOD_LOCK : 0u;

    wlc_msg_t m;
    m_begin(&m, c->keyboard, 4u);    /* modifiers(serial, dep, lat, lock, grp) */
    m_u32(&m, next_serial());
    m_u32(&m, depressed);
    m_u32(&m, 0u);
    m_u32(&m, locked);
    m_u32(&m, 0u);
    m_send(&m, c);

    m_begin(&m, c->keyboard, 3u);    /* key(serial, time, key, state) */
    m_u32(&m, next_serial());
    m_u32(&m, wlc_now_ms());
    m_u32(&m, evdev_code);
    m_u32(&m, pressed ? 1u : 0u);
    m_send(&m, c);
}

/* ------------------------------------------------------------------ */
/* surface presentation                                                 */
/* ------------------------------------------------------------------ */

/* Copy a committed wl_shm buffer into the window's own storage. Owning the
 * pixels means the buffer can be released immediately and the scene stays
 * recompositable even once the client has stopped drawing (or exited). */
static bool copy_buffer_to_window(wlc_client_t *c, wlc_window_t *win,
                                  uint32_t bufid)
{
    wlc_obj_t *b = obj(c, bufid);
    if (!b || b->type != O_BUFFER || b->b_w == 0u || b->b_h == 0u) return false;
    wlc_pool_t *pool = pool_get(b->b_pool);
    if (!pool || !pool->base) return false;
    if ((uint64_t)b->b_off + (uint64_t)b->b_stride * b->b_h > pool->size) {
        wlc_log("[wl] buffer outside its pool, dropped\n");
        return false;
    }

    uint64_t needed = (uint64_t)b->b_w * (uint64_t)b->b_h;
    if (needed > 64u * 1024u * 1024u) return false;
    if (win->pix_cap < needed) {
        uint32_t *p = (uint32_t *)malloc((size_t)needed * sizeof(uint32_t));
        if (!p) { wlc_log("[wl] out of memory for a surface\n"); return false; }
        free(win->pixels);
        win->pixels = p;
        win->pix_cap = (uint32_t)needed;
    }

    const uint8_t *src = pool->base + b->b_off;
    bool opaque = (b->b_fmt == 1u);
    for (uint32_t y = 0; y < b->b_h; y++) {
        const uint32_t *s = (const uint32_t *)(src + (size_t)y * b->b_stride);
        uint32_t *d = win->pixels + (size_t)y * b->b_w;
        if (opaque) {
            for (uint32_t x = 0; x < b->b_w; x++) d[x] = s[x] | 0xff000000u;
        } else {
            memcpy(d, s, (size_t)b->b_w * sizeof(uint32_t));
        }
    }
    win->w = b->b_w;
    win->h = b->b_h;
    return true;
}

/* ------------------------------------------------------------------ */
/* request dispatch                                                     */
/* ------------------------------------------------------------------ */

static void req_display(wlc_client_t *c, uint16_t op, const uint8_t *a, uint32_t n)
{
    if (op == 0u) {                          /* sync(callback) */
        if (n < 4u) return;
        uint32_t cb = r_u32(a);
        wlc_msg_t m;
        m_begin(&m, cb, 0u);                 /* wl_callback.done */
        m_u32(&m, next_serial());
        m_send(&m, c);
        wl_delete_id(c, cb);
    } else if (op == 1u) {                   /* get_registry(registry) */
        if (n < 4u) return;
        uint32_t reg = r_u32(a);
        obj_set(c, reg, O_REGISTRY);
        for (uint32_t i = 0; i < NGLOBAL; i++) {
            wlc_msg_t m;
            m_begin(&m, reg, 0u);            /* wl_registry.global */
            m_u32(&m, G[i].name);
            m_str(&m, G[i].iface);
            m_u32(&m, G[i].version);
            m_send(&m, c);
        }
        wlc_log("[wl] registry sent\n");
    }
}

static void req_registry(wlc_client_t *c, uint16_t op, const uint8_t *a, uint32_t n)
{
    if (op != 0u || n < 16u) return;         /* bind(name, iface, ver, new_id) */
    uint32_t ilen = r_u32(a + 4);
    uint32_t ipad = (ilen + 3u) & ~3u;
    if (8u + ipad + 8u > n || ilen == 0u) return;
    const char *iface = (const char *)(a + 8);
    if (iface[ilen - 1u] != '\0') return;
    uint32_t version = r_u32(a + 8 + ipad);
    uint32_t newid = r_u32(a + 8 + ipad + 4);

    if (!strcmp(iface, "wl_compositor")) {
        obj_set(c, newid, O_COMPOSITOR);
    } else if (!strcmp(iface, "wl_shm")) {
        obj_set(c, newid, O_SHM);
        send_shm_formats(c, newid);
    } else if (!strcmp(iface, "wl_subcompositor")) {
        obj_set(c, newid, O_SUBCOMPOSITOR);
    } else if (!strcmp(iface, "xdg_wm_base")) {
        obj_set(c, newid, O_XDG_WM_BASE);
    } else if (!strcmp(iface, "wl_seat")) {
        obj_set(c, newid, O_SEAT);
        c->seat_version = version;
        wlc_msg_t m;
        m_begin(&m, newid, 0u);              /* capabilities */
        m_u32(&m, 1u | 2u);                  /* pointer | keyboard */
        m_send(&m, c);
        m_begin(&m, newid, 1u);              /* name */
        m_str(&m, "seat0");
        m_send(&m, c);
    } else if (!strcmp(iface, "wl_output")) {
        obj_set(c, newid, O_OUTPUT);
        c->output = newid;
        send_output_info(c, newid);
    } else if (!strcmp(iface, "wl_data_device_manager")) {
        obj_set(c, newid, O_DDM);
    } else {
        wlc_log("[wl] bind of an unadvertised interface ignored\n");
        return;
    }
    wlc_log("[wl] bound ");
    wlc_log(iface);
    wlc_log("\n");
}

static void req_compositor(wlc_client_t *c, uint16_t op, const uint8_t *a, uint32_t n)
{
    if (n < 4u) return;
    uint32_t nid = r_u32(a);
    if (op == 0u)      obj_set(c, nid, O_SURFACE);
    else if (op == 1u) obj_set(c, nid, O_REGION);
}

static void req_shm(wlc_client_t *c, uint16_t op, const uint8_t *a, uint32_t n)
{
    /* create_pool(new_id, fd, size): an fd argument occupies no bytes on
     * the wire, so the payload is just the two u32s. */
    if (op != 0u || n < 8u) return;
    uint32_t nid = r_u32(a);
    uint32_t size = r_u32(a + 4);            /* fd is out-of-band */

    int32_t fd = -1;
    if (c->fd_tail != c->fd_head) {
        fd = c->rx_fds[c->fd_tail];
        c->fd_tail = (c->fd_tail + 1u) % WLC_MAX_RX_FDS;
    }

    wlc_obj_t *o = obj_set(c, nid, O_SHM_POOL);
    if (!o) return;
    if (fd < 0) { wlc_log("[wl] create_pool without an fd\n"); return; }

    int32_t h = os_memfd_shm_handle(fd);
    uint8_t *base = (h >= 0) ? (uint8_t *)os_shared_memory_map(h) : NULL;

    /* The mapping now holds its own reference, so the memfd has done its
     * job. Closing it keeps the fd table (and the kernel's 256 shared
     * objects) from filling up over a long session. */
    (void)file_close(fd);

    if (h < 0)   { wlc_log("[wl] create_pool: fd is not shm-backed\n"); return; }
    if (!base)   { wlc_log("[wl] create_pool: shm map failed\n"); return; }

    o->pool_index = pool_alloc(c, h, base, size);
    if (o->pool_index < 0) {
        wlc_log("[wl] too many wl_shm pools\n");
        (void)os_shared_memory_unmap(h, base);
    }
}

static void req_shm_pool(wlc_client_t *c, uint32_t id, uint16_t op,
                         const uint8_t *a, uint32_t n)
{
    if (op == 0u) {                          /* create_buffer */
        if (n < 24u) return;
        wlc_obj_t *pool = obj(c, id);
        uint32_t nid = r_u32(a);
        wlc_obj_t *b = obj_set(c, nid, O_BUFFER);
        if (!b || !pool) return;
        b->b_pool   = pool->pool_index;
        b->b_off    = r_u32(a + 4);
        b->b_w      = r_u32(a + 8);
        b->b_h      = r_u32(a + 12);
        b->b_stride = r_u32(a + 16);
        b->b_fmt    = r_u32(a + 20);
        pool_ref(b->b_pool);
    } else if (op == 1u) {                   /* destroy */
        wlc_obj_t *o = obj(c, id);
        if (o) pool_unref(o->pool_index);
        obj_destroy(c, id);
    } else if (op == 2u && n >= 4u) {        /* resize(size) */
        /* A wl_shm pool grows with the surface: the client extends the
         * memfd and then says so here. The kernel reserves the object in
         * whole powers of two, so the existing mapping already covers the
         * new size -- but only up to that reservation, which is what bounds
         * the request. Buffers are still range-checked against this size at
         * commit, so a client cannot talk its way past the mapping. */
        wlc_obj_t *o = obj(c, id);
        wlc_pool_t *pool = o ? pool_get(o->pool_index) : NULL;
        uint32_t want = r_u32(a);
        if (pool != NULL && want > pool->size) {
            uint32_t capacity = os_shared_memory_size(pool->handle);
            if (want <= capacity) {
                pool->size = want;
            } else {
                wlc_logf_u32("[wl] pool resize past its reservation: ", want);
            }
        }
    }
}

static void req_surface(wlc_client_t *c, uint32_t id, uint16_t op,
                        const uint8_t *a, uint32_t n)
{
    wlc_obj_t *s = obj(c, id);
    if (!s) return;

    switch (op) {
    case 0:                                  /* destroy */
        if (s->s_window >= 0) wlc_window_free(s->s_window);
        obj_destroy(c, id);
        break;

    case 1:                                  /* attach(buffer, x, y) */
        if (n >= 4u) s->s_pending_buf = r_u32(a);
        break;

    case 3: {                                /* frame(callback) */
        if (n < 4u) return;
        uint32_t cb = r_u32(a);
        /* Only one pending frame callback per surface; if the client asks
         * again before a commit, answer the older one straight away so no
         * id is left dangling. */
        if (s->s_frame_cb) {
            wlc_msg_t m;
            m_begin(&m, s->s_frame_cb, 0u);
            m_u32(&m, wlc_now_ms());
            m_send(&m, c);
            wl_delete_id(c, s->s_frame_cb);
        }
        s->s_frame_cb = cb;
        break;
    }

    case 6: {                                /* commit */
        int32_t index = s->s_window;
        if (s->s_pending_buf) {
            s->s_current_buf = s->s_pending_buf;
            s->s_pending_buf = 0u;

            if (index >= 0 && g_windows[index].used &&
                copy_buffer_to_window(c, &g_windows[index], s->s_current_buf)) {
                if (!g_windows[index].mapped) {
                    g_windows[index].mapped = true;
                    wlc_window_place(index);
                    wlc_window_raise(index);
                    wlc_seat_refresh_focus();
                    /* Tell the surface which output it landed on: GDK wants
                     * this before it settles on a scale factor. */
                    if (c->output) {
                        wlc_msg_t em;
                        m_begin(&em, id, 0u);        /* wl_surface.enter */
                        m_u32(&em, c->output);
                        m_send(&em, c);
                    }
                }
                g_dirty = true;
            }

            wlc_msg_t m;
            m_begin(&m, s->s_current_buf, 0u);   /* wl_buffer.release */
            m_send(&m, c);
        }
        /* A surface with a window has its frame callback answered once the
         * scene has actually been recomposited (wlc_frames_done), which is
         * what paces a client to the output instead of letting it redraw
         * flat out. A surface with no window -- a cursor, a subsurface --
         * would never be composited, so answer those straight away rather
         * than stall the client. */
        if (s->s_frame_cb && (index < 0 || !g_windows[index].used)) {
            wlc_msg_t m;
            m_begin(&m, s->s_frame_cb, 0u);      /* wl_callback.done */
            m_u32(&m, wlc_now_ms());
            m_send(&m, c);
            wl_delete_id(c, s->s_frame_cb);
            s->s_frame_cb = 0u;
        }
        break;
    }

    default:
        /* damage / set_opaque_region / set_input_region / transform /
         * scale / damage_buffer / offset: the whole surface is recomposited
         * every frame, so none of them change anything here. */
        break;
    }
}

static void req_xdg_wm_base(wlc_client_t *c, uint32_t id, uint16_t op,
                            const uint8_t *a, uint32_t n)
{
    switch (op) {
    case 0:                                  /* destroy */
        obj_destroy(c, id);
        break;
    case 1:                                  /* create_positioner */
        if (n >= 4u) obj_set(c, r_u32(a), O_POSITIONER);
        break;
    case 2: {                                /* get_xdg_surface(id, surface) */
        if (n < 8u) return;
        uint32_t nid = r_u32(a), surf = r_u32(a + 4);
        wlc_obj_t *xs = obj_set(c, nid, O_XDG_SURFACE);
        wlc_obj_t *s = obj(c, surf);
        if (!xs || !s || s->type != O_SURFACE) return;
        xs->x_surface = surf;
        s->s_xdg = nid;
        break;
    }
    default:                                 /* pong */
        break;
    }
}

static void send_xdg_configure(wlc_client_t *c, uint32_t xdg_surface)
{
    wlc_msg_t m;
    m_begin(&m, xdg_surface, 0u);            /* xdg_surface.configure */
    m_u32(&m, next_serial());
    m_send(&m, c);
}

/* Where a popup goes, from its xdg_positioner: the anchor rectangle is in
 * the parent's coordinates, and the popup hangs off its bottom-left. Anchor
 * and gravity flags are not honoured -- menus and combo lists land close
 * enough, and nothing here reads back a constrained position. */
static void place_popup(wlc_client_t *c, int32_t index, uint32_t positioner)
{
    wlc_obj_t *p = obj(c, positioner);
    wlc_window_t *w = &g_windows[index];
    int32_t px = 0, py = 0;

    for (uint32_t i = 0; i < WLC_MAX_WINDOWS; i++) {
        if (g_windows[i].used && g_windows[i].surface == w->parent_surface &&
            g_windows[i].client == c) {
            px = g_windows[i].x;
            py = g_windows[i].y;
            break;
        }
    }

    int32_t rx = 0, ry = 0;
    uint32_t rw = 0, rh = 0;
    if (p && p->type == O_POSITIONER) {
        rx = p->p_ax + p->p_ox;
        ry = p->p_ay + (int32_t)p->p_ah + p->p_oy;
        rw = p->p_w;
        rh = p->p_h;
    }

    w->x = px + rx;
    w->y = py + ry;
    if (rw) w->w = rw;
    if (rh) w->h = rh;

    /* Keep it on the output. */
    if (w->w && w->x + (int32_t)w->w > (int32_t)g_out.w)
        w->x = (int32_t)g_out.w - (int32_t)w->w;
    if (w->h && w->y + (int32_t)w->h > (int32_t)g_out.h)
        w->y = (int32_t)g_out.h - (int32_t)w->h;
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;

    wlc_msg_t m;
    m_begin(&m, w->role, 0u);                /* xdg_popup.configure */
    m_i32(&m, rx);
    m_i32(&m, ry);
    m_i32(&m, (int32_t)(rw ? rw : 1u));
    m_i32(&m, (int32_t)(rh ? rh : 1u));
    m_send(&m, c);
}

static void req_xdg_surface(wlc_client_t *c, uint32_t id, uint16_t op,
                            const uint8_t *a, uint32_t n)
{
    wlc_obj_t *xs = obj(c, id);
    if (!xs) return;

    switch (op) {
    case 0:                                  /* destroy */
        obj_destroy(c, id);
        break;

    case 1: {                                /* get_toplevel(id) */
        if (n < 4u) return;
        uint32_t nid = r_u32(a);
        wlc_obj_t *t = obj_set(c, nid, O_XDG_TOPLEVEL);
        if (!t) return;
        t->r_xdg = id;
        xs->x_role = nid;

        int32_t index = wlc_window_alloc(c, xs->x_surface, id);
        if (index < 0) { wlc_log("[wl] no free window slot\n"); return; }
        g_windows[index].role = nid;
        g_windows[index].popup = false;
        wlc_obj_t *s = obj(c, xs->x_surface);
        if (s) s->s_window = index;

        /* A zero size lets the client pick, which is what GTK wants for a
         * first map; the committed buffer then defines the window size. */
        wlc_msg_t m;
        m_begin(&m, nid, 0u);                /* xdg_toplevel.configure */
        m_i32(&m, 0);
        m_i32(&m, 0);
        m_array_empty(&m);                   /* no states: not maximized */
        m_send(&m, c);
        send_xdg_configure(c, id);
        wlc_log("[wl] toplevel configured\n");
        break;
    }

    case 2: {                                /* get_popup(id, parent, pos) */
        if (n < 12u) return;
        uint32_t nid = r_u32(a);
        uint32_t parent_xdg = r_u32(a + 4);
        uint32_t positioner = r_u32(a + 8);
        wlc_obj_t *pobj = obj_set(c, nid, O_XDG_POPUP);
        if (!pobj) return;
        pobj->r_xdg = id;
        xs->x_role = nid;

        int32_t index = wlc_window_alloc(c, xs->x_surface, id);
        if (index < 0) { wlc_log("[wl] no free window slot\n"); return; }
        g_windows[index].role = nid;
        g_windows[index].popup = true;
        wlc_obj_t *parent = obj(c, parent_xdg);
        g_windows[index].parent_surface =
            (parent && parent->type == O_XDG_SURFACE) ? parent->x_surface : 0u;
        wlc_obj_t *s = obj(c, xs->x_surface);
        if (s) s->s_window = index;

        place_popup(c, index, positioner);
        send_xdg_configure(c, id);
        break;
    }

    default:                                 /* set_window_geometry, ack */
        break;
    }
}

static void req_xdg_toplevel(wlc_client_t *c, uint32_t id, uint16_t op,
                             const uint8_t *a, uint32_t n)
{
    (void)a; (void)n;
    wlc_obj_t *t = obj(c, id);
    if (!t) return;

    if (op == 0u) {                          /* destroy */
        wlc_obj_t *xs = obj(c, t->r_xdg);
        if (xs) {
            wlc_obj_t *s = obj(c, xs->x_surface);
            if (s && s->s_window >= 0) {
                wlc_window_free(s->s_window);
                s->s_window = -1;
            }
        }
        obj_destroy(c, id);
    }
    /* set_title / set_app_id / move / resize / maximize / minimize: this
     * compositor draws no decorations and does not resize on request, so
     * they are all no-ops. GTK draws its own decorations. */
}

static void req_xdg_popup(wlc_client_t *c, uint32_t id, uint16_t op,
                          const uint8_t *a, uint32_t n)
{
    (void)a; (void)n;
    wlc_obj_t *p = obj(c, id);
    if (!p) return;

    if (op == 0u) {                          /* destroy */
        wlc_obj_t *xs = obj(c, p->r_xdg);
        if (xs) {
            wlc_obj_t *s = obj(c, xs->x_surface);
            if (s && s->s_window >= 0) {
                wlc_window_free(s->s_window);
                s->s_window = -1;
            }
        }
        obj_destroy(c, id);
    }
    /* grab: keyboard focus already follows the topmost surface, so an
     * explicit grab needs nothing extra. */
}

static void req_positioner(wlc_client_t *c, uint32_t id, uint16_t op,
                           const uint8_t *a, uint32_t n)
{
    wlc_obj_t *p = obj(c, id);
    if (!p) return;

    switch (op) {
    case 0:                                          /* destroy */
        obj_destroy(c, id);
        break;
    case 1:                                          /* set_size(w, h) */
        if (n >= 8u) { p->p_w = r_u32(a); p->p_h = r_u32(a + 4); }
        break;
    case 2:                                          /* set_anchor_rect */
        if (n >= 16u) {
            p->p_ax = (int32_t)r_u32(a);
            p->p_ay = (int32_t)r_u32(a + 4);
            p->p_aw = r_u32(a + 8);
            p->p_ah = r_u32(a + 12);
        }
        break;
    case 3:                                          /* set_anchor */
        if (n >= 4u) p->p_anchor = r_u32(a);
        break;
    case 4:                                          /* set_gravity */
        if (n >= 4u) p->p_gravity = r_u32(a);
        break;
    case 6:                                          /* set_offset(x, y) */
        if (n >= 8u) {
            p->p_ox = (int32_t)r_u32(a);
            p->p_oy = (int32_t)r_u32(a + 4);
        }
        break;
    default:                                         /* constraint, reactive */
        break;
    }
}

static void req_seat(wlc_client_t *c, uint32_t id, uint16_t op,
                     const uint8_t *a, uint32_t n)
{
    if (op == 3u) { obj_destroy(c, id); return; }   /* release */
    if (n < 4u) return;
    uint32_t nid = r_u32(a);
    if (op == 0u) {                          /* get_pointer */
        obj_set(c, nid, O_POINTER);
        c->pointer = nid;
    } else if (op == 1u) {                   /* get_keyboard */
        obj_set(c, nid, O_KEYBOARD);
        c->keyboard = nid;
        send_keymap(c, nid);
        /* If this client already owns the focused surface, tell it now --
         * the enter that went out before get_keyboard was lost. */
        if (g_keyboard_focus >= 0 && g_windows[g_keyboard_focus].client == c) {
            keyboard_enter(g_keyboard_focus);
        }
    } else if (op == 2u) {                   /* get_touch */
        obj_set(c, nid, O_NONE);
    }
}

static void req_pointer(wlc_client_t *c, uint32_t id, uint16_t op,
                        const uint8_t *a, uint32_t n)
{
    (void)a; (void)n;
    if (op == 1u) {                          /* release */
        if (c->pointer == id) c->pointer = 0u;
        obj_destroy(c, id);
    }
    /* set_cursor: this compositor draws its own pointer, so a client cursor
     * surface is ignored rather than composited. */
}

static void req_keyboard(wlc_client_t *c, uint32_t id, uint16_t op,
                         const uint8_t *a, uint32_t n)
{
    (void)a; (void)n;
    if (op == 0u) {                          /* release */
        if (c->keyboard == id) c->keyboard = 0u;
        obj_destroy(c, id);
    }
}

static void req_subcompositor(wlc_client_t *c, uint32_t id, uint16_t op,
                              const uint8_t *a, uint32_t n)
{
    if (op == 0u) { obj_destroy(c, id); return; }
    if (op == 1u && n >= 4u) obj_set(c, r_u32(a), O_SUBSURFACE);
}

static void req_ddm(wlc_client_t *c, uint32_t id, uint16_t op,
                    const uint8_t *a, uint32_t n)
{
    (void)id;
    if (n < 4u) return;
    uint32_t nid = r_u32(a);
    if (op == 0u)      obj_set(c, nid, O_DATA_SOURCE);
    else if (op == 1u) obj_set(c, nid, O_DATA_DEVICE);
}

/* Every request, one line each, for bring-up. Off by default: the volume is
 * enormous once a client is drawing, and COM1 is written a byte at a time
 * from inside the syscall path. Build with -DWLC_PROTOCOL_TRACE to turn on.
 *
 * Even switched on it stops after WLC_TRACE_MAX lines. In QEMU the serial
 * port is the only channel out and it costs a syscall per byte, so an
 * uncapped trace changes the timing of the very thing it is measuring --
 * and startup, which is what a trace is wanted for, is over long before
 * the cap. */
#ifdef WLC_PROTOCOL_TRACE
#ifndef WLC_TRACE_MAX
#define WLC_TRACE_MAX 600u
#endif
static uint32_t g_trace_left = WLC_TRACE_MAX;
static void trace_request(uint32_t id, uint16_t op, uint8_t type, uint32_t alen)
{
    if (g_trace_left == 0u) return;
    if (--g_trace_left == 0u) { wlc_log("[wl] trace cap reached\n"); return; }
    wlc_log_u32("[wl] req id=", id);
    wlc_log_u32(" op=", op);
    wlc_log_u32(" type=", type);
    wlc_logf_u32(" argbytes=", alen);
}
#else
#define trace_request(id, op, type, alen) ((void)0)
#endif

static void dispatch(wlc_client_t *c, uint32_t id, uint16_t op,
                     const uint8_t *args, uint32_t alen)
{
    if (id == 1u) {
        trace_request(id, op, O_DISPLAY, alen);
        req_display(c, op, args, alen);
        return;
    }

    wlc_obj_t *o = obj(c, id);
    if (!o || o->type == O_NONE) {
        trace_request(id, op, O_NONE, alen);
        return;
    }
    trace_request(id, op, o->type, alen);

    switch (o->type) {
    case O_REGISTRY:      req_registry(c, op, args, alen); break;
    case O_COMPOSITOR:    req_compositor(c, op, args, alen); break;
    case O_SHM:           req_shm(c, op, args, alen); break;
    case O_SHM_POOL:      req_shm_pool(c, id, op, args, alen); break;
    case O_BUFFER:
        if (op == 0u) { pool_unref(o->b_pool); obj_destroy(c, id); }
        break;
    case O_REGION:        if (op == 0u) obj_destroy(c, id); break;
    case O_SURFACE:       req_surface(c, id, op, args, alen); break;
    case O_XDG_WM_BASE:   req_xdg_wm_base(c, id, op, args, alen); break;
    case O_XDG_SURFACE:   req_xdg_surface(c, id, op, args, alen); break;
    case O_XDG_TOPLEVEL:  req_xdg_toplevel(c, id, op, args, alen); break;
    case O_XDG_POPUP:     req_xdg_popup(c, id, op, args, alen); break;
    case O_POSITIONER:    req_positioner(c, id, op, args, alen); break;
    case O_SEAT:          req_seat(c, id, op, args, alen); break;
    case O_POINTER:       req_pointer(c, id, op, args, alen); break;
    case O_KEYBOARD:      req_keyboard(c, id, op, args, alen); break;
    case O_OUTPUT:        if (op == 0u) { c->output = 0u; obj_destroy(c, id); } break;
    case O_SUBCOMPOSITOR: req_subcompositor(c, id, op, args, alen); break;
    case O_SUBSURFACE:    if (op == 0u) obj_destroy(c, id); break;
    case O_DDM:           req_ddm(c, id, op, args, alen); break;
    case O_DATA_DEVICE:   if (op == 2u) obj_destroy(c, id); break;
    case O_DATA_SOURCE:   if (op == 1u) obj_destroy(c, id); break;
    default: break;
    }
}

/* Answer every frame callback held back at commit. Called once per output
 * frame, so clients redraw in step with the compositor. Returns true if
 * anything was released. */
bool wlc_frames_done(void)
{
    bool any = false;
    for (uint32_t i = 0; i < WLC_MAX_WINDOWS; i++) {
        wlc_window_t *w = &g_windows[i];
        if (!w->used || !w->client) continue;
        wlc_obj_t *s = obj(w->client, w->surface);
        if (!s || s->type != O_SURFACE || !s->s_frame_cb) continue;
        wlc_msg_t m;
        m_begin(&m, s->s_frame_cb, 0u);          /* wl_callback.done */
        m_u32(&m, wlc_now_ms());
        m_send(&m, w->client);
        wl_delete_id(w->client, s->s_frame_cb);
        s->s_frame_cb = 0u;
        any = true;
    }
    return any;
}

/* ------------------------------------------------------------------ */
/* socket pump                                                          */
/* ------------------------------------------------------------------ */

/* One recvmsg: append bytes, queue any SCM_RIGHTS fds. Returns the byte
 * count, 0 on EOF, or a negative errno (-11 = nothing to read). */
static int32_t pump_read(wlc_client_t *c)
{
    if (c->rxlen >= WLC_RX_CAP) return -11;

    struct iovec_k iov = { (uint64_t)(uintptr_t)(c->rx + c->rxlen),
                           WLC_RX_CAP - c->rxlen };
    uint8_t cbuf[64];
    struct msghdr_k msg;
    memset(&msg, 0, sizeof(msg));
    msg.iov = (uint64_t)(uintptr_t)&iov;
    msg.iovlen = 1;
    msg.control = (uint64_t)(uintptr_t)cbuf;
    msg.controllen = sizeof(cbuf);

    int64_t r = (int64_t)syscall2(SYS_UNIX_RECVMSG, (uint64_t)c->fd,
                                  (uint64_t)(uintptr_t)&msg);
    if (r <= 0) return (int32_t)r;
    c->rxlen += (uint32_t)r;

    if (msg.controllen >= sizeof(struct cmsghdr_k)) {
        struct cmsghdr_k *cm = (struct cmsghdr_k *)cbuf;
        if (cm->level == 1 && cm->type == 1 &&
            cm->len >= (uint32_t)sizeof(*cm)) {
            uint32_t nf = (cm->len - (uint32_t)sizeof(*cm)) / 4u;
            int32_t *fds = (int32_t *)(cbuf + sizeof(*cm));
            for (uint32_t i = 0; i < nf; i++) {
                uint32_t next = (c->fd_head + 1u) % WLC_MAX_RX_FDS;
                if (next != c->fd_tail) {
                    c->rx_fds[c->fd_head] = fds[i];
                    c->fd_head = next;
                }
            }
        }
    }
    return (int32_t)r;
}

static void process_rx(wlc_client_t *c)
{
    uint32_t off = 0;
    while (c->rxlen - off >= 8u) {
        const uint8_t *m = c->rx + off;
        uint32_t id = r_u32(m);
        uint32_t w1 = r_u32(m + 4);
        uint16_t op = (uint16_t)(w1 & 0xffffu);
        uint32_t size = w1 >> 16;
        if (size < 8u || size > WLC_RX_CAP) {
            wlc_log("[wl] malformed message, dropping the connection\n");
            wlc_client_drop(c);
            return;
        }
        if (c->rxlen - off < size) break;         /* partial */
        dispatch(c, id, op, m + 8, size - 8u);
        if (c->fd < 0) return;                    /* dropped mid-dispatch */
        off += size;                              /* sizes are 4-aligned */
    }
    if (off > 0u) {
        memmove(c->rx, c->rx + off, c->rxlen - off);
        c->rxlen -= off;
    }
}

/* Returns true if anything was read, so the main loop knows not to nap. */
bool wlc_client_pump(wlc_client_t *c)
{
    if (!c || c->fd < 0) return false;
    if (c->broken) { wlc_client_drop(c); return true; }

    bool any = false;
    for (uint32_t i = 0u; i < 64u; i++) {
        int32_t r = pump_read(c);
        if (r == 0) { wlc_client_drop(c); return true; }
        if (r < 0) break;                         /* EAGAIN */
        any = true;
        process_rx(c);
        if (c->fd < 0) return true;
    }
    if (c->broken) { wlc_client_drop(c); return true; }
    return any;
}
