#ifndef TIBIA_STORE_WINDOW_H
#define TIBIA_STORE_WINDOW_H

#include <windows.h>

namespace StoreWindow
{
	BYTE ServerOpcode() noexcept;
	bool Install(DWORD clientBase) noexcept;
	void Shutdown() noexcept;

	bool Toggle() noexcept;
	void Close() noexcept;
	bool IsVisible() noexcept;
	bool HandleMouseMessage(WPARAM message,
		const MSLLHOOKSTRUCT& mouse) noexcept;

	void HandleServerPacket() noexcept;
	void Draw(int surface, bool online) noexcept;
}

#endif
