#ifndef TIBIA_PING_OVERLAY_H
#define TIBIA_PING_OVERLAY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace PingOverlay
{
	namespace Detail
	{
		inline constexpr std::size_t PingSlotCount = 32;
		inline constexpr DWORD WaitingTimeoutMs = 30'000;
		inline constexpr DWORD ClosedTimeoutMs = 60'000;

		enum class PingSlotState : std::uint8_t
		{
			Empty,
			WaitingServerAck,
			Closed
		};

		struct PingSlot
		{
			DWORD id = 0;
			DWORD sentAt = 0;
			DWORD stateSince = 0;
			PingSlotState state = PingSlotState::Empty;
		};

		struct PingState
		{
			std::array<PingSlot, PingSlotCount> slots = {};
		};

		enum class PingPacketAction : std::uint8_t
		{
			SendPong,
			CompleteRtt,
			IgnoreDuplicate,
			IgnoreInvalid
		};

		struct PingPacketResult
		{
			PingPacketAction action = PingPacketAction::IgnoreInvalid;
			DWORD elapsed = 0;
			bool slotEvicted = false;
		};

		PingPacketResult HandlePacket(PingState& state, DWORD id, DWORD now) noexcept;
		void Reset(PingState& state) noexcept;
		std::size_t Size(const PingState& state) noexcept;
	}

	bool Install(DWORD clientBase, bool customPing,
		bool consumeImpactTracker);
	void Draw(int surface, bool insideGame);
	void Shutdown();
}

#endif
