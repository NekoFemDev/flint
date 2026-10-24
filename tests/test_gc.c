/*
 * test_gc.c — automatic-memory bench: conservative mark-sweep collector.
 *
 * Host-level API tests (alloc/free/stats/roots/pins/overflow) run against
 * the collector directly; program-level tests assemble small FLINT
 * sources and run them with mem_mode = auto. Acceptance targets from
 * TODOS.md: escaping objects survive frame exit, unreachable cycles are
 * reclaimed, allocation loops run in bounded memory, stress mode keeps
 * output correct, and manual mode stays untouched.
 */
#include <stdio.h>
#include <string.h>
#include "flint.h"

static int fails = 0;
#define CHECK(cond, msg) \
    do { if (cond) printf("PASS  %s\n", (msg)); \
         else     { printf("FAIL  %s\n", (msg)); fails++; } } while (0)

static flint_byte dummy_code[] = { OP_HLT };
static flint_byte mem[65536];

static uint32_t m32(const flint_byte *m, uint32_t a)
{
    return (uint32_t)m[a] | ((uint32_t)m[a + 1] << 8) |
           ((uint32_t)m[a + 2] << 16) | ((uint32_t)m[a + 3] << 24);
}

static void put_word(uint32_t at, flint_word v)
{
    int i;
    for (i = 0; i < 8; i++) mem[at + i] = (flint_byte)((uint64_t)v >> (8 * i));
}

/* fresh auto-mode VM over the shared image, zeroed for determinism */
static void vm_auto(flint_vm *vm, uint32_t heap_start, uint32_t memsz)
{
    uint32_t i;
    for (i = 0; i < memsz; i++) mem[i] = 0;
    flint_init(vm, dummy_code, sizeof dummy_code, mem, memsz);
    flint_heap_set(vm, heap_start, FLINT_MEM_AUTO);
}

static flint_word alloc(flint_vm *vm, uint32_t n)
{
    return flint_gc_alloc(vm, n);
}

static int gc_live(const flint_vm *vm)
{
    flint_gc_stats gs;
    flint_gc_get_stats(vm, &gs);
    return (int)gs.bytes_live;
}

/* ---- header / zeroing ---- */

static void t_layout(void)
{
    flint_vm vm;
    flint_word a, b;
    uint32_t i;

    vm_auto(&vm, 64, 4096);

    /* stale bytes where the first two blocks will land */
    for (i = 80; i < 160; i++) mem[i] = 0xAA;

    a = alloc(&vm, 24);                 /* block 64..104, payload 80..104 */
    b = alloc(&vm, 40);                 /* block 104..160, payload 120..160 */

    CHECK(vm.err == FLINT_OK, "alloc: clean run");
    CHECK(a == 64 + 16, "payload sits right after the 16-byte header");
    CHECK(b == 104 + 16, "second block follows the first");
    CHECK(vm.heap_ptr == 160, "frontier advanced by header+payload");
    CHECK(m32(mem, 64) == 24, "header: payload size recorded");
    CHECK(m32(mem, 68) == FLINT_GC_F_ALLOC, "header: allocated flag");
    CHECK(m32(mem, 76) == FLINT_GC_MAGIC, "header: magic written");

    for (i = 0; i < 24; i++)
        if (mem[a + i] != 0) break;
    CHECK(i == 24, "payload zeroed on allocation");
    for (i = 0; i < 40; i++)
        if (mem[b + i] != 0) break;
    CHECK(i == 40, "second payload zeroed");
}

/* ---- stats / explicit free / shrink ---- */

