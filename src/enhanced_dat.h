#ifndef TIBIA_ENHANCED_DAT_H
#define TIBIA_ENHANCED_DAT_H

#include "main.h"

namespace EnhancedDat
{
	bool Install(DWORD clientBase, bool extendedSprites);
	void Shutdown();
	void UpdateSession(bool online);
	void BeginCreatureDraw(DWORD creaturePtr);
	void EndCreatureDraw();
}

#endif
