#pragma once

/*
 * The JIT's guest-independent runtime: per-thread code and metadata
 * arenas, the guest-PC -> translation map, the indirect-branch target
 * cache the generated code probes inline, host-PC -> translation lookup
 * for faults, and invalidation when code the translator trusted changes.
 *
 * A guest's JIT (guest/<isa>/<isa>_jit.c) decodes and emits; this file
 * only keeps translations. Each guest thread has its own cache (like the
 * block engine's), so nothing here takes a lock on the hot path and a
 * cache is only ever modified by the thread that runs it.
 *
 * Code changes. A translation made from a range mapped executable and not
 * writable ("read-only code") is trusted until the translator sees that
 * range unmapped, remapped or made writable (syscall.c logs those; every
 * cache replays the log the next time its thread enters the dispatcher).
 * Anything else is compared against guest memory by the translation
 * itself each time it is entered. A translation is never freed on its own:
 * it is unlinked, and its first instruction is overwritten with a jump to
 * a stub that re-enters the dispatcher, so jumps already chained to it
 * stay correct. Space comes back when the whole cache is flushed.
 */

#include "xl.h"

#define JIT_MAX_SEGS   4
#define JIT_HASH_BITS  15
#define JIT_IBTC_BITS  12
#define JIT_IBTC_SIZE  (1u << JIT_IBTC_BITS)

typedef struct jit_block {
    uint64_t pc;                /* guest entry */
    uint8_t *code;              /* host entry */
    uint32_t code_size;         /* bytes, cold stubs included */
    uint8_t ro;                 /* trusted read-only code (no self check) */
    uint8_t nsegs;
    uint8_t dead;
    uint8_t _pad;
    uint32_t nsites;            /* fault sites, sorted by host offset */
    uint32_t site_size;
    void *sites;
    uint8_t *redirect;          /* stub: "go back to the dispatcher for pc" */
    struct { uint64_t start; uint64_t end; } seg[JIT_MAX_SEGS];
    struct jit_block *hnext;
} jit_block_t;

typedef struct {
    uint64_t pc;
    uint64_t host;
} jit_ibtc_t;

typedef struct jit_cache {
    /* Offsets 0 and 64 are read by generated code. */
    uint64_t patch_site;        /* set by a chainable exit: the branch to patch */
    uint64_t _r0[7];
    jit_ibtc_t ibtc[JIT_IBTC_SIZE];

    jit_block_t *hash[1u << JIT_HASH_BITS];
    uint8_t *code, *code_end, *code_cur, *code_base;   /* code_base: after the trampolines */
    uint8_t *meta, *meta_end, *meta_cur;
    jit_block_t **blocks;       /* in allocation (= host address) order */
    uint32_t nblocks, blocks_cap;
    uint64_t inval_seen;        /* g_xl_inval_seq replayed so far */
    uint64_t lo_pc, hi_pc;      /* guest code range the live translations cover */
    int mt;                     /* translated for a multi-threaded process */
    uint32_t flushes;
    void *tramp[8];             /* guest JIT's trampolines */
    void *scratch;              /* guest JIT's translation workspace */
} jit_cache_t;

#define JIT_IBTC_OFF 64u

/* The thread's cache (created on first use with the given arena sizes;
 * emit_tramp then writes the guest JIT's trampolines at code_base). */
jit_cache_t *jit_cache_get(gthread_t *t, size_t code_size, size_t meta_size,
                           void (*emit_tramp)(jit_cache_t *jc));
/* Drop every translation (the trampolines stay). */
void jit_flush(jit_cache_t *jc);
/* Make room for a translation of at most this size; flushes the cache
 * (and returns false) when there is not enough left. */
bool jit_room(jit_cache_t *jc, size_t code_bytes, size_t meta_bytes);
/* Metadata (block headers, fault sites, code copies): never executed. */
void *jit_meta_alloc(jit_cache_t *jc, size_t n);
jit_block_t *jit_lookup(const jit_cache_t *jc, uint64_t pc);
/* Register a finished translation (map + host-address order). */
void jit_insert(jit_cache_t *jc, jit_block_t *b);
/* Unlink b and point its entry at its redirect stub. */
void jit_kill(jit_cache_t *jc, jit_block_t *b);
/* The translation whose code contains host_pc, or NULL. */
jit_block_t *jit_find_host(const jit_cache_t *jc, uint64_t host_pc);

static inline uint32_t jit_ibtc_slot(uint64_t pc)
{
    return (uint32_t)((pc ^ (pc >> 12)) & (JIT_IBTC_SIZE - 1u));
}

static inline void jit_ibtc_set(jit_cache_t *jc, uint64_t pc, const void *host)
{
    jit_ibtc_t *e = &jc->ibtc[jit_ibtc_slot(pc)];
    e->pc = pc;
    e->host = (uint64_t)(uintptr_t)host;
}

/* Replay the invalidation log: kill translations of code that changed. */
void jit_replay_inval(jit_cache_t *jc);

/* Host hooks (jit.c, per host CPU). */
void jit_host_sync(void *begin, void *end);         /* I-cache coherency */
void jit_host_patch_jump(uint8_t *site, const uint8_t *target);
