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
 *   8  stash.sv/.hsv     (shared stash -- nonvolatile)
 *   9  diablo.ini        (settings -- nonvolatile; see the note below, the
 *                         Hellfire instance binds this slot to hellfire.ini)
 *   10-19 single_0..9.sv (single-player saves -- nonvolatile)
 *   20-22 hfmonk/hfmusic/hfvoice.mpq (Hellfire expansion data)
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

/* Bind `name` -> `slot` unless the kernel registry already resolves `name`.
 *
 * The registry (kernel syscall.c file_slot_register) is an APPEND-ONLY
 * 32-entry table: it does not de-duplicate by name, and once full it returns
 * silently. Boot discovery (filesystem_init -> dir_probe_slots) already binds
 * every filename the instance JSON declares -- 21 of them in the Hellfire
 * layout -- so re-registering blindly is what overflowed the table and
 * dropped the bindings at the tail. On Pocket of_file_slot_find() is exactly
 * a registry lookup (of_file_resolve_name() returns -1 here), so this is an
 * accurate "already bound?" test rather than a guess. */
static void bind_if_unbound(uint32_t slot, const char *name)
{
	uint32_t bound;
	if (of_file_slot_find(name, &bound) == 0)
		return;
	of_file_slot_register(slot, name);
}

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

	/* Settings FIRST -- this is the one binding the game cannot do without
	 * and the one the Hellfire instance never supplies.
	 *
	 * DevilutionX always opens "diablo.ini": options.cpp GetIniPath() hard-
	 * codes that name in BOTH modes. The Hellfire instance JSON, however,
	 * binds slot 9 to "hellfire.ini", so boot discovery never registers
	 * "diablo.ini" there and it has to come from here. It used to be
	 * registered last, past the point where the 32-entry registry had
	 * filled up, so the binding was silently dropped and every settings
	 * write died with ENOENT ("Failed to write ini file to diablo.ini: No
	 * such file or directory") -- settings never persisted in Hellfire.
	 * Diablo mode was unaffected because its instance binds "diablo.ini"
	 * directly, which is why this survived testing.
	 *
	 * A write needs a REGISTRY binding specifically: Pocket's
	 * of_file_resolve_name() and of_file_config_slot() both return -1, and
	 * sys_openat's remaining fallbacks only match ".sav"-style names, so an
	 * unbound writable name cannot be opened at all. */
	bind_if_unbound(9, "diablo.ini");

	/* Read-only data slots are deliberately NOT registered here.
	 *
	 * When the file is present, boot discovery has already bound it and a
	 * call here is a no-op. When it is absent, binding the name is worse than
	 * useless: the slot has no file, so sys_openat's size check rejects the
	 * open with ENOENT anyway -- we would only have spent one of the 32
	 * registry entries to arrive at the same answer. In the Diablo layout
	 * that reclaims four entries (hellfire + the three hf*.mpq) which the
	 * save aliases below genuinely need. */

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
	 * native save-on-exit writeback runs (menu exit / sleep).
	 *
	 * DevilutionX picks the save extension from the GAME MODE, not from the
	 * instance (pfile.cpp GetSavePath): ".sv" for Diablo, ".hsv" for
	 * Hellfire. Those two normally agree, but they are decided independently
	 * and CAN disagree: init.cpp:341 sets gbIsHellfire only if hellfire.mpq
	 * actually loads, so a Hellfire instance whose expansion MPQs fail to
	 * load runs as Diablo and opens ".sv" while the instance bound ".hsv".
	 *
	 * Both spellings are therefore registered. An earlier version of this
	 * file registered only the instance's own extension to save table
	 * entries; that produced exactly the failure above -- `single_0.sv`
	 * unbound, `fopen(..., "ab")` -> ENOENT, MpqWriter disabling the save
	 * subsystem, then a hard app_fatal("Unable to open archive") out of
	 * pfile_read_player_from_save. Never trade this insurance for table
	 * space: bind_if_unbound already makes the matching spelling free
	 * (discovery bound it), so the real cost is only the 10 aliases for the
	 * extension this run does not use.
	 *
	 * Order matters. Registration is append-only and silently stops at 32,
	 * so the entries are laid out most-critical-first: the ini above, then
	 * the hero slots here, then the stash last. If anything is dropped it is
	 * the tail -- and the tail is the opposite-extension stash alias, the one
	 * name in this whole list that no build ever opens. */
	for (int i = 0; i < 10; i++) {
		char name[24];
		snprintf(name, sizeof name, "single_%d.sv", i);
		bind_if_unbound(10 + i, name);
		snprintf(name, sizeof name, "single_%d.hsv", i);
		bind_if_unbound(10 + i, name);
	}

	/* Shared stash (DevilutionX GetStashSavePath -> "stash.sv" / "stash.hsv").
	 * Previously UNREGISTERED: every stash save silently failed (MpqWriter's
	 * "r+b" open found no slot, valid_=false) and the stash reset each boot.
	 * Slot 8 is the otherwise-unused nonvolatile window at 0x20380000
	 * (data.json binds its SD file). Both spellings for the same reason as
	 * the hero slots, and LAST because this is the one pair that can be
	 * safely lost to a full table: the mode-matching name is already bound by
	 * discovery, and the other is never opened.
	 * NOTE: shareware ("stash_spawn.sv") and multiplayer ("multi_N.sv")
	 * names remain unregistered -- those modes have no slots on this core. */
	bind_if_unbound(8, "stash.sv");
	bind_if_unbound(8, "stash.hsv");

	/* The MPQs are opened directly by basename via the slot service. */
}

/* DevilutionX BasePath: empty so MPQs resolve by basename. */
const char *of_platform_base_path(void) { return ""; }

/* DevilutionX PrefPath/ConfigPath: empty, so bare names (diablo.ini,
 * single_N.sv, devilutionx.mpq, fonts.mpq) resolve to the slots above. */
const char *of_platform_pref_path(void) { return ""; }
