//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_input_types.h -- Canonical input ABI types and constants
 *
 * The button bitmask values and the of_input_state_t struct definition.
 * Both the SDK (api/of_input.h) and the kernel HAL (os/hal/input.h)
 * include this header so they share one source of truth without
 * duplicating the types or pulling in each other's accessor inlines.
 */

#ifndef OF_INPUT_TYPES_H
#define OF_INPUT_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define OF_MAX_PLAYERS  2

/* APF input report types carried in KEY[31:28].  Pocket reserves Player 3
 * for dock keyboard reports and Player 4 for dock mouse reports. */
#define OF_INPUT_TYPE_NONE          0u
#define OF_INPUT_TYPE_POCKET        1u
#define OF_INPUT_TYPE_GAMEPAD       2u
#define OF_INPUT_TYPE_GAMEPAD_ANALOG 3u
#define OF_INPUT_TYPE_KEYBOARD      4u
#define OF_INPUT_TYPE_MOUSE         5u

#define OF_KEYBOARD_MAX_USAGE   256u
#define OF_KEYBOARD_WORDS       (OF_KEYBOARD_MAX_USAGE / 32u)
#define OF_KEYBOARD_REPORT_KEYS 6u

#define OF_KEYMOD_LCTRL         (1u << 0)
#define OF_KEYMOD_LSHIFT        (1u << 1)
#define OF_KEYMOD_LALT          (1u << 2)
#define OF_KEYMOD_LGUI          (1u << 3)
#define OF_KEYMOD_RCTRL         (1u << 4)
#define OF_KEYMOD_RSHIFT        (1u << 5)
#define OF_KEYMOD_RALT          (1u << 6)
#define OF_KEYMOD_RGUI          (1u << 7)

/* Canonical button masks. Targets translate their native register
 * format into this layout in their input HAL. The Pocket APF low 16
 * button bits match OF_BTN_* bit-for-bit, but KEY[31:28] carries the
 * APF report type and is not part of the stable button mask. */
#define OF_BTN_UP       (1 << 0)
#define OF_BTN_DOWN     (1 << 1)
#define OF_BTN_LEFT     (1 << 2)
#define OF_BTN_RIGHT    (1 << 3)
#define OF_BTN_A        (1 << 4)
#define OF_BTN_B        (1 << 5)
#define OF_BTN_X        (1 << 6)
#define OF_BTN_Y        (1 << 7)
#define OF_BTN_L1       (1 << 8)
#define OF_BTN_R1       (1 << 9)
#define OF_BTN_L2       (1 << 10)
#define OF_BTN_R2       (1 << 11)
#define OF_BTN_L3       (1 << 12)
#define OF_BTN_R3       (1 << 13)
#define OF_BTN_SELECT   (1 << 14)
#define OF_BTN_START    (1 << 15)

typedef struct {
    uint32_t buttons;
    uint32_t buttons_pressed;
    uint32_t buttons_released;
    int16_t  joy_lx, joy_ly;
    int16_t  joy_rx, joy_ry;
    uint16_t trigger_l, trigger_r;
} of_input_state_t;

typedef struct {
    uint8_t  present;
    uint8_t  reserved0;
    uint16_t modifiers;
    uint16_t modifiers_pressed;
    uint16_t modifiers_released;
    uint32_t keys[OF_KEYBOARD_WORDS];
    uint32_t keys_pressed[OF_KEYBOARD_WORDS];
    uint32_t keys_released[OF_KEYBOARD_WORDS];
    uint8_t  report_keys[OF_KEYBOARD_REPORT_KEYS];
    uint8_t  reserved1[2];
} of_keyboard_state_t;

typedef struct {
    uint8_t  present;
    uint8_t  reserved0;
    uint16_t buttons;
    uint16_t buttons_pressed;
    uint16_t buttons_released;
    uint16_t report_counter;
    int32_t  dx;
    int32_t  dy;
} of_mouse_state_t;

#ifdef __cplusplus
}
#endif

#endif /* OF_INPUT_TYPES_H */