static void t_stats(void)
{
    flint_vm vm;
    flint_word a1, a2;
    flint_gc_stats gs;
    uint32_t b;

    vm_auto(&vm, 64, 4096);

    a1 = alloc(&vm, 24);
    a2 = alloc(&vm, 40);
    flint_gc_get_stats(&vm, &gs);
    CHECK(gs.bytes_live == 64 && gs.bytes_alloced == 64,
          "stats: live and lifetime bytes after two allocs");

    vm.r[1] = a1;                       /* make them reachable to the VM */
    vm.r[2] = a2;
    flint_gc_collect(&vm);
    flint_gc_get_stats(&vm, &gs);
    CHECK(gs.collections == 1 && gs.bytes_live == 64 && gs.bytes_free == 0,
          "stats: collection keeps rooted blocks live=64");

    vm.r[1] = 0;
    vm.r[2] = 0;
    flint_gc_free(&vm, a1);             /* explicit release */
    flint_gc_free(&vm, a2);
    flint_gc_get_stats(&vm, &gs);
    CHECK(vm.err == FLINT_OK && gs.bytes_live == 0 && gs.bytes_free == 64,
          "stats: explicit frees drop live, raise free");

    flint_gc_collect(&vm);              /* coalesce + shrink the tail */
    flint_gc_get_stats(&vm, &gs);
    b = vm.heap_ptr;
    CHECK(gs.collections == 2 && gs.bytes_free == 0 && b == 64,
          "stats: free tail coalesced and shrunk back to the base");

    a1 = alloc(&vm, 24);                /* now collect it as garbage */
    flint_gc_get_stats(&vm, &gs);
    CHECK(gs.bytes_alloced == 88, "stats: lifetime alloc counter");
    flint_gc_collect(&vm);
    flint_gc_get_stats(&vm, &gs);
    CHECK(gs.collections == 3 && gs.bytes_reclaimed == 24 &&
          gs.bytes_live == 0 && vm.heap_ptr == 64,
          "stats: swept garbage counted and reclaimed");
    (void)a1;
}

/* ---- first-fit reuse + block splitting ---- */

static void t_reuse_split(void)
{
    flint_vm vm;
    flint_word a0, a1, b, c;
    flint_gc_stats gs;

    vm_auto(&vm, 64, 4096);

    a0 = alloc(&vm, 100);               /* need 104: block 64..184  */
    a1 = alloc(&vm, 100);               /* block 184..304           */
    CHECK(a0 == 80 && a1 == 200 && vm.err == FLINT_OK, "setup blocks (100+100)");

    flint_gc_free(&vm, a0);
    b = alloc(&vm, 50);                 /* need 56: splits the 104-byte hole */
    CHECK(b == 80, "freed block reused first-fit (same address)");
    CHECK(m32(mem, 64) == 56, "reused block split down to the request");
    CHECK(m32(mem, 136) == 32 && m32(mem, 136 + 4) == 0 &&
          m32(mem, 136 + 12) == FLINT_GC_MAGIC,
          "split remainder is a valid free block (size 32)");

    c = alloc(&vm, 30);                 /* need 32: takes the remainder whole */
    CHECK(c == 152, "remainder block reused whole when a split is too small");
    CHECK(m32(mem, 136) == 32, "remainder keeps its size");

    flint_gc_get_stats(&vm, &gs);
    CHECK(gs.bytes_live == 104 + 56 + 32, "live sums a1 + reused + remainder");

    flint_gc_free(&vm, c);
    vm.r[1] = a1;                       /* root the survivors */
    vm.r[2] = b;
    flint_gc_collect(&vm);
    flint_gc_get_stats(&vm, &gs);
    CHECK(gs.bytes_free == 32 && gs.bytes_live == 104 + 56,
          "explicitly freed remainder available after collect");
}

/* ---- coalescing adjacent free blocks ---- */

static void t_coalesce(void)
{
    flint_vm vm;
    flint_word a, b, c, d, e;
    flint_gc_stats gs;

    vm_auto(&vm, 64, 4096);

    a = alloc(&vm, 32);                 /* 64..112   payload 80     */
    b = alloc(&vm, 32);                 /* 112..160  payload 128    */
    c = alloc(&vm, 32);                 /* 160..208  payload 176    */
    d = alloc(&vm, 32);                 /* 208..256  payload 224    */
    CHECK(a == 80 && b == 128 && c == 176 && d == 224 && vm.err == FLINT_OK,
          "setup blocks a..d");

    flint_gc_free(&vm, b);
    flint_gc_free(&vm, c);
    vm.r[1] = a;                        /* root the outer blocks */
    vm.r[2] = d;
    flint_gc_collect(&vm);              /* b+c merge into one 80-byte hole */
    flint_gc_get_stats(&vm, &gs);
    CHECK(gs.bytes_free == 80 && gs.bytes_live == 64 && vm.heap_ptr == 256,
          "adjacent free blocks coalesced (32+16+32=80)");

    e = alloc(&vm, 64);                 /* fits the merged hole as a whole */
    CHECK(e == 128, "merged hole serves an allocation that neither piece could");
    flint_gc_get_stats(&vm, &gs);
    CHECK(gs.bytes_free == 0 && gs.bytes_live == 64 + 80,
          "merged hole handed out; no sliver left");
}

