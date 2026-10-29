#ifndef TIBIA_CLIENT_MEMORY_H
#define TIBIA_CLIENT_MEMORY_H

#include <windows.h>

#include <cstddef>
#include <cstring>

namespace ClientMemory
{
	inline bool MatchesCode(DWORD address, const BYTE* expected,
		std::size_t size) noexcept
	{
		if(!address || !expected || size == 0)
			return false;
		__try {
			return std::memcmp(reinterpret_cast<const void*>(address), expected,
				size) == 0;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}
}

#endif
