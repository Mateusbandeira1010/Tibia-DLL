#include "../src/ping_overlay.h"

#include <cstdint>
#include <iostream>
#include <limits>

namespace
{
	namespace Ping = PingOverlay::Detail;
	int g_checks = 0;
	int g_failures = 0;

	void Check(bool condition, const char* expression, int line)
	{
		++g_checks;
		if(condition)
			return;
		++g_failures;
		std::cerr << "line " << line << ": check failed: " << expression << '\n';
	}

#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	void TestSingleHandshakeAndReplay()
	{
		Ping::PingState state;
		auto result = Ping::HandlePacket(state, 10, 1'000);
		CHECK(result.action == Ping::PingPacketAction::SendPong);
		CHECK(!result.slotEvicted);

		result = Ping::HandlePacket(state, 10, 1'025);
		CHECK(result.action == Ping::PingPacketAction::CompleteRtt);
		CHECK(result.elapsed == 25);

		for(std::size_t i = 0; i < 1'000; ++i)
			CHECK(Ping::HandlePacket(state, 10, 1'026 + static_cast<DWORD>(i)).action ==
				Ping::PingPacketAction::IgnoreDuplicate);
	}

	void TestOverlapInBothAckOrders()
	{
		Ping::PingState state;
		CHECK(Ping::HandlePacket(state, 100, 1'000).action == Ping::PingPacketAction::SendPong);
		CHECK(Ping::HandlePacket(state, 200, 6'000).action == Ping::PingPacketAction::SendPong);
		CHECK(Ping::HandlePacket(state, 200, 6'040).action == Ping::PingPacketAction::CompleteRtt);
		CHECK(Ping::HandlePacket(state, 100, 6'050).action == Ping::PingPacketAction::CompleteRtt);

		Ping::Reset(state);
		CHECK(Ping::HandlePacket(state, 100, 10'000).action == Ping::PingPacketAction::SendPong);
		CHECK(Ping::HandlePacket(state, 200, 15'000).action == Ping::PingPacketAction::SendPong);
		CHECK(Ping::HandlePacket(state, 100, 15'020).action == Ping::PingPacketAction::CompleteRtt);
		CHECK(Ping::HandlePacket(state, 200, 15'030).action == Ping::PingPacketAction::CompleteRtt);
	}

	void TestLateAckIsClosedInsteadOfReopened()
	{
		Ping::PingState state;
		CHECK(Ping::HandlePacket(state, 77, 1'000).action == Ping::PingPacketAction::SendPong);
		CHECK(Ping::HandlePacket(state, 77, 1'000 + Ping::WaitingTimeoutMs + 1).action ==
			Ping::PingPacketAction::IgnoreDuplicate);
		CHECK(Ping::HandlePacket(state, 77, 1'000 + Ping::WaitingTimeoutMs + 5'000).action ==
			Ping::PingPacketAction::IgnoreDuplicate);
	}

	void TestFixedCapacityAndDeterministicEviction()
	{
		Ping::PingState state;
		std::size_t evictions = 0;
		for(DWORD id = 1; id <= 100; ++id)
		{
			const auto result = Ping::HandlePacket(state, id, 2'000);
			CHECK(result.action == Ping::PingPacketAction::SendPong);
			evictions += result.slotEvicted ? 1 : 0;
		}
		CHECK(Ping::Size(state) == Ping::PingSlotCount);
		CHECK(evictions == 100 - Ping::PingSlotCount);

		Ping::Reset(state);
		CHECK(Ping::Size(state) == 0);
	}

	void TestStressIsBoundedByTrackedIds()
	{
		Ping::PingState state;
		std::size_t pongs = 0;
		std::size_t completions = 0;
		for(DWORD i = 0; i < 10'000; ++i)
		{
			const auto result = Ping::HandlePacket(state, (i % 16) + 1, 10'000 + i);
			pongs += result.action == Ping::PingPacketAction::SendPong ? 1 : 0;
			completions += result.action == Ping::PingPacketAction::CompleteRtt ? 1 : 0;
		}
		CHECK(pongs == 16);
		CHECK(completions == 16);
		CHECK(Ping::Size(state) == 16);
	}

	void TestTimerWrapAndZeroId()
	{
		Ping::PingState state;
		const DWORD beforeWrap = (std::numeric_limits<DWORD>::max)() - 15;
		CHECK(Ping::HandlePacket(state, 55, beforeWrap).action == Ping::PingPacketAction::SendPong);
		const auto wrapped = Ping::HandlePacket(state, 55, 32);
		CHECK(wrapped.action == Ping::PingPacketAction::CompleteRtt);
		CHECK(wrapped.elapsed == 48);
		CHECK(Ping::HandlePacket(state, 0, 33).action == Ping::PingPacketAction::IgnoreInvalid);
	}
}

int main()
{
	TestSingleHandshakeAndReplay();
	TestOverlapInBothAckOrders();
	TestLateAckIsClosedInsteadOfReopened();
	TestFixedCapacityAndDeterministicEviction();
	TestStressIsBoundedByTrackedIds();
	TestTimerWrapAndZeroId();

	if(g_failures == 0)
	{
		std::cout << g_checks << " checks passed\n";
		return 0;
	}

	std::cerr << g_failures << " of " << g_checks << " checks failed\n";
	return 1;
}
