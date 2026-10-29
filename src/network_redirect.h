#ifndef TIBIA_NETWORK_REDIRECT_H
#define TIBIA_NETWORK_REDIRECT_H

#include <cstdint>

namespace NetworkRedirect
{
	bool Install(std::uintptr_t clientBase, bool enabled) noexcept;
	void Shutdown() noexcept;
}

#endif
