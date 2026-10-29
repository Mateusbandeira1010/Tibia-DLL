#ifndef TIBIA_EXTENDED_ENGINES_H
#define TIBIA_EXTENDED_ENGINES_H

//#define DX7_RENDER 0
#define OGL_RENDER 1
#define DX9_RENDER 2

class ExtendedEngine
{
	public:
		virtual ~ExtendedEngine() = default;

		virtual int getRenderId() const = 0;
		virtual void LoadSprite(int surface, int x, int y, int w, int h, void* data) = 0;

		virtual void enableAlpha() = 0;
		virtual void disableAlpha() = 0;

	protected:
		ExtendedEngine() = default;
};

#endif
