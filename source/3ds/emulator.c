#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "3ds/gfx.h"
#include "uxn.h"
#include "util.h"
#include "devices/audio.h"
#include "devices/datetime.h"
#include "devices/file.h"
#include "ctr_keyboard.h"
#include "ctr_screen.h"
#include "devices/system.h"

/*
Copyright (c) 2021 Devine Lu Linvega
Copyright (c) 2021 Adrian "asie" Siekierka

Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE.
*/

#define PPU_PIXELS_WIDTH 320
#define PPU_PIXELS_HEIGHT 240
#define AUDIO_BUFFER_SIZE 2048

static C3D_RenderTarget *topLeft, *topRight, *bottom;

static bool soundFillBlock;
static ndspWaveBuf soundBuffer[2];
static u8 *soundData;
static LightLock soundLock;

#ifdef DEBUG
#define DEBUG_CONSOLE
#endif

#define PAD 0

#define REQDRAW_DISPLAY 1
#define REQDRAW_ALL 1

Uint8 dispswap = 0, reqdraw = 0;

int prompt_reset(Uxn *u);

int
error(char *msg, const char *err)
{
#ifndef DEBUG_CONSOLE
	consoleInit(GFX_BOTTOM, NULL);
#endif
	iprintf("Error %s: %s\n", msg, err);
	gfxSwapBuffers();
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) break;
		gspWaitForVBlank();
	}
	exit(0);
	return 0;
}

void
audio_finished_handler(int instance)
{

}

static void
audio_callback(void *u)
{
	if (soundBuffer[soundFillBlock].status == NDSP_WBUF_DONE) {
		int i;
		Sint16 *samples = (Sint16 *) soundBuffer[soundFillBlock].data_vaddr;
		memset(samples, 0, AUDIO_BUFFER_SIZE * 4);
		LightLock_Lock(&soundLock);
		for(i = 0; i < POLYPHONY; ++i)
			audio_render(i, samples, samples + (AUDIO_BUFFER_SIZE * 2));
		LightLock_Unlock(&soundLock);
		DSP_FlushDataCache(samples, AUDIO_BUFFER_SIZE * 4);
		ndspChnWaveBufAdd(0, &soundBuffer[soundFillBlock]);
		soundFillBlock = !soundFillBlock;
	}
}

void
redraw(Uxn *u)
{
	C2D_DrawParams drawParams;
	float slider = osGet3DSliderState();
	int x_offset_bg = (int) (slider * 7.0f);
	int x_offset_fg = (slider > 0.0f) ? -1 : 0;

	ctr_screen_redraw(&uxn_ctr_screen);

	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

	memset(&drawParams, 0, sizeof(drawParams));
	drawParams.pos.y = 0.0f;
	drawParams.pos.w = 320.0f;
	drawParams.pos.h = 240.0f;

	if (!dispswap) {
		C2D_TargetClear(topLeft, C2D_Color32(0, 0, 0, 0));
		C2D_SceneBegin(topLeft);
		drawParams.pos.x = 40.0f - x_offset_bg;
		drawParams.depth = 0.0f;
		C2D_DrawImage(uxn_ctr_screen.bg.gpuImage, &drawParams, NULL);
		drawParams.pos.x = 40.0f - x_offset_fg;
		drawParams.depth = 1.0f;
		C2D_DrawImage(uxn_ctr_screen.fg.gpuImage, &drawParams, NULL);

		if (slider > 0.0f) {
			C2D_TargetClear(topRight, C2D_Color32(0, 0, 0, 0));
			C2D_SceneBegin(topRight);
			drawParams.pos.x = 40.0f + x_offset_bg;
			drawParams.depth = 0.0f;
			C2D_DrawImage(uxn_ctr_screen.bg.gpuImage, &drawParams, NULL);
			drawParams.pos.x = 40.0f + x_offset_fg;
			drawParams.depth = 1.0f;
			C2D_DrawImage(uxn_ctr_screen.fg.gpuImage, &drawParams, NULL);
		}

#ifndef DEBUG_CONSOLE
		C2D_TargetClear(bottom, C2D_Color32(0, 0, 0, 0));
		C2D_SceneBegin(bottom);

		keyboard_draw();
#endif
	} else {
#ifndef DEBUG_CONSOLE
		C2D_TargetClear(bottom, C2D_Color32(0, 0, 0, 0));
		C2D_SceneBegin(bottom);
		drawParams.pos.x = 0.0f;
		drawParams.depth = 0.0f;
		C2D_DrawImage(uxn_ctr_screen.bg.gpuImage, &drawParams, NULL);
		drawParams.depth = 1.0f;
		C2D_DrawImage(uxn_ctr_screen.fg.gpuImage, &drawParams, NULL);
#endif

		C2D_TargetClear(topLeft, C2D_Color32(0, 0, 0, 0));
		C2D_SceneBegin(topLeft);
	}

	C3D_FrameEnd(0);
	reqdraw = 0;
}

