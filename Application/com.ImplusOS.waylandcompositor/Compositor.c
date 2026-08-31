/*
 * com.ImplusOS.waylandcompositor - a minimal Wayland compositor for ImplusOS.
 *
 * It speaks the Wayland wire protocol by hand (no libwayland) over an AF_UNIX
 * socket at $XDG_RUNTIME_DIR/wayland-0 (= /tmp/wayland-0), and bridges one
 * client's committed wl_shm surface into a single ImplusOS window-manager
 * window (Window.h backing store).
 *
 * Scope (first light): wl_display / wl_registry / wl_callback, wl_compositor,
 * wl_shm (+pool+buffer), wl_surface (attach/damage/frame/commit), xdg_wm_base
 * / xdg_surface / xdg_toplevel, plus stub wl_seat (no caps), wl_output,
 * wl_subcompositor and wl_data_device_manager so GTK3's Wayland backend gets
 * far enough to map and present a window. Input is not wired yet.
 *
 * Design notes:
 *  - one client connection at a time (GTK).
 *  - object ids: client-allocated < 0xff000000, server-allocated from
 *    0xff000000 up. obj[] is a flat table indexed by id (client ids only;
 *    server ids tracked separately and small in number).
 *  - the only client->server fd is wl_shm.create_pool's; received fds are
 *    queued in g_rx_fds and consumed when that request is parsed.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "Process.h"
#include "Window.h"
#include "Memory.h"

extern uint64_t syscall1(uint64_t, uint64_t);
extern uint64_t syscall2(uint64_t, uint64_t, uint64_t);
extern uint64_t syscall3(uint64_t, uint64_t, uint64_t, uint64_t);

/* --- native syscall numbers (mirror Kernel/Core/syscall/Syscall_Main.h) --- */
#define SYS_UNIX_SOCKET   220
#define SYS_UNIX_BIND     221
#define SYS_UNIX_LISTEN   222
#define SYS_UNIX_ACCEPT   223
#define SYS_UNIX_CONNECT  224
#define SYS_UNIX_SEND     225
#define SYS_UNIX_RECV     226
#define SYS_UNIX_RECVMSG  228
#define SYS_UNIX_CLOSE    229

static int u_socket(void)            { return (int)syscall1(SYS_UNIX_SOCKET, 1); }
static int u_bind(int fd, const char *p){ return (int)syscall2(SYS_UNIX_BIND, (uint64_t)fd, (uint64_t)(uintptr_t)p); }
static int u_listen(int fd)           { return (int)syscall2(SYS_UNIX_LISTEN, (uint64_t)fd, 8); }
static int u_accept(int fd)           { return (int)syscall1(SYS_UNIX_ACCEPT, (uint64_t)fd); }
static int u_send(int fd, const void *b, uint32_t n){ return (int)syscall3(SYS_UNIX_SEND, (uint64_t)fd, (uint64_t)(uintptr_t)b, n); }
static void dbg(const char *s){ syscall1(2 /*SYSCALL_SERIAL_PUTS*/, (uint64_t)(uintptr_t)s); }

/* struct msghdr, x86-64 glibc layout (Kernel/IPC/UnixSocket.c) */
struct msghdr_k {
    uint64_t name; uint32_t namelen; uint32_t _p0;
    uint64_t iov;  uint64_t iovlen;
    uint64_t control; uint64_t controllen;
    int32_t flags; uint32_t _p1;
};
struct iovec_k { uint64_t base; uint64_t len; };
struct cmsghdr_k { uint32_t len; uint32_t _pad; int32_t level; int32_t type; };

/* ------------------------------------------------------------------ */

#define WL_SOCK_PATH "/tmp/wayland-0"
#define RX_CAP   (256 * 1024)
#define TX_CAP   (64 * 1024)
#define MAX_ID   0x4000u          /* client id table size */
#define SERVER_ID_BASE 0xff000000u

enum {
    O_NONE = 0, O_DISPLAY, O_REGISTRY, O_CALLBACK, O_COMPOSITOR, O_SHM,
    O_SHM_POOL, O_BUFFER, O_SURFACE, O_REGION, O_XDG_WM_BASE, O_XDG_SURFACE,
    O_XDG_TOPLEVEL, O_SEAT, O_POINTER, O_KEYBOARD, O_OUTPUT, O_SUBCOMPOSITOR,
    O_SUBSURFACE, O_DDM, O_DATA_DEVICE, O_DATA_SOURCE, O_POSITIONER
};

