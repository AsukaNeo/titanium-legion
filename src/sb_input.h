/*
 * The Steel Battalion controller, played on a pad and a keyboard
 * (sb_input.c). Button numbers are the controller's report bits, 0-38 from
 * report byte 2, in the order the controller is documented in. Each was
 * checked on the game's CONTROLLER FUNCTION VERIFICATION screen; only the
 * order within the function grid's columns (13-15, 19-21) couldn't be read
 * there (docs/controller.md).
 */
#ifndef TL_SB_INPUT_H
#define TL_SB_INPUT_H

enum {
    SB_MAIN_WEAPON, SB_FIRE, SB_LOCK_ON, SB_EJECT, SB_HATCH, SB_IGNITION,
    SB_START, SB_MULTIMON_OPEN, SB_MAP_ZOOM, SB_MODE_SELECT, SB_SUB_MONITOR,
    SB_ZOOM_IN, SB_ZOOM_OUT, SB_FSS, SB_MANIPULATOR, SB_LINE_COLOR,
    SB_WASHING, SB_EXTINGUISHER, SB_CHAFF, SB_TANK_DETACH, SB_OVERRIDE,
    SB_NIGHT_SCOPE, SB_F1, SB_F2, SB_F3, SB_WEAPON_MAIN, SB_WEAPON_SUB,
    SB_MAGAZINE, SB_COMM1, SB_COMM2, SB_COMM3, SB_COMM4, SB_COMM5,
    SB_SIGHT_CHANGE, SB_TOGGLE_FILTER, SB_TOGGLE_OXYGEN, SB_TOGGLE_FUEL_FLOW,
    SB_TOGGLE_BUFFER, SB_TOGGLE_VT_LOCATION
};

/* Hook the controller's report and lamps into the toolkit. Before the
 * entry point. */
void tl_sb_input_init(void);

/* Lamp `lamp` (0-39) as the game last set it, 0-15. */
int tl_sb_lamp(int lamp);

#endif /* TL_SB_INPUT_H */
