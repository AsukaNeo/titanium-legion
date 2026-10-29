/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

#include "gen/recomp_types.h"   /* thread-local register model (replaces a plain extern) */
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
extern ptrdiff_t g_xbox_mem_offset;


/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[NULLCALL] ret=%08X arg0=%08X eax=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebx=%08X"
                    " esp=%08X\n", (unsigned)MEM32(g_esp), (unsigned)MEM32(g_esp+4), (unsigned)g_eax,
            (unsigned)g_ecx, (unsigned)g_edx, (unsigned)g_esi, (unsigned)g_edi, (unsigned)g_ebx, (unsigned)g_esp);
    { uint32_t o = g_esi - 16; int q; if (o < 0x08000000u) { fprintf(stderr, "[NULLOBJ] esi-16:");
      for (q = 0; q < 80; q++) fprintf(stderr, "%s%02X", (q%16==0)?"\n  ":" ", (unsigned)MEM8(o+q));
      fprintf(stderr, "\n"); } }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

int xbox_gdlock_insn(const char *text, uint32_t va);   /* gd1 */
void recomp_unimpl(const char *text, uint32_t va)
{
    if (xbox_gdlock_insn(text, va)) return;   /* gd1: cli/sti */
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* ── Titanium Legion overrides ─────────────────────────────── */

#include <windows.h>

/* Every round of a spin-wait loop (lift.sh --spin-wait). Bring-up only: logs
 * which loop is waiting and the registers it waits with, at rounds 1000,
 * 100000 and every 10 million after, so a stall names its loop. D3D's fence
 * fields are device+0x30 (the counter) and the word at *(device+0x34). */
static void tl_spin_wait_hook(uint32_t va)
{
    static __declspec(thread) uint64_t n;
    static __declspec(thread) uint32_t last_va;
    uint32_t dev, fptr;

    if (va != last_va) {
        last_va = va;
        n = 0;
    }
    n++;
    if (n != 1000 && n != 100000 && n % 10000000 != 0)
        return;
    dev = MEM32(0x0021AFE0u);
    fptr = dev ? MEM32(dev + 0x34) : 0;
    fprintf(stderr, "[spin] %08X round %llu tid %lu: eax %08X ecx %08X edx %08X"
                    " esi %08X edi %08X | D3D fence counter %08X word %08X\n",
            va, (unsigned long long)n, GetCurrentThreadId(), g_eax, g_ecx,
            g_edx, g_esi, g_edi, dev ? MEM32(dev + 0x30) : 0,
            fptr ? MEM32(fptr) : 0);
    fflush(stderr);
}

/* [esp] Stack balance probes. The main thread's esp crept upward until its
 * pushes landed on the TLS block above the stack (a mission crashed in the
 * CRT's rand() after ~25 minutes): something returns with esp higher than
 * it should. Each wrapped function records how far esp moves across the
 * call; a balanced one moves it the same amount every time, so the first
 * different delta names a function whose subtree leaks. Off unless
 * TL_ESP_PROBE=1. */
static void esp_probe(const char *name, uint32_t entry_esp, uint32_t exit_esp,
                      int32_t *first, int *seen, int *told)
{
    static int on = -1;
    int32_t d = (int32_t)(exit_esp - entry_esp);
    if (on < 0)
        on = getenv("TL_ESP_PROBE") != NULL;
    if (!on)
        return;
    if (!*seen) {
        *seen = 1;
        *first = d;
        fprintf(stderr, "[esp] %s: delta %+d (entry %08X)\n", name, d, entry_esp);
        return;
    }
    if (d != *first && *told < 20) {
        int k;
        (*told)++;
        fprintf(stderr, "[esp] %s: delta %+d, first was %+d (entry %08X, return %08X)\n",
                name, d, *first, entry_esp, MEM32(entry_esp));
        /* The indirect calls just made, oldest first: a leak behind a
         * function pointer is one of these. */
        fprintf(stderr, "[esp]   recent indirect calls:");
        for (k = 0; k < 16; k++)
            fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + k) & 15]);
        fprintf(stderr, "\n");
    }
}

