/*
 * The JIT's guest-independent runtime; see jit.h.
 */

#include "jit.h"
#include "host.h"

#if defined(__aarch64__)
#include "a64emit.h"
#endif

static void *map_exec(size_t size)
{
    long p = host_mmap(0, XL_PAGE_UP(size),
                       HOST_PROT_READ | HOST_PROT_WRITE | HOST_PROT_EXEC,
                       HOST_MAP_PRIVATE | HOST_MAP_ANONYMOUS | HOST_MAP_NORESERVE, -1, 0);
    if (p < 0 && p > -4096) {
        return NULL;
    }
    return (void *)(uintptr_t)p;
}

static void *map_data(size_t size)
{
    long p = host_mmap(0, XL_PAGE_UP(size), HOST_PROT_READ | HOST_PROT_WRITE,
                       HOST_MAP_PRIVATE | HOST_MAP_ANONYMOUS | HOST_MAP_NORESERVE, -1, 0);
    if (p < 0 && p > -4096) {
        return NULL;
    }
    return (void *)(uintptr_t)p;
}

jit_cache_t *jit_cache_get(gthread_t *t, size_t code_size, size_t meta_size,
                           void (*emit_tramp)(jit_cache_t *jc))
{
    jit_cache_t *jc = t->engine;
    if (XL_LIKELY(jc != NULL)) {
        return jc;
    }
    jc = map_data(sizeof(jit_cache_t));
    if (jc == NULL) {
        xl_fatal("out of memory for the JIT cache");
    }
    jc->code = map_exec(code_size);
    jc->meta = map_data(meta_size);
    jc->blocks_cap = 1u << 16;
    jc->blocks = map_data(jc->blocks_cap * sizeof(jit_block_t *));
    if (jc->code == NULL || jc->meta == NULL || jc->blocks == NULL) {
        xl_fatal("out of memory for the JIT cache (code %lu, data %lu bytes)",
                 (unsigned long)code_size, (unsigned long)meta_size);
    }
    jc->code_end = jc->code + code_size;
    jc->code_cur = jc->code;
    jc->meta_end = jc->meta + meta_size;
    jc->meta_cur = jc->meta;
    jc->inval_seen = g_xl_inval_seq;
    jc->mt = g_xl_mt;
    jc->lo_pc = ~0ULL;
    jc->hi_pc = 0;
    emit_tramp(jc);
    jc->code_cur = (uint8_t *)(((uintptr_t)jc->code_cur + 63u) & ~(uintptr_t)63u);
    jc->code_base = jc->code_cur;
    jit_host_sync(jc->code, jc->code_base);
    t->engine = jc;
    return jc;
}

void jit_flush(jit_cache_t *jc)
{
    memset(jc->ibtc, 0, sizeof(jc->ibtc));
    memset(jc->hash, 0, sizeof(jc->hash));
    jc->code_cur = jc->code_base;
    jc->meta_cur = jc->meta;
    jc->nblocks = 0;
    jc->patch_site = 0;
    jc->lo_pc = ~0ULL;
    jc->hi_pc = 0;
    jc->flushes++;
}

bool jit_room(jit_cache_t *jc, size_t code_bytes, size_t meta_bytes)
{
    if ((size_t)(jc->code_end - jc->code_cur) < code_bytes ||
        (size_t)(jc->meta_end - jc->meta_cur) < meta_bytes ||
        jc->nblocks == jc->blocks_cap) {
        jit_flush(jc);
        return false;
    }
    return true;
}

void *jit_meta_alloc(jit_cache_t *jc, size_t n)
{
    n = (n + 15u) & ~(size_t)15u;
    if ((size_t)(jc->meta_end - jc->meta_cur) < n) {
        return NULL;
    }
    void *p = jc->meta_cur;
    jc->meta_cur += n;
    return p;
}

static inline uint32_t hash_of(uint64_t pc)
{
    return (uint32_t)((pc ^ (pc >> JIT_HASH_BITS) ^ (pc >> 30)) & ((1u << JIT_HASH_BITS) - 1u));
}

