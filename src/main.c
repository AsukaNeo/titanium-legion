/**
 * Titanium Legion: Steel Battalion (Xbox, 2002), recompiled - game entry
 * point.
 *
 * Hosts the game's code, lifted to C by xboxrecomp: loads default.xbe, maps
 * it at its Xbox addresses, brings up the kernel replacement, the APU, USB
 * and the NV2A executor, then calls the lifted entry point.
 *
 * The supported release (tools.xbe_parser):
 *   Title:       Steel Battalion, USA, version 1
 *   Title ID:    0x43430002
 *   XDK:         1.0.4627, D3D8 (its own section, not LTCG)
 *   Base addr:   0x00010000
 *   Entry point: 0x00192C47
 *   Sections:    78 (.text, D3D, XGRPH, DSOUND, BINK*, XPP, .rdata, .data,
 *                seg00-seg50 (mission data, loaded on demand), audio DSP, ...)
 *   Kernel imports: 123
 */

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>
#include "game_check.h"
#include "game_import.h"

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS uint32_t g_mxcsr;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#define YOUR_GAME_ENTRY_POINT   0x00192C47

/* Where the game's files (default.xbe and its media folder) are, found at
 * start-up rather than fixed at build time, so a build runs wherever it is
 * unpacked. The first of these that holds default.xbe wins:
 *   RECOMP_GAME_DIR          set by hand
 *   <exe folder>\game        a packaged build
 *   <exe folder>\..\..\game  build\Release\ in a checkout (build.bat)
 *   TL_FALLBACK_GAME_DIR     the toolkit's game_files, from CMake */
#ifndef TL_FALLBACK_GAME_DIR
#define TL_FALLBACK_GAME_DIR "..\\xboxrecomp\\game_files"
#endif
static char s_game_dir[MAX_PATH];
static char s_game_xbe[MAX_PATH];
#define YOUR_GAME_DIR           s_game_dir
#define YOUR_GAME_XBE_PATH      s_game_xbe

static int tl_try_game_dir(const char *dir)
{
    char xbe[MAX_PATH];
    if (!dir || !*dir)
        return 0;
    snprintf(xbe, sizeof xbe, "%s\\default.xbe", dir);
    if (GetFileAttributesA(xbe) == INVALID_FILE_ATTRIBUTES)
        return 0;
    if (!GetFullPathNameA(dir, MAX_PATH, s_game_dir, NULL))
        snprintf(s_game_dir, sizeof s_game_dir, "%s", dir);
    snprintf(s_game_xbe, sizeof s_game_xbe, "%s\\default.xbe", s_game_dir);
    return 1;
}

static int tl_find_game_dir(void)
{
    char exe[MAX_PATH], cand[MAX_PATH], *slash;
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    if ((slash = strrchr(exe, '\\')) != NULL)
        *slash = 0;
    if (tl_try_game_dir(getenv("RECOMP_GAME_DIR")))
        return 1;
    tl_game_import_remembered(exe, cand, sizeof cand);  /* a folder picked before */
    if (tl_try_game_dir(cand))
        return 1;
    snprintf(cand, sizeof cand, "%s\\game", exe);
    if (tl_try_game_dir(cand))
        return 1;
    snprintf(cand, sizeof cand, "%s\\..\\..\\game", exe);
    if (tl_try_game_dir(cand))
        return 1;
#ifdef TL_RELEASE
    return 0;                    /* only where a player keeps it */
#else
    return tl_try_game_dir(TL_FALLBACK_GAME_DIR);
#endif
}

/* The switches a playable run needs, which play.sh used to supply. Set
 * here so the exe works when started any other way; a variable already in
 * the environment wins, so play.sh, run.sh and RECOMP_X=0 still decide. */
