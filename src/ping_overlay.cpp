#include "ping_overlay.h"

// PING_OVERLAY_TRACKER_TEST keeps only the pure PingOverlay::Detail state
// machine below, dropping the hook, client memory and timer dependencies so
// tests/ping_tracker_tests.cpp can link it. See the ping_tracker_tests target
// in CMakeLists.txt; nothing but that target should define it.
#ifndef PING_OVERLAY_TRACKER_TEST
#include "client_memory.h"
#include "config.h"
#include "hook.h"
#include "main.h"
#include "security_build_config.h"
#include "store_window.h"
#include "timer.h"
#include "weather_system.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#endif

namespace PingOverlay::Detail
{
	namespace
	{
		DWORD Elapsed(DWORD now, DWORD then) noexcept
		{
			return now - then;
		}

		void Cleanup(PingState& state, DWORD now) noexcept
		{
			for(PingSlot& slot : state.slots)
			{
				if(slot.state == PingSlotState::WaitingServerAck)
				{
					const DWORD age = Elapsed(now, slot.sentAt);
					if(age >= WaitingTimeoutMs + ClosedTimeoutMs)
						slot = {};
					else if(age >= WaitingTimeoutMs)
					{
						slot.state = PingSlotState::Closed;
						slot.stateSince = slot.sentAt + WaitingTimeoutMs;
					}
				}
				else if(slot.state == PingSlotState::Closed &&
					Elapsed(now, slot.stateSince) >= ClosedTimeoutMs)
					slot = {};
			}
		}

		PingSlot* SelectSlot(PingState& state, DWORD now) noexcept
		{
			for(PingSlot& slot : state.slots)
			{
				if(slot.state == PingSlotState::Empty)
					return &slot;
			}

			PingSlot* oldestClosed = nullptr;
			for(PingSlot& slot : state.slots)
			{
				if(slot.state != PingSlotState::Closed)
					continue;
				if(!oldestClosed || Elapsed(now, slot.stateSince) > Elapsed(now, oldestClosed->stateSince))
					oldestClosed = &slot;
			}
			if(oldestClosed)
				return oldestClosed;

			PingSlot* oldestWaiting = &state.slots.front();
			for(PingSlot& slot : state.slots)
			{
				if(Elapsed(now, slot.sentAt) > Elapsed(now, oldestWaiting->sentAt))
					oldestWaiting = &slot;
			}
			return oldestWaiting;
		}
	}

	PingPacketResult HandlePacket(PingState& state, DWORD id, DWORD now) noexcept
	{
		Cleanup(state, now);
		if(id == 0)
			return {};

		for(PingSlot& slot : state.slots)
		{
			if(slot.state == PingSlotState::Empty || slot.id != id)
				continue;
			if(slot.state == PingSlotState::Closed)
				return {PingPacketAction::IgnoreDuplicate, 0, false};

			const DWORD elapsed = Elapsed(now, slot.sentAt);
			slot.state = PingSlotState::Closed;
			slot.stateSince = now;
			return {PingPacketAction::CompleteRtt, elapsed, false};
		}

		PingSlot* selected = SelectSlot(state, now);
		const bool evicted = selected->state != PingSlotState::Empty;
		*selected = {id, now, now, PingSlotState::WaitingServerAck};
		return {PingPacketAction::SendPong, 0, evicted};
	}

	void Reset(PingState& state) noexcept
	{
		state.slots = {};
	}

	std::size_t Size(const PingState& state) noexcept
	{
		std::size_t count = 0;
		for(const PingSlot& slot : state.slots)
			count += slot.state != PingSlotState::Empty ? 1 : 0;
		return count;
	}
}

#ifndef PING_OVERLAY_TRACKER_TEST

namespace
{
	using ReadU8Fn = BYTE (__cdecl*)();
	using ReadU32Fn = DWORD (__cdecl*)();
	using ReadStringFn = void (__cdecl*)(char*, DWORD);
	using AddByteFn = void (__cdecl*)(DWORD);
	using AddU32Fn = void (__cdecl*)(DWORD);
	using SendPacketFn = void (__cdecl*)(DWORD);