void
quit(void)
{
	keyboard_exit();

	// APU
	ndspExit();
	LightLock_Lock(&soundLock);
	linearFree(soundData);
	LightLock_Unlock(&soundLock);

	// PPU
	ctr_screen_free(&uxn_ctr_screen);
	C2D_Fini();
	C3D_Fini();
	gfxExit();

	romfsExit();
	exit(0);
}

int
init(void)
{
	osSetSpeedupEnable(1);

	// PPU
	ctr_screen_init(&uxn_ctr_screen, 320, 240);
	gfxInitDefault();
	gfxSet3D(true);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(4096);
	C2D_Prepare();
	topLeft = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	topRight = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
	bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

	// APU
	float soundMix[12];
	memset(soundMix, 0, sizeof(soundMix));
	soundMix[0] = soundMix[1] = 1.0f;
	soundData = (u8*) linearAlloc(AUDIO_BUFFER_SIZE * 8);
	memset(soundData, 0, AUDIO_BUFFER_SIZE * 8);
	LightLock_Init(&soundLock);
	ndspInit();
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspChnReset(0);
	ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
	ndspChnSetRate(0, SAMPLE_FREQUENCY);
	ndspChnSetFormat(0, NDSP_CHANNELS(2) | NDSP_ENCODING(NDSP_ENCODING_PCM16));
	ndspChnSetMix(0, soundMix);
	ndspSetOutputCount(1);
	ndspSetMasterVol(1.0f);
	ndspSetCallback(audio_callback, soundBuffer);

	memset(soundBuffer, 0, sizeof(soundBuffer));
	soundBuffer[0].data_vaddr = &soundData[0];
	soundBuffer[0].nsamples = AUDIO_BUFFER_SIZE;
	soundBuffer[1].data_vaddr = &soundData[AUDIO_BUFFER_SIZE * 4];
	soundBuffer[1].nsamples = AUDIO_BUFFER_SIZE;

	DSP_FlushDataCache(soundData, AUDIO_BUFFER_SIZE * 8);

	ndspChnWaveBufAdd(0, &soundBuffer[0]);
	ndspChnWaveBufAdd(0, &soundBuffer[1]);

	if (R_FAILED(romfsInit())) {
		error("romfsInit", "Failed");
	}

	if (!keyboard_init()) {
		error("keyboard init", "Failed");
	}
	return 1;
}

static u8 ctrl_flags = 0;

void
doctrl(Uxn *u)
{
	bool changed = false;
	u8 old_flags = ctrl_flags;
	int key = dispswap ? -1 : keyboard_update();

	int pressed = keysDown();
	int held = pressed | keysHeld();

	if (pressed & (KEY_L | KEY_R)) {
		dispswap ^= 1;
	}

	ctrl_flags = (held & 0x0F)
		| (keyboard_is_held(K_CTRL) ? 0x01 : 0)
		| (keyboard_is_held(K_ALT) ? 0x02 : 0)
		| (keyboard_is_held(K_SHIFT) ? 0x04 : 0)
		| ((key == K_HOME) ? 0x08 : 0)
		| ((held & KEY_UP) ? 0x10 : 0)
		| ((held & KEY_DOWN) ? 0x20 : 0)
		| ((held & KEY_RIGHT) ? 0x80 : 0)
		| ((held & KEY_LEFT) ? 0x40 : 0);

	if (key > 0 && key < 128) {
		u->dev[0x83] = key;
		changed = true;
	}
	if (old_flags != ctrl_flags) {
		changed = true;
	}
	if (changed) {
		// clear only changed bits
		u->dev[0x82] = (u->dev[0x82] & ~(old_flags & (~ctrl_flags))) | (ctrl_flags & (~old_flags));
		uxn_eval(u, GETVEC(u->dev + 0x80));
		if (key > 0 && key < 128) {
			u->dev[0x83] = 0;
		}
	}

	if (key == K_SYSTEM) {
		prompt_reset(u);
	}
}

static bool istouching = false;

void
domouse(Uxn *u)
{
	bool changed = false;

	if (dispswap && (hidKeysHeld() & KEY_TOUCH)) {
		if (!istouching) {
			u->dev[0x96] = 0x01;
			istouching = true;
			changed = true;
		}

		touchPosition tpos;
		hidTouchRead(&tpos);
		if (peek16(u->dev + 0x90, 0x2) != tpos.px
			|| peek16(u->dev + 0x90, 0x4) != tpos.py)
		{
			poke16(u->dev + 0x90, 0x2, tpos.px);
			poke16(u->dev + 0x90, 0x4, tpos.py);
			changed = true;
		}
	} else if (istouching) {
		u->dev[0x96] = 0x00;
		istouching = false;
		changed = true;
	}

	if (changed) {
		uxn_eval(u, GETVEC(u->dev + 0x90));
	}
}

#pragma mark - Devices

static Uint8
audio_dei(int instance, Uint8 *d, Uint8 port)
{
	switch(port) {
	case 0x4: return audio_get_vu(instance);
	case 0x2: POKE2(d + 0x2, audio_get_position(instance)); /* fall through */
	default: return d[port];
	}
}

static void
audio_deo(int instance, Uint8 *d, Uint8 port, Uxn *u)
{
	if(port == 0xf) {
		audio_start(instance, d, u);
	}
}

