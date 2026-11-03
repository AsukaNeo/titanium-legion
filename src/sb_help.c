/*
 * The help over the frame: a prompt for the start-up step the game is
 * waiting on, and a card of the controls while Tab is held (or RB+Back).
 * Painted with GDI into the window's layer (g_xbox_fb_layer_hook), on the
 * presenting thread, only when what it shows changes.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sb_input.h"

extern int (*g_xbox_fb_layer_hook)(HDC dc, uint32_t *bits, int w, int h);

#define INK      RGB(226, 232, 214)
#define DIM      RGB(150, 165, 140)
#define ACCENT   RGB(120, 240, 170)

/* What the game's lamps stand for: lamp n is button n + 3. */
static const struct { const char *name, *how; } s_lamps[30] = {
    {"EJECT", "Shift+Backspace"},          {"Cockpit hatch", "H"},
    {"Ignition", "I"},                     {"Start", "Enter / Start"},
    {"Multi-monitor", "M / Back"},         {"Map zoom", "N"},
    {"Mode select", "B"},                  {"Sub monitor", "V"},
    {"Monitor zoom in", "D-pad left"},     {"Monitor zoom out", "D-pad right"},
    {"FSS", "F"},                          {"Manipulator", "G"},
    {"Line colour", "C"},                  {"Washing", "W / left stick click"},
    {"Extinguisher", "X / RB+B"},          {"Chaff", "Z / RB+A"},
    {"Tank detach", "T"},                  {"Override", "O / RB+Y"},
    {"Night scope", "L / RB+X"},           {"F1", "F1"},
    {"F2", "F2"},                          {"F3", "F3"},
    {"Main weapon select", "Q"},           {"Sub weapon select", "E"},
    {"Magazine change", "R / Y"},          {"Comm 1", "1"},
    {"Comm 2", "2"},                       {"Comm 3", "3"},
    {"Comm 4", "4"},                       {"Comm 5", "5"},
};

typedef struct { const char *what, *pad, *keys; } Row;

static const Row s_simple[] = {
    {"Walk (gears follow)", "Left stick up", "-"},
    {"Brake, then reverse", "Left stick down", "-"},
    {"Turn", "Left stick left / right", "-"},
    {"Aim", "Right stick", "-"},
    {"Main weapon", "RT", "Space"},
    {"Lock on", "LT", "-"},
    {"Sub weapon (fire)", "A", "-"},
    {"Slide step", "LB", "-"},
    {"Main weapon select", "X", "Q"},
    {"Sub weapon select", "B", "E"},
    {"Magazine change", "Y", "R"},
    {"Monitor zoom in / out", "D-pad left / right", "-"},
    {"Map zoom", "D-pad up", "N"},
    {"Sub monitor mode", "D-pad down", "V"},
    {"Sight change stick", "RB + right stick", "Arrow keys"},
    {"Chaff", "RB + A", "Z"},
    {"Extinguisher", "RB + B", "X"},
    {"Night scope", "RB + X", "L"},
    {"Override", "RB + Y", "O"},
    {"Washing", "Left stick click", "W"},
};

static const Row s_fight[] = {
    {"Walk (accelerator)", "RT", "-"},
    {"Brake", "LT", "-"},
    {"Slide step", "LB", "-"},
    {"Gear up / down", "D-pad up / down", "Page Up / Down"},
    {"Turn", "Left stick left / right", "-"},
    {"Aim", "Right stick", "-"},
    {"Sight up / down", "Left stick up / down", "Arrow keys"},
    {"Sight change stick", "RB + right stick", "Arrow keys"},
    {"Main weapon", "A", "Space"},
    {"Sub weapon (fire)", "B", "-"},
    {"Lock on", "X", "-"},
    {"Magazine change", "Y", "R"},
    {"Monitor zoom in / out", "D-pad left / right", "-"},
    {"Chaff", "RB + A", "Z"},
    {"Extinguisher", "RB + B", "X"},
    {"Night scope", "RB + X", "L"},
    {"Override", "RB + Y", "O"},
    {"Washing", "Left stick click", "W"},
    {"Sight change press", "Right stick click", "-"},
    {"Multi-monitor", "Back", "M"},
};