/* ---- OOM after a collection ---- */

static void t_oom(void)
{
    flint_vm vm;
    flint_word a, c;
    flint_gc_stats gs;

    vm_auto(&vm, 64, 512);              /* tiny heap: 448 usable bytes */

    a = alloc(&vm, 400);                /* fits (bump 64..480) */
    CHECK(a == 80 && vm.err == FLINT_OK, "tiny heap: 400-byte block fits");

    (void)alloc(&vm, 440);              /* only fits after freeing `a`... no */
    CHECK(vm.err == FLINT_ERR_OOM, "impossible allocation traps OOM");

    flint_gc_get_stats(&vm, &gs);
    CHECK(gs.collections >= 1, "OOM path ran a collection before giving up");

    vm.err = FLINT_OK;                  /* host-style recovery for the bench */
    c = alloc(&vm, 64);
    CHECK(c == 80 && vm.err == FLINT_OK,
          "collection reclaimed the dropped 400-byte block for reuse");
}

/* ---- explicit-free traps ---- */

static void t_free_traps(void)
{
    flint_vm vm;
    flint_word a;

    vm_auto(&vm, 64, 4096);
    a = alloc(&vm, 32);

    flint_gc_free(&vm, a + 3);
    CHECK(vm.err == FLINT_ERR_BADMEM, "interior pointer rejected by explicit free");
    vm.err = FLINT_OK;

    flint_gc_free(&vm, a);
    CHECK(vm.err == FLINT_OK && gc_live(&vm) == 0, "exact payload pointer frees");

    flint_gc_free(&vm, a);
    CHECK(vm.err == FLINT_ERR_BADMEM, "double free traps");
    vm.err = FLINT_OK;

    flint_gc_free(&vm, 0);
    CHECK(vm.err == FLINT_ERR_BADMEM, "free(0) traps");
    vm.err = FLINT_OK;

    flint_gc_free(&vm, -8);
    CHECK(vm.err == FLINT_ERR_BADMEM, "negative pointer traps");
}

/* ---- roots: registers, stack, data ---- */

static void t_roots_reg(void)
{
    flint_vm vm;
    flint_word a;

    vm_auto(&vm, 64, 4096);
    a = alloc(&vm, 32);

    vm.r[5] = a;
    flint_gc_collect(&vm);
    CHECK(gc_live(&vm) == 32, "block reachable only from a register survives");

    vm.r[5] = 0;
    flint_gc_collect(&vm);
    CHECK(gc_live(&vm) == 0, "dropping the register frees the block");
}

static void t_roots_stack(void)
{
    flint_vm vm;
    flint_word a;

    vm_auto(&vm, 64, 4096);
    a = alloc(&vm, 32);

    vm.sp = vm.mem_size - 16;
    put_word(vm.mem_size - 16, a);
    flint_gc_collect(&vm);
    CHECK(gc_live(&vm) == 32, "block reachable only from the stack survives");

    put_word(vm.mem_size - 16, 0);
    flint_gc_collect(&vm);
    CHECK(gc_live(&vm) == 0, "clearing the stack slot frees the block");
}

static void t_roots_data(void)
{
    flint_vm vm;
    flint_word a;

    vm_auto(&vm, 64, 4096);
    a = alloc(&vm, 32);

    put_word(0, a);
    flint_gc_collect(&vm);
    CHECK(gc_live(&vm) == 32, "block reachable only from data survives");

    put_word(0, 0);
    flint_gc_collect(&vm);
    CHECK(gc_live(&vm) == 0, "clearing the data word frees the block");
}

