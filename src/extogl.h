#ifndef TIBIA_EXTENDED_OPENGL_H
#define TIBIA_EXTENDED_OPENGL_H

#include <GL/gl.h>
#include "extEngines.h"

class ExtendedEngineOGL : public ExtendedEngine
{
	public:
		ExtendedEngineOGL() = default;
		~ExtendedEngineOGL() override = default;

		int getRenderId() const override { return OGL_RENDER; }
		void LoadSprite(int surface, int x, int y, int w, int h,
			void* data) override;

		void enableAlpha() override;
		void disableAlpha() override;
};

#endif