static void tl_default_env(void)
{
    static const char *const defaults[][2] = {
        { "RECOMP_GDLOCK",          "1" },   /* ISR/DPC vs raised IRQL */
        /* One guest thread at a time, as on the console's single CPU: the
         * loading-screen thread runs below normal priority and must not draw
         * while the main thread waits in D3D's BlockOnFence. */
        { "RECOMP_GUEST_LOCK",      "1" },
        { "RECOMP_VBLANK",          "1" },   /* the intro waits on vblank */
        { "RECOMP_AC97_READY",      "1" },   /* APU: audio, voice stops */
        { "RECOMP_HEAP_RECLAIM",    "1" },
        { "RECOMP_USB",             "1" },
        { "RECOMP_USB_PADS",        "1" },
        { "RECOMP_USB_NDP",         "4" },   /* pad on the player 1 port */
        { "RECOMP_USB_PORT",        "2" },
        /* Player 1's controller is a Steel Battalion controller, which the
         * title insists on; sb_input.c plays it from a pad and the
         * keyboard. Not RECOMP_KEYBOARD: that would make the keyboard a
         * gamepad too. */
        { "RECOMP_USB_SB",          "0" },
        { "RECOMP_PB_EXEC",         "1" },   /* the NV2A executor draws */
        { "RECOMP_FB_WINDOW",       "1" },
        { "RECOMP_PRESENT_ON_CLEAR","1" },
        /* Reads on a handle opened without FILE_SYNCHRONOUS_IO return
         * STATUS_PENDING, as a DVD read always did. The loader reads
         * its first archive that way and waits on a flag only its pending branch
         * sets: answered synchronously, it waits forever (0x000C84C1). */
        { "RECOMP_ASYNC_IO",        "1" },
    };
    size_t i;
    for (i = 0; i < sizeof defaults / sizeof defaults[0]; i++)
        if (!getenv(defaults[i][0]))
            _putenv_s(defaults[i][0], defaults[i][1]);

    /* The renderer: the GPU back end unless [display] renderer=software.
     * Settled here, before anything asks, because the back end is chosen
     * once. The ini is the one the window reads (fb_present.c): RECOMP_INI,
     * then the working directory, then beside the exe. TL_PB_BACKEND_SET
     * marks a value set here rather than by you: a copy started by "Restart
     * now" inherits it and must read the ini again. */
    if (getenv("TL_PB_BACKEND_SET")) {
        _putenv_s("RECOMP_PB_BACKEND", "");
        _putenv_s("TL_PB_BACKEND_SET", "");
    }
    if (!getenv("RECOMP_PB_BACKEND")) {
        char ini[MAX_PATH], val[32], *slash;
        const char *v = getenv("RECOMP_INI");
        DWORD a;
        ini[0] = 0;
        if (v && *v)
            snprintf(ini, sizeof ini, "%s", v);
        else {
            if (GetCurrentDirectoryA(sizeof ini, ini))
                strncat(ini, "\\titaniumlegion.ini", sizeof ini - strlen(ini) - 1);
            a = GetFileAttributesA(ini);
            if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) {
                GetModuleFileNameA(NULL, ini, MAX_PATH);
                if ((slash = strrchr(ini, '\\')) != NULL)
                    *slash = 0;
                strncat(ini, "\\titaniumlegion.ini", sizeof ini - strlen(ini) - 1);
            }
        }
        GetPrivateProfileStringA("display", "renderer", "gpu", val, sizeof val, ini);
        _putenv_s("RECOMP_PB_BACKEND", _stricmp(val, "software") ? "d3d11" : "cpu");
        _putenv_s("TL_PB_BACKEND_SET", "1");

        /* [display] frame_rate: the game's frame rate, 60 (the console's)
         * or more, up to 240. It sets the guest vblank (RECOMP_VBLANK_HZ),
         * which D3D presents on, and recomp_manual.c lowers the game's
         * shortest frame step to match so it runs at the right speed. */
        /* frame_rate=display: the monitor's own refresh rate, with the
         * guest vblank locked to its real vertical blank
         * (RECOMP_VBLANK_SYNC), so each frame is shown for exactly one
         * refresh. A fixed rate the display does not divide judders. */
        if (!getenv("RECOMP_VBLANK_HZ")) {
            char fr[16];
            int fps;
            GetPrivateProfileStringA("display", "frame_rate", "60", fr, sizeof fr, ini);
            if (!_stricmp(fr, "display")) {
                DEVMODEA dm;
                memset(&dm, 0, sizeof dm);
                dm.dmSize = sizeof dm;
                fps = EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)
                      ? (int)dm.dmDisplayFrequency : 60;
                if (!getenv("RECOMP_VBLANK_SYNC"))
                    _putenv_s("RECOMP_VBLANK_SYNC", "1");
                fprintf(stderr, "[BOOT] frame rate: the display's, %d Hz\n", fps);
            } else {
                fps = atoi(fr);
            }
            if (fps >= 60 && fps <= 240 && fps != 60) {
                snprintf(val, sizeof val, "%d", fps);
                _putenv_s("RECOMP_VBLANK_HZ", val);
            }
        }
    }
}

