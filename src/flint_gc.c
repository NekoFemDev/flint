/*
 * flint_gc.c — FLINT automatic memory: conservative, non-moving mark-sweep
 * collector over the shared mem image (auto mode).
 *
 * Design constraints (the "why", see README / TODOS.md):
 *   - FLINT is typeless: the VM cannot tell a pointer from an integer, so
 *     roots are scanned CONSERVATIVELY — a word is a candidate pointer
 *     when it lands anywhere inside the payload of an allocated block.
 *   - FLINT exposes raw addresses, so the collector must not move
 *     objects. Mark-sweep reclaims cycles; reference counting would not.
 *   - Swept blocks must be reusable, so allocation is a first-fit free
 *     list over the block sequence (plus the bump frontier); adjacent
 *     free blocks are coalesced and a trailing free block shrinks the
 *     frontier.
 *   - Zero libc, no recursion, no hidden state: everything (including the
 *     fixed mark worklist) lives inside vm->gc.
 *
 * Heap layout (offsets into vm->mem):
 *
 *   heap_begin ...... first block (heap_start rounded up to 16)
 *   [ hdr 16 ][ payload size ][ hdr 16 ][ payload size ] ... <- heap_ptr
 *                                                            (grows up)
 *
 * flint_gc_alloc returns payload = block + FLINT_GC_HDR. The payload is
 * zeroed at allocation, so uninitialized words cannot retain garbage.
 * Every block carries at least 8 payload bytes; a request is rounded up
 * to 8 and a reused free block may hand out more than was asked — the
 * contract is "at least the requested size".
 *
 * Collection: mark from roots (r0..r15, active stack [sp, mem_size), data
 * [0, heap_start), host roots, pins), drain an explicit mark worklist by
 * conservatively scanning every aligned word of marked payloads, then
 * sweep. If the worklist overflows, the collector falls back to repeated
 * full rescans of all marked blocks until a pass drains cleanly —
 * correct, slower; overflow_rescans counts those passes.
 *
 * Failure policy: a block header that fails validation during a
 * structural walk means the program wrote outside every block and
 * corrupted the heap — the VM traps FLINT_ERR_BADMEM and the collection
 * aborts. Candidates that fail validation are simply not pointers.
 */
#include "flint.h"

/* ---- little-endian accessors (byte-wise, like the core) ---- */

static uint32_t u32_at(const flint_vm *vm, uint32_t a)
{
    return (uint32_t)vm->mem[a]
         | ((uint32_t)vm->mem[a + 1] << 8)
         | ((uint32_t)vm->mem[a + 2] << 16)
         | ((uint32_t)vm->mem[a + 3] << 24);
}

static void u32_put(flint_vm *vm, uint32_t a, uint32_t v)
{
    vm->mem[a]     = (flint_byte)(v & 0xFF);
    vm->mem[a + 1] = (flint_byte)((v >> 8) & 0xFF);
    vm->mem[a + 2] = (flint_byte)((v >> 16) & 0xFF);
    vm->mem[a + 3] = (flint_byte)((v >> 24) & 0xFF);
}

static flint_word word_at(const flint_vm *vm, uint32_t a)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++) v |= (uint64_t)vm->mem[a + i] << (8 * i);
    return (flint_word)v;
}

/* ---- block headers ---- */

static uint32_t blk_size(const flint_vm *vm, uint32_t blk)
{ return u32_at(vm, blk); }

static uint32_t blk_flags(const flint_vm *vm, uint32_t blk)
{ return u32_at(vm, blk + 4); }

static void blk_set_flags(flint_vm *vm, uint32_t blk, uint32_t f)
{ u32_put(vm, blk + 4, f); }

static uint32_t blk_payload(uint32_t blk)
{ return blk + FLINT_GC_HDR; }

/* write a complete header (magic last, so a torn write cannot validate) */
static void hdr_write(flint_vm *vm, uint32_t blk, uint32_t size, uint32_t flags)
{
    u32_put(vm, blk + 0,  size);
    u32_put(vm, blk + 4,  flags);
    u32_put(vm, blk + 8,  FLINT_GC_NIL);
    u32_put(vm, blk + 12, FLINT_GC_MAGIC);
}

