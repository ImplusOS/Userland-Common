/*
 * XdgOpen -- /usr/bin/xdg-open for Linux programs.
 *
 * Linux desktop programs hand "open this file / show this folder" to
 * xdg-open. Chromium does it for a finished download ("Open", clicking it in
 * the download bubble or chrome://downloads) and for "Show in folder" once
 * the D-Bus FileManager1 call has failed. ImplusOS had no xdg-open, so those
 * actions did nothing at all. This native helper is installed as
 * /usr/bin/xdg-open (top-level Makefile, STAGE_XDG_OPEN) and forwards to the
 * ImplusOS apps:
 *
 *   directory                  -> the file manager, opened on it
 *   text-like file             -> the editor
 *   anything else              -> the file manager, on the folder holding it
 *
 * The kernel passes a native program exec'd from a Linux one its argv[1..]
 * as the launch argument (process_execve), which is where the target comes
 * from. A file:// URL is accepted as well as a plain path.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "File.h"
#include "Process.h"
#include "Serial.h"

#define PATH_MAX 512

static const char *FILE_MANAGER =
    "/Userland/com.ImplusOS.filemanager/com.ImplusOS.filemanager.ELF";
static const char *EDITOR =
    "/Userland/com.ImplusOS.editor/com.ImplusOS.editor.ELF";

static bool is_text_name(const char *n)
{
    const char *dot = strrchr(n, '.');
    if (!dot) return false;
    static const char *ext[] = { ".txt", ".md", ".c", ".h", ".cpp", ".log",
                                 ".cfg", ".ini", ".sh", ".json", ".xml",
                                 ".list", ".conf", ".py", ".csv" };
    for (unsigned i = 0; i < sizeof(ext) / sizeof(ext[0]); ++i) {
        if (strcmp(dot, ext[i]) == 0) return true;
    }
    return false;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* file:///a/b%20c -> /a/b c ; a plain path is copied as is. */
static void to_path(const char *in, char *out, uint32_t cap)
{
    const char *p = in;
    if (strncmp(p, "file://", 7) == 0) {
        p += 7;
        if (strncmp(p, "localhost/", 10) == 0) p += 9;
    }
    uint32_t o = 0;
    while (*p != '\0' && o + 1u < cap) {
        if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
            out[o++] = (char)(hexval(p[1]) * 16 + hexval(p[2]));
            p += 3;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
}

void _start(void);
void _start(void)
{
    char arg[PATH_MAX];
    char path[PATH_MAX];
    int32_t n = process_get_launch_argument(arg, sizeof(arg));
    if (n <= 0 || arg[0] == '\0') {
        serial_write_string("[xdg-open] no target\n");
        process_exit(1);
    }
    /* The launch argument is argv[1..] joined with spaces; xdg-open takes a
     * single target, so all of it is the path (a file name with spaces
     * comes back together this way). */
    to_path(arg, path, sizeof(path));

    file_stat_t st;
    int exists = file_stat(path, &st) >= 0 && st.exists;
    if (!exists) {
        serial_write_string("[xdg-open] not found: ");
        serial_write_string(path);
        serial_write_string("\n");
        process_exit(2);
    }

    int32_t pid;
    if (st.is_dir) {
        pid = process_spawn_with_arg(FILE_MANAGER, path);
    } else if (is_text_name(path)) {
        pid = process_spawn_with_arg(EDITOR, path);
    } else {
        char dir[PATH_MAX];
        strncpy(dir, path, sizeof(dir) - 1u);
        dir[sizeof(dir) - 1u] = '\0';
        char *slash = strrchr(dir, '/');
        if (slash != NULL && slash != dir) *slash = '\0';
        else strcpy(dir, "/");
        pid = process_spawn_with_arg(FILE_MANAGER, dir);
    }
    serial_write_string(pid >= 0 ? "[xdg-open] opened " : "[xdg-open] could not open ");
    serial_write_string(path);
    serial_write_string("\n");
    process_exit(pid >= 0 ? 0 : 3);
}