typedef struct {
    uint8_t  type;
    /* wl_shm_pool */
    uint8_t *pool_base;
    uint32_t pool_size;
    int32_t  pool_handle;
    /* wl_buffer */
    uint32_t b_pool;      /* pool object id */
    uint32_t b_off, b_w, b_h, b_stride, b_fmt;
    /* wl_surface */
    uint32_t s_pending_buf, s_current_buf, s_frame_cb, s_xdg;
    /* xdg_surface */
    uint32_t x_surface, x_toplevel, x_serial;
} obj_t;

static obj_t   g_obj[MAX_ID];
static int     g_client = -1;
static uint32_t g_next_serial = 1;

static uint8_t g_rx[RX_CAP];
static uint32_t g_rxlen = 0;
static int32_t g_rx_fds[16];
static uint32_t g_rx_fd_head = 0, g_rx_fd_tail = 0;

static uint8_t g_tx[TX_CAP];

static window_id_t g_win = 0;
static uint32_t   *g_winpx = NULL;
static uint32_t    g_win_w = 0, g_win_h = 0;

/* ---- object helpers ---- */
static obj_t *obj(uint32_t id) { return (id && id < MAX_ID) ? &g_obj[id] : NULL; }
static void obj_set(uint32_t id, uint8_t t) { if (id && id < MAX_ID) { memset(&g_obj[id], 0, sizeof(obj_t)); g_obj[id].type = t; } }

/* ---- wire writers ---- */
static void w_u32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static uint32_t r_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

/* Build+send one event: object id, opcode, then `argc` u32 args (ints/uints/
 * objects/new_ids). Strings/arrays are not needed for the events we emit
 * except wl_registry.global, which has its own path below. */
static void ev(uint32_t id, uint16_t op, const uint32_t *args, uint32_t argc)
{
    uint32_t size = 8 + argc * 4;
    uint8_t m[8 + 16 * 4];
    w_u32(m, id);
    w_u32(m + 4, ((uint32_t)size << 16) | op);
    for (uint32_t i = 0; i < argc; i++) w_u32(m + 8 + i * 4, args[i]);
    if (g_client >= 0) u_send(g_client, m, size);
}

/* wl_registry.global(name, interface, version) */
static void ev_global(uint32_t reg, uint32_t name, const char *iface, uint32_t version)
{
    uint32_t ilen = (uint32_t)strlen(iface) + 1;
    uint32_t ipad = (ilen + 3) & ~3u;
    uint32_t size = 8 + 4 + 4 + ipad + 4;
    uint8_t *m = g_tx;
    w_u32(m, reg);
    w_u32(m + 4, (size << 16) | 0 /*global*/);
    w_u32(m + 8, name);
    w_u32(m + 12, ilen);
    memset(m + 16, 0, ipad);
    memcpy(m + 16, iface, strlen(iface));
    w_u32(m + 16 + ipad, version);
    if (g_client >= 0) u_send(g_client, m, size);
}

__attribute__((unused)) static void wl_display_error(uint32_t bad, uint32_t code, const char *msg)
{
    uint32_t mlen = (uint32_t)strlen(msg) + 1, mpad = (mlen + 3) & ~3u;
    uint32_t size = 8 + 4 + 4 + 4 + mpad;
    uint8_t *m = g_tx;
    w_u32(m, 1); w_u32(m + 4, (size << 16) | 0);
    w_u32(m + 8, bad); w_u32(m + 12, code); w_u32(m + 16, mlen);
    memset(m + 20, 0, mpad); memcpy(m + 20, msg, strlen(msg));
    if (g_client >= 0) u_send(g_client, m, size);
    dbg("[wl] protocol error sent\n");
}

static void wl_delete_id(uint32_t id)
{
    uint32_t a[1] = { id };
    ev(1, 1 /*delete_id*/, a, 1);
}

/* ---- registry globals ---- */
struct global { uint32_t name; const char *iface; uint32_t version; };
static const struct global G[] = {
    { 1, "wl_compositor",           4 },
    { 2, "wl_shm",                  1 },
    { 3, "wl_subcompositor",        1 },
    { 4, "xdg_wm_base",             3 },
    { 5, "wl_seat",                 7 },
    { 6, "wl_output",               3 },
    { 7, "wl_data_device_manager",  3 },
};
#define NGLOBAL (sizeof(G) / sizeof(G[0]))

