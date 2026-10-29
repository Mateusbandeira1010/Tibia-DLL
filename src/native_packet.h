#ifndef TIBIA_NATIVE_PACKET_H
#define TIBIA_NATIVE_PACKET_H

#include <windows.h>

#include <cstddef>

namespace NativePacketBuffer
{
	inline constexpr DWORD MAXIMUM_SIZE = 65535;

	struct Packet
	{
		const BYTE* data = nullptr;
		DWORD size = 0;
		DWORD start = 0;
	};

	inline bool Capture(DWORD bufferAddress, DWORD maximumSize,
		Packet& packet) noexcept
	{
		if(!bufferAddress)
			return false;
		__try {
			const DWORD* native = reinterpret_cast<const DWORD*>(bufferAddress);
			packet.data = reinterpret_cast<const BYTE*>(native[0]);
			packet.size = native[1];
			packet.start = native[2];
			return packet.data && packet.size <= maximumSize &&
				packet.start <= packet.size;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			packet = Packet();
			return false;
		}
	}

	inline bool SetPosition(DWORD bufferAddress, const Packet& packet,
		std::size_t consumed) noexcept
	{
		if(!bufferAddress || packet.start > packet.size ||
			consumed > packet.size - packet.start)
			return false;
		__try {
			DWORD* native = reinterpret_cast<DWORD*>(bufferAddress);
			if(reinterpret_cast<const BYTE*>(native[0]) != packet.data ||
				native[1] != packet.size || native[2] != packet.start)
				return false;
			native[2] = packet.start + static_cast<DWORD>(consumed);
			return true;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}
}

#endif