static const Row s_panel[] = {
    {"Next start-up step", "Start", "Enter"},
    {"Hatch / Ignition", "-", "H / I"},
    {"Five switches", "-", "6 7 8 9 0"},
    {"Weapon main / sub", "-", "Q / E"},
    {"Comm 1-5", "-", "1 2 3 4 5"},
    {"Tuner dial", "-", "[ and ]"},
    {"Map zoom / mode", "-", "N / B"},
    {"Sub monitor mode", "-", "V"},
    {"FSS / Manipulator", "-", "F / G"},
    {"Line colour / Tank detach", "-", "C / T"},
    {"F1-F3", "-", "F1 F2 F3"},
    {"Eject", "-", "Shift+Backspace"},
    {"This card", "RB + Back", "Tab (hold)"},
    {"Pilot view", "RB + Start", "F9"},
    {"Prompts on / off", "-", "F8"},
    {"Easy Pilot on / off", "-", "F7"},
    {"Menus: move", "Left stick, D-pad", "Arrows"},
    {"Menus: OK / back", "A, Start / X", "Enter, Space / Bksp"},
    {"Settings", "-", "F10"},
};

static void fill_alpha(uint32_t *bits, int w, int h, const RECT *r, uint32_t rgb, unsigned a)
{
    int x, y;
    for (y = r->top < 0 ? 0 : r->top; y < r->bottom && y < h; y++)
        for (x = r->left < 0 ? 0 : r->left; x < r->right && x < w; x++)
            bits[(size_t)y * w + x] = rgb | (a << 24);
}

/* GDI text clears alpha: give back the box's opacity. */
static void seal_alpha(uint32_t *bits, int w, int h, const RECT *r, unsigned a)
{
    int x, y;
    GdiFlush();
    for (y = r->top < 0 ? 0 : r->top; y < r->bottom && y < h; y++)
        for (x = r->left < 0 ? 0 : r->left; x < r->right && x < w; x++)
            bits[(size_t)y * w + x] = (bits[(size_t)y * w + x] & 0xFFFFFFu) | (a << 24);
}

static HFONT font(int px, int bold)
{
    return CreateFontA(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, "Segoe UI");
}