/* The lifter finds a wrapper by its literal "extern void sub_X_gen(void);"
 * line (tools/recomp/manual_scan.py), so each is written out. */
/* the mission loop's frame (from sub_00045DF0) */
extern void sub_00045840_gen(void);
void sub_00045840(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00045840_gen();
    esp_probe("sub_00045840", e, g_esp, &first, &seen, &told);
}

/* called each frame; the crash came through it */
extern void sub_00043A20_gen(void);
void sub_00043A20(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00043A20_gen();
    esp_probe("sub_00043A20", e, g_esp, &first, &seen, &told);
}

/* main-monitor drawing; calls rand() */
extern void sub_00017630_gen(void);
void sub_00017630(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00017630_gen();
    esp_probe("sub_00017630", e, g_esp, &first, &seen, &told);
}

extern void sub_00017700_gen(void);
void sub_00017700(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00017700_gen();
    esp_probe("sub_00017700", e, g_esp, &first, &seen, &told);
}

/* waits for vblank each frame */
extern void sub_0002DB50_gen(void);
void sub_0002DB50(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0002DB50_gen();
    esp_probe("sub_0002DB50", e, g_esp, &first, &seen, &told);
}

extern void sub_00042870_gen(void);
void sub_00042870(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00042870_gen();
    esp_probe("sub_00042870", e, g_esp, &first, &seen, &told);
}

extern void sub_0020CE10_gen(void);
void sub_0020CE10(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0020CE10_gen();
    esp_probe("sub_0020CE10", e, g_esp, &first, &seen, &told);
}

extern void sub_000C5920_gen(void);
void sub_000C5920(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_000C5920_gen();
    esp_probe("sub_000C5920", e, g_esp, &first, &seen, &told);
}

/* Callees of sub_00045840, whose esp delta varies (+4, sometimes +20). */
extern void sub_00015390_gen(void);
void sub_00015390(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00015390_gen();
    esp_probe("sub_00015390", e, g_esp, &first, &seen, &told);
}

extern void sub_00015530_gen(void);
void sub_00015530(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00015530_gen();
    esp_probe("sub_00015530", e, g_esp, &first, &seen, &told);
}

extern void sub_00016C90_gen(void);
void sub_00016C90(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00016C90_gen();
    esp_probe("sub_00016C90", e, g_esp, &first, &seen, &told);
}

extern void sub_000173F0_gen(void);
void sub_000173F0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_000173F0_gen();
    esp_probe("sub_000173F0", e, g_esp, &first, &seen, &told);
}

extern void sub_0001A6E0_gen(void);
void sub_0001A6E0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0001A6E0_gen();
    esp_probe("sub_0001A6E0", e, g_esp, &first, &seen, &told);
}

extern void sub_000223B0_gen(void);
void sub_000223B0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_000223B0_gen();
    esp_probe("sub_000223B0", e, g_esp, &first, &seen, &told);
}

extern void sub_00022760_gen(void);
void sub_00022760(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00022760_gen();
    esp_probe("sub_00022760", e, g_esp, &first, &seen, &told);
}

extern void sub_00023250_gen(void);
void sub_00023250(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00023250_gen();
    esp_probe("sub_00023250", e, g_esp, &first, &seen, &told);
}

extern void sub_00023410_gen(void);
void sub_00023410(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00023410_gen();
    esp_probe("sub_00023410", e, g_esp, &first, &seen, &told);
}

extern void sub_00024AD0_gen(void);
void sub_00024AD0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00024AD0_gen();
    esp_probe("sub_00024AD0", e, g_esp, &first, &seen, &told);
}

extern void sub_00024D30_gen(void);
void sub_00024D30(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00024D30_gen();
    esp_probe("sub_00024D30", e, g_esp, &first, &seen, &told);
}

extern void sub_00029350_gen(void);
void sub_00029350(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00029350_gen();
    esp_probe("sub_00029350", e, g_esp, &first, &seen, &told);
}

extern void sub_00029520_gen(void);
void sub_00029520(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00029520_gen();
    esp_probe("sub_00029520", e, g_esp, &first, &seen, &told);
}