/* structural validity of the block at blk inside [.., limit) */
static int blk_ok(const flint_vm *vm, uint32_t blk, uint32_t limit)
{
    uint32_t sz;
    if (blk > limit || limit - blk < FLINT_GC_HDR) return 0;
    if (u32_at(vm, blk + 12) != FLINT_GC_MAGIC) return 0;
    sz = u32_at(vm, blk);
    if (sz & (FLINT_GC_ALIGN - 1u)) return 0;
    if ((uint64_t)blk + FLINT_GC_HDR + sz > (uint64_t)limit) return 0;
    return 1;
}

static void gc_corrupt(flint_vm *vm)
{
    vm->err = FLINT_ERR_BADMEM;
    vm->err_text = "managed heap corrupt (block header damaged)";
}

static void gc_cache_clear(flint_vm *vm)
{
    vm->gc.find_blk = 0;
    vm->gc.find_end = 0;
}

/* ---- candidate pointer validation ----
 *
 * 1 and *out = containing allocated block, when w lands in the payload of
 * an allocated block; 0 when w is not a heap pointer; -1 when a
 * structural walk proves the heap corrupt (vm->err set). The cache holds
 * the payload span of the last hit, so scanning a payload full of
 * pointers into one block stays cheap. */
static int gc_lookup(flint_vm *vm, uint64_t w, uint32_t *out)
{
    uint32_t c, blk;

    if (w < (uint64_t)vm->gc.heap_begin || w >= (uint64_t)vm->heap_ptr)
        return 0;
    c = (uint32_t)w;

    if (c >= vm->gc.find_blk && c < vm->gc.find_end) {   /* cached hit */
        *out = vm->gc.find_blk - FLINT_GC_HDR;
        return 1;
    }

    blk = vm->gc.heap_begin;
    while (blk < vm->heap_ptr) {
        uint32_t p, e, sz;
        if (!blk_ok(vm, blk, vm->heap_ptr)) { gc_corrupt(vm); return -1; }
        sz = blk_size(vm, blk);
        p  = blk_payload(blk);
        e  = p + sz;
        if (c >= p && c < e) {                  /* inside this payload */
            if (!(blk_flags(vm, blk) & FLINT_GC_F_ALLOC))
                return 0;                       /* a free block retains nothing */
            vm->gc.find_blk = p;
            vm->gc.find_end = e;
            *out = blk;
            return 1;
        }
        if (c < p) return 0;                    /* header or padding: not a ptr */
        blk = e;
    }
    return 0;
}

/* ---- marking ---- */

static void gc_mark_block(flint_vm *vm, uint32_t blk)
{
    uint32_t fl = blk_flags(vm, blk);
    if (fl & FLINT_GC_F_MARK) return;
    blk_set_flags(vm, blk, fl | FLINT_GC_F_MARK);
    if (vm->gc.msp < FLINT_GC_MARKSTACK)
        vm->gc.markstack[vm->gc.msp++] = blk;
    else
        vm->gc.oflow = 1;   /* marked, not queued: the rescan path covers it */
}

static void gc_mark_word(flint_vm *vm, flint_word v)
{
    uint32_t blk;
    if (vm->err) return;
    if (gc_lookup(vm, (uint64_t)v, &blk) == 1)
        gc_mark_block(vm, blk);
}

static void gc_scan_payload(flint_vm *vm, uint32_t blk)
{
    uint32_t p = blk_payload(blk), sz = blk_size(vm, blk), o;
    for (o = 0; o + 8 <= sz; o += 8)
        gc_mark_word(vm, word_at(vm, p + o));
}

static void gc_drain(flint_vm *vm)
{
    while (vm->gc.msp && !vm->err)
        gc_scan_payload(vm, vm->gc.markstack[--vm->gc.msp]);
}

static void gc_mark_roots(flint_vm *vm)
{
    uint32_t i, a;

    /* registers — all 16, including the ones the program never read */
    for (i = 0; i < FLINT_NREGS; i++) gc_mark_word(vm, vm->r[i]);

    /* active stack: [sp, mem_size), aligned words */
    for (a = vm->sp & ~(FLINT_GC_ALIGN - 1u); a + 8 <= vm->mem_size; a += 8)
        gc_mark_word(vm, word_at(vm, a));

    /* data / globals: [0, heap_start) */
    for (a = 0; a + 8 <= vm->heap_start; a += 8)
        gc_mark_word(vm, word_at(vm, a));

    /* host-registered root slots and pins */
    for (i = 0; i < vm->gc.nroots; i++)
        if (vm->gc.roots[i]) gc_mark_word(vm, *vm->gc.roots[i]);
    for (i = 0; i < vm->gc.npins; i++)
        gc_mark_word(vm, vm->gc.pins[i]);
}