static void text(HDC dc, const char *s, int x, int y, int w, int h, UINT fmt, COLORREF c)
{
    RECT r;
    SetRect(&r, x, y, x + w, y + h);
    SetTextColor(dc, c);
    DrawTextA(dc, s, -1, &r, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

static void table(HDC dc, const char *title, const Row *rows, int n, int x, int y, int w,
                  int row, HFONT plain, HFONT bold)
{
    int i, c1 = w * 37 / 100, c2 = w * 36 / 100, c3 = w - c1 - c2;
    SelectObject(dc, bold);
    text(dc, title, x, y, c1, row, DT_LEFT, ACCENT);
    text(dc, "Pad", x + c1, y, c2, row, DT_LEFT, ACCENT);
    text(dc, "Keyboard", x + c1 + c2, y, c3, row, DT_LEFT, ACCENT);
    y += row;
    SelectObject(dc, plain);
    for (i = 0; i < n; i++, y += row) {
        text(dc, rows[i].what, x, y, c1, row, DT_LEFT, INK);
        text(dc, rows[i].pad, x + c1, y, c2, row, DT_LEFT, strcmp(rows[i].pad, "-") ? INK : DIM);
        text(dc, rows[i].keys, x + c1 + c2, y, c3, row, DT_LEFT, strcmp(rows[i].keys, "-") ? INK : DIM);
    }
}

static void paint_card(HDC dc, uint32_t *bits, int w, int h)
{
    int nf = (int)(sizeof s_fight / sizeof s_fight[0]);
    int side = w / 40, top = h / 30;
    int row = (h - 2 * top) / (nf + 3), px = row * 7 / 10;
    int col = (w - 3 * side) / 2;
    HFONT plain, bold, old;
    RECT all;

    if (px > 26) px = 26;
    if (px * 31 > col) px = col / 31;       /* a narrow window */
    plain = font(px, 0);
    bold = font(px, 1);
    SetRect(&all, 0, 0, w, h);
    fill_alpha(bits, w, h, &all, 0x0A100C, 0xE6);
    SetBkMode(dc, TRANSPARENT);
    old = (HFONT)SelectObject(dc, bold);
    text(dc, tl_sb_guide()->classic ? "CONTROLS: CLASSIC" : "CONTROLS: EASY PILOT",
         side, top, w - 2 * side, row, DT_LEFT, INK);
    if (tl_sb_guide()->classic)
        table(dc, "The fight", s_fight, nf, side, top + row + row / 2, col, row, plain, bold);
    else
        table(dc, "The fight", s_simple, (int)(sizeof s_simple / sizeof s_simple[0]),
              side, top + row + row / 2, col, row, plain, bold);
    table(dc, "The panel", s_panel, (int)(sizeof s_panel / sizeof s_panel[0]),
          side * 2 + col, top + row + row / 2, col, row, plain, bold);
    SelectObject(dc, old);
    DeleteObject(plain);
    DeleteObject(bold);
    seal_alpha(bits, w, h, &all, 0xE6);
}

static void paint_prompt(HDC dc, uint32_t *bits, int w, int h, const char *head, const char *how)
{
    int px = h / 28, bw = w * 3 / 5, bh;
    HFONT big, small, old;
    RECT box;

    if (px < 14) px = 14;
    bh = px * 7 / 2;
    SetRect(&box, (w - bw) / 2, h - bh - h / 40, (w + bw) / 2, h - h / 40);
    fill_alpha(bits, w, h, &box, 0x0A100C, 0xD0);
    big = font(px, 1);
    small = font(px * 3 / 4, 0);
    SetBkMode(dc, TRANSPARENT);
    old = (HFONT)SelectObject(dc, big);
    text(dc, head, box.left, box.top + px / 4, bw, px * 3 / 2, DT_CENTER, ACCENT);
    SelectObject(dc, small);
    text(dc, how, box.left, box.top + px * 7 / 4, bw, px * 3 / 2, DT_CENTER, INK);
    SelectObject(dc, old);
    DeleteObject(big);
    DeleteObject(small);
    seal_alpha(bits, w, h, &box, 0xD0);
}

static int layer(HDC dc, uint32_t *bits, int w, int h)
{
    static int last_key = -1, last_w, last_h;
    const TlSbGuide *g = tl_sb_guide();
    char head[80], how[120];
    int key;

    head[0] = 0;
    how[0] = 0;
    if (g->hints && !g->card) {
        switch (g->step) {
        case TL_STEP_HATCH:
            strcpy(head, "Close the cockpit hatch");
            strcpy(how, "Pad: Start     Keyboard: H or Enter");
            break;
        case TL_STEP_IGNITION:
            strcpy(head, "Turn on the ignition");
            strcpy(how, "Pad: Start     Keyboard: I or Enter");
            break;
        case TL_STEP_SWITCHES:
            snprintf(head, sizeof head, "Flip the five switches (%d of 5)", g->switches);
            strcpy(how, "Pad: Start flips them all     Keyboard: 6 7 8 9 0, or Enter");
            break;
        case TL_STEP_START:
            strcpy(head, "Press Start");
            strcpy(how, "Pad: Start     Keyboard: Enter");
            break;
        case TL_STEP_ENGINE:
            strcpy(head, "Start the engine: hold Start");
            strcpy(how, "It fires when all five bars are past the marker. Pedals up.");
            break;
        case TL_STEP_SHIFT:
            strcpy(head, "Shift to gear 1");
            strcpy(how, "Pad: D-pad up     Keyboard: Page Up");
            break;
        case TL_STEP_ACCELERATE:
            strcpy(head, g->classic ? "Hold the accelerator to walk" : "Push the left stick up to walk");
            strcpy(how, g->classic ? "Pad: RT.  Hold Tab or press RB+Back for all the controls."
                                   : "RT fires.  Hold Tab or press RB+Back for all the controls.");
            break;
        default:
            if (g->ask_lamp >= 0 && g->ask_lamp < 30) {
                strcpy(head, s_lamps[g->ask_lamp].name);
                strcpy(how, s_lamps[g->ask_lamp].how);
            }
            break;
        }
    }
    if (!g->card && !head[0]) {
        last_key = -1;
        return 0;
    }
    key = g->card ? 1000000 + g->classic : g->step * 2 + g->classic + 100 * g->step * 100 + g->switches * 10000 + g->ask_lamp + 1;
    if (key == last_key && w == last_w && h == last_h)
        return 1;
    last_key = key;
    last_w = w;
    last_h = h;
    memset(bits, 0, (size_t)w * h * 4);
    if (g->card)
        paint_card(dc, bits, w, h);
    else
        paint_prompt(dc, bits, w, h, head, how);
    return 2;
}

void tl_sb_help_init(void)
{
    g_xbox_fb_layer_hook = layer;
}
