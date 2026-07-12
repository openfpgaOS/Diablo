/*
 * of_gpu_bridge.c -- the single translation unit that includes of_gpu.h.
 * (The header carries per-app static ring/palookup state and must be
 * included from exactly one TU -- see of_gpu.h "Ring Buffer State".)
 *
 * EXPERIMENTAL GPU bring-up scaffolding, built only with `make GPU=1`
 * (-DOF_DIABLO_GPU). Stage 1 -- this file -- does no per-frame rendering:
 *
 *   - probe + boot-time feature report (which OF_HW_GPU_* bits the running
 *     bitstream actually sets; the os20 2D variant is expected to report
 *     SPAN/FRAGPIPE/PARAM_SPAN_LIST/ALPHA but NOT SPAN_GROUP/COLUMN_LIST);
 *   - LightTables -> palookup slot 0 upload (Diablo's 16 light levels map
 *     onto rows 0..15 of the 64-row shade axis; rows 16..63 repeat black);
 *   - paletteTransparencyLookup -> GPU translucency RAM upload (the fabric
 *     LUT is 128x256 -- the low bit of the source axis is dropped, so GPU
 *     blends are quantised vs. the CPU's 256x256 table);
 *   - a boot smoke test: clear_rect + one COLORMAP span through the
 *     long-form param-span-list command (the only span form that decodes
 *     on lean variants), fenced and verified by CPU readback.
 *
 * Stage 2 (GPU floor pass) builds on this once the feature report and
 * smoke test pass on os20 hardware. Until then the game's render path is
 * untouched: the smoke test draws before the first game frame and is
 * immediately overdrawn.
 *
 * NOTE: color cycling (CycleColors, caves/crypt/nest) rotates the CPU
 * transparency table per game tick but nothing re-uploads it here; the
 * GPU copy goes stale. Irrelevant while the GPU renders nothing --
 * revisit when translucent surfaces move to the GPU.
 */
#include "of.h"
#include "of_gpu.h"

#include <stdio.h>
#include <string.h>

static int g_probe_state; /* 0 = unprobed, 1 = ready, -1 = unavailable */

static void report_features(uint32_t feat)
{
	static const struct { uint32_t bit; const char *name; } kBits[] = {
		{ OF_HW_GPU_SPAN, "span" },
		{ OF_HW_GPU_FRAGPIPE, "fragpipe" },
		{ OF_HW_GPU_PARAM_SPAN_LIST, "param-span-list" },
		{ OF_HW_GPU_ALPHA, "alpha" },
		{ OF_HW_GPU_PERSP, "persp" },
		{ OF_HW_GPU_SPAN_GROUP, "span-group" },
		{ OF_HW_GPU_COLUMN_LIST, "column-list" },
		{ OF_HW_GPU_PARAM_TRI, "param-tri" },
		{ OF_HW_GPU_VERT_TRI, "vert-tri" },
		{ OF_HW_GPU_VCOLOR, "vcolor" },
		{ OF_HW_GPU_FAST_TEX, "fast-tex" },
	};
	printf("[of] gpu features:");
	for (unsigned i = 0; i < sizeof(kBits) / sizeof(kBits[0]); i++)
		if (feat & kBits[i].bit)
			printf(" %s", kBits[i].name);
	printf("\n");
}

/* Probe once. Returns 1 when the GPU exists and supports the pieces the
 * Diablo offload path needs (baseline spans through the long-form
 * param-span-list, 1px/cycle fragment pipe). */
int of_gpub_ready(void)
{
	if (g_probe_state != 0)
		return g_probe_state == 1;

	const struct of_capabilities *caps = of_get_caps();
	if (caps == NULL || caps->gpu_base == 0 || caps->sdram_base == 0) {
		printf("[of] gpu: no GPU on this core (gpu_base=0)\n");
		g_probe_state = -1;
		return 0;
	}
	of_gpu_init();
	report_features(caps->hw_features);
	if (!of_has_feature(OF_HW_GPU_SPAN)
	    || !of_has_feature(OF_HW_GPU_FRAGPIPE)
	    || !of_has_feature(OF_HW_GPU_PARAM_SPAN_LIST)) {
		printf("[of] gpu: missing span/fragpipe/param-span-list; GPU path disabled\n");
		g_probe_state = -1;
		return 0;
	}
	g_probe_state = 1;
	return 1;
}

