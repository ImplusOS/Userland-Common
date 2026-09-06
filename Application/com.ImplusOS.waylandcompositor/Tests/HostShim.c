/*
 * Host harness: builds the compositor's real Compositor.c + Wayland.c on
 * Linux by supplying the ImplusOS syscall surface they use, so the wire
 * protocol can be driven by a real libwayland client (the very gtk3-demo
 * binary the image ships) before any QEMU boot.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "Graphics.h"
#include "Input.h"
#include "Window.h"

#define SYS_UNIX_SOCKET   220
#define SYS_UNIX_BIND     221
#define SYS_UNIX_LISTEN   222
#define SYS_UNIX_ACCEPT   223
#define SYS_UNIX_SEND     225
#define SYS_UNIX_SENDMSG  227
#define SYS_UNIX_RECVMSG  228
#define SYS_UNIX_CLOSE    229
#define SYS_SERIAL_PUTS   2

static const char *test_dir(void)
{
    const char *d = getenv("WLC_TEST_DIR");
    return d ? d : "/tmp/wlc-test";
}

uint64_t syscall0(uint64_t n) { (void)n; return 0; }

uint64_t syscall1(uint64_t n, uint64_t a)
{
    switch (n) {
    case SYS_UNIX_SOCKET: {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) fcntl(fd, F_SETFL, O_NONBLOCK);
        return (uint64_t)(int64_t)fd;
    }
    case SYS_UNIX_ACCEPT: {
        int fd = accept((int)a, NULL, NULL);
        if (fd >= 0) fcntl(fd, F_SETFL, O_NONBLOCK);
        return (uint64_t)(int64_t)(fd < 0 ? -11 : fd);
    }
    case SYS_UNIX_CLOSE: close((int)a); return 0;
    case SYS_SERIAL_PUTS: fputs((const char *)(uintptr_t)a, stderr); return 0;
    default: return 0;
    }
}

uint64_t syscall2(uint64_t n, uint64_t a, uint64_t b)
{
    switch (n) {
    case SYS_UNIX_BIND: {
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/wayland-0", test_dir());
        unlink(sa.sun_path);
        return (uint64_t)(int64_t)bind((int)a, (struct sockaddr *)&sa, sizeof(sa));
    }
    case SYS_UNIX_LISTEN: return (uint64_t)(int64_t)listen((int)a, (int)b);
    case SYS_UNIX_SENDMSG: {
        ssize_t r = sendmsg((int)a, (struct msghdr *)(uintptr_t)b, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (r < 0) return (uint64_t)(int64_t)(errno == EAGAIN ? -11 : -5);
        return (uint64_t)r;
    }
    case SYS_UNIX_RECVMSG: {
        struct msghdr *m = (struct msghdr *)(uintptr_t)b;
        ssize_t r = recvmsg((int)a, m, MSG_DONTWAIT);
        if (r < 0) return (uint64_t)(int64_t)(errno == EAGAIN ? -11 : -5);
        if (r == 0) return 0;
        /* The ImplusOS kernel reports the used control length the same way. */
        return (uint64_t)r;
    }
    default: return 0;
    }
}

uint64_t syscall3(uint64_t n, uint64_t a, uint64_t b, uint64_t c)
{
    if (n == SYS_UNIX_SEND) {
        ssize_t r = send((int)a, (const void *)(uintptr_t)b, (size_t)c,
                         MSG_DONTWAIT | MSG_NOSIGNAL);
        if (r < 0) return (uint64_t)(int64_t)(errno == EAGAIN ? -11 : -5);
        return (uint64_t)r;
    }
    return 0;
}

/* ---- shared memory: an ImplusOS handle is just the fd here ---- */
int32_t os_shared_memory_create(uint32_t size)
{
    int fd = memfd_create("wlc", 0);
    if (fd < 0) return -1;
    if (ftruncate(fd, size) < 0) { close(fd); return -1; }
    return fd;
}
int32_t os_memfd_shm_handle(int32_t fd) { return dup(fd); }
int32_t os_memfd_from_shm(int32_t handle) { return dup(handle); }
void *os_shared_memory_map(int32_t handle)
{
    struct stat st;
    if (fstat(handle, &st) < 0 || st.st_size == 0) return NULL;
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, handle, 0);
    return p == MAP_FAILED ? NULL : p;
}
uint32_t os_shared_memory_size(int32_t handle)
{
    struct stat st;
    if (fstat(handle, &st) < 0) return 0u;
    return (uint32_t)st.st_size;
}
int32_t os_shared_memory_unmap(int32_t handle, void *address)
{
    struct stat st;
    if (address && fstat(handle, &st) == 0) munmap(address, (size_t)st.st_size);
    close(handle);
    return 0;
}
int32_t file_close(int32_t fd) { return close(fd); }

/* ---- process / time ---- */
void process_yield(void) { sched_yield(); }
void process_exit(int32_t s) { exit(s); }
void sleep_ms(uint64_t ms) { usleep((useconds_t)(ms * 1000u)); }
uint64_t get_uptime_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}
int32_t process_get_launch_argument(char *buf, uint32_t cap)
{
    const char *a = getenv("WLC_ARG");
    if (!a) return -1;
    snprintf(buf, cap, "%s", a);
    return 0;
}
void serial_write_string(const char *s) { fputs(s, stderr); fflush(stderr); }
void serial_write_uint32(uint32_t v) { fprintf(stderr, "%u", v); fflush(stderr); }

/* ---- display / input: a panel-mode output backed by plain memory ---- */
#define OUT_W 1280u
#define OUT_H 800u
static uint32_t g_fake_fb[OUT_W * OUT_H];
static unsigned g_frames;

