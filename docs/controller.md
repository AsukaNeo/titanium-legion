# The Steel Battalion controller, as the game sees it

Everything here was read from the supported `default.xbe` (SHA-1
`338b3e10...660e`), not from the hardware or another emulator. Addresses are
guest virtual addresses in that build.

## How XPP knows the device

XPP links four XID types, as pointers at `0x255D04`:

| Record | bType | Max handles | Input data | Output data | New-data routine |
|---|---|---|---|---|---|
| `0x255F30` | **0x80** Steel Battalion | 2 | 24 bytes | 20 bytes | `0x25D059` |
| `0x255DEC` | 0x01 gamepad | 4 | 18 bytes | 4 bytes | `0x259B4C` |
| `0x255E68` | 0x03 | 1 | | | `0x259B8D` |
| `0x255E30` | 0x02 | 1 | | | `0x259BB1` |

The data sizes don't count the 2-byte report header (report id 0, length), so
on the wire the **input report is 26 bytes (0x1A)** and the **output report is
22 bytes (0x16)**.

The XID descriptor handler (`0x25A0E3`) looks the device up by bType
(`sub_00256E62`). It then accepts the device when the descriptor's
bMaxInputReportSize is 2..0x20 and both report sizes are no larger than the
type's. bSubType is not checked. The emulated device's XID descriptor must
therefore say bType 0x80, input size 26 and output size 22.

The new-data routine copies the report body into the device state. If any of
the first three 16-bit words (the buttons) is non-zero, it resets the
auto-power-down timer.

## The game's XPP calls

| Address | Call | Notes |
|---|---|---|
| `0x256BBC` | XGetDevices | |
| `0x256BDE` | XGetDeviceChanges | |
| `0x256EC4` | XInputOpen | the SB is opened with a poll-rate record at `0x4AFBB8` (3, 8, 8, 0) |
| `0x256F1A` | XInputClose | |
| `0x256F26` | XInputGetCapabilities | used on the gamepad only |
| `0x257103` | XInputGetState | |
| `0x257172` | XInputSetState | the lamps |

The device type is `0x255EE4` (the gamepad's is `0x255DB4`, and the game opens
those too, for menus). Opening happens in `sub_000492A0`; polling happens in
`sub_00049DD0`, once a frame, for up to four ports.

## "Not connected"

`sub_00049DD0` sets `[0x351CB0]` to 1 whenever it read an SB this frame. With
no SB:

- **Before a mission**, the title screen waits for the SB's Main Weapon button
  ("PRESS MAIN WEAPON BUTTON"). A gamepad doesn't get past it: the game reads
  gamepads, but nothing on the title takes their buttons.
- **In a mission**, the main loop (`sub_00045DF0` at `0x465F4`) sees
  `[0x351CB0] == 0` and loops showing "THE CONTROLLER FOR STEEL BATTALION IS
  NOT CONNECTED. PLEASE CONNECT THE CONTROLLER THEN..." (`sub_00030BE0(1)`)
  until an SB reports again and a button is pressed (`sub_00048B80(0, 6,
  0x30)`).

## Input report (26 bytes)

XInputGetState fills `0x4AF638 + port*0x1C`: the packet number, then the 24
data bytes. The offsets below count from the start of the report, so offset 2
is the first data byte.

| Offset | Size | Field | How the game reads it (`sub_00049DD0`) |
|---|---|---|---|
| 0 | 1 | report id, 0 | |
| 1 | 1 | length, 0x1A | |
| 2-3 | u16 | buttons 0-15 | bit by bit; see below |
| 4-5 | u16 | buttons 16-31 | bit by bit |
| 6 | u8 | buttons 32-38 (bits 0-6) | bit by bit; bit 7 unused |
| 7 | u8 | (unused) | |
| 8-9 | u16 | analog | `(0x7FFF - v) >> 8`, minus a calibration offset, scaled, clamped to ±127: an aiming axis |
| 10-11 | u16 | analog | the same treatment: the other aiming axis |
| 12 | u8 | (low byte of a word, unused) | |
| 13 | u8 | analog, signed | taken as is: the rotation lever |
| 14 | u8 | (low byte, unused) | |
| 15 | u8 | analog, signed | taken as is: sight change, one axis |
| 16-17 | u16 | analog | `(0xFFFF - v) >> 8`: sight change, the other axis (inverted) |
| 18-19 | u16 | pedal | unsigned, dead zone subtracted, scaled |
| 20-21 | u16 | pedal | unsigned, same |
| 22-23 | u16 | pedal | unsigned, same |
| 24 | u8 | tuner dial | stored raw |
| 25 | u8 | gear lever | `0xFE` = R, `0xFF` = N, `1`-`5` = gears, `0` = between gears (keeps the last) |