static Uint8
emu_dei(Uxn *u, Uint8 addr)
{
	Uint8 p = addr & 0x0f, d = addr & 0xf0;
	switch(d) {
	case 0x20: return ctr_screen_dei(u, addr);
	case 0x30: return audio_dei(0, &u->dev[d], p);
	case 0x40: return audio_dei(1, &u->dev[d], p);
	case 0x50: return audio_dei(2, &u->dev[d], p);
	case 0x60: return audio_dei(3, &u->dev[d], p);
	case 0xa0: return file_dei(0, &u->dev[d], p);
	case 0xb0: return file_dei(1, &u->dev[d], p);
	case 0xc0: return datetime_dei(u, addr);
	}
	return u->dev[addr];
}

static void
emu_deo(Uxn *u, Uint8 addr, Uint8 v)
{
	Uint8 p = addr & 0x0f, d = addr & 0xf0;
	u->dev[addr] = v;
	switch(d) {
	case 0x00:
		system_deo(u, &u->dev[d], p);
		if(p > 0x7 && p < 0xe)
			ctr_screen_palette(&uxn_ctr_screen, &u->dev[0x8]);
		break;
	case 0x10: console_deo(&u->dev[d], p); break;
	case 0x20: ctr_screen_deo(u->ram.dat, &u->dev[d], p); break;
	case 0x30: audio_deo(0, &u->dev[d], p, u); break;
	case 0x40: audio_deo(1, &u->dev[d], p, u); break;
	case 0x50: audio_deo(2, &u->dev[d], p, u); break;
	case 0x60: audio_deo(3, &u->dev[d], p, u); break;
	case 0xa0: file_deo(0, u->ram.dat, &u->dev[d], p); break;
	case 0xb0: file_deo(1, u->ram.dat, &u->dev[d], p); break;
	}
}

static int
uxn_load_boot(Uxn *u)
{
	if(system_load(u, "romfs:/boot.rom")) {
		chdir("romfs:/");
		return 1;
	}
	if(system_load(u, "boot.rom")) {
		return 1;
	}
	if(system_load(u, "/uxn/boot.rom")) {
		chdir("/uxn");
		return 1;
	}
	if(system_load(u, "/uxn/launcher.rom")) {
		chdir("/uxn");
		return 1;
	}
	return 0;
}

int
prompt_reset(Uxn *u)
{
#ifndef DEBUG_CONSOLE
	consoleInit(GFX_BOTTOM, NULL);
#endif
	consoleClear();
	iprintf("\n\n\n\n\n\n\n\n\n\n\n\n\n        Would you like to reset?\n\n          [A] - Yes\n          [B] - No\n");
        gfxSwapBuffers();
	while(aptMainLoop()) {
                gspWaitForVBlank();
		hidScanInput();
		int allHeld = hidKeysDown() | hidKeysHeld();
		if (allHeld & KEY_A) {
			consoleClear();
			break;
		} else if (allHeld & KEY_B) {
			consoleClear();
			goto restoreGfx;
		}
	}

	iprintf("Resetting...\n");

	if(!resetuxn(u))
		return error("Resetting", "Failed");
	if(!uxn_load_boot(u))
		return error("Load", "Failed");
	ctr_screen_free(&uxn_ctr_screen);
	ctr_screen_init(&uxn_ctr_screen, 320, 240);
	keyboard_clear();
	while(aptMainLoop()) {
                gspWaitForVBlank();
		hidScanInput();
		if ((hidKeysDown() | hidKeysHeld()) == 0) break;
	}
	ctrl_flags = 0;
	uxn_eval(u, 0x0100);

restoreGfx:
#ifndef DEBUG_CONSOLE
	gfxInitDefault();
	gfxSet3D(true);
#endif
	return 0;
}

int
start(Uxn *u)
{
	uxn_eval(u, 0x0100);
	redraw(u);
	while(aptMainLoop()) {
		hidScanInput();
		if ((hidKeysHeld() &
			(KEY_L | KEY_R | KEY_START | KEY_SELECT))
			== (KEY_L | KEY_R | KEY_START | KEY_SELECT)) {
			break;
		}
		doctrl(u);
		domouse(u);
		uxn_eval(u, GETVEC(u->dev + 0x20));
		redraw(u);
	}
	return 1;
}

static Uxn u;

int
main(int argc, char **argv)
{
	if(!init())
		return error("Init", "Failed");
#ifdef DEBUG_CONSOLE
	consoleInit(GFX_BOTTOM, NULL);
	iprintf("uxn3ds\n");
#endif

	if(!uxn_boot(&u, (Uint8 *)calloc(0x10000 * RAM_PAGES, sizeof(Uint8)), emu_dei, emu_deo))
		return error("Boot", "Failed");
	if(!uxn_load_boot(&u)) {
                dprintf("Halted: Missing input rom.\n");
		return error("Load", "Failed");
	}

	// Write screen size to dev/screen
	poke16(u.dev + 0x20, 2, PPU_PIXELS_WIDTH);
	poke16(u.dev + 0x20, 4, PPU_PIXELS_HEIGHT);

	start(&u);
	quit();
	return 0;
}