/* Pack DevilutionX's LightTables (`levels` rows x 256 entries, level 0 =
 * fully lit) into palookup slot 0's 64-row shade axis. Span commands then
 * select the light level with light_origin = level << 16. Rows past the
 * last level repeat it (fully dark), so out-of-range shades clamp black
 * instead of sampling garbage. */
void of_gpub_upload_light_tables(const uint8_t *tables, unsigned levels)
{
	static uint8_t slab[64 * 256];

	if (!of_gpub_ready() || tables == NULL || levels == 0 || levels > 64)
		return;
	for (unsigned row = 0; row < 64; row++) {
		const unsigned src = row < levels ? row : levels - 1;
		memcpy(&slab[row * 256], &tables[src * 256], 256);
	}
	of_gpu_palookup_upload(0, slab, sizeof(slab));
	printf("[of] gpu: light tables -> palookup slot 0 (%u levels)\n", levels);
}

/* Upload the 64 KB paletteTransparencyLookup to GPU translucency RAM. */
void of_gpub_upload_translucency(const uint8_t *table)
{
	if (!of_gpub_ready() || table == NULL)
		return;
	of_gpu_translucency_upload(table, 65536);
	printf("[of] gpu: translucency table uploaded\n");
}

/* Shared header setup for the smoke/probe spans: affine COLORMAP spans,
 * one texel per pixel, sampling `tex` (32x32 ramp) via palookup slot 0. */
static void span_header(of_gpu_param_span_list_t *p, uint8_t *fb_base,
    uint32_t stride, uint32_t tex_addr)
{
	memset(p, 0, sizeof(*p));
	p->fb_base = (uint32_t)(uintptr_t)fb_base;
	p->fb_major_step = (int32_t)stride; /* v steps one FB row */
	p->fb_minor_step = 1;               /* u steps one pixel */
	p->tex_addr = tex_addr;
	p->tex_width = 32;
	p->tex_w_mask = 31;
	p->tex_h_mask = 31;
	p->flags = OF_GPU_SPAN_COLORMAP;
	p->colormap_id = 0;
	p->attr_mode = OF_GPU_PARAM_ATTR_AFFINE;
	p->span_axis = OF_GPU_PARAM_AXIS_X;
	p->z_mode = OF_GPU_PARAM_Z_NONE;
	p->attr_du[0] = 1 << 16; /* s: one texel per pixel */
	p->attr_dv[1] = 1 << 16; /* t: one texel row per span row */
	p->light_origin = 0 << 16;
}

/* Boot smoke test + fill-rate probe. Self-contained: uploads an IDENTITY
 * table to palookup slot 0 first (MakeLightTable overwrites it with the
 * real light tables at level load), sentinel-fills the target rows, then
 * draws a clear_rect strip and one 32px COLORMAP span sampling a ramp
 * texture -- so the expected readback is unambiguous: 0xAA = span did not
 * write, [00 01 02 ..] = span + palookup both work. Follows with a
 * throughput probe (2000 x 32px span records) timed CPU-side, printing
 * px/us -- the number that sizes the stage-2 floor pass (a full 640x352
 * floor is ~220K span pixels). Draws into the CURRENT CPU draw buffer;
 * the first game frame overwrites it. */
