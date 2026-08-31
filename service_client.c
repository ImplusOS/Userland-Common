/*
 * Userland service client -- see service_client.h.
 *
 * Thin wrapper over the userland dynamic linker (libc dlfcn) that loads a
 * Userland service by name from /Userland/Service/<name>/<name>.so and
 * lets it be dropped again at runtime (hot load / unload).
 */
#include "service_client.h"

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern int32_t file_open(const char *path, uint64_t flags);
extern int64_t file_read(int32_t fd, void *buffer, uint64_t len);
extern int32_t file_close(int32_t fd);

extern void serial_write_string(const char *s);

#define SERVICE_DIR      "/Userland/Service/"
#define SERVICE_LIST     "/Userland/Service/services.list"
#define SERVICE_NAME_MAX 96

static void build_so_path(const char *name, char *out, size_t out_len)
{
    /* /Userland/Service/<name>/<name>.so */
    size_t p = 0;
    const char *dir = SERVICE_DIR;
    while (*dir && p + 1 < out_len) out[p++] = *dir++;
    size_t name_start = p;
    const char *n = name;
    while (*n && p + 1 < out_len) out[p++] = *n++;
    if (p + 1 < out_len) out[p++] = '/';
    for (size_t i = name_start; i < name_start + (size_t)(n - name) && p + 1 < out_len; ++i)
        out[p++] = out[i];
    const char *ext = ".so";
    while (*ext && p + 1 < out_len) out[p++] = *ext++;
    out[p] = '\0';
}

void *service_load(const char *name)
{
    if (name == NULL || name[0] == '\0') return NULL;

    char path[256];
    build_so_path(name, path, sizeof(path));

    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    return h;
}

void *service_sym(void *handle, const char *symbol)
{
    if (handle == NULL || symbol == NULL) return NULL;
    return dlsym(handle, symbol);
}

int service_unload(void *handle)
{
    if (handle == NULL) return -1;
    int rc = dlclose(handle);
    return rc;
}

int service_load_all(void)
{
    int32_t fd = file_open(SERVICE_LIST, 0);
    if (fd < 0) return 0;

    static char buf[2048];
    int64_t n = file_read(fd, buf, sizeof(buf) - 1);
    file_close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    int loaded = 0;
    size_t i = 0;
    while (buf[i] != '\0') {
        /* skip leading whitespace */
        while (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n') i++;
        if (buf[i] == '\0') break;
        if (buf[i] == '#') {
            while (buf[i] != '\0' && buf[i] != '\n') i++;
            continue;
        }
        char name[SERVICE_NAME_MAX];
        size_t k = 0;
        while (buf[i] != '\0' && buf[i] != '\n' && buf[i] != '\r' &&
               buf[i] != ' ' && buf[i] != '\t' && k + 1 < sizeof(name)) {
            name[k++] = buf[i++];
        }
        name[k] = '\0';
        while (buf[i] != '\0' && buf[i] != '\n') i++;
        if (k > 0 && service_load(name) != NULL) loaded++;
    }
    return loaded;
}