static void t_roots_host(void)
{
    flint_vm vm;
    flint_word a, b, slot;

    vm_auto(&vm, 64, 4096);
    a = alloc(&vm, 32);

    slot = a;
    CHECK(flint_gc_root(&vm, &slot) == 0, "host root slot registers");
    flint_gc_collect(&vm);
    CHECK(gc_live(&vm) == 32, "block reachable only from a host root survives");
    slot = 0;
    flint_gc_collect(&vm);
    CHECK(gc_live(&vm) == 0, "cleared host root stops retaining");
    CHECK(flint_gc_unroot(&vm, &slot) == 0, "host root unregisters");
    CHECK(flint_gc_unroot(&vm, &slot) == -1, "double unroot reports -1");

    b = alloc(&vm, 32);
    CHECK(flint_gc_pin(&vm, b) == 0, "pin registers");
    flint_gc_collect(&vm);
    CHECK(gc_live(&vm) == 32, "pinned block survives a collection");
    CHECK(flint_gc_unpin(&vm, b) == 0, "pin unregisters");
    flint_gc_collect(&vm);
    CHECK(gc_live(&vm) == 0, "unpinned block is reclaimed");
    CHECK(flint_gc_unpin(&vm, b) == -1, "double unpin reports -1");
}

/* ---- heap-object graphs and cycles ---- */

static void t_graph_cycle(void)
{
    flint_vm vm;
    flint_word A, B, C;

    vm_auto(&vm, 64, 4096);

    A = alloc(&vm, 32);
    B = alloc(&vm, 32);
    C = alloc(&vm, 32);
    put_word(A, B);                     /* A -> B -> C */
    put_word(B, C);
    vm.r[1] = A;
    flint_gc_collect(&vm);
    CHECK(gc_live(&vm) == 96, "chain reachable through one root stays live");

    vm.r[1] = 0;
    flint_gc_collect(&vm);
    CHECK(gc_live(&vm) == 0, "whole chain reclaimed when the root drops");

    A = alloc(&vm, 32);
    B = alloc(&vm, 32);
    put_word(A, B);                     /* A <-> B cycle, no root */
    put_word(B, A);
    flint_gc_collect(&vm);
    CHECK(gc_live(&vm) == 0, "unreachable cycle is collected");
}

/* ---- interior pointers (conservative containment) ---- */

static void t_interior(void)
{
    flint_vm vm;
    flint_word a, b;

    vm_auto(&vm, 64, 4096);

    a = alloc(&vm, 80);
    vm.r[5] = a + 41;                   /* mid-payload, unaligned */
    flint_gc_collect(&vm);
    CHECK(gc_live(&vm) == 80, "interior pointer marks the containing block");

    vm.r[5] = a + 80;                   /* one past the payload end */
    flint_gc_collect(&vm);
    CHECK(gc_live(&vm) == 0, "one-past-the-end does not retain");

    b = alloc(&vm, 80);
    vm.r[5] = b - 16;                   /* the block header itself */
    flint_gc_collect(&vm);
    CHECK(gc_live(&vm) == 0, "header address does not retain");

    b = alloc(&vm, 80);                 /* reallocated after the sweep */
    vm.r[5] = b;                        /* exact payload start */
    flint_gc_collect(&vm);
    CHECK(gc_live(&vm) == 80, "exact payload start retains");
}

/* ---- mark-stack overflow slow path ---- */

static void t_markstack_overflow(void)
{
    flint_vm vm;
    flint_gc_stats gs;
    uint32_t i;

    vm_auto(&vm, 16384, 65536);

    for (i = 0; i < 1200; i++) {
        flint_word p = alloc(&vm, 24);
        if (!p) break;
        put_word(i * 8, p);             /* 1200 distinct root words */
    }
    CHECK(vm.err == FLINT_OK && i == 1200,
          "1200 root-referenced blocks allocated");

    flint_gc_collect(&vm);
    flint_gc_get_stats(&vm, &gs);
    CHECK(gs.bytes_live == 1200u * 24u,
          "every root block retained through the overflow path");
    CHECK(gs.overflow_rescans >= 1,
          "mark-stack overflow exercised the full-rescan slow path");

    for (i = 0; i < 1200; i++) put_word(i * 8, 0);
    flint_gc_collect(&vm);
    flint_gc_get_stats(&vm, &gs);
    CHECK(gs.bytes_live == 0, "dropping all roots reclaims all blocks");
}

/* ---- bounded memory over a long allocation loop ---- */