uint32_t get_display_width(void) { return OUT_W; }
uint32_t get_display_height(void) { return OUT_H; }
void *sys_get_display_framebuffer(void) { return g_fake_fb; }
int64_t display_get_monitor_mode_info(uint32_t mi, uint32_t xi, display_mode_info_t *o)
{ (void)mi; (void)xi; if (o) { memset(o, 0, sizeof(*o)); o->stride = OUT_W; } return 0; }
static void dump_to(const char *path);
void draw_present_rects(const display_rect_t *r, uint32_t n)
{
    (void)r; (void)n;
    g_frames++;
    if (getenv("WLC_DUMP_EACH")) {
        char path[256];
        snprintf(path, sizeof path, "%s/frame%03u.ppm", test_dir(), g_frames);
        dump_to(path);
    }
}
void draw_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c)
{ (void)x; (void)y; (void)w; (void)h; (void)c; }

int32_t window_register_service(void) { return 0; }
int32_t window_get_wm_pid(void) { return -1; }          /* no WM -> panel mode */
/* Scripted input: "<frame> m <dx> <dy> <buttons>" or "<frame> k <sc> <down>"
 * one per line in $WLC_INPUT, delivered once the given frame has been
 * presented. Lets a run exercise pointer and keyboard routing without a
 * human at the keyboard. */
typedef struct { unsigned at; char kind; int a, b, c; int fired; } script_ev_t;
static script_ev_t g_script[64];
static unsigned g_script_n;
static int g_script_loaded;

static void script_load(void)
{
    g_script_loaded = 1;
    const char *path = getenv("WLC_INPUT");
    if (!path) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (g_script_n < 64 && fgets(line, sizeof line, f)) {
        script_ev_t e; memset(&e, 0, sizeof e);
        if (sscanf(line, "%u %c %d %d %d", &e.at, &e.kind, &e.a, &e.b, &e.c) >= 3)
            g_script[g_script_n++] = e;
    }
    fclose(f);
    fprintf(stderr, "[harness] %u scripted input events\n", g_script_n);
}

static script_ev_t *script_next(char kind)
{
    if (!g_script_loaded) script_load();
    for (unsigned i = 0; i < g_script_n; i++) {
        if (!g_script[i].fired && g_script[i].kind == kind &&
            g_frames >= g_script[i].at) {
            g_script[i].fired = 1;
            return &g_script[i];
        }
    }
    return NULL;
}

int32_t input_read_keyboard(input_keyboard_event_t *e)
{
    script_ev_t *ev = script_next('k');
    if (!ev || !e) return 0;
    memset(e, 0, sizeof *e);
    e->keycode = (uint16_t)ev->a;
    e->pressed = (uint8_t)ev->b;
    e->modifiers = (uint8_t)ev->c;
    fprintf(stderr, "[harness] key sc=%d down=%d\n", ev->a, ev->b);
    return 1;
}

int32_t input_read_mouse(input_mouse_event_t *e)
{
    script_ev_t *ev = script_next('m');
    if (!ev || !e) return 0;
    memset(e, 0, sizeof *e);
    e->x = (uint16_t)(int16_t)ev->a;
    e->y = (uint16_t)(int16_t)ev->b;
    e->buttons = (uint8_t)ev->c;
    fprintf(stderr, "[harness] mouse dx=%d dy=%d btn=%d\n", ev->a, ev->b, ev->c);
    return 1;
}

/* hosted-mode entry points, unused in this harness */
window_id_t window_create(uint32_t w, uint32_t h, const char *t)
{ (void)w; (void)h; (void)t; return 0; }
void window_destroy(window_id_t w) { (void)w; }
uint32_t *window_get_backing_store(window_id_t w, uint32_t *ow, uint32_t *oh)
{ (void)w; (void)ow; (void)oh; return NULL; }
void window_damage(window_id_t w, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ (void)w; (void)a; (void)b; (void)c; (void)d; }
void window_end_transaction(window_id_t w) { (void)w; }
void window_show(window_id_t w) { (void)w; }
void window_raise(window_id_t w) { (void)w; }
int32_t window_set_surface_opaque(window_id_t w, bool o) { (void)w; (void)o; return 0; }
int32_t window_subscribe_keyboard(window_id_t w) { (void)w; return 0; }
int32_t window_subscribe_mouse(window_id_t w) { (void)w; return 0; }
int32_t window_input_keyboard_poll(input_keyboard_event_t *e) { (void)e; return 0; }
int32_t window_input_mouse_poll(input_mouse_event_t *e) { (void)e; return 0; }

/* ---- harness driver ---- */
void wlc_main(void);                      /* Compositor.c's _start, renamed */
extern uint32_t *wlc_test_canvas(void);
extern uint32_t wlc_test_frames(void);

uint32_t wlc_test_frames(void) { return g_frames; }

/* Dump the scanout so the run can be inspected as an image. */
static void dump_to(const char *p)
{
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", OUT_W, OUT_H);
    for (unsigned i = 0; i < OUT_W * OUT_H; i++) {
        uint32_t c = g_fake_fb[i];
        fputc((int)((c >> 16) & 0xff), f);
        fputc((int)((c >> 8) & 0xff), f);
        fputc((int)(c & 0xff), f);
    }
    fclose(f);
}

static void dump(void)
{
    const char *p = getenv("WLC_DUMP");
    if (p) dump_to(p);
    fprintf(stderr, "[harness] %u frames presented\n", g_frames);
}

#include <signal.h>
static void on_term(int sig) { (void)sig; dump(); _exit(0); }

int main(void)
{
    mkdir(test_dir(), 0700);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    wlc_main();
    return 0;
}
