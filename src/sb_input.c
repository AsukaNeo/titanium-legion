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
 * full travel (P the positive end), PL PM PR (left, middle, right pedal) to the floor. GR GN G1-G5
 * put the gear lever there and T<n> turns the tuner to n; both stay. */
enum { AX_AIM_X, AX_AIM_Y, AX_ROT, AX_SIGHT_X, AX_SIGHT_Y, AX_COUNT };

typedef struct {
    uint64_t bits;
    int8_t axis[AX_COUNT];
    uint8_t pedals;                 /* 1 left, 2 middle, 4 right */
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
    };
    size_t i;
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
        for (a = 0; a < AX_COUNT; a++)
            if (s_script[i].out.axis[a])
                o->axis[a] = s_script[i].out.axis[a];
    }
}

/* ---- the report --------------------------------------------------------- */

/* Pad buttons with and without RB held. */
static const struct { WORD button; int bit, shifted; } s_pad_map[] = {
    { XINPUT_GAMEPAD_A,              SB_MAIN_WEAPON,  SB_CHAFF },
    { XINPUT_GAMEPAD_B,              SB_FIRE,         SB_EXTINGUISHER },
    { XINPUT_GAMEPAD_X,              SB_LOCK_ON,      SB_NIGHT_SCOPE },
    { XINPUT_GAMEPAD_Y,              SB_MAGAZINE,     SB_OVERRIDE },
    { XINPUT_GAMEPAD_DPAD_LEFT,      SB_ZOOM_IN,      SB_ZOOM_IN },
    { XINPUT_GAMEPAD_DPAD_RIGHT,     SB_ZOOM_OUT,     SB_ZOOM_OUT },
    { XINPUT_GAMEPAD_START,          SB_START,        SB_START },
    { XINPUT_GAMEPAD_BACK,           SB_MULTIMON_OPEN, SB_MULTIMON_OPEN },
    { XINPUT_GAMEPAD_LEFT_THUMB,     SB_WASHING,      SB_WASHING },
    { XINPUT_GAMEPAD_RIGHT_THUMB,    SB_SIGHT_CHANGE, SB_SIGHT_CHANGE },
};

/* Keys held for a button. Eject is Shift+Backspace, below. */
static const struct { int vk, bit; } s_key_map[] = {
    { VK_SPACE, SB_MAIN_WEAPON },
    { 'H', SB_HATCH }, { 'I', SB_IGNITION }, { VK_RETURN, SB_START },
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
    int b;

    if (pad != 0)
        return;
    memset(&xs, 0, sizeof xs);
    if (XInputGetState(0, &xs) == ERROR_SUCCESS)
        g = &xs.Gamepad;

    if (g) {
        shift = (g->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
        for (i = 0; i < sizeof s_pad_map / sizeof s_pad_map[0]; i++)
            if (g->wButtons & s_pad_map[i].button)
                set_bit(report, shift ? s_pad_map[i].shifted : s_pad_map[i].bit);
        rot = stick(g->sThumbLX);
        /* The left stick's Y is the sight-change stick's, which sits on the
         * left lever: it is also what moves the menus. */
        sight_y = stick(g->sThumbLY);
        if (shift) {
            sight_x = stick(g->sThumbRX);
            sight_y = stick(g->sThumbRY);
        } else {
            aim_x = stick(g->sThumbRX);
            aim_y = stick(g->sThumbRY);
        }
        if (g->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)
            left = 0xFFFF;
        middle = g->bLeftTrigger * 257u;
        right = g->bRightTrigger * 257u;
    }

    for (i = 0; i < sizeof s_key_map / sizeof s_key_map[0]; i++)
        if (key(s_key_map[i].vk))
            set_bit(report, s_key_map[i].bit);
    if (key(VK_SHIFT) && key(VK_BACK))
        set_bit(report, SB_EJECT);

    /* The levers and dials that stay put. */
    if (edge(0, (g && (g->wButtons & XINPUT_GAMEPAD_DPAD_UP)) || key(VK_PRIOR))
        && s_gear < 5)
        s_gear++;
    if (edge(1, (g && (g->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)) || key(VK_NEXT))
        && s_gear > -1)
        s_gear--;
    if (edge(2, key(VK_OEM_6)))                     /* ] */
        s_tuner = (s_tuner + 1) & 15;
    if (edge(3, key(VK_OEM_4)))                     /* [ */
        s_tuner = (s_tuner + 15) & 15;
    for (b = 0; b < 5; b++) {
        static const int vk[5] = { '6', '7', '8', '9', '0' };
        if (edge(4 + b, key(vk[b])))
            s_toggle[b] = !s_toggle[b];
        if (s_toggle[b])
            set_bit(report, SB_TOGGLE_FILTER + b);
    }

    script_apply(&sc);
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
}