static void send_shm_formats(uint32_t shm)
{
    uint32_t a0[1] = { 0 }; ev(shm, 0 /*format*/, a0, 1);   /* ARGB8888 */
    uint32_t a1[1] = { 1 }; ev(shm, 0, a1, 1);              /* XRGB8888 */
}

static void send_output_info(uint32_t out)
{
    /* geometry(x,y,pw,ph,subpixel,make,model,transform) - make/model strings */
    const char *mk = "ImplusOS", *md = "Wayland";
    uint32_t mkl = 9, mkp = 12, mdl = 8, mdp = 8;
    uint8_t *m = g_tx;
    uint32_t size = 8 + 4*4 + 4 + (4 + mkp) + (4 + mdp) + 4;
    uint32_t o = 0;
    w_u32(m + o, out); o += 4;
    w_u32(m + o, (size << 16) | 0 /*geometry*/); o += 4;
    w_u32(m + o, 0); o += 4; w_u32(m + o, 0); o += 4;      /* x,y */
    w_u32(m + o, 300); o += 4; w_u32(m + o, 200); o += 4;  /* phys mm */
    w_u32(m + o, 0); o += 4;                                /* subpixel unknown */
    w_u32(m + o, mkl); o += 4; memset(m + o, 0, mkp); memcpy(m + o, mk, 8); o += mkp;
    w_u32(m + o, mdl); o += 4; memset(m + o, 0, mdp); memcpy(m + o, md, 7); o += mdp;
    w_u32(m + o, 0); o += 4;                                /* transform normal */
    if (g_client >= 0) u_send(g_client, m, size);

    uint32_t mode[4] = { 0x1 /*current*/, (uint32_t)(g_win_w ? g_win_w : 900),
                         (uint32_t)(g_win_h ? g_win_h : 700), 60000 };
    ev(out, 1 /*mode*/, mode, 4);
    uint32_t sc[1] = { 1 }; ev(out, 3 /*scale*/, sc, 1);
    ev(out, 2 /*done*/, NULL, 0);
}

/* ---- blit a committed wl_shm buffer into the WM window ---- */
static void present_buffer(uint32_t bufid)
{
    obj_t *b = obj(bufid);
    if (!b || b->type != O_BUFFER) return;
    obj_t *pool = obj(b->b_pool);
    if (!pool || pool->type != O_SHM_POOL || !pool->pool_base) return;
    if ((uint64_t)b->b_off + (uint64_t)b->b_stride * b->b_h > pool->pool_size) return;

    if (!g_winpx) return;

    const uint8_t *src = pool->pool_base + b->b_off;
    uint32_t rows = b->b_h < g_win_h ? b->b_h : g_win_h;
    uint32_t cols = b->b_w < g_win_w ? b->b_w : g_win_w;
    for (uint32_t y = 0; y < rows; y++) {
        const uint32_t *s = (const uint32_t *)(src + (size_t)y * b->b_stride);
        uint32_t *d = g_winpx + (size_t)y * g_win_w;
        for (uint32_t x = 0; x < cols; x++) {
            uint32_t px = s[x];
            if (b->b_fmt == 1) px |= 0xff000000u;   /* XRGB -> opaque */
            d[x] = px;
        }
    }
    window_damage(g_win, 0, 0, cols, rows);
    window_end_transaction(g_win);
}

/* ---- request dispatch ---- */
static void req_display(uint32_t id, uint16_t op, const uint8_t *a, uint32_t n)
{
    (void)id;
    if (op == 0) {                       /* sync(callback) */
        if (n < 4) return;
        uint32_t cb = r_u32(a);
        obj_set(cb, O_CALLBACK);
        uint32_t d[1] = { g_next_serial++ };
        ev(cb, 0 /*done*/, d, 1);
        wl_delete_id(cb);
    } else if (op == 1) {                /* get_registry(registry) */
        if (n < 4) return;
        uint32_t reg = r_u32(a);
        obj_set(reg, O_REGISTRY);
        for (uint32_t i = 0; i < NGLOBAL; i++)
            ev_global(reg, G[i].name, G[i].iface, G[i].version);
        dbg("[wl] registry sent\n");
    }
}