/* ---- statistics ---- */

/* recompute bytes_live/bytes_free from the block sequence (anchors the
 * incremental bookkeeping after every collection) */
static void gc_recount(flint_vm *vm)
{
    uint32_t blk = vm->gc.heap_begin, live = 0, freeb = 0;
    while (blk < vm->heap_ptr) {
        uint32_t sz;
        if (!blk_ok(vm, blk, vm->heap_ptr)) { gc_corrupt(vm); return; }
        sz = blk_size(vm, blk);
        if (blk_flags(vm, blk) & FLINT_GC_F_ALLOC) live += sz;
        else                                       freeb += sz;
        blk = blk_payload(blk) + sz;
    }
    vm->gc.bytes_live = live;
    vm->gc.bytes_free = freeb;
}

/* ---- collection ---- */

void flint_gc_collect(flint_vm *vm)
{
    uint32_t blk;

    if (!vm->gc.enabled || vm->err) return;

    vm->gc.msp   = 0;
    vm->gc.oflow = 0;
    gc_cache_clear(vm);

    /* mark */
    gc_mark_roots(vm);
    gc_drain(vm);

    /* mark-stack overflow: rescan every marked block until a full pass
     * leaves the worklist empty and drains cleanly (correct, slower) */
    while (vm->gc.oflow && !vm->err) {
        vm->gc.oflow = 0;
        vm->gc.overflow_rescans++;
        blk = vm->gc.heap_begin;
        while (blk < vm->heap_ptr && !vm->err) {
            uint32_t sz, fl;
            if (!blk_ok(vm, blk, vm->heap_ptr)) { gc_corrupt(vm); break; }
            sz = blk_size(vm, blk);
            fl = blk_flags(vm, blk);
            if ((fl & FLINT_GC_F_ALLOC) && (fl & FLINT_GC_F_MARK))
                gc_scan_payload(vm, blk);
            blk = blk_payload(blk) + sz;
        }
        gc_drain(vm);
    }
    if (vm->err) return;

    /* sweep: unmarked allocated blocks become free; clear surviving marks */
    blk = vm->gc.heap_begin;
    while (blk < vm->heap_ptr) {
        uint32_t sz, fl;
        if (!blk_ok(vm, blk, vm->heap_ptr)) { gc_corrupt(vm); return; }
        sz = blk_size(vm, blk);
        fl = blk_flags(vm, blk);
        if (fl & FLINT_GC_F_ALLOC) {
            if (!(fl & FLINT_GC_F_MARK)) {
                blk_set_flags(vm, blk, fl & ~FLINT_GC_F_ALLOC);
                vm->gc.bytes_reclaimed += sz;
                if (vm->gc.stress) {
                    /* debug poison: use-after-free reads 0xDE... */
                    uint32_t o, p = blk_payload(blk);
                    for (o = 0; o < sz; o++) vm->mem[p + o] = 0xDE;
                }
            } else {
                blk_set_flags(vm, blk, fl & ~FLINT_GC_F_MARK);
            }
        }
        blk = blk_payload(blk) + sz;
    }

    /* coalesce: a run of adjacent free blocks becomes the first block */
    blk = vm->gc.heap_begin;
    while (blk < vm->heap_ptr) {
        uint32_t sz, fl, e;
        if (!blk_ok(vm, blk, vm->heap_ptr)) { gc_corrupt(vm); return; }
        sz = blk_size(vm, blk);
        fl = blk_flags(vm, blk);
        e  = blk_payload(blk) + sz;
        if (!(fl & FLINT_GC_F_ALLOC)) {
            while (e < vm->heap_ptr) {
                uint32_t nsz, nfl;
                if (!blk_ok(vm, e, vm->heap_ptr)) { gc_corrupt(vm); return; }
                nfl = blk_flags(vm, e);
                if (nfl & FLINT_GC_F_ALLOC) break;
                nsz = blk_size(vm, e);
                sz += FLINT_GC_HDR + nsz;
                u32_put(vm, blk, sz);
                e = blk_payload(blk) + sz;
            }
        }
        blk = e;
    }

    /* shrink the bump frontier while the last block is free */
    if (vm->heap_ptr > vm->gc.heap_begin) {
        uint32_t last = vm->gc.heap_begin, w = last;
        while (w < vm->heap_ptr) {
            uint32_t sz;
            if (!blk_ok(vm, w, vm->heap_ptr)) { gc_corrupt(vm); return; }
            last = w;
            sz = blk_size(vm, w);
            w = blk_payload(w) + sz;
        }
        if (!(blk_flags(vm, last) & FLINT_GC_F_ALLOC))
            vm->heap_ptr = last;
    }

    gc_recount(vm);
    if (vm->err) return;
    vm->gc.collections++;
    vm->gc.since = 0;
    gc_cache_clear(vm);
}

