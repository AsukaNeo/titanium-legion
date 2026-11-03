/*
 * The Steel Battalion controller, played on a pad and a keyboard.
 *
 * The toolkit plugs a Steel Battalion controller into the first port
 * (RECOMP_USB_SB=0) and asks this file for its 26-byte report on every poll
 * (g_xbox_sb_input_hook). The report layout is in docs/controller.md; the
 * button numbers are the controller's bit order, 0-38 from byte 2.
 *
 * The pad does what you do in a fight: aim, turn, pedals, weapons, gears.
 * The left stick's Y and the arrow keys are the sight-change stick, which
 * is also what moves the game's menus.
 * Holding RB turns the right stick into the sight-change stick and the face
 * buttons into chaff, extinguisher, night scope and override. The keyboard
 * does the rest, the panel you work through before a sortie: hatch,
 * ignition, start, the five toggle switches, comms, the tuner and eject.
 *
 * Start is also "the next start-up step": it works whichever control the
 * game is blinking for (guide_update), so the start-up can be done from the
 * pad alone, and held at the diagnostics bars it presses when they are all
 * past the marker. Tab or RB+Back shows the controls (sb_help.c), F9 or
 * RB+Start the pilot view, F8 turns the prompts on or off until the next run.
 *
 * That is the classic layout. The simple one, Easy Pilot (the default; on the
 * title card, in the F10 settings, F7 or TL_CONTROLS=classic for the other),
 * changes the pad once the engine runs:
 * the left stick walks, with the gears following its travel, and pulled back
 * brakes and then reverses (drive); RT and LT are Main Weapon and Lock On, A
 * is Fire. Easy Pilot brings the prompts with it, and off takes them away.
 * Menus and the start-up keep the classic layout, since the game's
 * menus want the sight-change stick and Main Weapon.
 *
 * Keys come from the game's window (xbox_FramebufferKeyDown), so they only
 * count while it has the focus. The pad is read with XInput directly, not
 * through the toolkit's pad layer, which would fold the keyboard in as a
 * gamepad on top.
 *
 * TL_SB_SCRIPT drives it without anyone at it, for scripted runs:
 * "<ms>:<control>[+<control>...][:<hold ms>],...", with times from the first
 * report, buttons as bit numbers and the levers by name (see
 * script_token): TL_SB_SCRIPT=20000:0 presses Main Weapon 20 s in, and
 * TL_SB_SCRIPT=@file reads the same from a file.
 */
#include <windows.h>
#include <xinput.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sb_input.h"

extern void (*g_xbox_sb_input_hook)(int pad, uint8_t *report);
extern void (*g_xbox_sb_lamps_hook)(int pad, const uint8_t *lamps, int n);
extern int xbox_FramebufferKeyDown(int vk);

/* ---- what the controller is doing ------------------------------------- */

/* The controls that stay where you leave them: the gear lever (-1 R, 0 N,
 * 1-5), the tuner dial and the five toggle switches. */
static int s_gear;
static int s_tuner;
static int s_toggle[5];