static void req_registry(uint32_t id, uint16_t op, const uint8_t *a, uint32_t n)
{
    (void)id;
    if (op != 0 || n < 12) return;      /* bind(name, iface, version, new_id) */
    uint32_t name = r_u32(a);
    uint32_t ilen = r_u32(a + 4);
    uint32_t ipad = (ilen + 3) & ~3u;
    if (8 + ipad + 8 > n) return;
    const char *iface = (const char *)(a + 8);
    uint32_t newid = r_u32(a + 8 + ipad + 4);

    if (!strcmp(iface, "wl_compositor"))            obj_set(newid, O_COMPOSITOR);
    else if (!strcmp(iface, "wl_shm"))             { obj_set(newid, O_SHM); send_shm_formats(newid); }
    else if (!strcmp(iface, "wl_subcompositor"))    obj_set(newid, O_SUBCOMPOSITOR);
    else if (!strcmp(iface, "xdg_wm_base"))         obj_set(newid, O_XDG_WM_BASE);
    else if (!strcmp(iface, "wl_seat"))            { obj_set(newid, O_SEAT);
        uint32_t c[1] = { 0 }; ev(newid, 0 /*capabilities*/, c, 1);
        const char *nm = "seat0"; uint32_t l = 6, p = 8; uint8_t *m = g_tx;
        uint32_t sz = 8 + 4 + p; w_u32(m, newid); w_u32(m + 4, (sz << 16) | 1 /*name*/);
        w_u32(m + 8, l); memset(m + 12, 0, p); memcpy(m + 12, nm, 5);
        if (g_client >= 0) u_send(g_client, m, sz); }
    else if (!strcmp(iface, "wl_output"))          { obj_set(newid, O_OUTPUT); send_output_info(newid); }
    else if (!strcmp(iface, "wl_data_device_manager")) obj_set(newid, O_DDM);
    else { dbg("[wl] bind unknown iface\n"); obj_set(newid, O_NONE); }
    (void)name;
}

static void req_compositor(uint32_t id, uint16_t op, const uint8_t *a, uint32_t n)
{
    (void)id;
    if (n < 4) return;
    uint32_t nid = r_u32(a);
    if (op == 0)      obj_set(nid, O_SURFACE);      /* create_surface */
    else if (op == 1) obj_set(nid, O_REGION);       /* create_region  */
}

static void req_shm(uint32_t id, uint16_t op, const uint8_t *a, uint32_t n)
{
    (void)id;
    if (op != 0 || n < 8) return;                   /* create_pool(id, fd, size) */
    uint32_t nid = r_u32(a);
    uint32_t size = r_u32(a + 4);
    int32_t fd = -1;
    if (g_rx_fd_tail != g_rx_fd_head) {
        fd = g_rx_fds[g_rx_fd_tail];
        g_rx_fd_tail = (g_rx_fd_tail + 1) % 16;
    }
    obj_set(nid, O_SHM_POOL);
    obj_t *o = obj(nid);
    if (fd < 0) { dbg("[wl] create_pool without fd!\n"); return; }
    int32_t h = os_memfd_shm_handle(fd);
    if (h < 0) { dbg("[wl] create_pool: fd is not shm-backed\n"); return; }
    uint8_t *base = (uint8_t *)os_shared_memory_map(h);
    if (!base) { dbg("[wl] create_pool: shm map failed\n"); return; }
    o->pool_base = base;
    o->pool_size = size;
    o->pool_handle = h;
    dbg("[wl] pool mapped\n");
}

static void req_shm_pool(uint32_t id, uint16_t op, const uint8_t *a, uint32_t n)
{
    if (op == 0) {                                  /* create_buffer */
        if (n < 24) return;
        uint32_t nid = r_u32(a);
        obj_set(nid, O_BUFFER);
        obj_t *b = obj(nid);
        b->b_pool   = id;
        b->b_off    = r_u32(a + 4);
        b->b_w      = r_u32(a + 8);
        b->b_h      = r_u32(a + 12);
        b->b_stride = r_u32(a + 16);
        b->b_fmt    = r_u32(a + 20);
    }
    /* destroy / resize: ignored (fixed pool for MVP) */
}

static void req_surface(uint32_t id, uint16_t op, const uint8_t *a, uint32_t n)
{
    obj_t *s = obj(id);
    if (!s) return;
    switch (op) {
    case 1: /* attach(buffer, x, y) */
        if (n >= 4) s->s_pending_buf = r_u32(a);
        break;
    case 3: /* frame(callback) */
        if (n >= 4) { uint32_t cb = r_u32(a); obj_set(cb, O_CALLBACK); s->s_frame_cb = cb; }
        break;
    case 6: /* commit */
        if (s->s_pending_buf) {
            s->s_current_buf = s->s_pending_buf;
            s->s_pending_buf = 0;
            present_buffer(s->s_current_buf);

            ev(s->s_current_buf, 0 /*wl_buffer.release*/, NULL, 0);
        }
        if (s->s_frame_cb) {
            uint32_t d[1] = { g_next_serial++ };
            ev(s->s_frame_cb, 0 /*done*/, d, 1);
            wl_delete_id(s->s_frame_cb);
            s->s_frame_cb = 0;
        }
        break;
    default: break; /* damage/opaque/input/scale/transform/offset: ignored */
    }
}

