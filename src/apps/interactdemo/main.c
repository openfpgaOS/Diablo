//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * interactdemo -- live view of the public openfpgaOS input stack.
 *
 * SNAC is owned by the OS and folded into the canonical player snapshots.
 * This app intentionally uses only SDK input/Analogizer calls; it does not
 * read hardware registers or call any SNAC-specific APIs.
 */

#include "of.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CSI       "\033["
#define GOTO(r,c) printf(CSI "%d;%dH", (r), (c))
#define CLR       printf(CSI "2J" CSI "H")
#define RESET     printf(CSI "0m")
#define BOLD      printf(CSI "1m")
#define DIM       printf(CSI "2m")
#define FG_GREEN  printf(CSI "92m")
#define FG_YELLOW printf(CSI "93m")
#define FG_CYAN   printf(CSI "96m")
#define FG_GRAY   printf(CSI "90m")

static const char *const video_modes[] = {
    "RGB", "NTSC", "PAL", "YC-NTSC", "YC-PAL", "SCART"
};

typedef struct {
    uint8_t id;
    const char *name;
} id_name_t;

static const id_name_t snac_types[] = {
    {0x00, "None"},
    {0x01, "DB15"},
    {0x02, "NES"},
    {0x03, "SNES"},
    {0x04, "PCE-2btn"},
    {0x05, "PCE-6btn"},
    {0x06, "PCE-MT"},
    {0x09, "DB15-Fast"},
    {0x0b, "SNES-Swap"},
    {0x10, "PSX"},
    {0x11, "PSX-Fast"},
    {0x12, "PSX-Analog"},
    {0x13, "PSX-Analog-Fast"},
};

static const char *const snac_assign[] = {
    "SNAC->P1, APF->P2",
    "APF->P1, SNAC->P2",
    "SNAC P1->P1, P2->P2",
    "SNAC P1->P2, P2->P1",
};

typedef struct {
    of_input_state_t p1;
    of_input_state_t p2;
    of_keyboard_state_t keyboard;
    of_mouse_state_t mouse;
    of_analogizer_state_t analog;
    int analog_ok;
} view_state_t;

static void clear_row(int row)
{
    GOTO(row, 1);
    printf(CSI "2K");
    GOTO(row, 2);
}

static const char *lookup_name(const char *const *names, int count,
                               uint32_t value, char *fallback,
                               int fallback_size)
{
    if (value < (uint32_t)count)
        return names[value];
    snprintf(fallback, (size_t)fallback_size, "#%lu", (unsigned long)value);
    return fallback;
}

static const char *lookup_id_name(const id_name_t *names, int count,
                                  uint32_t value, char *fallback,
                                  int fallback_size)
{
    for (int i = 0; i < count; ++i) {
        if (names[i].id == value)
            return names[i].name;
    }
    snprintf(fallback, (size_t)fallback_size, "#%02lx", (unsigned long)value);
    return fallback;
}

static char bit(uint32_t buttons, uint32_t mask, char label)
{
    return (buttons & mask) ? label : '.';
}

static const char *source_for_player(const view_state_t *state, int player,
                                     char *out, size_t out_size)
{
    if (!state->analog_ok) {
        snprintf(out, out_size, "unknown");
        return out;
    }

    if (state->analog.snac_type == 0) {
        snprintf(out, out_size, "APF/DOCK");
        return out;
    }

    switch (state->analog.snac_assignment) {
    case 0:
        snprintf(out, out_size, "%s", player == 0 ? "SNAC" : "APF/DOCK");
        break;
    case 1:
        snprintf(out, out_size, "%s", player == 0 ? "APF/DOCK" : "SNAC");
        break;
    case 2:
        snprintf(out, out_size, "%s", player == 0 ? "SNAC1" : "SNAC2");
        break;
    case 3:
        snprintf(out, out_size, "%s", player == 0 ? "SNAC2" : "SNAC1");
        break;
    default:
        snprintf(out, out_size, "APF/DOCK");
        break;
    }
    return out;
}