extern void sub_000296E0_gen(void);
void sub_000296E0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_000296E0_gen();
    esp_probe("sub_000296E0", e, g_esp, &first, &seen, &told);
}

extern void sub_0002ACD0_gen(void);
void sub_0002ACD0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0002ACD0_gen();
    esp_probe("sub_0002ACD0", e, g_esp, &first, &seen, &told);
}

extern void sub_0002AD30_gen(void);
void sub_0002AD30(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0002AD30_gen();
    esp_probe("sub_0002AD30", e, g_esp, &first, &seen, &told);
}

extern void sub_0002AF90_gen(void);
void sub_0002AF90(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0002AF90_gen();
    esp_probe("sub_0002AF90", e, g_esp, &first, &seen, &told);
}

extern void sub_0002B440_gen(void);
void sub_0002B440(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0002B440_gen();
    esp_probe("sub_0002B440", e, g_esp, &first, &seen, &told);
}

extern void sub_0002EE00_gen(void);
void sub_0002EE00(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0002EE00_gen();
    esp_probe("sub_0002EE00", e, g_esp, &first, &seen, &told);
}

extern void sub_0002F0E0_gen(void);
void sub_0002F0E0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0002F0E0_gen();
    esp_probe("sub_0002F0E0", e, g_esp, &first, &seen, &told);
}

extern void sub_00030020_gen(void);
void sub_00030020(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00030020_gen();
    esp_probe("sub_00030020", e, g_esp, &first, &seen, &told);
}

extern void sub_00030A90_gen(void);
void sub_00030A90(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00030A90_gen();
    esp_probe("sub_00030A90", e, g_esp, &first, &seen, &told);
}

extern void sub_00030EB0_gen(void);
void sub_00030EB0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00030EB0_gen();
    esp_probe("sub_00030EB0", e, g_esp, &first, &seen, &told);
}

extern void sub_00031270_gen(void);
void sub_00031270(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00031270_gen();
    esp_probe("sub_00031270", e, g_esp, &first, &seen, &told);
}

extern void sub_00031420_gen(void);
void sub_00031420(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00031420_gen();
    esp_probe("sub_00031420", e, g_esp, &first, &seen, &told);
}

extern void sub_00032B60_gen(void);
void sub_00032B60(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00032B60_gen();
    esp_probe("sub_00032B60", e, g_esp, &first, &seen, &told);
}

extern void sub_00037120_gen(void);
void sub_00037120(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00037120_gen();
    esp_probe("sub_00037120", e, g_esp, &first, &seen, &told);
}

extern void sub_000450C0_gen(void);
void sub_000450C0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_000450C0_gen();
    esp_probe("sub_000450C0", e, g_esp, &first, &seen, &told);
}

extern void sub_00046E50_gen(void);
void sub_00046E50(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00046E50_gen();
    esp_probe("sub_00046E50", e, g_esp, &first, &seen, &told);
}

extern void sub_0004D0A0_gen(void);
void sub_0004D0A0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0004D0A0_gen();
    esp_probe("sub_0004D0A0", e, g_esp, &first, &seen, &told);
}

extern void sub_0005C450_gen(void);
void sub_0005C450(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0005C450_gen();
    esp_probe("sub_0005C450", e, g_esp, &first, &seen, &told);
}

extern void sub_0005D240_gen(void);
void sub_0005D240(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0005D240_gen();
    esp_probe("sub_0005D240", e, g_esp, &first, &seen, &told);
}

extern void sub_0008A6B0_gen(void);
void sub_0008A6B0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0008A6B0_gen();
    esp_probe("sub_0008A6B0", e, g_esp, &first, &seen, &told);
}

extern void sub_0008A6D0_gen(void);
void sub_0008A6D0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0008A6D0_gen();
    esp_probe("sub_0008A6D0", e, g_esp, &first, &seen, &told);
}

extern void sub_0008E480_gen(void);
void sub_0008E480(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0008E480_gen();
    esp_probe("sub_0008E480", e, g_esp, &first, &seen, &told);
}