static void req_xdg_wm_base(uint32_t id, uint16_t op, const uint8_t *a, uint32_t n)
{
    (void)id;
    if (op == 1) {                                  /* create_positioner */
        if (n >= 4) obj_set(r_u32(a), O_POSITIONER);
    } else if (op == 2) {                           /* get_xdg_surface(id, surface) */
        if (n < 8) return;
        uint32_t nid = r_u32(a), surf = r_u32(a + 4);
        obj_set(nid, O_XDG_SURFACE);
        obj_t *xs = obj(nid);
        xs->x_surface = surf;
        obj_t *s = obj(surf);
        if (s) s->s_xdg = nid;
    }
    /* pong / destroy: ignored */
}

static void req_xdg_surface(uint32_t id, uint16_t op, const uint8_t *a, uint32_t n)
{
    obj_t *xs = obj(id);
    if (!xs) return;
    if (op == 1) {                                  /* get_toplevel(id) */
        if (n < 4) return;
        uint32_t nid = r_u32(a);
        obj_set(nid, O_XDG_TOPLEVEL);
        xs->x_toplevel = nid;
        /* Initial configure: let the client choose its size (0,0), then ack. */
        uint32_t tc[3] = { 0, 0, 0 };               /* width,height,states(array len 0) */
        /* xdg_toplevel.configure has an array arg; send it with len 0. */
        uint8_t *m = g_tx; uint32_t sz = 8 + 4 + 4 + 4;
        w_u32(m, nid); w_u32(m + 4, (sz << 16) | 0 /*configure*/);
        w_u32(m + 8, tc[0]); w_u32(m + 12, tc[1]); w_u32(m + 16, 0 /*array len*/);
        if (g_client >= 0) u_send(g_client, m, sz);
        xs->x_serial = g_next_serial++;
        uint32_t sc[1] = { xs->x_serial };
        ev(id, 0 /*xdg_surface.configure*/, sc, 1);
        dbg("[wl] xdg toplevel configured\n");
    } else if (op == 4) {                           /* ack_configure(serial) */
        /* fine */
    }
}

static void req_seat(uint32_t id, uint16_t op, const uint8_t *a, uint32_t n)
{
    (void)id;
    if (n < 4) return;
    uint32_t nid = r_u32(a);
    if (op == 0)      obj_set(nid, O_POINTER);
    else if (op == 1) obj_set(nid, O_KEYBOARD);
    else if (op == 2) obj_set(nid, O_NONE);         /* touch */
}

static void req_subcompositor(uint32_t id, uint16_t op, const uint8_t *a, uint32_t n)
{
    (void)id;
    if (op == 1 && n >= 4) obj_set(r_u32(a), O_SUBSURFACE);  /* get_subsurface */
}

static void req_ddm(uint32_t id, uint16_t op, const uint8_t *a, uint32_t n)
{
    (void)id;
    if (n < 4) return;
    uint32_t nid = r_u32(a);
    if (op == 0)      obj_set(nid, O_DATA_SOURCE);
    else if (op == 1) obj_set(nid, O_DATA_DEVICE);
}

static void dispatch(uint32_t id, uint16_t op, const uint8_t *args, uint32_t alen)
{
    if (id == 1) { req_display(id, op, args, alen); return; }
    obj_t *o = obj(id);
    if (!o) { dbg("[wl] msg to unknown id\n"); return; }
    switch (o->type) {
    case O_REGISTRY:      req_registry(id, op, args, alen); break;
    case O_COMPOSITOR:    req_compositor(id, op, args, alen); break;
    case O_SHM:           req_shm(id, op, args, alen); break;
    case O_SHM_POOL:      req_shm_pool(id, op, args, alen); break;
    case O_SURFACE:       req_surface(id, op, args, alen); break;
    case O_XDG_WM_BASE:   req_xdg_wm_base(id, op, args, alen); break;
    case O_XDG_SURFACE:   req_xdg_surface(id, op, args, alen); break;
    case O_XDG_TOPLEVEL:  /* set_title/app_id/etc: ignored */ break;
    case O_SEAT:          req_seat(id, op, args, alen); break;
    case O_SUBCOMPOSITOR: req_subcompositor(id, op, args, alen); break;
    case O_DDM:           req_ddm(id, op, args, alen); break;
    default: break;
    }
}