jit_block_t *jit_lookup(const jit_cache_t *jc, uint64_t pc)
{
    for (jit_block_t *b = jc->hash[hash_of(pc)]; b != NULL; b = b->hnext) {
        if (b->pc == pc) {
            return b;
        }
    }
    return NULL;
}

void jit_insert(jit_cache_t *jc, jit_block_t *b)
{
    const uint32_t h = hash_of(b->pc);
    b->hnext = jc->hash[h];
    jc->hash[h] = b;
    jc->blocks[jc->nblocks++] = b;
    for (unsigned k = 0; k < b->nsegs; ++k) {
        if (b->seg[k].start < jc->lo_pc) jc->lo_pc = b->seg[k].start;
        if (b->seg[k].end > jc->hi_pc) jc->hi_pc = b->seg[k].end;
    }
}

void jit_kill(jit_cache_t *jc, jit_block_t *b)
{
    if (b->dead) {
        return;
    }
    b->dead = 1;
    jit_block_t **pp = &jc->hash[hash_of(b->pc)];
    while (*pp != NULL) {
        if (*pp == b) {
            *pp = b->hnext;
            break;
        }
        pp = &(*pp)->hnext;
    }
    jit_ibtc_t *e = &jc->ibtc[jit_ibtc_slot(b->pc)];
    if (e->host == (uint64_t)(uintptr_t)b->code) {
        e->pc = 0;
        e->host = 0;
    }
    jit_host_patch_jump(b->code, b->redirect);
}

jit_block_t *jit_find_host(const jit_cache_t *jc, uint64_t host_pc)
{
    uint32_t lo = 0, hi = jc->nblocks;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2u;
        const jit_block_t *b = jc->blocks[mid];
        const uint64_t s = (uint64_t)(uintptr_t)b->code;
        if (host_pc < s) {
            hi = mid;
        } else if (host_pc >= s + b->code_size) {
            lo = mid + 1u;
        } else {
            return jc->blocks[mid];
        }
    }
    return NULL;
}

static void inval_range(uint64_t s, uint64_t e, void *arg)
{
    jit_cache_t *jc = arg;
    if (e <= jc->lo_pc || s >= jc->hi_pc) {
        return;
    }
    for (uint32_t i = 0; i < jc->nblocks; ++i) {
        jit_block_t *b = jc->blocks[i];
        if (b->dead || !b->ro) {
            continue;
        }
        for (unsigned k = 0; k < b->nsegs; ++k) {
            if (b->seg[k].start < e && b->seg[k].end > s) {
                jit_kill(jc, b);
                break;
            }
        }
    }
}

void jit_replay_inval(jit_cache_t *jc)
{
    if (XL_LIKELY(jc->inval_seen == g_xl_inval_seq)) {
        return;
    }
    if (!xl_code_inval_since(&jc->inval_seen, inval_range, jc)) {
        jit_flush(jc);
    }
}

/* ---- host hooks ---------------------------------------------------------- */

#if defined(__aarch64__)

void jit_host_sync(void *begin, void *end)
{
    a64_sync_icache(begin, end);
}

void jit_host_patch_jump(uint8_t *site, const uint8_t *target)
{
    uint32_t *p = (uint32_t *)(void *)site;
    *p = a64_enc_b(p, target);
    a64_sync_icache(p, p + 1);
}

#elif defined(__x86_64__)

void jit_host_sync(void *begin, void *end)
{
    (void)begin;
    (void)end;
}

void jit_host_patch_jump(uint8_t *site, const uint8_t *target)
{
    /* jmp rel32 (5 bytes; every patchable site reserves them) */
    const int32_t rel = (int32_t)(target - (site + 5));
    uint8_t buf[5] = { 0xE9, 0, 0, 0, 0 };
    memcpy(buf + 1, &rel, 4);
    memcpy(site, buf, 5);
}

#endif