static const char *mouse_buttons(uint16_t buttons, char out[7])
{
    out[0] = (buttons & 0x01u) ? 'L' : '.';
    out[1] = (buttons & 0x02u) ? 'R' : '.';
    out[2] = (buttons & 0x04u) ? 'M' : '.';
    out[3] = (buttons & 0x08u) ? '4' : '.';
    out[4] = (buttons & 0x10u) ? '5' : '.';
    out[5] = (buttons & 0x20u) ? '6' : '.';
    out[6] = '\0';
    return out;
}

static void format_usage_list(const uint32_t words[OF_KEYBOARD_WORDS],
                              char *out, size_t out_size)
{
    size_t used = 0;
    int count = 0;

    if (out_size == 0)
        return;
    out[0] = '\0';

    for (uint32_t usage = 0; usage < OF_KEYBOARD_MAX_USAGE && count < 8; ++usage) {
        if (((words[usage >> 5] >> (usage & 31)) & 1u) == 0)
            continue;

        int n = snprintf(out + used, out_size - used, "%s%02x",
                         count ? " " : "", usage);
        if (n < 0 || (size_t)n >= out_size - used)
            break;
        used += (size_t)n;
        ++count;
    }

    if (count == 0)
        snprintf(out, out_size, ".");
}

static void draw_frame(void)
{
    CLR;
    GOTO(1, 2);
    FG_CYAN;
    BOLD;
    printf("openfpgaOS Input Stack");
    RESET;

    GOTO(2, 2);
    FG_GRAY;
    DIM;
    printf("public SDK APIs; SNAC appears in P1/P2");
    RESET;

    GOTO(4, 2);
    FG_YELLOW;
    BOLD;
    printf("Analogizer / SNAC");
    RESET;

    GOTO(8, 2);
    FG_YELLOW;
    BOLD;
    printf("Player 1");
    RESET;

    GOTO(13, 2);
    FG_YELLOW;
    BOLD;
    printf("Player 2");
    RESET;

    GOTO(18, 2);
    FG_YELLOW;
    BOLD;
    printf("Keyboard");
    RESET;

    GOTO(22, 2);
    FG_YELLOW;
    BOLD;
    printf("Mouse");
    RESET;
}

static void read_state(view_state_t *state)
{
    memset(state, 0, sizeof(*state));

    of_input_poll();
    of_input_state(0, &state->p1);
    of_input_state(1, &state->p2);
    of_input_keyboard_state(&state->keyboard);
    of_input_mouse_state(&state->mouse);
    state->analog_ok = (of_analogizer_state(&state->analog) == 0);
}

static void update_analogizer(const view_state_t *state)
{
    char video_fallback[16];
    char snac_fallback[16];
    char assign_fallback[16];
    char p1_source[12];
    char p2_source[12];

    const char *video = state->analog_ok ?
        lookup_name(video_modes, (int)(sizeof(video_modes) / sizeof(video_modes[0])),
                    state->analog.video_mode, video_fallback, sizeof(video_fallback)) : "n/a";
    int snac_active = state->analog_ok && state->analog.snac_type != 0;
    const char *snac = state->analog_ok ?
        lookup_id_name(snac_types, (int)(sizeof(snac_types) / sizeof(snac_types[0])),
                       state->analog.snac_type, snac_fallback, sizeof(snac_fallback)) : "n/a";
    const char *assign = snac_active ?
        lookup_name(snac_assign, (int)(sizeof(snac_assign) / sizeof(snac_assign[0])),
                    state->analog.snac_assignment, assign_fallback, sizeof(assign_fallback)) : "no SNAC";

    clear_row(5);
    printf("VideoOut:%-3s Mode:%-9s",
           (state->analog_ok && state->analog.enabled) ? "on" : "off", video);

    clear_row(6);
    if (snac_active) {
        printf("SNAC:%-17.17s Assign:%u",
               snac, (unsigned)state->analog.snac_assignment);
    } else {
        printf("SNAC:%-17.17s Assign:-", snac);
    }

    clear_row(7);
    printf("P1=%-8s P2=%-8s",
           source_for_player(state, 0, p1_source, sizeof(p1_source)),
           source_for_player(state, 1, p2_source, sizeof(p2_source)));

    clear_row(26);
    FG_GRAY;
    DIM;
    printf("Route: %-30.30s", assign);
    RESET;
}

