//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_input.h -- Input subsystem API for openfpgaOS
 *
 * 2 controllers, d-pad + ABXY + L/R + sticks + triggers.
 */

#ifndef OF_INPUT_H
#define OF_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "of_input_types.h"

#ifndef OF_PC

#include "of_services.h"

#define OF_INPUT_SVC_FIELD_INDEX(field) \
    ((uint32_t)((offsetof(struct of_services_table, field) - 12u) / sizeof(void *)))

/* Polled controller snapshots live in of_init.c so every translation unit
 * sees the same input state while the of_btn* helpers remain inline. */
extern of_input_state_t _of_input_p0;
extern of_input_state_t _of_input_p1;

static inline void of_input_poll(void) {
    OF_SVC->input_poll();
    OF_SVC->input_get_state(0, &_of_input_p0);
    OF_SVC->input_get_state(1, &_of_input_p1);
}

/* Single-player fast path: poll + get P0 in one call */
static inline void of_input_poll_p0(void) {
    OF_SVC->input_poll_p0(&_of_input_p0);
}

static inline int of_btn(uint32_t mask) {
    return (_of_input_p0.buttons & mask) != 0;
}

static inline int of_btn_pressed(uint32_t mask) {
    return (_of_input_p0.buttons_pressed & mask) != 0;
}

static inline int of_btn_released(uint32_t mask) {
    return (_of_input_p0.buttons_released & mask) != 0;
}

static inline int of_btn_p2(uint32_t mask) {
    return (_of_input_p1.buttons & mask) != 0;
}

static inline int of_btn_pressed_p2(uint32_t mask) {
    return (_of_input_p1.buttons_pressed & mask) != 0;
}

static inline int of_btn_released_p2(uint32_t mask) {
    return (_of_input_p1.buttons_released & mask) != 0;
}

static inline uint32_t of_input_state(int player, of_input_state_t *state) {
    OF_SVC->input_get_state(player, state);
    return state->buttons;
}

static inline void of_input_clear_bytes(void *ptr, uint32_t size) {
    uint8_t *p = (uint8_t *)ptr;
    while (size--)
        *p++ = 0;
}

static inline void of_input_keyboard_state(of_keyboard_state_t *state) {
    if (!state)
        return;
    if (OF_SVC->count > OF_INPUT_SVC_FIELD_INDEX(input_get_keyboard_state) &&
        OF_SVC->input_get_keyboard_state) {
        OF_SVC->input_get_keyboard_state(state);
    } else {
        of_input_clear_bytes(state, sizeof(*state));
    }
}

static inline void of_input_mouse_state(of_mouse_state_t *state) {
    if (!state)
        return;
    if (OF_SVC->count > OF_INPUT_SVC_FIELD_INDEX(input_read_mouse_state) &&
        OF_SVC->input_read_mouse_state) {
        OF_SVC->input_read_mouse_state(state);
    } else {
        of_input_clear_bytes(state, sizeof(*state));
    }
}

static inline int of_keyboard_key(const of_keyboard_state_t *state,
                                  uint8_t usage) {
    return state &&
           ((state->keys[usage >> 5] >> (usage & 31)) & 1u) != 0;
}

static inline int of_keyboard_key_pressed(const of_keyboard_state_t *state,
                                          uint8_t usage) {
    return state &&
           ((state->keys_pressed[usage >> 5] >> (usage & 31)) & 1u) != 0;
}

static inline int of_keyboard_key_released(const of_keyboard_state_t *state,
                                           uint8_t usage) {
    return state &&
           ((state->keys_released[usage >> 5] >> (usage & 31)) & 1u) != 0;
}

static inline void of_input_set_deadzone(int16_t deadzone) {
    OF_SVC->input_set_deadzone(deadzone);
}

#else /* OF_PC */

void     of_input_poll(void);
void     of_input_poll_p0(void);
int      of_btn(uint32_t mask);
int      of_btn_pressed(uint32_t mask);
int      of_btn_released(uint32_t mask);
int      of_btn_p2(uint32_t mask);
int      of_btn_pressed_p2(uint32_t mask);
int      of_btn_released_p2(uint32_t mask);
uint32_t of_input_state(int player, of_input_state_t *state);
void     of_input_keyboard_state(of_keyboard_state_t *state);
void     of_input_mouse_state(of_mouse_state_t *state);
void     of_input_set_deadzone(int16_t deadzone);

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_INPUT_H */
