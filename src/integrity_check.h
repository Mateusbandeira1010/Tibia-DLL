#ifndef TIBIA_INTEGRITY_CHECK_H
#define TIBIA_INTEGRITY_CHECK_H

#include <cstdint>

namespace IntegrityCheck
{
	enum class Status : std::uint8_t
	{
		Invalid = 0,
		Valid = 1
	};

	Status GetStatus() noexcept;
}

#endif