void of_gpub_smoke_test(void)
{
	static uint8_t tex[32 * 32] __attribute__((aligned(64)));
	static uint8_t identity[256];

	if (!of_gpub_ready())
		return;

	of_video_mode_t mode;
	of_video_get_mode(&mode);
	uint8_t *fb = of_video_surface();
	const uint32_t stride = mode.stride != 0 ? mode.stride : mode.width;
	if (fb == NULL || mode.width < 64 || mode.height < 48) {
		printf("[of] gpu smoke: video mode unusable, skipped\n");
		return;
	}

	for (unsigned i = 0; i < sizeof(identity); i++)
		identity[i] = (uint8_t)i;
	of_gpub_upload_light_tables(identity, 1);

	for (unsigned i = 0; i < sizeof(tex); i++)
		tex[i] = (uint8_t)(i & 0xFF);
	memset(fb + (size_t)4 * stride, 0xAA, 64); /* span-row sentinel */

	/* Hand the CPU-written texture and FB region to the GPU: flush so no
	 * dirty D-cache lines shadow what the AXI masters read, and so the
	 * CPU re-reads the GPU's pixels instead of stale lines afterwards. */
	of_cache_flush_range(tex, sizeof(tex));
	of_cache_flush_range(fb, (size_t)stride * 48);

	of_gpu_set_framebuffer((uint32_t)(uintptr_t)fb, (uint16_t)stride);
	of_gpu_clear_rect_strided((uint32_t)(uintptr_t)fb, 64, 4, (uint16_t)stride, 0x5A);

	of_gpu_param_span_list_t p;
	of_gpu_param_span_record_t rec[4];
	span_header(&p, fb + (size_t)4 * stride, stride, (uint32_t)(uintptr_t)tex);
	for (unsigned i = 0; i < 4; i++) {
		rec[i].u = 0;
		rec[i].v = (uint16_t)i;
		rec[i].count = 32;
	}
	of_gpu_draw_param_span_list(&p, rec, 4);

	const uint32_t token = of_gpu_fence();
	of_gpu_kick_now();
	of_gpu_wait(token);

	int clear_ok = 1;
	for (unsigned x = 0; x < 64; x++)
		if (fb[x] != 0x5A)
			clear_ok = 0;
	const uint8_t *span = fb + (size_t)4 * stride;
	int span_ok = 1;
	for (unsigned x = 0; x < 32; x++)
		if (span[x] != (uint8_t)x)
			span_ok = 0;
	printf("[of] gpu smoke: clear=%s span=%s [%02x %02x %02x %02x %02x %02x %02x %02x] fence=%lu\n",
	    clear_ok ? "ok" : "FAIL", span_ok ? "ok" : "FAIL",
	    span[0], span[1], span[2], span[3], span[4], span[5], span[6], span[7],
	    (unsigned long)token);

	/* Fill-rate probe, two record widths over the same 64,000 pixels
	 * (rows 8..39, overdraw fine -- this measures throughput not
	 * output). Includes command build + DMA + raster + fence, i.e. the
	 * end-to-end cost a floor pass would pay. Comparing 32px records
	 * (floor-tile shaped, 2000 records) against 64px records (1000
	 * records) separates the fixed per-record overhead from the true
	 * per-pixel rate: with times A and B,
	 *   per-record us = (A - B) / 1000, per-pixel us = rest / 64000. */
	{
		static of_gpu_param_span_record_t precs[500];
		unsigned us[2];
		const struct { uint16_t count, ustep, umask; unsigned batches; } cfg[2] = {
			{ 32, 64, 7, 4 },   /* 4 x 500 x 32px */
			{ 64, 128, 3, 2 },  /* 2 x 500 x 64px */
		};
		for (unsigned v = 0; v < 2; v++) {
			const unsigned t0 = of_time_us();
			for (unsigned batch = 0; batch < cfg[v].batches; batch++) {
				span_header(&p, fb + (size_t)8 * stride, stride,
				    (uint32_t)(uintptr_t)tex);
				for (unsigned i = 0; i < 500; i++) {
					precs[i].u = (uint16_t)((i & cfg[v].umask) * cfg[v].ustep);
					precs[i].v = (uint16_t)((i / (cfg[v].umask + 1u)) & 31);
					precs[i].count = cfg[v].count;
				}
				of_gpu_draw_param_span_list(&p, precs, 500);
			}
			const uint32_t ptoken = of_gpu_fence();
			of_gpu_kick_now();
			of_gpu_wait(ptoken);
			us[v] = of_time_us() - t0;
		}
		printf("[of] gpu probe: 32px: 64000 px in %u us (%u px/us); 64px: in %u us (%u px/us)\n",
		    us[0], us[0] != 0 ? 64000u / us[0] : 0u,
		    us[1], us[1] != 0 ? 64000u / us[1] : 0u);
	}
}
