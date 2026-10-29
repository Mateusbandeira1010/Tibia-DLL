#ifndef TIBIA_MAIN_H
#define TIBIA_MAIN_H

#include "security_build_config.h"

#define PROJECT_INFO "Tibia %s Client\n%s%sVersion %d.%02d\n" \
	"Modification by: Mateuzkl\n" \
	"\xA9" "Copyright" "\xA9" " 1997-2018\n" \
	"CipSoft GmbH - " "\xAE" "rights reserved" "\xAE"
#define PROJECT_NAME "Extented Tibia"

#include <memory>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

using SpritePixels = std::unique_ptr<unsigned char[]>;

SpritePixels LoadSpriteAlpha(uint32_t sprite);

using GetEngineAddrFn = uint32_t (*)();
extern GetEngineAddrFn GetEngineAddr;

using DrawSkinFn = void (*)(int nSurface, int X, int Y, int W, int H,
	int SkinId, int dX, int dY);
extern DrawSkinFn DrawSkin;

using PrintTextFn = void (*)(int nSurface, int nX, int nY, int nFont,
	int nRed, int nGreen, int nBlue, const char* lpText, int nAlign);
extern PrintTextFn PrintText;

#endif
