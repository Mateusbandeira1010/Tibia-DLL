#include "main.h"
#include "extdx9.h"

void ExtendedEngineDX9::LoadSprite(int surface, int x, int y, int w, int h, void* data)
{
	if(surface < 0 || w <= 0 || h <= 0 || w > 32 || h > 32)
		return;
	SpritePixels rgbaPixels = LoadSpriteAlpha(
		static_cast<uint32_t>(reinterpret_cast<uintptr_t>(data)));
	if(!rgbaPixels)
		return;

	RECT d3drect;
	d3drect.left = x;
	d3drect.right = x + w;
	d3drect.top = y;
	d3drect.bottom = y + h;
	const DWORD engineAddress = GetEngineAddr ? GetEngineAddr() : 0;
	if(!engineAddress)
		return;
	IDirect3DTexture9* texture = reinterpret_cast<IDirect3DTexture9*>(
		*reinterpret_cast<DWORD*>(engineAddress + 0x200 + surface * 4));
	if(!texture)
		return;

	D3DLOCKED_RECT rect = {};
	HRESULT hr = IDirect3DTexture9_LockRect(texture, 0, &rect, &d3drect, D3DLOCK_NOSYSLOCK);
	if(FAILED(hr))
		return;

	unsigned char* tBits = static_cast<unsigned char*>(rect.pBits);
	if(!tBits || rect.Pitch < w * 4)
	{
		IDirect3DTexture9_UnlockRect(texture, 0);
		return;
	}

	uint32_t readData = 0, pixel = 0;
	for(int j = 0; j < h; j++)
	{
		for(int k = 0; k < w; k++)
		{
			pixel = j*rect.Pitch+k*4;
			tBits[pixel] = rgbaPixels[readData+2];//use bgra format instead of rgba
			tBits[pixel+1] = rgbaPixels[readData+1];
			tBits[pixel+2] = rgbaPixels[readData];
			tBits[pixel+3] = rgbaPixels[readData+3];
			readData += 4;
		}
	}

	IDirect3DTexture9_UnlockRect(texture, 0);
}
