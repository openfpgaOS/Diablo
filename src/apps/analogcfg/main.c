//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * analogcfg -- shared analogizer config editor
 *
 * Reads the current analogizer state via of_analogizer_state(), lets
 * the user edit it, and writes the result to a fixed shared config
 * file `analogizer.cfg`.  The kernel will (Phase 2) read the file at
 * boot and apply the saved state before any app launches, so every
 * other app in the SDK can rely on of_analogizer_state() reflecting
 * the user's preferences without carrying its own configuration menu.
 *
 * Live-preview hooks (of_analogizer_apply / of_analogizer_persist)
 * are stubbed via direct fopen for now; the kernel side that actually
 * applies the file at boot is the next phase of the plan and is not
 * required for the file format to be correct.
 *
 * Controls (P1 gamepad):
 *   D-pad Up/Down    move cursor between fields
 *   D-pad Left/Right change selected field's value
 *   A                save to analogizer.cfg and exit
 *   B                exit without saving
 *   Start            restore the values that were live on entry
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "of.h"

/* On-disk format — keep in lockstep with the plan in
 * ~/Repos/analogizer_config_app.md.  Fixed 32 bytes, little-endian.
 * Magic + version + CRC let the (future) kernel reader validate
 * before applying. */
#define ANALOGCFG_MAGIC    0x47464341u  /* 'ACFG' */
#define ANALOGCFG_VERSION  1
#define ANALOGCFG_PATH     "analogizer.cfg"

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint8_t  enabled;
    uint8_t  video_mode;
    uint8_t  snac_type;
    uint8_t  snac_assignment;
    int8_t   h_offset;
    int8_t   v_offset;
    uint8_t  reserved[2];
    uint32_t crc32;
    uint8_t  pad[12];
} analogcfg_t;

/* CRC32 (IEEE 802.3 / zlib polynomial), bytewise — small + sufficient
 * for a 20-byte struct.  Matches the polynomial the kernel reader will
 * use; both sides agree because both compute it the same way over the
 * same byte range. */
static uint32_t crc32_compute(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return ~crc;
}

/* CRC covers magic..reserved (everything before the crc32 field). */
#define ANALOGCFG_CRC_LEN  offsetof(analogcfg_t, crc32)

/* ---- field model ----------------------------------------------- */

typedef enum {
    F_ENABLED,
    F_VIDEO_MODE,
    F_SNAC_TYPE,
    F_SNAC_ASSIGN,
    F_H_OFFSET,
    F_V_OFFSET,
    F_COUNT
} field_id_t;

static const char *VIDEO_MODES[] = {
    "RGB", "NTSC", "PAL", "YC-NTSC", "YC-PAL", "SCART"
};
static const int N_VIDEO_MODES = sizeof(VIDEO_MODES) / sizeof(VIDEO_MODES[0]);

static const char *SNAC_TYPES[] = {
    "None", "DB15", "NES", "SNES", "PCE-2btn", "PCE-6btn", "PCE-MT",
    "PSX", "PSX-Fast", "PSX-Analog", "PSX-Analog-Fast"
};
static const int N_SNAC_TYPES = sizeof(SNAC_TYPES) / sizeof(SNAC_TYPES[0]);

static const char *SNAC_ASSIGN[] = {
    "SNAC->P1, APF->P2",
    "APF->P1, SNAC->P2",
    "SNAC P1->P1, P2->P2",
    "SNAC P1->P2, P2->P1",
};
static const int N_SNAC_ASSIGN = sizeof(SNAC_ASSIGN) / sizeof(SNAC_ASSIGN[0]);

static const char *FIELD_LABEL[F_COUNT] = {
    "Output       ",
    "Video mode   ",
    "SNAC type    ",
    "SNAC assign  ",
    "H offset     ",
    "V offset     ",
};

/* clamp helper */
static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Step a field by `delta` (typically -1 / +1).  Each field has its
 * own value range; we clamp to keep the on-disk struct lawful. */
static void field_step(of_analogizer_state_t *s, field_id_t f, int delta) {
    switch (f) {
    case F_ENABLED:
        s->enabled = (uint8_t)((s->enabled + delta + 2) & 1);
        break;
    case F_VIDEO_MODE:
        s->video_mode = (uint8_t)clamp((int)s->video_mode + delta,
                                       0, N_VIDEO_MODES - 1);
        break;
    case F_SNAC_TYPE:
        s->snac_type = (uint8_t)clamp((int)s->snac_type + delta,
                                      0, N_SNAC_TYPES - 1);
        break;
    case F_SNAC_ASSIGN:
        s->snac_assignment = (uint8_t)clamp((int)s->snac_assignment + delta,
                                            0, N_SNAC_ASSIGN - 1);
        break;
    case F_H_OFFSET: s->h_offset = (int8_t)clamp(s->h_offset + delta, -16, 16); break;
    case F_V_OFFSET: s->v_offset = (int8_t)clamp(s->v_offset + delta, -16, 16); break;
    case F_COUNT: break;
    }
}

/* Pretty-print one field's value into a small buffer.  `arrow_left` /
 * `arrow_right` show whether the current value can step in that
 * direction, so the user can tell at a glance that they've hit a
 * range boundary. */