/* ---- allocation ---- */

/* one attempt: first-fit over the free blocks, then the bump frontier */
static flint_word gc_try_alloc(flint_vm *vm, uint32_t need)
{
    uint32_t blk = vm->gc.heap_begin;

    while (blk < vm->heap_ptr) {
        uint32_t sz, fl, p, e, used, o;

        if (!blk_ok(vm, blk, vm->heap_ptr)) { gc_corrupt(vm); return 0; }
        sz = blk_size(vm, blk);
        fl = blk_flags(vm, blk);
        p  = blk_payload(blk);
        e  = p + sz;

        if (!(fl & FLINT_GC_F_ALLOC) && sz >= need) {
            if (sz >= need + FLINT_GC_HDR + FLINT_GC_ALIGN) {
                /* split: carve `need` off the front, remainder stays free */
                uint32_t rem = sz - need - FLINT_GC_HDR;
                uint32_t nb  = p + need;
                u32_put(vm, blk, need);
                hdr_write(vm, nb, rem, 0);
                vm->gc.bytes_free -= need + FLINT_GC_HDR;
                vm->gc.bytes_live += need;
                used = need;
            } else {
                /* whole block (>= request is the contract) */
                vm->gc.bytes_free -= sz;
                vm->gc.bytes_live += sz;
                used = sz;
            }
            u32_put(vm, blk + 4, (fl & ~FLINT_GC_F_MARK) | FLINT_GC_F_ALLOC);
            for (o = 0; o < used; o++) vm->mem[p + o] = 0;
            gc_cache_clear(vm);
            return (flint_word)p;
        }
        blk = e;
    }

    /* bump a fresh block at the frontier if it fits below sp */
    if ((uint64_t)vm->heap_ptr + FLINT_GC_HDR + need <= (uint64_t)vm->sp) {
        uint32_t p = vm->heap_ptr + FLINT_GC_HDR, o;
        hdr_write(vm, vm->heap_ptr, need, FLINT_GC_F_ALLOC);
        for (o = 0; o < need; o++) vm->mem[p + o] = 0;
        vm->heap_ptr = p + need;
        vm->gc.bytes_live += need;
        gc_cache_clear(vm);
        return (flint_word)p;
    }
    return 0;
}

flint_word flint_gc_alloc(flint_vm *vm, uint32_t size)
{
    uint32_t need;
    int pass;

    if (vm->err) return 0;
    if (!vm->gc.enabled) { vm->err = FLINT_ERR_INIT; return 0; }
    if (size > vm->mem_size || size > 0xFFFFFFF0u) {
        vm->err = FLINT_ERR_OOM;
        return 0;
    }
    need = (size + (FLINT_GC_ALIGN - 1u)) & ~(FLINT_GC_ALIGN - 1u);
    if (need < FLINT_GC_ALIGN) need = FLINT_GC_ALIGN;

    /* proactive trigger: stress mode collects before every allocation;
     * otherwise a byte threshold between collections */
    if (vm->gc.stress ||
        (vm->gc.threshold && vm->gc.since >= vm->gc.threshold))
        flint_gc_collect(vm);
    if (vm->err) return 0;

    for (pass = 0; pass < 2; pass++) {
        flint_word p = gc_try_alloc(vm, need);
        if (p) {
            vm->gc.since += need;
            vm->gc.bytes_alloced += need;
            return p;
        }
        if (vm->err) return 0;
        if (pass == 0) flint_gc_collect(vm);   /* reclaim, then retry once */
        if (vm->err) return 0;
    }
    vm->err = FLINT_ERR_OOM;
    return 0;
}