So the report holds 39 buttons, two aiming-lever axes, the rotation lever, two
sight-change axes, three pedals, the tuner dial and the gear lever.

## Checked on the game's own screen

CONFIG → CONTROLLER SETTINGS → CONTROLLER FUNCTION VERIFICATION draws the
whole controller and lights each control as it is used (F1 + Lock On leaves).
Every bit and axis was pressed there on its own (`logs/sweep*_script.txt`,
2026-10-28), and each lit one control:

| Bits | Control on the diagram |
|---|---|
| 0, 1, 2 | Aiming lever: MAIN WEAPON pill, the trigger bar beside it, LOCK ON |
| 3, 4, 5, 6 | Right block, top to bottom: eject cover, COCKPIT HATCH, IGNITION, START |
| 7, 8 | Multi-monitor OPEN/CLOSE, MAP ZOOM IN/OUT |
| 9, 10 | MODE SELECT, SUB MONITOR MODE SELECT |
| 11, 12 | MAIN MONITOR ZOOM in, out |
| 13, 14, 15 | Function grid, right column, top to bottom |
| 19, 20, 21 | Function grid, middle column, top to bottom |
| 22, 23, 24 | Function grid, left column: F1, F2, F3 |
| 16, 17, 18 | WASHING, EXTINGUISHER, CHAFF |
| 25, 26, 27 | Weapon con MAIN, SUB, MAGAZINE CHANGE |
| 28-32 | Communication switches 1-5, left to right |
| 33 | Pressing the SIGHT CHANGE stick |
| 34-38 | Toggle switches, left to right |

That is the documented order `src/sb_input.h` uses. One caveat: the function
grid's labels are about two pixels tall at 640x480 and can't be read, so which
of FSS / Manipulator / Line Colour Change (13-15) and Tank Detach / Override /
Night Scope (19-21) sits where within its column is taken from the
documentation, not seen.

Axes, as the diagram moves:

| Report | Value | Diagram |
|---|---|---|
| 8-9 aiming X | above 0x7FFF | lever right |
| 10-11 aiming Y | below 0x7FFF | lever up |
| 13 rotation | positive | lever right |
| 15 sight X | positive | stick right |
| 16-17 sight Y | `(0xFFFF - v) >> 8` positive | stick up |
| 18, 20, 22 | 0xFFFF | left, middle, right pedal down |
| 24 tuner | 0-15 | the dial's positions |
| 25 gear | `FE`, `FF`, `1`-`5` | R, N, 1-5 |

The aiming X axis is the one that runs against `(0x7FFF - v)`: the first
mapping had it backwards. CONFIG's "AIMING LEVER Y-AXIS" and "SIGHT CHANGE
STICK Y-AXIS" (both "Inverted" by default) flip the Y axes in the game.

**The menus move on the sight-change stick** (down = next item; a held stick
repeats). Main Weapon accepts and Lock On goes back. What the code shows: on menus,
`sub_00049DD0` uses bit 1 as confirm (it feeds the same flags as the gamepad's
A), bit 0 as cancel (the gamepad's B), and bits 2, 11, 12, 33 (byte 6 bit 1),
7, 8, 9 and 10 for the menu moves the gamepad's d-pad, Start and stick click
give.

## Output report (22 bytes): the lamps

XInputSetState is sent with the feedback blocks at `0x4AF800 + port*0x56`. The
report sits at +0x40, so the lamp bytes are `0x4AF842 + port*0x56`, 20 of them.

`sub_00049340(lamp, level)` sets one lamp. Lamp *n* is byte `n / 2`: an even
*n* is the low nibble and an odd *n* the high nibble, so each lamp has a 4-bit
brightness (0-15), for 40 slots in all. `sub_000493B0` fades the lamps.
`0x2B3870` holds a table that maps each button to its lamp number plus one
(0 = no lamp).

The game sends a new report when the last one has finished
(`Status != ERROR_IO_PENDING`, 0x3E5).
