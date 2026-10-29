#ifndef TIBIA_EXTENDED_DX9_H
#define TIBIA_EXTENDED_DX9_H

#include <d3d9.h>
#include "extEngines.h"

class ExtendedEngineDX9 : public ExtendedEngine
{
	public:
		ExtendedEngineDX9() = default;
		~ExtendedEngineDX9() override = default;

		int getRenderId() const override { return DX9_RENDER; }
		void LoadSprite(int surface, int x, int y, int w, int h,
			void* data) override;

		void enableAlpha() override {} // Alpha is enabled by default in DX9.
		void disableAlpha() override {}
};

#endif