void flint_gc_free(flint_vm *vm, flint_word payload)
{
    uint32_t blk;
    int r;

    if (vm->err) return;
    if (!vm->gc.enabled) { vm->err = FLINT_ERR_INIT; return; }

    r = gc_lookup(vm, (uint64_t)payload, &blk);
    if (r != 1 || blk_payload(blk) != (uint32_t)payload ||
        !(blk_flags(vm, blk) & FLINT_GC_F_ALLOC)) {
        /* invalid, interior or already-free pointer: explicit-free trap */
        vm->err = FLINT_ERR_BADMEM;
        return;
    }
    {
        uint32_t sz = blk_size(vm, blk);
        blk_set_flags(vm, blk, blk_flags(vm, blk) & ~FLINT_GC_F_ALLOC);
        vm->gc.bytes_live -= sz;
        vm->gc.bytes_free += sz;
    }
    gc_cache_clear(vm);
}

/* ---- initialization / configuration ---- */

void flint_gc_init(flint_vm *vm)
{
    uint32_t from = vm->heap_start;          /* zero the whole scratch area */
    uint64_t hb   = ((uint64_t)from + 15u) & ~(uint64_t)15u;
    unsigned char *z = (unsigned char *)&vm->gc;
    uint32_t i;

    if (hb > vm->mem_size) hb = vm->mem_size;

    for (i = from; i < vm->mem_size; i++) vm->mem[i] = 0;
    for (i = 0; i < sizeof vm->gc; i++) z[i] = 0;

    vm->gc.enabled     = 1;
    vm->gc.threshold   = FLINT_GC_DEFAULT_THRESHOLD;
    vm->gc.heap_begin  = (uint32_t)hb;
    vm->heap_start     = (uint32_t)hb;
    vm->heap_ptr       = (uint32_t)hb;
}

void flint_gc_get_stats(const flint_vm *vm, flint_gc_stats *out)
{
    if (!out) return;
    out->collections      = vm->gc.collections;
    out->bytes_live       = vm->gc.bytes_live;
    out->bytes_free       = vm->gc.bytes_free;
    out->bytes_alloced    = vm->gc.bytes_alloced;
    out->bytes_reclaimed  = vm->gc.bytes_reclaimed;
    out->threshold        = vm->gc.threshold;
    out->overflow_rescans = vm->gc.overflow_rescans;
    out->enabled          = vm->gc.enabled;
    out->stress           = vm->gc.stress;
}

void flint_gc_set_threshold(flint_vm *vm, uint32_t bytes)
{
    vm->gc.threshold = bytes;    /* 0: collect on allocation failure only */
}

void flint_gc_set_stress(flint_vm *vm, int on)
{
    vm->gc.stress = on ? 1 : 0;
}

int flint_gc_root(flint_vm *vm, flint_word *slot)
{
    if (!slot || vm->gc.nroots >= FLINT_GC_ROOTS_MAX) return -1;
    vm->gc.roots[vm->gc.nroots++] = slot;
    return 0;
}

int flint_gc_unroot(flint_vm *vm, flint_word *slot)
{
    uint32_t i, j;
    for (i = 0; i < vm->gc.nroots; i++)
        if (vm->gc.roots[i] == slot) {
            for (j = i + 1; j < vm->gc.nroots; j++)
                vm->gc.roots[j - 1] = vm->gc.roots[j];
            vm->gc.nroots--;
            return 0;
        }
    return -1;
}

int flint_gc_pin(flint_vm *vm, flint_word addr)
{
    if (vm->gc.npins >= FLINT_GC_PINS_MAX) return -1;
    vm->gc.pins[vm->gc.npins++] = addr;
    return 0;                    /* validated lazily at the mark phase */
}

int flint_gc_unpin(flint_vm *vm, flint_word addr)
{
    uint32_t i, j;
    for (i = 0; i < vm->gc.npins; i++)
        if (vm->gc.pins[i] == addr) {
            for (j = i + 1; j < vm->gc.npins; j++)
                vm->gc.pins[j - 1] = vm->gc.pins[j];
            vm->gc.npins--;
            return 0;
        }
    return -1;
}