	ReadU8Fn g_readU8 = nullptr;
	ReadU32Fn g_readU32 = nullptr;
	ReadStringFn g_readString = nullptr;
	AddByteFn g_addByte = nullptr;
	AddU32Fn g_addU32 = nullptr;
	SendPacketFn g_sendPacket = nullptr;
	DWORD g_dispatchReturn = 0;
	DWORD g_packetLoop = 0;
	DWORD g_lastPacketTypeAddress = 0;
	DWORD g_storePacketOpcode = 0;
	DWORD g_weatherPacketOpcode = 0;
	bool g_customPing = false;
	bool g_weatherEnabled = false;
	bool g_consumeImpactTracker = false;
	SRWLOCK g_pingLock = SRWLOCK_INIT;
	PingOverlay::Detail::PingState g_pingState;
	std::atomic<int> g_pingMilliseconds(-1);

	void SendPingPacket(DWORD id)
	{
		if(!g_addByte || !g_addU32 || !g_sendPacket)
			return;

		g_addByte(0x1E);
		g_addU32(id);
		g_sendPacket(1);
	}

	void __stdcall HandlePingPacket()
	{
		if(!g_readU32)
			return;

		const DWORD id = g_readU32();
		const DWORD now = getTimerTick();
		AcquireSRWLockExclusive(&g_pingLock);
		const PingOverlay::Detail::PingPacketResult result = PingOverlay::Detail::HandlePacket(g_pingState, id, now);
		ReleaseSRWLockExclusive(&g_pingLock);

		if(result.action == PingOverlay::Detail::PingPacketAction::CompleteRtt)
		{
			g_pingMilliseconds.store(static_cast<int>(std::min<DWORD>(result.elapsed,
				static_cast<DWORD>((std::numeric_limits<int>::max)()))));
			return;
		}

		// Do not hold the tracker lock while calling into the client's packet
		// sender: the hook may be re-entered from code outside our control.
		if(result.action == PingOverlay::Detail::PingPacketAction::SendPong)
			SendPingPacket(id);
	}

	void __stdcall HandleImpactTrackerPacket()
	{
		// 0xCC belongs to the OTClient/Mehah Impact Tracker. Some accounts
		// can retain its server storage when they reconnect with the classic
		// DLL. Consume the known payload so it cannot desynchronise 8.60.
		if(!g_readU8 || !g_readU32)
			return;
		const BYTE analyzerType = g_readU8();
		g_readU32(); // absolute damage/healing amount
		if(analyzerType == 1)
			g_readU8(); // combat effect
		else if(analyzerType == 2)
		{
			g_readU8(); // combat effect
			if(g_readString)
			{
				char targetName[256] = {};
				g_readString(targetName, sizeof(targetName) - 1);
				targetName[sizeof(targetName) - 1] = '\0';
			}
		}
	}

	void __stdcall HandleStorePacket()
	{
		StoreWindow::HandleServerPacket();
	}

	void __stdcall HandleWeatherPacket()
	{
		WeatherSystem::HandleServerPacket();
	}

	__declspec(naked) void HookPingDispatch()
	{
		__asm {
			cmp esi, 1Eh
			jne checkStore
			cmp byte ptr [g_customPing], 0
			jne pingPacket
			jmp nativePacket
		checkStore:
			cmp byte ptr [g_weatherEnabled], 0
			je checkStoreOpcode
			cmp esi, dword ptr [g_weatherPacketOpcode]
			je weatherPacket
		checkStoreOpcode:
			cmp esi, dword ptr [g_storePacketOpcode]
			je storePacket
			cmp esi, 0CCh
			je impactPacket
			jmp nativePacket

		pingPacket:
			pushad
			call HandlePingPacket
			popad
			mov eax, dword ptr [g_lastPacketTypeAddress]
			mov dword ptr [eax], esi
			jmp dword ptr [g_packetLoop]

		storePacket:
			pushad
			call HandleStorePacket
			popad
			mov eax, dword ptr [g_lastPacketTypeAddress]
			mov dword ptr [eax], esi
			jmp dword ptr [g_packetLoop]

		weatherPacket:
			pushad
			call HandleWeatherPacket
			popad
			mov eax, dword ptr [g_lastPacketTypeAddress]
			mov dword ptr [eax], esi
			jmp dword ptr [g_packetLoop]

		impactPacket:
			cmp byte ptr [g_consumeImpactTracker], 0
			je nativePacket
			pushad
			call HandleImpactTrackerPacket
			popad
			mov eax, dword ptr [g_lastPacketTypeAddress]
			mov dword ptr [eax], esi
			jmp dword ptr [g_packetLoop]

		nativePacket:
			lea eax, dword ptr [esi-0Ah]
			cmp eax, 0E8h
			jmp dword ptr [g_dispatchReturn]
		}
	}

