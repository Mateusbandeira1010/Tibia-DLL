#include "timer.h"
#include "config.h"

namespace
{
	INIT_ONCE g_timerInit = INIT_ONCE_STATIC_INIT;
	bool g_hiresTimerAvailable = false;
	LARGE_INTEGER g_hiresStart = {};
	LARGE_INTEGER g_hiresFrequency = {};
	ULONGLONG g_fallbackStart = 0;
	volatile LONG g_lastTimerTick = 0;

	BOOL CALLBACK InitializeTimer(PINIT_ONCE, PVOID, PVOID*)
	{
		g_fallbackStart = GetTickCount64();
		#ifdef __CONFIG__
		g_hiresTimerAvailable = should_use_hirestimer &&
			QueryPerformanceFrequency(&g_hiresFrequency) &&
			QueryPerformanceCounter(&g_hiresStart);
		#endif
		return TRUE;
	}

	DWORD PublishMonotonicTick(DWORD tick) noexcept
	{
		LONG observed = InterlockedCompareExchange(&g_lastTimerTick, 0, 0);
		for(;;)
		{
			const DWORD observedTick = static_cast<DWORD>(observed);
			// Signed subtraction preserves native DWORD wrap-around semantics.
			if(static_cast<LONG>(tick - observedTick) <= 0)
				return observedTick;
			const LONG previous = InterlockedCompareExchange(&g_lastTimerTick,
				static_cast<LONG>(tick), observed);
			if(previous == observed)
				return tick;
			observed = previous;
		}
	}
}

DWORD getTimerTick()
{
	if(!InitOnceExecuteOnce(&g_timerInit, InitializeTimer, nullptr, nullptr))
		return 0;

	if(g_hiresTimerAvailable)
	{
		LARGE_INTEGER now = {};
		if(QueryPerformanceCounter(&now))
		{
			const LONGLONG elapsed = now.QuadPart - g_hiresStart.QuadPart;
			const LONGLONG milliseconds =
				(elapsed * 1000) / g_hiresFrequency.QuadPart;
			return PublishMonotonicTick(static_cast<DWORD>(milliseconds));
		}
	}

	return PublishMonotonicTick(
		static_cast<DWORD>(GetTickCount64() - g_fallbackStart));
}
