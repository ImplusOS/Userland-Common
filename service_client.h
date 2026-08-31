/*
 * Userland service client -- hot load / unload of Userland services.
 *
 * A service is a single position-independent shared object staged at
 * /Userland/Service/<name>/<name>.so (see Userland/Service/Common.mk).
 * These helpers wrap dlopen/dlsym/dlclose so any process can pull a
 * service in at runtime and drop it again without a reboot.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Load the service <name> (e.g. "com.ImplusOS.posix"). Returns an opaque
 * handle, or NULL on failure. Idempotent: loading an already-loaded
 * service returns the existing handle. */
void *service_load(const char *name);

/* Resolve a symbol exported by a loaded service handle. */
void *service_sym(void *handle, const char *symbol);

/* Unload a service previously returned by service_load(). Returns 0 on
 * success. The service is actually unmapped only once every loader has
 * released it. */
int service_unload(void *handle);

/* Load every service listed in /Userland/Service/services.list (one
 * service name per line; '#' comments and blank lines ignored). Returns
 * the number of services successfully loaded. Safe to call more than
 * once. */
int service_load_all(void);

#ifdef __cplusplus
}
#endif