/* The launcher: a title card with Play, Settings and Exit to Windows before
 * the game starts (xboxrecomp fb_present.c). On in a release build, off in a
 * developer build so scripted runs start straight away; RECOMP_LAUNCHER=1 or
 * 0 says otherwise. The renderer picked there replaces the ini's, unless you
 * set RECOMP_PB_BACKEND yourself. */
#ifndef TL_VERSION
#define TL_VERSION "dev"
#endif

/* Copy a folder's files and subfolders into another, leaving files already
 * there alone. Small trees only: the game's saves. */
static void tl_copy_tree(const char *from, const char *to)
{
    char pattern[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    CreateDirectoryA(to, NULL);
    snprintf(pattern, sizeof pattern, "%s\\*", from);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        snprintf(src, sizeof src, "%s\\%s", from, fd.cFileName);
        snprintf(dst, sizeof dst, "%s\\%s", to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            tl_copy_tree(src, dst);
        else
            CopyFileA(src, dst, TRUE);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int tl_has_files(const char *dir)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int any = 0;
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do
        any = strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..");
    while (!any && FindNextFileA(h, &fd));
    FindClose(h);
    return any;
}

/* Saves beside the game, never in a user-data folder: the game, the
 * settings and the saves move together. A "saves" folder next to the "game"
 * folder (beside the exe in a download, the checkout's root in a developer
 * build; beside the exe when the game was picked from elsewhere) holds the title's E:\UDATA and E:\TDATA (RECOMP_SAVE_DIR, which moves just
 * those); the Xbox's partition images (5 GB, sparse) and caches, which the
 * game rebuilds on its own, stay in old_dir on this machine. [game] saves in
 * titaniumlegion.ini names another folder (relative to the exe), and a
 * RECOMP_SAVE_DIR you set wins. Where the exe's folder can't be written
 * (Program Files), the saves stay in old_dir as before. Saves an earlier
 * version left in old_dir are copied over once. */
/* Mods: each folder in "mods" beside the game is laid over the game folder
 * (RECOMP_MODS, kernel_path.c), files there replacing the game's at the same
 * path -- mods\<name>\media\assets\audio\sellout.wma for that song.
 * Folders are taken in name order, the first holding a file winning; one
 * whose name starts with '_' is skipped, which turns a mod off. */
static void tl_mods(void)
{
    char exe_dir[MAX_PATH], base[MAX_PATH], pattern[MAX_PATH], list[2048], *slash;
    WIN32_FIND_DATAA fd;
    HANDLE h;

    if (getenv("RECOMP_MODS"))
        return;
    GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    if ((slash = strrchr(exe_dir, '\\')) != NULL)
        *slash = 0;
    snprintf(base, sizeof base, "%s", exe_dir);
    if (s_game_dir[0]) {
        char g[MAX_PATH];
        snprintf(g, sizeof g, "%s", s_game_dir);
        if ((slash = strrchr(g, '\\')) != NULL && !_stricmp(slash + 1, "game")) {
            *slash = 0;
            snprintf(base, sizeof base, "%s", g);
        }
    }
    snprintf(pattern, sizeof pattern, "%s\\mods\\*", base);
    list[0] = 0;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.'
                || fd.cFileName[0] == '_')
            continue;
        if (strlen(list) + strlen(base) + strlen(fd.cFileName) + 8 >= sizeof list)
            break;
        if (list[0])
            strcat(list, ";");
        strcat(list, base);
        strcat(list, "\\mods\\");
        strcat(list, fd.cFileName);
        fprintf(stderr, "[BOOT] mod: %s\n", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (list[0])
        _putenv_s("RECOMP_MODS", list);
}

static void tl_portable_saves(const char *old_dir)
{
    char exe_dir[MAX_PATH], ini[MAX_PATH], want[MAX_PATH], dir[MAX_PATH];
    char probe[MAX_PATH], from[MAX_PATH], to[MAX_PATH], *slash;
    static const char *const subs[] = { "UDATA", "TDATA" };
    char base[MAX_PATH];
    HANDLE f;
    size_t i;

    if (getenv("RECOMP_SAVE_DIR") && *getenv("RECOMP_SAVE_DIR"))
        return;
    GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    if ((slash = strrchr(exe_dir, '\\')) != NULL)
        *slash = 0;
    snprintf(ini, sizeof ini, "%s\\titaniumlegion.ini", exe_dir);
    /* The folder that holds "game", else the exe's. */
    snprintf(base, sizeof base, "%s", exe_dir);
    if (s_game_dir[0]) {
        char g[MAX_PATH];
        snprintf(g, sizeof g, "%s", s_game_dir);
        if ((slash = strrchr(g, '\\')) != NULL && !_stricmp(slash + 1, "game")) {
            *slash = 0;
            snprintf(base, sizeof base, "%s", g);
        }
    }
    GetPrivateProfileStringA("game", "saves", "saves", want, sizeof want, ini);
    if (want[0] == '\\' || (want[0] && want[1] == ':'))
        snprintf(dir, sizeof dir, "%s", want);
    else
        snprintf(dir, sizeof dir, "%s\\%s", base, want);

    /* Writable? Make it and drop a file in it. */
    CreateDirectoryA(dir, NULL);
    snprintf(probe, sizeof probe, "%s\\.write-test", dir);
    f = CreateFileA(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[BOOT] saves: %s is not writable; keeping them in %s\n",
                dir, old_dir ? old_dir : "the default folder");
        return;
    }
    CloseHandle(f);

    for (i = 0; old_dir && i < sizeof subs / sizeof subs[0]; i++) {
        snprintf(from, sizeof from, "%s\\%s", old_dir, subs[i]);
        snprintf(to, sizeof to, "%s\\%s", dir, subs[i]);
        if (tl_has_files(from) && !tl_has_files(to)) {
            tl_copy_tree(from, to);
            fprintf(stderr, "[BOOT] saves: copied %s to %s\n", from, to);
        }
    }
    _putenv_s("RECOMP_SAVE_DIR", dir);
    fprintf(stderr, "[BOOT] saves: %s\n", dir);
}

extern void xbox_LauncherRun(const char *version);
extern int xbox_FramebufferWindowGpu(void);

static void tl_launcher(void)
{
    const char *v = getenv("RECOMP_LAUNCHER");
#ifdef TL_RELEASE
    int on = !(v && !strcmp(v, "0"));
#else
    int on = v && strcmp(v, "0") != 0;
#endif
    if (!on)
        return;
    xbox_LauncherRun(TL_VERSION);
    if (getenv("TL_PB_BACKEND_SET"))
        _putenv_s("RECOMP_PB_BACKEND", xbox_FramebufferWindowGpu() ? "d3d11" : "cpu");
}

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* Trapped hardware registers (APU, USB OHCI) -> their models,
         * through the toolkit's dispatcher (v0.14). "Handled" means the
         * access was decoded and the instruction stepped over, so
         * execution resumes instead of unwinding. */
        if (xbox_VehMmioFault(ep))
            return EXCEPTION_CONTINUE_EXECUTION;

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        {                                               /* [cr3] for sym.py */
            uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
            fprintf(stderr, "  image base 0x%llX, rip rva 0x%llX\n",
                    (unsigned long long)base,
                    (unsigned long long)(ep->ContextRecord->Rip - base));
            fflush(stderr);
        }
        print_guest_context((void *)ep->ContextRecord->Rip);
        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging. The exe loads
         * wherever ASLR puts it, so test against the real image range and
         * print RVAs ("rip rva" so sym.py < log resolves them too). [cr3] */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
            IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base +
                                   ((IMAGE_DOS_HEADER *)base)->e_lfanew);
            uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
            int shown = 0;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 256 && shown < 8; i++) {
                if (sp[i] >= base && sp[i] < end) {
                    fprintf(stderr, "    [%d] rip rva 0x%llX\n", i,
                            (unsigned long long)(sp[i] - base));
                    shown++;
                }
            }
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── WinMain ───────────────────────────────────────────────── */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=== " TL_APP " ===\n");
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);

    /* Step 1: Load XBE */
    tl_default_env();
    {
        /* A downloaded build has no game yet. A disc image, default.xbe or
         * the game's folder dropped onto the exe is set up first; with no
         * game found, the player is asked for one (game_import.c). */
        char exe_dir[MAX_PATH], dir[MAX_PATH], arg[MAX_PATH], *slash;
        DWORD attr = INVALID_FILE_ATTRIBUTES;
        int found = 0;
        arg[0] = 0;
        if (__argc > 1) {
            snprintf(arg, sizeof arg, "%s", __argv[1]);
            attr = GetFileAttributesA(arg);
            fprintf(stderr, "[BOOT] opened with %s%s\n", arg,
                    attr == INVALID_FILE_ATTRIBUTES ? " (not found)" : "");
        }
        /* A folder: its default.xbe, used in place like a chosen one. */
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            strncat(arg, "\\default.xbe", sizeof arg - strlen(arg) - 1);
            attr = GetFileAttributesA(arg);
        }
        GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
        if ((slash = strrchr(exe_dir, '\\')) != NULL)
            *slash = 0;
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)
            && tl_game_import(exe_dir, arg, dir, sizeof dir))
            found = tl_try_game_dir(dir);
        if (!found && !tl_find_game_dir()
            && tl_game_import(exe_dir, NULL, dir, sizeof dir))
            tl_try_game_dir(dir);
    }
    if (!s_game_dir[0]) {
        MessageBoxA(NULL, "Could not find the game files.\n\n"
                    TL_APP " needs your own copy of Steel Battalion (original "
                    "Xbox, USA release). Put its files (default.xbe and "
                    "its media folder) in a folder named 'game' next to this program, "
                    "or set RECOMP_GAME_DIR.",
                    TL_APP, MB_ICONERROR);
        return 1;
    }
    fprintf(stderr, "[BOOT] game files: %s\n", s_game_dir);
    if (!load_xbe(YOUR_GAME_XBE_PATH, &xbe_data, &xbe_size)) {
        MessageBoxA(NULL, "Failed to load default.xbe from the game folder.",
                    TL_APP, MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);
    {
        char found[41], msg[768];
        if (!tl_game_check(xbe_data, xbe_size, found)) {
            snprintf(msg, sizeof msg,
                     "This is not the supported Steel Battalion release.\n\n"
                     TL_APP " runs only on your own copy of Steel Battalion "
                     "for the original Xbox, USA release (title "
                     "43430002, version 1). No game files are included with it.\n\n"
                     "Found:    %s\nExpected: %s\n\nGame folder: %s",
                     found[0] ? found : "(could not be read)", TL_XBE_SHA1, s_game_dir);
            MessageBoxA(NULL, msg, TL_APP, MB_ICONERROR);
            free(xbe_data);
            return 1;
        }
    }
    tl_launcher();

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        const char *save_dir = NULL;
        /* The Xbox's partition images and caches (5 GB sparse, rebuilt by
         * the game on its own) under the game's own name, not the toolkit's
         * shared %LOCALAPPDATA%\xboxrecomp; the saves themselves beside the
         * game (tl_portable_saves), in every build. */
        static char cache_root[MAX_PATH];
        const char *local = getenv("LOCALAPPDATA");
        if (local && *local) {
            snprintf(cache_root, sizeof cache_root, "%s\\" TL_APP, local);
            save_dir = cache_root;
        }
        tl_portable_saves(save_dir);
        tl_mods();
        xbox_path_init(YOUR_GAME_DIR, save_dir);
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();
	{
        extern void xbox_OhciInit(void);
        xbox_OhciInit();   /* no-op unless RECOMP_USB is set */
    }

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* D3D's BlockOnFence (0x00211E11) spins on a dword the GPU is meant to
     * write: device = MEM32(0x0021AFE0); mirror MEM32(device+0x30) into
     * *(device+0x34), the XDK 4627 layout Aggressive Inline uses too. */
    xbox_Nv2aMirrorFence(0x0021AFE0, 0x30, 0x34);

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();
    {
        extern void tl_manual_init(void);   /* recomp_manual.c */
        extern void tl_sb_input_init(void); /* sb_input.c */
        tl_manual_init();
        tl_sb_input_init();
    }

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}
