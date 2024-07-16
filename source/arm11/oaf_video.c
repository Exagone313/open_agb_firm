/*
 *   This file is part of open_agb_firm
 *   Copyright (C) 2024 profi200
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <math.h>
#include <string.h>
#include "types.h"
#include "arm11/config.h"
#include "arm11/drivers/gx.h"
#include "util.h"
#include "oaf_error_codes.h"
#include "arm11/drivers/lgycap.h"
#include "arm11/bitmap.h"
#include "drivers/gfx.h"
#include "arm11/drivers/mcu.h"
#include "arm11/fmt.h"
#include "fsutil.h"
#include "kernel.h"
#include "kevent.h"
#include "arm11/gpu_cmd_lists.h"
#include "arm11/drivers/hid.h"



static void adjustGammaTableForGba(void)
{
	// Credits for this algo go to Extrems.
	const float targetGamma = g_oafConfig.gbaGamma;
	const float lcdGamma    = 1.f / g_oafConfig.lcdGamma;
	const float contrast    = g_oafConfig.contrast;
	const float brightness  = g_oafConfig.brightness / contrast;
	const float contrastInTargetGamma = powf(contrast, targetGamma);
	vu32 *const color_lut_data = &getGxRegs()->pdc0.color_lut_data;
	for(u32 i = 0; i < 256; i++)
	{
		// Adjust i with brightness and convert to target gamma.
		const float adjusted = powf((float)i / 255 + brightness, targetGamma);

		// Apply contrast, convert to LCD gamma, round to nearest and clamp.
		const u32 res = clamp_s32(lroundf(powf(contrastInTargetGamma * adjusted, lcdGamma) * 255), 0, 255);

		// Same adjustment for red/green/blue.
		*color_lut_data = res<<16 | res<<8 | res;
	}
}

static Result dumpFrameTex(void)
{
	// Stop LgyCap before dumping the frame to prevent glitches.
	LGYCAP_stop(LGYCAP_DEV_TOP);

	// A1BGR5 format (alpha ignored).
	constexpr u32 alignment = 0x80; // Make PPF happy.
	alignas(4) static BmpV1WithMasks bmpHeaders =
	{
		{
			.magic       = 0x4D42,
			.fileSize    = alignment + 240 * 160 * 2,
			.reserved    = 0,
			.reserved2   = 0,
			.pixelOffset = alignment
		},
		{
			.headerSize      = sizeof(Bitmapinfoheader),
			.width           = 240,
			.height          = -160,
			.colorPlanes     = 1,
			.bitsPerPixel    = 16,
			.compression     = BI_BITFIELDS,
			.imageSize       = 240 * 160 * 2,
			.xPixelsPerMeter = 0,
			.yPixelsPerMeter = 0,
			.colorsUsed      = 0,
			.colorsImportant = 0
		},
		.rMask = 0xF800,
		.gMask = 0x07C0,
		.bMask = 0x003E
	};

	u32 outDim   = PPF_DIM(240, 160);
	u32 fileSize = alignment + 240 * 160 * 2;
	if(g_oafConfig.scaler > 1)
	{
		outDim   = PPF_DIM(360, 240);
		fileSize = alignment + 360 * 240 * 2;

		bmpHeaders.header.fileSize = fileSize;
		bmpHeaders.dib.width     = 360;
		bmpHeaders.dib.height    = -240;
		bmpHeaders.dib.imageSize = 360 * 240 * 2;
	}

	// Transfer frame data out of the 512x512 texture.
	// We will use the currently hidden frame buffer as temporary buffer.
	// Note: This is a race with the currently displaying frame buffer
	//       because we just swapped buffers in the gfx handler function.
	u32 *const tmpBuf = GFX_getBuffer(GFX_LCD_TOP, GFX_SIDE_LEFT);
	GX_displayTransfer((u32*)0x18200000, PPF_DIM(512, 240), tmpBuf + (alignment / 4), outDim,
	                   PPF_O_FMT(GX_A1BGR5) | PPF_I_FMT(GX_A1BGR5) | PPF_CROP_EN);
	memcpy(tmpBuf, &bmpHeaders, sizeof(bmpHeaders));
	GFX_waitForPPF();

	// Get current date & time.
	RtcTimeDate td;
	MCU_getRtcTimeDate(&td);

	// Construct file path from date & time. Then write the file.
	char fn[36];
	ee_sprintf(fn, OAF_SCREENSHOT_DIR "/%04X_%02X_%02X_%02X_%02X_%02X.bmp",
	           td.y + 0x2000, td.mon, td.d, td.h, td.min, td.s);
	const Result res = fsQuickWrite(fn, tmpBuf, fileSize);

	// Restart LgyCap.
	LGYCAP_start(LGYCAP_DEV_TOP);

	return res;
}

static void gbaGfxHandler(void *args)
{
	const KHandle event = (KHandle)args;

	while(1)
	{
		if(waitForEvent(event) != KRES_OK) break;
		clearEvent(event);

		// All measurements are the worst timings in ~30 seconds of runtime.
		// Measured with timer prescaler 1.
		// BGR8:
		// 240x160 no scaling:    ~184 µs
		// 240x160 bilinear x1.5: ~408 µs
		// 360x240 no scaling:    ~437 µs
		//
		// A1BGR5:
		// 240x160 no scaling:    ~188 µs (25300 ticks)
		// 240x160 bilinear x1.5: ~407 µs (54619 ticks)
		// 360x240 no scaling:    ~400 µs (53725 ticks)
		static bool inited = false;
		u32 listSize;
		const u32 *list;
		if(inited == false)
		{
			inited = true;

			listSize = sizeof(gbaGpuInitList);
			list = (u32*)gbaGpuInitList;
		}
		else
		{
			listSize = sizeof(gbaGpuList2);
			list = (u32*)gbaGpuList2;
		}
		GX_processCommandList(listSize, list);
		GFX_waitForP3D();
		GX_displayTransfer((u32*)GPU_RENDER_BUF_ADDR, PPF_DIM(240, 400), GFX_getBuffer(GFX_LCD_TOP, GFX_SIDE_LEFT),
		                   PPF_DIM(240, 400), PPF_O_FMT(GX_BGR8) | PPF_I_FMT(GX_BGR8));
		GFX_waitForPPF();
		GFX_swapBuffers();

		// Trigger only if both are held and at least one is detected as newly pressed down.
		if(hidKeysHeld() == (KEY_Y | KEY_SELECT) && hidKeysDown() != 0)
			dumpFrameTex();
	}

	taskExit();
}

static KHandle setupFrameCapture(const u8 scaler)
{
	const bool is240x160 = scaler < 2;
	static s16 matrix[12 * 8] =
	{
		// Vertical.
		      0,       0,       0,       0,       0,       0,       0,       0,
		      0,       0,       0,       0,       0,       0,       0,       0,
		      0,  0x24B0,  0x4000,       0,  0x24B0,  0x4000,       0,       0,
		 0x4000,  0x2000,       0,  0x4000,  0x2000,       0,       0,       0,
		      0,  -0x4B0,       0,       0,  -0x4B0,       0,       0,       0,
		      0,       0,       0,       0,       0,       0,       0,       0,

		// Horizontal.
		      0,       0,       0,       0,       0,       0,       0,       0,
		      0,       0,       0,       0,       0,       0,       0,       0,
		      0,       0,  0x24B0,       0,       0,  0x24B0,       0,       0,
		 0x4000,  0x4000,  0x2000,  0x4000,  0x4000,  0x2000,       0,       0,
		      0,       0,  -0x4B0,       0,       0,  -0x4B0,       0,       0,
		      0,       0,       0,       0,       0,       0,       0,       0
	};

	const Result res = fsQuickRead("gba_scaler_matrix.bin", matrix, sizeof(matrix));
	if(res != RES_OK && res != RES_FR_NO_FILE)
	{
		ee_printf("Failed to load hardware scaling matrix: %s\n", result2String(res));
	}

	LgyCapCfg gbaCfg;
	gbaCfg.cnt   = LGYCAP_SWIZZLE | LGYCAP_ROT_NONE | LGYCAP_FMT_A1BGR5 | (is240x160 ? 0 : LGYCAP_HSCALE_EN | LGYCAP_VSCALE_EN);
	gbaCfg.w     = (is240x160 ? 240 : 360);
	gbaCfg.h     = (is240x160 ? 160 : 240);
	gbaCfg.irq   = 0;
	gbaCfg.vLen  = 6;
	gbaCfg.vPatt = 0b00011011;
	memcpy(gbaCfg.vMatrix, matrix, 6 * 8 * 2);
	gbaCfg.hLen  = 6;
	gbaCfg.hPatt = 0b00011011;
	memcpy(gbaCfg.hMatrix, &matrix[6 * 8], 6 * 8 * 2);

	return LGYCAP_init(LGYCAP_DEV_TOP, &gbaCfg);
}

KHandle OAF_videoInit(void)
{
#ifdef NDEBUG
	// Force black and turn the backlight off on the bottom screen.
	// Don't turn the backlight off on 2DS (1 panel).
	GFX_setForceBlack(false, true);
	if(MCU_getSystemModel() != SYS_MODEL_2DS)
		GFX_powerOffBacklight(GFX_BL_BOT);
#endif

	// Initialize frame capture and frame handler.
	const u8 scaler = g_oafConfig.scaler;
	const KHandle frameReadyEvent = setupFrameCapture(scaler);
	patchGbaGpuCmdList(scaler);
	createTask(0x800, 3, gbaGfxHandler, (void*)frameReadyEvent);

	// Adjust gamma table and setup button overrides.
	adjustGammaTableForGba();

	// Load border if any exists.
	if(scaler == 0) // No borders for scaled modes.
	{
		// Abuse currently invisible frame buffer as temporary buffer.
		void *const borderBuf = GFX_getBuffer(GFX_LCD_TOP, GFX_SIDE_LEFT);
		if(fsQuickRead("border.bgr", borderBuf, 400 * 240 * 3) == RES_OK)
		{
			// Copy border in swizzled form to GPU render buffer.
			GX_displayTransfer(borderBuf, PPF_DIM(240, 400), (u32*)GPU_RENDER_BUF_ADDR,
			                   PPF_DIM(240, 400), PPF_O_FMT(GX_BGR8) | PPF_I_FMT(GX_BGR8) | PPF_OUT_TILED);
			GFX_waitForPPF();
		}
	}

	return frameReadyEvent;
}

void OAF_videoExit(void)
{
	// frameReadyEvent deleted by this function.
	// gbaGfxHandler() will automatically terminate.
	LGYCAP_deinit(LGYCAP_DEV_TOP);
}