static void set_bit(uint8_t *report, int bit)
{
    report[2 + bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static void put16(uint8_t *report, int at, unsigned v)
{
    report[at] = (uint8_t)(v & 0xFF);
    report[at + 1] = (uint8_t)(v >> 8);
}

/* A press, not a hold: true on the poll the control goes down. */
static int edge(int id, int down)
{
    static uint8_t was[64];
    int pressed = down && !was[id];
    was[id] = (uint8_t)(down != 0);
    return pressed;
}

static int key(int vk)
{
    return xbox_FramebufferKeyDown(vk);
}

/* A stick axis with a dead zone, as -127..127. */
static int stick(SHORT v)
{
    int x = v / 256;
    if (x > -31 && x < 31)
        return 0;
    return x < -127 ? -127 : x;
}

/* ---- scripted presses -------------------------------------------------- */

/* A step holds buttons and may also push levers: AXP AXN AYP AYN (aiming
 * lever), ROTP ROTN (rotation lever), SXP SXN SYP SYN (sight change) to
 * full travel (P the positive end), DRP DRN the simple layout's left stick,
 * RT LT its triggers, PL PM PR (left, middle, right pedal) to the floor. GR GN G1-G5
 * put the gear lever there and T<n> turns the tuner to n; both stay. */
enum { AX_AIM_X, AX_AIM_Y, AX_ROT, AX_SIGHT_X, AX_SIGHT_Y, AX_DRIVE, AX_COUNT };

typedef struct {
    uint64_t bits;
    int8_t axis[AX_COUNT];
    uint8_t pedals;                 /* 1 left, 2 middle, 4 right */
    uint8_t step, view, card;       /* STEP VIEW CARD: Start, F9, RB+Back */
    uint8_t rt, lt;                 /* RT LT: the triggers, full */
} ScriptOut;

#define SCRIPT_MAX 256
static struct {
    DWORD at, hold;
    ScriptOut out;
    int gear, tuner;                /* 99: leave alone */
    int announced;
} s_script[SCRIPT_MAX];
static int s_script_len = -1;

static int script_token(const char *p, size_t n, int k)
{
    static const struct { const char *name; int axis, dir; } ax[] = {
        {"AXP", AX_AIM_X, 1}, {"AXN", AX_AIM_X, -1}, {"AYP", AX_AIM_Y, 1},
        {"AYN", AX_AIM_Y, -1}, {"ROTP", AX_ROT, 1}, {"ROTN", AX_ROT, -1},
        {"SXP", AX_SIGHT_X, 1}, {"SXN", AX_SIGHT_X, -1},
        {"SYP", AX_SIGHT_Y, 1}, {"SYN", AX_SIGHT_Y, -1},
        {"DRP", AX_DRIVE, 1}, {"DRN", AX_DRIVE, -1},
    };
    size_t i;
    if (n == 4 && !_strnicmp(p, "STEP", 4)) { s_script[k].out.step = 1; return 1; }
    if (n == 4 && !_strnicmp(p, "VIEW", 4)) { s_script[k].out.view = 1; return 1; }
    if (n == 4 && !_strnicmp(p, "CARD", 4)) { s_script[k].out.card = 1; return 1; }
    if (n == 2 && !_strnicmp(p, "RT", 2)) { s_script[k].out.rt = 1; return 1; }
    if (n == 2 && !_strnicmp(p, "LT", 2)) { s_script[k].out.lt = 1; return 1; }
    if (n && p[0] >= '0' && p[0] <= '9') {
        unsigned long b = strtoul(p, NULL, 10);
        if (b < 39)
            s_script[k].out.bits |= 1ull << b;
        return b < 39;
    }
    for (i = 0; i < sizeof ax / sizeof ax[0]; i++)
        if (strlen(ax[i].name) == n && !_strnicmp(ax[i].name, p, n)) {
            s_script[k].out.axis[ax[i].axis] = (int8_t)(127 * ax[i].dir);
            return 1;
        }
    if (n == 2 && (p[0] == 'P' || p[0] == 'p')) {
        const char *w = strchr("LMR", p[1] & ~0x20);
        if (w) {
            s_script[k].out.pedals |= (uint8_t)(1u << (w - "LMR"));
            return 1;
        }
    }
    if (n == 2 && (p[0] == 'G' || p[0] == 'g')) {
        char c = (char)(p[1] & ~0x20);
        if (c == 'R') { s_script[k].gear = -1; return 1; }
        if (c == 'N') { s_script[k].gear = 0; return 1; }
        if (p[1] >= '1' && p[1] <= '5') { s_script[k].gear = p[1] - '0'; return 1; }
    }
    if (n >= 2 && (p[0] == 'T' || p[0] == 't')) {
        s_script[k].tuner = atoi(p + 1) & 15;
        return 1;
    }
    return 0;
}

/* Append the steps in `p` ("<ms>:<controls>[:<hold>],..."). */
static void script_parse(const char *p)
{
    while (p && *p && s_script_len < SCRIPT_MAX) {
        char *end;
        int k = s_script_len;
        DWORD at;
        while (*p == ',' || *p == ' ' || *p == '\r' || *p == '\n')
            p++;
        if (!*p)
            break;
        at = strtoul(p, &end, 0);
        if (end == p || *end != ':')
            break;
        memset(&s_script[k], 0, sizeof s_script[k]);
        s_script[k].at = at;
        s_script[k].hold = 150;
        s_script[k].gear = s_script[k].tuner = 99;
        p = end;
        do {
            size_t n;
            p++;
            n = strcspn(p, "+:, \r\n");
            if (!script_token(p, n, k))
                fprintf(stderr, "  [SB] script: unknown control \"%.*s\"\n", (int)n, p);
            p += n;
        } while (*p == '+');
        if (*p == ':') {
            s_script[k].hold = strtoul(p + 1, &end, 0);
            p = end;
        }
        s_script_len++;
    }
}

static void script_load(void)
{
    const char *p = getenv("TL_SB_SCRIPT");
    char buf[8192];
    s_script_len = 0;
    if (p && p[0] == '@') {                     /* @file: the same, from a file */
        FILE *f = fopen(p + 1, "rb");
        size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        if (f)
            fclose(f);
        buf[n] = 0;
        p = buf;
    }
    script_parse(p);
    if (s_script_len)
        fprintf(stderr, "  [SB] script of %d step(s)\n", s_script_len);
}

/* TL_SB_LIVE=<file>: each line appended to the file while the game runs --
 * "<controls>[:<hold>]", no time -- is pressed as soon as it is read, one
 * line after another with 200 ms between. Look at a screenshot, append a
 * press, look again. */
static void script_live(DWORD t)
{
    static const char *path;
    static int checked;
    static long consumed;
    static DWORD last_poll, next_free;
    static char pending[256];
    static size_t npending;
    FILE *f;
    int c;

    if (!checked) {
        checked = 1;
        path = getenv("TL_SB_LIVE");
        if (path && *path)
            fprintf(stderr, "  [SB] live input from %s\n", path);
    }
    if (!path || !*path || (last_poll && t - last_poll < 50))
        return;
    last_poll = t;
    if (!(f = fopen(path, "rb")))
        return;
    if (fseek(f, consumed, SEEK_SET) == 0) {
        while ((c = fgetc(f)) != EOF) {
            consumed++;
            if (c != '\n' && c != '\r') {
                if (npending < sizeof pending - 1)
                    pending[npending++] = (char)c;
                continue;
            }
            if (!npending)
                continue;
            {
                char line[300];
                DWORD at = t > next_free ? t : next_free;
                int i, n = 0;
                pending[npending] = 0;
                npending = 0;
                /* "CLEAR": let go of everything held, toggles included. */
                if (!_stricmp(pending, "CLEAR")) {
                    s_script_len = 0;
                    next_free = t;
                    fprintf(stderr, "  [SB] live CLEAR at t=%lu ms\n", (unsigned long)t);
                    continue;
                }
                /* Make room: finished steps go. */
                for (i = 0; i < s_script_len; i++)
                    if (t < s_script[i].at + s_script[i].hold)
                        s_script[n++] = s_script[i];
                s_script_len = n;
                snprintf(line, sizeof line, "%lu:%s", (unsigned long)at, pending);
                script_parse(line);
                /* The next line waits for this one's press, but a long hold
                 * is a latch (a toggle switch left on): it holds up the
                 * queue for a second, not for its whole length. */
                if (s_script_len > n) {
                    DWORD hold = s_script[s_script_len - 1].hold;
                    next_free = at + (hold < 1000 ? hold : 1000) + 200;
                }
                fprintf(stderr, "  [SB] live \"%s\" at t=%lu ms\n", pending,
                        (unsigned long)at);
            }
        }
    }
    fclose(f);
}

static void script_apply(ScriptOut *o)
{
    static DWORD t0;
    DWORD t;
    int i, a;

    memset(o, 0, sizeof *o);
    if (s_script_len < 0)
        script_load();
    if (!t0)
        t0 = GetTickCount();
    t = GetTickCount() - t0;
    script_live(t);
    for (i = 0; i < s_script_len; i++) {
        if (t < s_script[i].at || t >= s_script[i].at + s_script[i].hold)
            continue;
        if (!s_script[i].announced) {
            s_script[i].announced = 1;
            fprintf(stderr, "  [SB] t=%lu ms step %d: buttons %llX\n",
                    (unsigned long)t, i, (unsigned long long)s_script[i].out.bits);
            if (s_script[i].gear != 99)
                s_gear = s_script[i].gear;
            if (s_script[i].tuner != 99)
                s_tuner = s_script[i].tuner;
        }
        o->bits |= s_script[i].out.bits;
        o->pedals |= s_script[i].out.pedals;
        o->step |= s_script[i].out.step;
        o->view |= s_script[i].out.view;
        o->card |= s_script[i].out.card;
        o->rt |= s_script[i].out.rt;
        o->lt |= s_script[i].out.lt;
        for (a = 0; a < AX_COUNT; a++)
            if (s_script[i].out.axis[a])
                o->axis[a] = s_script[i].out.axis[a];
    }
}

/* ---- the start-up guide ------------------------------------------------- */

/* Guest addresses: the mode flag (0 in a mission, not in menus), the
 * diagnostics bars, their marker. */
#define G_MODE   0x3B9400
#define G_BARS   0x55D8DC
#define G_MARKER 0x55D984

/* The part of the 640x480 frame that is the main monitor's window, for the
 * pilot view. */
#define VIEW_X 105.0f
#define VIEW_Y 59.0f
#define VIEW_W 430.0f
#define VIEW_H 242.0f

extern void xbox_FramebufferSetCrop(float x, float y, float w, float h);

static TlSbGuide s_guide = { TL_STEP_NONE, 0, -1, 0, 1, 0 };
static DWORD s_lamp_seen[40];       /* when each lamp last changed brightness */
static uint8_t s_lamp_last[40];
static enum {
    S_OUTSIDE, S_HATCH, S_IGNITION, S_SWITCHES, S_BOOTING, S_START,
    S_ENGINE, S_RUNNING
} s_stage;
static int s_accelerated, s_engine_fired, s_engine_tried, s_start_pressed;
static unsigned s_flipped;          /* switches turned on during the checklist */
static int s_auto_flip;             /* Start flipping them: next one, or 0 */
static DWORD s_auto_at;

const TlSbGuide *tl_sb_guide(void)
{
    return &s_guide;
}

static const uint8_t *guest(void)
{
    extern ptrdiff_t g_xbox_mem_offset;
    return (const uint8_t *)g_xbox_mem_offset;
}

/* Blinking now: it changed in the last second and is still lit. */
static int lamp_asking(int lamp, DWORD now)
{
    return s_lamp_seen[lamp] && now - s_lamp_seen[lamp] < 1000 && tl_sb_lamp(lamp);
}

static float bars_min(float *marker)
{
    const uint8_t *m = guest();
    float b[5], low;
    int i;
    memcpy(b, m + G_BARS, sizeof b);
    memcpy(marker, m + G_MARKER, sizeof *marker);
    low = b[0];
    for (i = 1; i < 5; i++)
        if (b[i] < low)
            low = b[i];
    return low;
}

/* The start-up only goes forwards: the lamps run across the whole panel while
 * the systems boot, so a lamp alone at some moment means nothing unless it is
 * the one due next. A menu starts it over. */
static void guide_update(DWORD now)
{
    static DWORD ask_until;
    static int ask_held = -1;
    float marker, low;
    int i, asking = 0, only = -1;

    for (i = 0; i < 40; i++) {
        int v = tl_sb_lamp(i);
        if (v != s_lamp_last[i]) {
            s_lamp_last[i] = (uint8_t)v;
            s_lamp_seen[i] = now;
        }
    }
    /* The gear lamps (30 up) only say where the lever is. */
    for (i = 0; i < 30; i++)
        if (lamp_asking(i, now)) {
            asking++;
            only = i;
        }

    if (*(const uint32_t *)(guest() + G_MODE) != 0) {
        s_stage = S_OUTSIDE;
        s_accelerated = s_engine_tried = s_start_pressed = 0;
        s_flipped = 0;
        s_auto_flip = 0;
        ask_held = -1;
    }
    low = bars_min(&marker);

    switch (s_stage) {
    case S_OUTSIDE:
        if (*(const uint32_t *)(guest() + G_MODE) == 0 && asking == 1 && only == 1)
            s_stage = S_HATCH;
        break;
    case S_HATCH:
        if (asking == 1 && only == 2)
            s_stage = S_IGNITION;
        break;
    case S_IGNITION:
        if (now - s_lamp_seen[2] > 1500 && !tl_sb_lamp(2)) {
            s_stage = S_SWITCHES;
            s_flipped = 0;
        }
        break;
    case S_SWITCHES:
        if (asking)                     /* the lamps start to run */
            s_stage = S_BOOTING;
        break;
    case S_BOOTING:
        if (asking == 1 && only == 3)
            s_stage = S_START;
        break;
    case S_START:
        /* The Start lamp blinks on through the diagnostics, so the bars are
         * what says the first press is over. */
        if (s_start_pressed && marker > 0.3f && low < 0.99f)
            s_stage = S_ENGINE;
        break;
    case S_ENGINE:
        if (s_engine_tried && low >= 0.99f)
            s_stage = S_RUNNING;
        break;
    default:
        break;
    }

    s_guide.step = s_stage == S_HATCH ? TL_STEP_HATCH
                 : s_stage == S_IGNITION ? TL_STEP_IGNITION
                 : s_stage == S_SWITCHES ? TL_STEP_SWITCHES
                 : s_stage == S_START ? TL_STEP_START
                 : s_stage == S_ENGINE ? TL_STEP_ENGINE
                 : s_stage == S_RUNNING && !s_accelerated
                     ? (s_gear < 1 && s_guide.classic ? TL_STEP_SHIFT : TL_STEP_ACCELERATE)
                 : TL_STEP_NONE;

    /* In a fight, one or two lamps blinking is the game asking; the whole
     * panel flashing is an alarm. A lamp fades out between blinks, so the
     * prompt stays up a moment. */
    if (s_stage == S_RUNNING && s_accelerated && asking >= 1 && asking <= 2
        && (only == 0 || only > 3)) {
        ask_held = only;
        ask_until = now + 1500;
    } else if (now > ask_until) {
        ask_held = -1;
    }
    s_guide.ask_lamp = ask_held;
    {
        int n = 0;
        for (i = 0; i < 5; i++)
            n += (s_flipped >> i) & 1;
        s_guide.switches = n;
        {
            static int shown = -1, shown_stage = -1;
            if (s_guide.step != shown || s_stage != shown_stage) {
                shown = s_guide.step;
                shown_stage = s_stage;
                fprintf(stderr, "  [SB] guide stage %d step %d\n", s_stage, shown);
            }
        }
    }
}

/* Start: the control the game is waiting for, or Start itself. Returns the
 * button to press this poll, or -1. */
static int step_button(int start_down, DWORD now)
{
    static int held;
    int pressed = start_down && !held;
    float marker;

    held = start_down;
    switch (s_guide.step) {
    case TL_STEP_HATCH:
        return start_down ? SB_HATCH : -1;
    case TL_STEP_IGNITION:
        return start_down ? SB_IGNITION : -1;
    case TL_STEP_SWITCHES:
        if (pressed && !s_auto_flip) {
            s_auto_flip = 1;
            s_auto_at = now;
        }
        return -1;
    case TL_STEP_ENGINE:
        /* Neutral, and only when all five are past the marker. */
        if (!start_down) {
            s_engine_fired = 0;
            return -1;
        }
        s_gear = 0;
        s_engine_tried = 1;
        if (!s_engine_fired && bars_min(&marker) > marker + 0.03f)
            s_engine_fired = 1;
        return s_engine_fired ? SB_START : -1;
    default:
        s_engine_fired = 0;
        if (start_down && s_guide.step == TL_STEP_START)
            s_start_pressed = 1;
        return start_down ? SB_START : -1;
    }
}

/* Start's run through the switches: an already-on one goes off first, since
 * only flipping one on during the checklist counts. */
static void auto_flip(DWORD now)
{
    int k;
    if (!s_auto_flip || now - s_auto_at < 250)
        return;
    s_auto_at = now;
    k = s_auto_flip - 1;
    if (s_guide.step != TL_STEP_SWITCHES || k >= 5) {
        s_auto_flip = 0;
        return;
    }
    if (s_flipped & (1u << k)) {
        s_auto_flip++;
    } else if (s_toggle[k]) {
        s_toggle[k] = 0;
    } else {
        s_toggle[k] = 1;
        s_flipped |= 1u << k;
        s_auto_flip++;
    }
}

/* ---- the simple layout's left stick ------------------------------------- */

#define GEAR_STEP_MS 800            /* a gear at a time, as a hand would */
#define REVERSE_MS   1200           /* pulled back this long: stopped, so reverse */

/* The layout in force on the pad right now. */
static int simple_live(void)
{
    return !s_guide.classic && s_stage == S_RUNNING;
}

/* The game's menus, not a mission: its mode flag is 0 only in a mission. */
static int in_menu(void)
{
    return *(const uint32_t *)(guest() + G_MODE) != 0;
}

/* `y` is the stick, -127..127: sets the gear and the pedals. */
static void drive(int y, unsigned *middle, unsigned *right, DWORD now)
{
    static DWORD gear_at, back_since;
    int mag = y < 0 ? -y : y;
    /* stick() leaves nothing under 31, so travel starts there. */
    unsigned travel = mag > 31 ? (unsigned)(mag - 31) * 65535u / 96u : 0;
    int want;

    if (y >= 0)
        back_since = 0;
    else if (!back_since)
        back_since = now;

    if (y < 0 && now - back_since >= REVERSE_MS) {
        s_gear = -1;
        *right = travel;
        return;
    }
    if (y < 0) {
        *middle = travel;               /* the brake, until it has stopped */
        return;
    }
    if (s_gear < 0)
        s_gear = 1;
    if (y == 0 && s_gear == 0)
        return;                         /* still in neutral from the start-up */
    want = travel < 23000 ? 1 : travel < 36000 ? 2 : travel < 46000 ? 3
         : travel < 56000 ? 4 : 5;
    if (s_gear < 1)
        s_gear = 1;
    if (s_gear != want && now - gear_at >= GEAR_STEP_MS) {
        s_gear += s_gear < want ? 1 : -1;
        gear_at = now;
    }
    /* Most of the pedal by half way: the gear is what sets the pace. */
    travel *= 2;
    *right = travel > 65535u ? 65535u : travel;
}

/* The saved layout: [controls] scheme in the ini beside the exe. */
static void ini_path(char *path, size_t n)
{
    const char *v = getenv("RECOMP_INI");
    char *slash;
    if (v && *v) {
        snprintf(path, n, "%s", v);
        return;
    }
    GetModuleFileNameA(NULL, path, (DWORD)n);
    slash = strrchr(path, '\\');
    if (slash)
        snprintf(slash + 1, n - (size_t)(slash + 1 - path), "titaniumlegion.ini");
}

static void scheme_load(void)
{
    const char *v = getenv("TL_CONTROLS");
    char path[MAX_PATH], val[16];
    if (v && *v) {
        s_guide.classic = !_stricmp(v, "classic");
        s_guide.hints = !s_guide.classic;
        return;
    }
    ini_path(path, sizeof path);
    GetPrivateProfileStringA("controls", "scheme", "simple", val, sizeof val, path);
    s_guide.classic = !_stricmp(val, "classic");
    s_guide.hints = !s_guide.classic;
}

static void scheme_save(void)
{
    char path[MAX_PATH];
    ini_path(path, sizeof path);
    WritePrivateProfileStringA("controls", "scheme", s_guide.classic ? "classic" : "simple", path);
}

static int easy_get(void)
{
    return !s_guide.classic;
}

static void easy_set(int on)
{
    s_guide.classic = !on;
    s_guide.hints = on;
    scheme_save();
}

/* Before the launcher, which shows the choice. */
void tl_sb_options_init(void)
{
    extern void xbox_LauncherSetOption(const char *label, int (*get)(void), void (*set)(int on));
    scheme_load();
    xbox_LauncherSetOption("Easy Pilot", easy_get, easy_set);
}

/* ---- the report --------------------------------------------------------- */

/* Pad buttons with and without RB held. */
static const struct { WORD button; int bit, shifted, simple; } s_pad_map[] = {
    { XINPUT_GAMEPAD_A,              SB_MAIN_WEAPON,  SB_CHAFF,        SB_FIRE },
    { XINPUT_GAMEPAD_B,              SB_FIRE,         SB_EXTINGUISHER, SB_WEAPON_SUB },
    { XINPUT_GAMEPAD_X,              SB_LOCK_ON,      SB_NIGHT_SCOPE,  SB_WEAPON_MAIN },
    { XINPUT_GAMEPAD_Y,              SB_MAGAZINE,     SB_OVERRIDE,     SB_MAGAZINE },
    { XINPUT_GAMEPAD_DPAD_UP,        -1,              -1,              SB_MAP_ZOOM },
    { XINPUT_GAMEPAD_DPAD_DOWN,      -1,              -1,              SB_SUB_MONITOR },
    { XINPUT_GAMEPAD_DPAD_LEFT,      SB_ZOOM_IN,      SB_ZOOM_IN,      SB_ZOOM_IN },
    { XINPUT_GAMEPAD_DPAD_RIGHT,     SB_ZOOM_OUT,     SB_ZOOM_OUT,     SB_ZOOM_OUT },
    { XINPUT_GAMEPAD_BACK,           SB_MULTIMON_OPEN, -1,             SB_MULTIMON_OPEN },
    { XINPUT_GAMEPAD_LEFT_THUMB,     SB_WASHING,      SB_WASHING,      SB_WASHING },
    { XINPUT_GAMEPAD_RIGHT_THUMB,    SB_SIGHT_CHANGE, SB_SIGHT_CHANGE, SB_SIGHT_CHANGE },
};

/* Keys held for a button. Eject is Shift+Backspace, below. */
static const struct { int vk, bit; } s_key_map[] = {
    { VK_SPACE, SB_MAIN_WEAPON },
    { 'H', SB_HATCH }, { 'I', SB_IGNITION },
    { 'M', SB_MULTIMON_OPEN }, { 'N', SB_MAP_ZOOM }, { 'B', SB_MODE_SELECT },
    { 'V', SB_SUB_MONITOR }, { 'F', SB_FSS }, { 'G', SB_MANIPULATOR },
    { 'C', SB_LINE_COLOR }, { 'W', SB_WASHING }, { 'X', SB_EXTINGUISHER },
    { 'Z', SB_CHAFF }, { 'T', SB_TANK_DETACH }, { 'O', SB_OVERRIDE },
    { 'L', SB_NIGHT_SCOPE },
    { VK_F1, SB_F1 }, { VK_F2, SB_F2 }, { VK_F3, SB_F3 },
    { 'Q', SB_WEAPON_MAIN }, { 'E', SB_WEAPON_SUB }, { 'R', SB_MAGAZINE },
    { '1', SB_COMM1 }, { '2', SB_COMM2 }, { '3', SB_COMM3 },
    { '4', SB_COMM4 }, { '5', SB_COMM5 },
};

static void sb_fill(int pad, uint8_t *report)
{
    XINPUT_STATE xs;
    const XINPUT_GAMEPAD *g = NULL;
    int shift = 0, aim_x = 0, aim_y = 0, rot = 0, sight_x = 0, sight_y = 0;
    unsigned left = 0, middle = 0, right = 0;
    ScriptOut sc;
    size_t i;
    int b, menu;

    if (pad != 0)
        return;
    menu = in_menu();
    memset(&xs, 0, sizeof xs);
    if (XInputGetState(0, &xs) == ERROR_SUCCESS)
        g = &xs.Gamepad;

    if (g) {
        shift = (g->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
        for (i = 0; i < sizeof s_pad_map / sizeof s_pad_map[0]; i++) {
            int bit = shift ? s_pad_map[i].shifted
                    : simple_live() ? s_pad_map[i].simple : s_pad_map[i].bit;
            /* The menus take the d-pad as the stick, below. */
            if (menu && !shift && (s_pad_map[i].button & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)))
                continue;
            if ((g->wButtons & s_pad_map[i].button) && bit >= 0)
                set_bit(report, bit);
        }
        rot = stick(g->sThumbLX);
        /* The left stick's Y is the sight-change stick's, which sits on the
         * left lever: it is also what moves the menus. */
        if (!simple_live())
            sight_y = stick(g->sThumbLY);
        if (shift) {
            sight_x = stick(g->sThumbRX);
            sight_y = stick(g->sThumbRY);
        } else {
            aim_x = stick(g->sThumbRX);
            aim_y = stick(g->sThumbRY);
        }
        /* The menus move on the sight-change stick, across too: the name
         * grid at pilot entry needs it. */
        if (menu && !shift) {
            sight_x = rot;
            rot = 0;
            if (g->wButtons & XINPUT_GAMEPAD_DPAD_LEFT)  sight_x = -127;
            if (g->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) sight_x = 127;
            if (g->wButtons & XINPUT_GAMEPAD_DPAD_UP)    sight_y = 127;
            if (g->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)  sight_y = -127;
        }
        if (g->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)
            left = 0xFFFF;
        if (!simple_live()) {
            middle = g->bLeftTrigger * 257u;
            right = g->bRightTrigger * 257u;
        }
    }

    for (i = 0; i < sizeof s_key_map / sizeof s_key_map[0]; i++)
        if (key(s_key_map[i].vk))
            set_bit(report, s_key_map[i].bit);
    if (key(VK_SHIFT) && key(VK_BACK))
        set_bit(report, SB_EJECT);
    else if (menu && key(VK_BACK))
        set_bit(report, SB_LOCK_ON);

    /* The levers and dials that stay put. */
    if (edge(0, (g && !menu && !simple_live() && (g->wButtons & XINPUT_GAMEPAD_DPAD_UP)) || key(VK_PRIOR))
        && s_gear < 5)
        s_gear++;
    if (edge(1, (g && !menu && !simple_live() && (g->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)) || key(VK_NEXT))
        && s_gear > -1)
        s_gear--;
    if (edge(2, key(VK_OEM_6)))                     /* ] */
        s_tuner = (s_tuner + 1) & 15;
    if (edge(3, key(VK_OEM_4)))                     /* [ */
        s_tuner = (s_tuner + 15) & 15;
    for (b = 0; b < 5; b++) {
        static const int vk[5] = { '6', '7', '8', '9', '0' };
        if (edge(4 + b, key(vk[b]))) {
            s_toggle[b] = !s_toggle[b];
            if (s_toggle[b] && s_guide.step == TL_STEP_SWITCHES)
                s_flipped |= 1u << b;
        }
        if (s_toggle[b])
            set_bit(report, SB_TOGGLE_FILTER + b);
    }

    script_apply(&sc);

    /* The window's help: Tab held or RB+Back for the card, F9 or RB+Start
     * for the pilot view, F8 for the prompts. */
    {
        DWORD now = GetTickCount();
        int pad_start = g && (g->wButtons & XINPUT_GAMEPAD_START) != 0;
        int pad_back = g && (g->wButtons & XINPUT_GAMEPAD_BACK) != 0;
        static int card_latched;
        int step;

        guide_update(now);
        auto_flip(now);
        if (edge(10, (shift && pad_back) || sc.card))
            card_latched = !card_latched;
        s_guide.card = card_latched || key(VK_TAB);
        if (edge(11, key(VK_F8)))
            s_guide.hints = !s_guide.hints;
        if (edge(13, key(VK_F7))) {
            easy_set(s_guide.classic);
        }
        if (edge(12, key(VK_F9) || (shift && pad_start) || sc.view))
            s_guide.view = !s_guide.view;
        if (s_guide.view && s_stage != S_OUTSIDE)
            xbox_FramebufferSetCrop(VIEW_X, VIEW_Y, VIEW_W, VIEW_H);
        else
            xbox_FramebufferSetCrop(0, 0, 0, 0);
        step = step_button((pad_start && !shift) || key(VK_RETURN) || sc.step, now);
        if (step >= 0)
            set_bit(report, menu && step == SB_START ? SB_MAIN_WEAPON : step);
        if (simple_live()) {
            int y = sc.axis[AX_DRIVE] ? sc.axis[AX_DRIVE] : g && !shift ? stick(g->sThumbLY) : 0;
            drive(y, &middle, &right, now);
            if ((g && g->bRightTrigger > 40) || sc.rt)
                set_bit(report, SB_MAIN_WEAPON);
            if ((g && g->bLeftTrigger > 40) || sc.lt)
                set_bit(report, SB_LOCK_ON);
        }
        if (s_guide.step == TL_STEP_ACCELERATE && (right > 0x4000 || (sc.pedals & 4)))
            s_accelerated = 1;
    }

    for (b = 0; b < 39; b++)
        if (sc.bits & (1ull << b))
            set_bit(report, b);
    if (sc.axis[AX_AIM_X]) aim_x = sc.axis[AX_AIM_X];
    if (sc.axis[AX_AIM_Y]) aim_y = sc.axis[AX_AIM_Y];
    if (sc.axis[AX_ROT]) rot = sc.axis[AX_ROT];
    if (sc.axis[AX_SIGHT_X]) sight_x = sc.axis[AX_SIGHT_X];
    if (sc.axis[AX_SIGHT_Y]) sight_y = sc.axis[AX_SIGHT_Y];
    if (sc.pedals & 1) left = 0xFFFF;
    if (sc.pedals & 2) middle = 0xFFFF;
    if (sc.pedals & 4) right = 0xFFFF;

    /* The arrows are the sight-change stick too, for the menus. */
    if (key(VK_LEFT))  sight_x = -127;
    if (key(VK_RIGHT)) sight_x = 127;
    if (key(VK_UP))    sight_y = 127;
    if (key(VK_DOWN))  sight_y = -127;

    /* The title reads the aiming lever's Y as (0x7FFF - v) >> 8 and the
     * sight-change stick's second axis as (0xFFFF - v) >> 8. Its X runs the
     * other way: the verification screen moves the lever right for a value
     * above 0x7FFF. */
    put16(report, 8, (unsigned)(0x7FFF + aim_x * 256));
    put16(report, 10, (unsigned)(0x7FFF - aim_y * 256));
    report[13] = (uint8_t)(int8_t)rot;
    report[15] = (uint8_t)(int8_t)sight_x;
    put16(report, 16, ((unsigned)(uint8_t)~(uint8_t)(int8_t)sight_y << 8) | 0xFF);
    put16(report, 18, left);
    put16(report, 20, middle);
    put16(report, 22, right);
    report[24] = (uint8_t)s_tuner;
    report[25] = s_gear < 0 ? 0xFE : s_gear == 0 ? 0xFF : (uint8_t)s_gear;

    /* TL_SB_PEEK=1: what the game made of it, every 2 s -- its gear
     * (0x351C58), its mode flag (0x3B9400, 1 in menus) and the first 14
     * bytes of the virtual pad sub_00049DD0 builds from the report
     * (0x4AFBC0: buttons, analog bytes, then LX LY RX RY, gear in LY). */
    {
        static int peek = -1;
        static DWORD last;
        extern ptrdiff_t g_xbox_mem_offset;
        if (peek < 0)
            peek = getenv("TL_SB_PEEK") != NULL;
        if (peek && GetTickCount() - last > 2000) {
            const uint8_t *m = (const uint8_t *)g_xbox_mem_offset;
            int i;
            last = GetTickCount();
            fprintf(stderr, "  [SB] game: gear %d mode %u pad", *(const int32_t *)(m + 0x351C58),
                    *(const uint32_t *)(m + 0x3B9400));
            for (i = 0; i < 14; i++)
                fprintf(stderr, " %02X", m[0x4AFBC0 + i]);
            fprintf(stderr, " | report gear %02X pedals %04X %04X %04X\n", report[25],
                    report[18] | report[19] << 8, report[20] | report[21] << 8,
                    report[22] | report[23] << 8);
        }
    }
}

/* ---- the lamps ---------------------------------------------------------- */

static uint8_t s_lamps[20];

int tl_sb_lamp(int lamp)
{
    if (lamp < 0 || lamp >= 40)
        return 0;
    return (s_lamps[lamp / 2] >> ((lamp & 1) * 4)) & 15;
}

/* TL_SB_LAMP_LOG=1 prints the lamps whenever they change, as 40 hex
 * digits (lamp 0 first), for working out which lamp is which. */
static void sb_lamps(int pad, const uint8_t *lamps, int n)
{
    static int log = -1;
    if (pad != 0)
        return;
    if (n > 20)
        n = 20;
    if (log < 0)
        log = getenv("TL_SB_LAMP_LOG") != NULL;
    if (log && memcmp(s_lamps, lamps, (size_t)n)) {
        char line[41];
        int i;
        memcpy(s_lamps, lamps, (size_t)n);
        for (i = 0; i < 40; i++)
            line[i] = "0123456789ABCDEF"[tl_sb_lamp(i)];
        line[40] = 0;
        fprintf(stderr, "  [SB] lamps %s\n", line);
    }
    memcpy(s_lamps, lamps, (size_t)n);
}

void tl_sb_input_init(void)
{
    g_xbox_sb_input_hook = sb_fill;
    g_xbox_sb_lamps_hook = sb_lamps;
    tl_sb_help_init();
}