extern void sub_000938B0_gen(void);
void sub_000938B0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_000938B0_gen();
    esp_probe("sub_000938B0", e, g_esp, &first, &seen, &told);
}

extern void sub_000A96A0_gen(void);
void sub_000A96A0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_000A96A0_gen();
    esp_probe("sub_000A96A0", e, g_esp, &first, &seen, &told);
}

extern void sub_000AA580_gen(void);
void sub_000AA580(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_000AA580_gen();
    esp_probe("sub_000AA580", e, g_esp, &first, &seen, &told);
}

extern void sub_000AFDF0_gen(void);
void sub_000AFDF0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_000AFDF0_gen();
    esp_probe("sub_000AFDF0", e, g_esp, &first, &seen, &told);
}

extern void sub_0011E0E0_gen(void);
void sub_0011E0E0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0011E0E0_gen();
    esp_probe("sub_0011E0E0", e, g_esp, &first, &seen, &told);
}

extern void sub_00160FF0_gen(void);
void sub_00160FF0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00160FF0_gen();
    esp_probe("sub_00160FF0", e, g_esp, &first, &seen, &told);
}

extern void sub_001750E0_gen(void);
void sub_001750E0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_001750E0_gen();
    esp_probe("sub_001750E0", e, g_esp, &first, &seen, &told);
}

extern void sub_00177040_gen(void);
void sub_00177040(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00177040_gen();
    esp_probe("sub_00177040", e, g_esp, &first, &seen, &told);
}

extern void sub_00177790_gen(void);
void sub_00177790(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_00177790_gen();
    esp_probe("sub_00177790", e, g_esp, &first, &seen, &told);
}

extern void sub_0020C210_gen(void);
void sub_0020C210(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0020C210_gen();
    esp_probe("sub_0020C210", e, g_esp, &first, &seen, &told);
}

extern void sub_0020C5F0_gen(void);
void sub_0020C5F0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0020C5F0_gen();
    esp_probe("sub_0020C5F0", e, g_esp, &first, &seen, &told);
}

extern void sub_0020D680_gen(void);
void sub_0020D680(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0020D680_gen();
    esp_probe("sub_0020D680", e, g_esp, &first, &seen, &told);
}

extern void sub_0020E6C0_gen(void);
void sub_0020E6C0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0020E6C0_gen();
    esp_probe("sub_0020E6C0", e, g_esp, &first, &seen, &told);
}

extern void sub_0020EAB0_gen(void);
void sub_0020EAB0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0020EAB0_gen();
    esp_probe("sub_0020EAB0", e, g_esp, &first, &seen, &told);
}

extern void sub_0020EBF0_gen(void);
void sub_0020EBF0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0020EBF0_gen();
    esp_probe("sub_0020EBF0", e, g_esp, &first, &seen, &told);
}

extern void sub_0020ED70_gen(void);
void sub_0020ED70(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0020ED70_gen();
    esp_probe("sub_0020ED70", e, g_esp, &first, &seen, &told);
}

extern void sub_0020EDD0_gen(void);
void sub_0020EDD0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0020EDD0_gen();
    esp_probe("sub_0020EDD0", e, g_esp, &first, &seen, &told);
}

extern void sub_0020EDF0_gen(void);
void sub_0020EDF0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0020EDF0_gen();
    esp_probe("sub_0020EDF0", e, g_esp, &first, &seen, &told);
}

extern void sub_0020EE10_gen(void);
void sub_0020EE10(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_0020EE10_gen();
    esp_probe("sub_0020EE10", e, g_esp, &first, &seen, &told);
}

extern void sub_002116B0_gen(void);
void sub_002116B0(void)
{
    static int32_t first; static int seen, told;
    uint32_t e = g_esp;
    sub_002116B0_gen();
    esp_probe("sub_002116B0", e, g_esp, &first, &seen, &told);
}

/* Called from main.c just before the lifted entry point. */
void tl_manual_init(void)
{
    extern void (*g_xbox_spin_wait_hook)(uint32_t va);
    g_xbox_spin_wait_hook = tl_spin_wait_hook;
}
