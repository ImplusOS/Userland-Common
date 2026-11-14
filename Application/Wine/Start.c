#include <stdint.h>

#include "File.h"
#include "LinuxEnv.h"
#include "Process.h"
#include "Serial.h"
#include "XSession.h"

/* The launcher provides a visible, deterministic Wine smoke test. To run a
 * Windows program instead, place its command line in
 * /Userland/Wine/Resource/launch.args; the first token is passed to wine as
 * the program name and the remaining tokens as its arguments. The file is
 * optional so the image remains useful without shipping proprietary PE
 * software. */
#define WINE_DEFAULT_ARGS "winecfg.exe"
#define WINE_LOADER_PATH "/usr/lib/wine/wine64"
#define WINE_ARGS_FILE "/Userland/Wine/Resource/launch.args"
/* Exit status of wine64 when it ran without a usable prefix ("could not
 * load kernel32.dll"), and how many times to wait for wineboot again. */
#define WINE_STATUS_PREFIX_NOT_READY 53
#define WINE_PREFIX_RETRIES 4u

static uint32_t text_length(const char *text)
{
    uint32_t n = 0u;
    while (text[n] != '\0') ++n;
    return n;
}

static void copy_args(char *out, uint32_t capacity, const char *in,
                      uint32_t length)
{
    uint32_t at = 0u;
    for (uint32_t i = 0u; i < length && at + 1u < capacity; ++i) {
        char c = in[i];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        out[at++] = c;
    }
    while (at != 0u && out[at - 1u] == ' ') --at;
    out[at] = '\0';
}

static const char *wine_args(void)
{
    static char args[512];
    char file_args[sizeof(args) - 1u];
    /* A desktop entry may name the Windows program after the launcher's
     * path ("Exec=/Userland/Wine/Wine.ELF explorer.exe"); that wins over
     * the image-wide launch.args file, so several Start-menu rows can share
     * this launcher. */
    if (process_get_launch_argument(file_args, (uint32_t)sizeof(file_args)) >= 0) {
        copy_args(args, (uint32_t)sizeof(args), file_args, text_length(file_args));
        if (args[0] != '\0') return args;
    }
    int32_t fd = file_open(WINE_ARGS_FILE, 0);
    if (fd >= 0) {
        int64_t got = file_read(fd, file_args, sizeof(file_args));
        (void)file_close(fd);
        if (got > 0) {
            copy_args(args, (uint32_t)sizeof(args), file_args, (uint32_t)got);
            if (args[0] != '\0') {
                serial_write_string("[wine] using ");
                serial_write_string(WINE_ARGS_FILE);
                serial_write_string("\n");
                return args;
            }
        }
    }
    copy_args(args, (uint32_t)sizeof(args), WINE_DEFAULT_ARGS,
              text_length(WINE_DEFAULT_ARGS));
    return args;
}

void _start(void)
{
    xsession_t session;
    if (xsession_open(&session, "Wine", 1024u, 680u) < 0) {
        serial_write_string("[wine] no X server; giving up\n");
        process_exit(1);
    }

    linux_env_t env;
    linux_env_init(&env);
    linux_env_add_wine(&env);
    /* /usr/bin/wine is Debian's POSIX shell wrapper. ImplusOS launches ELF
     * files directly, so enter through Wine's actual Unix ELF loader. */
    const char *args = wine_args();
    /* ImplusOS enters Wine through the real ELF loader. The PE winecfg
     * executable is named explicitly so Wine does not need the distro shell
     * wrapper or its external basename/cut helpers. */
    int32_t status = xsession_run_env(&session, WINE_LOADER_PATH, args,
                                      linux_env_entries(&env));
    /* The first start in a fresh prefix runs wineboot to build it, and
     * Wine's initial process gives that only five minutes before carrying on
     * without a prefix -- it then exits with "could not load kernel32.dll"
     * (status 53) while wineboot keeps working in the background. Copying
     * the ~700 MB of builtin DLLs into the tmpfs prefix can take longer than
     * that here. Starting again simply waits on the same wineboot (its
     * __wineboot_event) and then runs the program in the finished prefix. */
    for (uint32_t retry = 0u; status == WINE_STATUS_PREFIX_NOT_READY &&
                              retry < WINE_PREFIX_RETRIES; ++retry) {
        serial_write_string("[wine] prefix still being built; starting again\n");
        status = xsession_run_env(&session, WINE_LOADER_PATH, args,
                                  linux_env_entries(&env));
    }
    if (status < 0) serial_write_string("[wine] could not start wine\n");
    xsession_close(&session);
    process_exit(status < 0 ? 1 : status);
}