static void t_no_unbounded(void)
{
    flint_vm vm;
    flint_gc_stats gs;
    uint32_t i;

    vm_auto(&vm, 64, 8192);
    flint_gc_set_threshold(&vm, 2048);

    for (i = 0; i < 2000; i++) (void)alloc(&vm, 16);
    CHECK(vm.err == FLINT_OK, "2000 allocations without free: clean run");

    flint_gc_get_stats(&vm, &gs);
    CHECK(gs.bytes_alloced == 2000u * 16u, "lifetime counter tracks 2000 allocs");
    CHECK(gs.bytes_reclaimed > 0, "collector reclaimed blocks along the way");
    CHECK(gs.bytes_live <= 2048u + 32u && vm.heap_ptr <= 8192u,
          "live bytes stay bounded by the threshold window");
}

/* ---- program-level: escaping object ---- */

static const char *S_escape =
"code:\n"
"main:\n"
"    call helper\n"
"    alloc r1, 32\n"                   /* collect here: r0 must survive */
"    load r2, [r0]\n"
"    copy r0, r2\n"
"    exit\n"
"helper:\n"
"    enter 16\n"
"    alloc r0, 32\n"
"    set r2, 1234\n"
"    store [r0], r2\n"
"    leave\n"                          /* frame gone, block must escape */
"    return\n";

/* assemble and run one program; 1 = assembled, *rc filled */
static int run_auto(const char *src, uint32_t threshold,
                    flint_vm *vm, flint_word *rc)
{
    flint_prog p; char err[256] = "";

    if (flint_assemble(src, &p, 4096, err, sizeof err) != 0) {
        printf("FAIL  assemble: %s\n", err[0] ? err : "?");
        fails++;
        return 0;
    }
    flint_init(vm, p.code, p.code_size, p.mem, p.mem_size);
    flint_heap_set(vm, p.heap_start, FLINT_MEM_AUTO);
    if (threshold) flint_gc_set_threshold(vm, threshold);
    *rc = flint_run(vm, p.entry);
    flint_prog_free(&p);
    return 1;
}

static void t_escape_program(void)
{
    flint_vm vm; flint_word rc;
    flint_gc_stats gs;

    if (!run_auto(S_escape, 1, &vm, &rc)) return;
    CHECK(vm.err == FLINT_OK && rc == 1234,
          "object allocated in a callee survives leave + collection");
    flint_gc_get_stats(&vm, &gs);
    CHECK(gs.bytes_live == 64,
          "escaped block plus the later allocation stay live (64)");
}

/* ---- program-level: stack must not cross into the managed heap ---- */

static const char *S_stack_guard =
"code:\n"
"main:\n"
"    alloc r1, 3504\n"                /* heap_ptr = 3520, sp = 4096 */
"    set r2, 0\n"
"    set r3, 100\n"                   /* 100 pushes cross the frontier */
"loop:\n"
"    push r2\n"
"    inc r2\n"
"    jump.lt r2, r3, loop\n"
"    exit\n";

static void t_stack_guard_program(void)
{
    flint_vm vm; flint_word rc;
    flint_gc_stats gs;

    if (!run_auto(S_stack_guard, 0, &vm, &rc)) return;
    CHECK(vm.err == FLINT_ERR_STACK,
          "auto: push traps at heap_ptr instead of corrupting a header");
    flint_gc_get_stats(&vm, &gs);
    CHECK(gs.bytes_live == 3504 && vm.heap_ptr == 3520,
          "auto: stack guard leaves the managed heap intact");
    CHECK(m32(vm.mem, 12) == FLINT_GC_MAGIC,
          "auto: managed block header magic is still intact");
}

/* ---- program-level: cycle + `gc` opcode ---- */

static const char *S_gccycle =
"code:\n"
"main:\n"
"    alloc r1, 32\n"
"    alloc r2, 32\n"
"    store [r1], r2\n"                 /* r1 -> r2 -> r1 cycle */
"    store [r2], r1\n"
"    set r1, 0\n"
"    set r2, 0\n"
"    gc\n"                             /* collect now */
"    alloc r3, 32\n"                   /* must reuse the freed base */
"    copy r0, r3\n"
"    exit\n";

