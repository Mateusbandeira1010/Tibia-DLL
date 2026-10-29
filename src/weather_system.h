#ifndef TIBIA_WEATHER_SYSTEM_H
#define TIBIA_WEATHER_SYSTEM_H

#include <windows.h>

namespace WeatherSystem
{
	BYTE ServerOpcode() noexcept;
	bool Install(DWORD clientBase, bool enableLoginMarker) noexcept;
	void HandleServerPacket() noexcept;
	bool Draw(int surface, bool online) noexcept;
	void Shutdown() noexcept;
}

#endif
