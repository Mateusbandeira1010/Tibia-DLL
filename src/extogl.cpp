#include "main.h"
#include "extogl.h"

void ExtendedEngineOGL::LoadSprite(int surface, int x, int y, int w, int h, void* data)
{
	if(surface < 0 || w <= 0 || h <= 0 || w > 32 || h > 32)
		return;
	SpritePixels rgbaPixels = LoadSpriteAlpha(
		static_cast<uint32_t>(reinterpret_cast<uintptr_t>(data)));
	if(!rgbaPixels)
		return;

	const DWORD engineAddress = GetEngineAddr ? GetEngineAddr() : 0;
	if(!engineAddress)
		return;
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D,
		*reinterpret_cast<GLuint*>(engineAddress + 0x1D4 + surface * 32));
	glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA,
		GL_UNSIGNED_BYTE, rgbaPixels.get());
}

void ExtendedEngineOGL::enableAlpha()
{
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_BLEND);
}

void ExtendedEngineOGL::disableAlpha()
{
	glDisable(GL_BLEND);
}