static void field_render(const of_analogizer_state_t *s, field_id_t f,
                         char *out, size_t cap) {
    switch (f) {
    case F_ENABLED:
        snprintf(out, cap, "%s", s->enabled ? "On" : "Off");
        break;
    case F_VIDEO_MODE:
        snprintf(out, cap, "%s",
                 (s->video_mode < N_VIDEO_MODES) ? VIDEO_MODES[s->video_mode] : "?");
        break;
    case F_SNAC_TYPE:
        snprintf(out, cap, "%s",
                 (s->snac_type < N_SNAC_TYPES) ? SNAC_TYPES[s->snac_type] : "?");
        break;
    case F_SNAC_ASSIGN:
        snprintf(out, cap, "%s",
                 (s->snac_assignment < N_SNAC_ASSIGN) ? SNAC_ASSIGN[s->snac_assignment] : "?");
        break;
    case F_H_OFFSET: snprintf(out, cap, "%+d", s->h_offset); break;
    case F_V_OFFSET: snprintf(out, cap, "%+d", s->v_offset); break;
    case F_COUNT: out[0] = 0; break;
    }
}

/* ---- persistence ----------------------------------------------- */

static int cfg_write(const of_analogizer_state_t *s) {
    analogcfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic           = ANALOGCFG_MAGIC;
    cfg.version         = ANALOGCFG_VERSION;
    cfg.flags           = 0;
    cfg.enabled         = s->enabled;
    cfg.video_mode      = s->video_mode;
    cfg.snac_type       = s->snac_type;
    cfg.snac_assignment = s->snac_assignment;
    cfg.h_offset        = s->h_offset;
    cfg.v_offset        = s->v_offset;
    cfg.crc32           = crc32_compute(&cfg, ANALOGCFG_CRC_LEN);

    FILE *f = fopen(ANALOGCFG_PATH, "wb");
    if (!f) return -1;
    size_t n = fwrite(&cfg, 1, sizeof(cfg), f);
    int err = fclose(f);
    return (n == sizeof(cfg) && err == 0) ? 0 : -1;
}

/* ---- UI -------------------------------------------------------- */

static void ui_redraw(const of_analogizer_state_t *s, int cursor,
                      const char *status) {
    /* OVERLAY mode: terminal text is composited over app FB.  Use
     * VT control sequences (clear + home) so each redraw replaces
     * the previous frame in place rather than scrolling. */
    printf("\x1b[2J\x1b[H");
    printf("ANALOGIZER\n");
    printf("==========\n\n");
    for (int f = 0; f < F_COUNT; f++) {
        char val[40];
        field_render(s, (field_id_t)f, val, sizeof(val));
        printf("  %c %s : %s\n",
               cursor == f ? '>' : ' ',
               FIELD_LABEL[f], val);
    }
    printf("\n");
    printf("D-pad : navigate / change   A : save & exit\n");
    printf("B : cancel    Start : revert to entry values\n");
    if (status && *status) printf("\n%s\n", status);
}

int main(void) {
    of_video_init();
    /* TERMINAL mode: scanout reads the OS terminal FB directly so
     * every printf is immediately visible without an of_video_flip()
     * cycle.  OVERLAY mode would dim an app FB underneath the text,
     * but this app has no FB drawing — dropping into TERMINAL is the
     * right idiom for a pure-text config UI. */
    of_video_set_display_mode(OF_DISPLAY_TERMINAL);

    of_analogizer_state_t state, original;
    memset(&state, 0, sizeof(state));
    if (of_analogizer_state(&state) != 0) {
        printf("[analogcfg] of_analogizer_state failed; starting from zero\n");
        memset(&state, 0, sizeof(state));
    }
    original = state;

    int cursor = 0;
    char status[64] = "";
    ui_redraw(&state, cursor, status);

    while (1) {
        of_input_poll();

        int dirty = 0;

        if (of_btn_pressed(OF_BTN_UP)) {
            cursor = (cursor - 1 + F_COUNT) % F_COUNT;
            dirty = 1;
        }
        if (of_btn_pressed(OF_BTN_DOWN)) {
            cursor = (cursor + 1) % F_COUNT;
            dirty = 1;
        }
        if (of_btn_pressed(OF_BTN_LEFT)) {
            field_step(&state, (field_id_t)cursor, -1);
            dirty = 1;
        }
        if (of_btn_pressed(OF_BTN_RIGHT)) {
            field_step(&state, (field_id_t)cursor, +1);
            dirty = 1;
        }
        if (of_btn_pressed(OF_BTN_START)) {
            state = original;
            snprintf(status, sizeof(status), "Reverted to entry values.");
            dirty = 1;
        }
        if (of_btn_pressed(OF_BTN_A)) {
            int rc = cfg_write(&state);
            if (rc == 0) {
                snprintf(status, sizeof(status),
                         "Saved to " ANALOGCFG_PATH ". Reboot to apply.");
            } else {
                snprintf(status, sizeof(status),
                         "Save failed (rc=%d). Settings NOT persisted.", rc);
            }
            ui_redraw(&state, cursor, status);
            /* Stick around so the user can read the status line, then
             * exit on next B-press.  Don't auto-exit: a successful
             * save should still let the user verify. */
            while (!of_btn_pressed(OF_BTN_B) && !of_btn_pressed(OF_BTN_A)) {
                of_input_poll();
            }
            return 0;
        }
        if (of_btn_pressed(OF_BTN_B)) {
            return 0;
        }

        if (dirty) ui_redraw(&state, cursor, status);
    }
}