/* ---- socket read: one recvmsg, append bytes to g_rx, queue any fds ---- */
static int pump_read(void)
{
    if (g_rxlen >= RX_CAP) return 0;
    struct iovec_k iov = { (uint64_t)(uintptr_t)(g_rx + g_rxlen), RX_CAP - g_rxlen };
    uint8_t cbuf[64];
    struct msghdr_k msg;
    memset(&msg, 0, sizeof(msg));
    msg.iov = (uint64_t)(uintptr_t)&iov;
    msg.iovlen = 1;
    msg.control = (uint64_t)(uintptr_t)cbuf;
    msg.controllen = sizeof(cbuf);
    int64_t r = (int64_t)syscall2(SYS_UNIX_RECVMSG, (uint64_t)g_client, (uint64_t)(uintptr_t)&msg);
    if (r <= 0) return (int)r;
    g_rxlen += (uint32_t)r;
    if (msg.controllen >= sizeof(struct cmsghdr_k)) {
        struct cmsghdr_k *c = (struct cmsghdr_k *)cbuf;
        if (c->level == 1 && c->type == 1) {
            uint32_t nf = (c->len - (uint32_t)sizeof(*c)) / 4;
            int32_t *fds = (int32_t *)(cbuf + sizeof(*c));
            for (uint32_t i = 0; i < nf; i++) {
                uint32_t next = (g_rx_fd_head + 1) % 16;
                if (next != g_rx_fd_tail) { g_rx_fds[g_rx_fd_head] = fds[i]; g_rx_fd_head = next; }
            }
        }
    }
    return (int)r;
}

static void process_rx(void)
{
    uint32_t off = 0;
    while (g_rxlen - off >= 8) {
        const uint8_t *m = g_rx + off;
        uint32_t id = r_u32(m);
        uint32_t w1 = r_u32(m + 4);
        uint16_t op = (uint16_t)(w1 & 0xffff);
        uint32_t size = w1 >> 16;
        if (size < 8 || size > RX_CAP) { dbg("[wl] bad msg size\n"); off = g_rxlen; break; }
        if (g_rxlen - off < size) break;                 /* partial */
        dispatch(id, op, m + 8, size - 8);
        off += (size + 3) & ~3u;                          /* messages are 4-aligned */
    }
    if (off > 0) {
        memmove(g_rx, g_rx + off, g_rxlen - off);
        g_rxlen -= off;
    }
}

void _start(void)
{
    dbg("[wl] compositor starting\n");

    /* our on-screen window (created before any client so it exists early) */
    while (window_get_wm_pid() < 0) process_yield();
    g_win = window_create(900, 700, "Wayland");
    if (g_win) g_winpx = window_get_backing_store(g_win, &g_win_w, &g_win_h);
    if (g_winpx) {
        for (uint32_t i = 0; i < g_win_w * g_win_h; i++) g_winpx[i] = 0xff202428u;
        window_damage(g_win, 0, 0, g_win_w, g_win_h);
        window_end_transaction(g_win);
    }

    int ls = u_socket();
    dbg(ls < 0 ? "[wl] u_socket FAILED\n" : "[wl] u_socket ok\n");
    if (ls < 0) process_exit(1);
    int br = u_bind(ls, WL_SOCK_PATH);
    dbg(br < 0 ? "[wl] u_bind FAILED\n" : "[wl] u_bind ok\n");
    if (br < 0) process_exit(1);
    int lr = u_listen(ls);
    dbg(lr < 0 ? "[wl] u_listen FAILED\n" : "[wl] u_listen ok\n");
    if (lr < 0) process_exit(1);
    dbg("[wl] listening on " WL_SOCK_PATH "\n");

    for (;;) {
        if (g_client < 0) {
            int c = u_accept(ls);
            if (c >= 0) {
                g_client = c;
                memset(g_obj, 0, sizeof(g_obj));
                g_obj[1].type = O_DISPLAY;
                g_rxlen = 0; g_rx_fd_head = g_rx_fd_tail = 0;
                dbg("[wl] client connected\n");
            } else {
                process_yield();
                continue;
            }
        }
        int r = pump_read();
        if (r > 0) {
            process_rx();
        } else {
            /* nothing to read yet; yield + brief nap so we don't spin a core */
            sleep_ms(4);
        }
    }
}