static void print_buttons(uint32_t buttons)
{
    printf("D:%c%c%c%c F:%c%c%c%c S:%c%c L:%c%c R:%c%c C:%c%c",
           bit(buttons, OF_BTN_UP, 'U'),
           bit(buttons, OF_BTN_DOWN, 'D'),
           bit(buttons, OF_BTN_LEFT, 'L'),
           bit(buttons, OF_BTN_RIGHT, 'R'),
           bit(buttons, OF_BTN_A, 'A'),
           bit(buttons, OF_BTN_B, 'B'),
           bit(buttons, OF_BTN_X, 'X'),
           bit(buttons, OF_BTN_Y, 'Y'),
           bit(buttons, OF_BTN_SELECT, 'S'),
           bit(buttons, OF_BTN_START, 'T'),
           bit(buttons, OF_BTN_L1, '1'),
           bit(buttons, OF_BTN_L2, '2'),
           bit(buttons, OF_BTN_R1, '1'),
           bit(buttons, OF_BTN_R2, '2'),
           bit(buttons, OF_BTN_L3, 'L'),
           bit(buttons, OF_BTN_R3, 'R'));
}

static void update_player(int row, const char *label, int player,
                          const of_input_state_t *pad,
                          const view_state_t *state)
{
    char source[12];

    clear_row(row);
    FG_GREEN;
    printf("%s", label);
    RESET;
    printf(" source:%-8s", source_for_player(state, player, source, sizeof(source)));

    clear_row(row + 1);
    print_buttons(pad->buttons);

    clear_row(row + 2);
    printf("LS x:%6d y:%6d LT:%5u",
           pad->joy_lx, pad->joy_ly, (unsigned)pad->trigger_l);

    clear_row(row + 3);
    printf("RS x:%6d y:%6d RT:%5u",
           pad->joy_rx, pad->joy_ry, (unsigned)pad->trigger_r);
}

static void update_keyboard(const view_state_t *state)
{
    char pressed[32];
    const of_keyboard_state_t *k = &state->keyboard;

    format_usage_list(k->keys_pressed, pressed, sizeof(pressed));

    clear_row(19);
    FG_GREEN;
    printf("Present:%-3s", k->present ? "yes" : "no");
    RESET;
    printf(" mod:%04x p:%04x",
           (unsigned)k->modifiers, (unsigned)k->modifiers_pressed);

    clear_row(20);
    printf("Down:%02x %02x %02x %02x %02x %02x",
           k->report_keys[0], k->report_keys[1], k->report_keys[2],
           k->report_keys[3], k->report_keys[4], k->report_keys[5]);

    clear_row(21);
    printf("Pressed:%-28.28s", pressed);
}

static void update_mouse(const view_state_t *state)
{
    char btns[7];
    const of_mouse_state_t *m = &state->mouse;

    clear_row(23);
    FG_GREEN;
    printf("Present:%-3s", m->present ? "yes" : "no");
    RESET;
    printf(" btn:%s dx:%-5ld dy:%-5ld",
           mouse_buttons(m->buttons, btns), (long)m->dx, (long)m->dy);

    clear_row(24);
    printf("Pressed:%04x Released:%04x Ctr:%5u",
           (unsigned)m->buttons_pressed, (unsigned)m->buttons_released,
           (unsigned)m->report_counter);
}

int main(void)
{
    view_state_t state;

    of_video_init();
    of_video_set_display_mode(OF_DISPLAY_TERMINAL);
    draw_frame();

    for (;;) {
        read_state(&state);
        update_analogizer(&state);
        update_player(9, "P1", 0, &state.p1, &state);
        update_player(14, "P2", 1, &state.p2, &state);
        update_keyboard(&state);
        update_mouse(&state);
        usleep(16000);
    }
}