	void ResetPingState()
	{
		AcquireSRWLockExclusive(&g_pingLock);
		PingOverlay::Detail::Reset(g_pingState);
		ReleaseSRWLockExclusive(&g_pingLock);
		g_pingMilliseconds.store(-1);
	}

}

namespace PingOverlay
{
	bool Install(DWORD clientBase, bool customPing,
		bool consumeImpactTracker)
	{
		if(!clientBase)
			return false;
		g_customPing = customPing;
		g_weatherEnabled = should_use_weather;
		g_consumeImpactTracker = consumeImpactTracker;
		g_readU8 = reinterpret_cast<ReadU8Fn>(clientBase + 0xF9A60);
		g_readU32 = reinterpret_cast<ReadU32Fn>(clientBase + 0xF9DA0);
		g_readString = reinterpret_cast<ReadStringFn>(clientBase + 0xF9F40);
		g_addByte = reinterpret_cast<AddByteFn>(clientBase + 0xF8290);
		g_addU32 = reinterpret_cast<AddU32Fn>(clientBase + 0xF88A0);
		g_sendPacket = reinterpret_cast<SendPacketFn>(clientBase + 0xF8E40);
		g_dispatchReturn = clientBase + 0x5C57F;
		g_packetLoop = clientBase + 0x5C3A5;
		g_lastPacketTypeAddress = clientBase + 0x239F4C;
		g_storePacketOpcode = StoreWindow::ServerOpcode();
		g_weatherPacketOpcode = WeatherSystem::ServerOpcode();
		if(g_storePacketOpcode == 0 || g_weatherPacketOpcode == 0)
			return false;
		const BYTE expectedDispatch[] = {
			0x8D, 0x46, 0xF6, 0x3D, 0xE8, 0x00, 0x00, 0x00
		};
		const DWORD dispatchAddress = clientBase + 0x5C577;
		if(!ClientMemory::MatchesCode(dispatchAddress, expectedDispatch,
			sizeof(expectedDispatch)))
			return false;
		return HookJMP(dispatchAddress, (DWORD)&HookPingDispatch) &&
			Nop(dispatchAddress + 5, 3);
	}

	void Draw(int surface, bool insideGame)
	{
		if(!g_customPing)
		{
			ResetPingState();
			return;
		}
		if(!PrintText)
		{
			ResetPingState();
			return;
		}

		if(!insideGame)
		{
			ResetPingState();
			return;
		}

		const int ping = g_pingMilliseconds.load();
		const char* status = "Waiting";
		int red = 255;
		int green = 255;
		int blue = 0;
		if(ping >= 0 && ping <= 80)
		{
			status = "Good";
			red = 100;
			green = 255;
		}
		else if(ping > 80 && ping <= 160)
			status = "Average";
		else if(ping > 160)
		{
			status = "Bad";
			red = 255;
			green = 52;
			blue = 52;
		}

		char text[48] = {};
		if(ping >= 0)
			_snprintf_s(text, sizeof(text), _TRUNCATE, "Ping: %d ms - %s",
				ping, status);
		else
			_snprintf_s(text, sizeof(text), _TRUNCATE, "Ping: -- ms - %s",
				status);
		PrintText(surface, 4, 4, 2, red, green, blue, text, 0);
	}

	void Shutdown()
	{
		ResetPingState();
		g_customPing = false;
		g_weatherEnabled = false;
		g_consumeImpactTracker = false;
		g_storePacketOpcode = 0;
		g_weatherPacketOpcode = 0;
	}
}

#endif
