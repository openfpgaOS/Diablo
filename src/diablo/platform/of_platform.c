/*
 * of_platform.c -- Diablo platform provisioning for openfpgaOS.
 *
 * Wires DevilutionX's file model onto the Analogue Pocket's APF data slots:
 *   - Maps the Diablo data MPQs directly from the core's common folder.
 *     DevilutionX's BasePath is empty, so it opens DIABDAT.MPQ and the
 *     optional Hellfire MPQs by basename through the slot file service.
 *   - Maps the bare filenames DevilutionX opens for its own assets, config,
 *     and saves (devilutionx.mpq, fonts.mpq, diablo.ini, single_N.sv) to
 *     data/save slots; the SDK resolves fopen() by basename, so
 *     PrefPath/ConfigPath are empty.
 *
 * The SDL shim (of_sdl2.cpp) calls of_platform_init() from SDL_Init and
 * returns these paths from SDL_GetBasePath()/SDL_GetPrefPath(), so no
 * DevilutionX source changes are needed.
 *
 * Slot layout (must match the instance JSON in dist/diablo/):
 *   4  DIABDAT.mpq       (Diablo game data)
 *   5  devilutionx.mpq   (DevilutionX's own assets -- REQUIRED, not on the CD)
 *   6  fonts.mpq         (extra fonts -- optional)
 *   7  hellfire.mpq      (Hellfire expansion -- optional)
 *   --  hfmonk/hfmusic/hfvoice.mpq: NO readable slot (ids 0-19 only;
 *       4-7 are taken). Not loadable here -- Hellfire mode is incomplete.
 *   10-19 single_0..9.sv (single-player saves -- nonvolatile)
 */
#include "of.h"

#include <stdio.h>

/* Startup banner: prints before main() so the serial log immediately shows
 * whether the freshly-built binary is the one actually running on the SD. */
__attribute__((constructor)) static void of_diablo_banner(void)
{
	printf("[of] Diablo core build %s %s\n", __DATE__, __TIME__);
}

static int g_done;

void of_platform_init(void)
{
	if (g_done)
		return;
	g_done = 1;

	/* Switch the display to the 640x480 app framebuffer immediately — this runs
	 * from the first SDL_GetBasePath()/SDL_Init() call, before DevilutionX loads
	 * its archives, so the boot terminal/console isn't shown during startup. The
	 * game's own SDL video init re-runs this harmlessly (idempotent). On a crash
	 * the OS trap handler flips the display back to the terminal. */
	{
		of_video_mode_t want = { 640, 480, 0, OF_VIDEO_MODE_8BIT, 0 };
		of_video_init();
		of_video_set_mode(&want);
	}

	/* Read-only data slots. The kernel auto-discovers APF filenames at
	 * boot; registering here makes fopen() resolution explicit and
	 * order-independent. */
	of_file_slot_register(4, "DIABDAT.mpq");
	of_file_slot_register(5, "devilutionx.mpq");
	of_file_slot_register(6, "fonts.mpq");
	of_file_slot_register(7, "hellfire.mpq");
	/* Hellfire monk/music/voice MPQs at ids 20-22. The OS datatable scan
	 * (targets/pocket/file.c datatable_entry_scan_for_slot) resolves ids
	 * beyond the old 0-19 hardcoded map, and the APF 32-slot cap leaves room,
	 * so these load like any other read-only data slot. Hellfire mode requires
	 * all three (see init.cpp:357) — without them DevilutionX quits. */
	of_file_slot_register(20, "hfmonk.mpq");
	of_file_slot_register(21, "hfmusic.mpq");
	of_file_slot_register(22, "hfvoice.mpq");

	/* Single-player save slots (nonvolatile, slots 10-19).
	 *
	 * Re-enabled now that the OS's `sys_close` (syscall.c:1430-1449) does
	 * NOT issue a synchronous `DS_CMD_WRITE` for save fds -- the launcher's
	 * native save-on-exit path persists CRAM to SD. So writes are pure
	 * CPU->CRAM and the original "writeback stall" reboot pattern doesn't
	 * apply on the current OS. DevilutionX hits these slots via
	 * `MpqWriter`/`pfile_read_player_from_save` (`single_N.sv`).
	 *
	 * Config slot 9 (`diablo.ini`) rides the SAME mechanism: save.c's
	 * nvslot_map sends ids 8/9 to the CRAM0 pre-save window, and sys_close
	 * treats every nvslot fd identically (datatable size commit only, no
	 * synchronous bridge write), so the old first-write reset class cannot
	 * fire. The instance JSON previously bound diablo.ini to slot 2 (the
	 * read-only OS Config slot) -- that was the "Permission denied" on
	 * SaveIni; it now binds slot 9. Settings persist when the Pocket's
	 * native save-on-exit writeback runs (menu exit / sleep). */
	for (int i = 0; i < 10; i++) {
		char name[24];
		snprintf(name, sizeof name, "single_%d.sv", i);
		of_file_slot_register(10 + i, name);
	}
	of_file_slot_register(9, "diablo.ini");
	of_file_slot_register(9, "hellfire.ini"); /* Hellfire instance binds this name */

	/* The MPQs are opened directly by basename via the slot service. */
}

/* DevilutionX BasePath: empty so MPQs resolve by basename. */
const char *of_platform_base_path(void) { return ""; }

/* DevilutionX PrefPath/ConfigPath: empty, so bare names (diablo.ini,
 * single_N.sv, devilutionx.mpq, fonts.mpq) resolve to the slots above. */
const char *of_platform_pref_path(void) { return ""; }