static void t_gc_program(void)
{
    flint_vm vm; flint_word rc;
    flint_gc_stats gs;

    if (!run_auto(S_gccycle, 0, &vm, &rc)) return;
    CHECK(vm.err == FLINT_OK, "cycle program runs clean");
    CHECK(rc == 16, "post-gc alloc reused the freed base (payload at 16)");

    flint_gc_get_stats(&vm, &gs);
    CHECK(gs.collections == 1 && gs.bytes_reclaimed == 64 &&
          gs.bytes_live == 32,
          "gc opcode: cycle reclaimed (64), one new block live (32)");
}

/* ---- program-level: stress mode keeps results correct ---- */

static const char *S_stress =
"code:\n"
"main:\n"
"    set r10, 50\n"
"loop:\n"
"    alloc r4, 64\n"
"    dec r10\n"
"    jump.ne r10, 0, loop\n"
"    set r0, 7\n"
"    exit\n";

static void t_stress_program(void)
{
    flint_prog p; char err[256] = "";
    flint_vm vm; flint_word rc;
    flint_gc_stats gs;

    if (flint_assemble(S_stress, &p, 4096, err, sizeof err) != 0) {
        printf("FAIL  stress assemble: %s\n", err); fails++; return;
    }
    flint_init(&vm, p.code, p.code_size, p.mem, p.mem_size);
    flint_heap_set(&vm, p.heap_start, FLINT_MEM_AUTO);
    flint_gc_set_stress(&vm, 1);
    rc = flint_run(&vm, p.entry);
    flint_prog_free(&p);

    CHECK(vm.err == FLINT_OK && rc == 7, "stress mode: loop output unchanged");
    flint_gc_get_stats(&vm, &gs);
    CHECK(gs.collections >= 50 && gs.stress == 1,
          "stress mode collected before every allocation");
    CHECK(gs.bytes_live <= 128, "stress loop stays in bounded live memory");
}

/* ---- manual mode is untouched ---- */

static const char *S_manual_gc =
"data:\n"
"pad: bytes 64\n"
"code:\n"
"main:\n"
"    alloc r0, 16\n"
"    gc\n"                             /* no-op in manual */
"    exit\n";

static void t_manual(void)
{
    flint_prog p; char err[256] = "";
    flint_vm vm; flint_word rc, hs;
    flint_gc_stats gs;

    memset(mem, 0, 4096);
    flint_init(&vm, dummy_code, sizeof dummy_code, mem, 4096);
    flint_heap_set(&vm, 64, FLINT_MEM_MANUAL);

    CHECK(flint_gc_alloc(&vm, 16) == 0 && vm.err == FLINT_ERR_INIT,
          "gc_alloc in manual mode is a misuse (INIT)");
    vm.err = FLINT_OK;
    flint_gc_free(&vm, 64);
    CHECK(vm.err == FLINT_ERR_INIT, "gc_free in manual mode is a misuse (INIT)");
    vm.err = FLINT_OK;

    flint_gc_collect(&vm);
    flint_gc_get_stats(&vm, &gs);
    CHECK(gs.enabled == 0 && gs.collections == 0,
          "gc_collect in manual mode is a no-op");

    /* the gc opcode is a no-op too: bump alloc still lands at heap_start */
    if (flint_assemble(S_manual_gc, &p, 4096, err, sizeof err) != 0) {
        printf("FAIL  manual assemble: %s\n", err); fails++; return;
    }
    flint_init(&vm, p.code, p.code_size, p.mem, p.mem_size);
    flint_heap_set(&vm, p.heap_start, FLINT_MEM_MANUAL);
    hs = (flint_word)p.heap_start;
    rc = flint_run(&vm, p.entry);
    flint_prog_free(&p);
    CHECK(vm.err == FLINT_OK && rc == hs,
          "manual mode: alloc still bumps, gc is a no-op");
}

int main(void)
{
    printf("flint gc bench %s\n", FLINT_VERSION);
    t_layout();
    t_stats();
    t_reuse_split();
    t_coalesce();
    t_oom();
    t_free_traps();
    t_roots_reg();
    t_roots_stack();
    t_roots_data();
    t_roots_host();
    t_graph_cycle();
    t_interior();
    t_markstack_overflow();
    t_no_unbounded();
    t_escape_program();
    t_stack_guard_program();
    t_gc_program();
    t_stress_program();
    t_manual();
    printf("fails: %d\n", fails);
    return fails ? 1 : 0;
}