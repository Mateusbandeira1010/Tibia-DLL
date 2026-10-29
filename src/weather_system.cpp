#include "weather_system.h"

#include "client_memory.h"
#include "hook.h"
#include "native_packet.h"
#include "obfuscated_string.h"
#include "timer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace
{
	constexpr std::size_t ZONE_WEATHER_PAYLOAD_SIZE = 23;
	constexpr DWORD WEATHER_SERVER_CHECK_TIMEOUT_MS = 3000;
	constexpr UINT WEATHER_AUTH_FAILURE_CODE = 0xE086003C;
	constexpr std::size_t PARTICLE_COUNT = 256;
	constexpr DWORD MAX_FRAME_TIME = 250;
	constexpr float MIN_VISIBLE_INTENSITY = 0.01F;
	constexpr DWORD GUI_PARENT_OFFSET = 0x0C;
	constexpr DWORD GUI_OFFSET_X = 0x14;
	constexpr DWORD GUI_OFFSET_Y = 0x18;
	constexpr DWORD GUI_WIDTH = 0x1C;
	constexpr DWORD GUI_HEIGHT = 0x20;
	constexpr std::size_t MAX_GUI_PARENT_DEPTH = 64;

	enum class WeatherType : BYTE
	{
		None = 0,
		Rain = 1,
		Snow = 2,
		Sand = 3
	};

	enum class WeatherServerCheckState : BYTE
	{
		Waiting,
		Valid,
		Invalid
	};

	using NativePacket = NativePacketBuffer::Packet;

	struct WeatherCommand
	{
		DWORD sequence = 0;
		WeatherType type = WeatherType::None;
		BYTE intensity = 0;
		std::int8_t windX = 0;
		std::int8_t windY = 0;
		WORD transitionMilliseconds = 0;
	};

	struct Particle
	{
		float x = 0.0F;
		float y = 0.0F;
		float speed = 0.0F;
		float phase = 0.0F;
		BYTE size = 1;
	};

	struct Rect
	{
		int x = 0;
		int y = 0;
		int width = 0;
		int height = 0;
	};

	using WeatherGetEngineAddrFn = DWORD (__cdecl*)();
	using DrawRectangleFn = void (__fastcall*)(DWORD, DWORD, DWORD, DWORD,
		DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
	using LoginWriteByteFn = void (__thiscall*)(void*, DWORD);

	DWORD g_networkBufferAddress = 0;
	DWORD g_mapWindowReturn = 0;
	volatile DWORD g_mapWindow = 0;
	volatile DWORD g_mapDrawGeneration = 0;
	DWORD g_observedMapDrawGeneration = 0;
	WeatherGetEngineAddrFn g_getEngineAddr = nullptr;
	LoginWriteByteFn g_loginWriteByte = nullptr;
	std::mutex g_mutex;
	std::array<Particle, PARTICLE_COUNT> g_particles = {};
	bool g_particlesInitialized = false;
	bool g_playerInsideGame = false;
	WeatherServerCheckState g_weatherServerCheck =
		WeatherServerCheckState::Waiting;
	DWORD g_weatherCheckStartedAt = 0;
	DWORD g_expectedWeatherSequence = 0;
	WeatherType g_type = WeatherType::None;
	float g_intensity = 0.0F;
	float g_transitionStartIntensity = 0.0F;
	float g_transitionTargetIntensity = 0.0F;
	DWORD g_transitionStartedAt = 0;
	DWORD g_transitionDuration = 0;
	DWORD g_lastFrameAt = 0;
	bool g_hasPendingCommand = false;
	WeatherCommand g_pendingCommand;
	DWORD g_pendingFadeInDuration = 0;
	std::int8_t g_windX = 0;
	std::int8_t g_windY = 0;

	[[noreturn]] void FailWeatherServerCheck() noexcept
	{
		TerminateProcess(GetCurrentProcess(), WEATHER_AUTH_FAILURE_CODE);
		ExitProcess(WEATHER_AUTH_FAILURE_CODE);
	}

	float RandomUnit(std::uint32_t& state) noexcept
	{
		state = state * 1664525U + 1013904223U;
		return static_cast<float>((state >> 8) & 0x00FFFFFFU) /
			static_cast<float>(0x01000000U);
	}

	void InitializeParticlesLocked() noexcept
	{
		if(g_particlesInitialized)
			return;

		std::uint32_t randomState = 0x0A57A860U;
		for(Particle& particle : g_particles)
		{
			particle.x = RandomUnit(randomState);
			particle.y = RandomUnit(randomState);
			particle.speed = 0.55F + RandomUnit(randomState) * 0.9F;
			particle.phase = RandomUnit(randomState) * 6.283185307F;
			particle.size = static_cast<BYTE>(1 +
				static_cast<int>(RandomUnit(randomState) * 3.0F));
		}
		g_particlesInitialized = true;
	}

	void ResetWeatherLocked() noexcept
	{
		g_particles.fill(Particle{});
		g_particlesInitialized = false;
		g_type = WeatherType::None;
		g_intensity = 0.0F;
		g_transitionStartIntensity = 0.0F;
		g_transitionTargetIntensity = 0.0F;
		g_transitionStartedAt = 0;
		g_transitionDuration = 0;
		g_lastFrameAt = 0;
		g_hasPendingCommand = false;
		g_pendingCommand = WeatherCommand();
		g_pendingFadeInDuration = 0;
		g_windX = 0;
		g_windY = 0;
		g_weatherServerCheck = WeatherServerCheckState::Waiting;
		g_weatherCheckStartedAt = 0;
		g_expectedWeatherSequence = 0;
		g_playerInsideGame = false;
		g_mapWindow = 0;
		g_observedMapDrawGeneration = g_mapDrawGeneration;
	}

	void BeginTransitionLocked(float target, DWORD duration, DWORD now) noexcept
	{
		g_transitionStartIntensity = g_intensity;
		g_transitionTargetIntensity = (std::clamp)(target, 0.0F, 100.0F);
		g_transitionStartedAt = now;
		g_transitionDuration = duration;
		if(duration == 0)
			g_intensity = g_transitionTargetIntensity;
	}

	void StartPendingCommandLocked(DWORD now) noexcept
	{
		if(!g_hasPendingCommand)
			return;

		const WeatherCommand command = g_pendingCommand;
		const DWORD fadeInDuration = g_pendingFadeInDuration;
		g_hasPendingCommand = false;
		g_pendingCommand = WeatherCommand();
		g_pendingFadeInDuration = 0;
		g_type = command.intensity == 0 ? WeatherType::None : command.type;
		g_windX = command.windX;
		g_windY = command.windY;
		g_intensity = 0.0F;
		BeginTransitionLocked(static_cast<float>(command.intensity),
			fadeInDuration, now);
	}

	void AdvanceTransitionLocked(DWORD now) noexcept
	{
		if(g_transitionDuration != 0)
		{
			const DWORD elapsed = now - g_transitionStartedAt;
			const float progress = (std::min)(1.0F,
				static_cast<float>(elapsed) /
				static_cast<float>(g_transitionDuration));
			g_intensity = g_transitionStartIntensity +
				(g_transitionTargetIntensity - g_transitionStartIntensity) * progress;
			if(progress < 1.0F)
				return;
			g_transitionDuration = 0;
		}

		g_intensity = g_transitionTargetIntensity;
		if(g_hasPendingCommand && g_intensity <= MIN_VISIBLE_INTENSITY)
		{
			StartPendingCommandLocked(now);
			return;
		}

		if(g_intensity <= MIN_VISIBLE_INTENSITY &&
			g_transitionTargetIntensity <= MIN_VISIBLE_INTENSITY)
			g_type = WeatherType::None;
	}

	void ApplyCommandLocked(const WeatherCommand& command, DWORD now) noexcept
	{
		AdvanceTransitionLocked(now);

		WeatherCommand normalized = command;
		if(normalized.type == WeatherType::None)
			normalized.intensity = 0;

		if(normalized.type == g_type || g_type == WeatherType::None ||
			g_intensity <= MIN_VISIBLE_INTENSITY)
		{
			g_hasPendingCommand = false;
			g_pendingCommand = WeatherCommand();
			g_pendingFadeInDuration = 0;
			g_type = normalized.intensity == 0 ? g_type : normalized.type;
			g_windX = normalized.windX;
			g_windY = normalized.windY;
			BeginTransitionLocked(static_cast<float>(normalized.intensity),
				normalized.transitionMilliseconds, now);
			if(normalized.intensity == 0 &&
				normalized.transitionMilliseconds == 0)
				g_type = WeatherType::None;
			return;
		}

		if(normalized.transitionMilliseconds == 0)
		{
			g_hasPendingCommand = false;
			g_type = normalized.intensity == 0 ? WeatherType::None : normalized.type;
			g_intensity = static_cast<float>(normalized.intensity);
			g_transitionStartIntensity = g_intensity;
			g_transitionTargetIntensity = g_intensity;
			g_transitionDuration = 0;
			g_windX = normalized.windX;
			g_windY = normalized.windY;
			return;
		}

		const DWORD fadeOutDuration =
			(static_cast<DWORD>(normalized.transitionMilliseconds) + 1U) / 2U;
		g_pendingCommand = normalized;
		g_pendingFadeInDuration =
			static_cast<DWORD>(normalized.transitionMilliseconds) - fadeOutDuration;
		g_hasPendingCommand = true;
		BeginTransitionLocked(0.0F, fadeOutDuration, now);
	}

	DWORD ReadU32LittleEndian(const BYTE* data) noexcept
	{
		return static_cast<DWORD>(data[0]) |
			(static_cast<DWORD>(data[1]) << 8) |
			(static_cast<DWORD>(data[2]) << 16) |
			(static_cast<DWORD>(data[3]) << 24);
	}

	WORD ReadU16LittleEndian(const BYTE* data) noexcept
	{
		return static_cast<WORD>(static_cast<WORD>(data[0]) |
			static_cast<WORD>(static_cast<WORD>(data[1]) << 8));
	}

	BYTE WeatherProtocolVersion() noexcept
	{
		auto value = TIBIA_OBF_BYTES(0x02);
		return value[0];
	}

	DWORD WeatherBuildId() noexcept
	{
		auto value = TIBIA_OBF_BYTES(0x6F, 0x27, 0x35, 0x01);
		return static_cast<DWORD>(value[0]) |
			(static_cast<DWORD>(value[1]) << 8) |
			(static_cast<DWORD>(value[2]) << 16) |
			(static_cast<DWORD>(value[3]) << 24);
	}

	bool MatchesWeatherServerMagic(const BYTE* payload) noexcept
	{
		auto magic = TIBIA_OBF_BYTES(
			0xD7, 0x2B, 0x91, 0x4E, 0xC3, 0x68, 0xAF, 0x15);
		return payload && std::memcmp(payload, magic.data(), magic.size()) == 0;
	}

	bool ParseCommand(const NativePacket& packet, WeatherCommand& command) noexcept
	{
		if(packet.size - packet.start < ZONE_WEATHER_PAYLOAD_SIZE)
			return false;
		__try {
			const BYTE* payload = packet.data + packet.start;
			if(!MatchesWeatherServerMagic(payload))
				return false;
			const BYTE version = payload[8];
			const DWORD buildId = ReadU32LittleEndian(payload + 9);
			const DWORD sequence = ReadU32LittleEndian(payload + 13);
			const BYTE rawType = payload[17];
			const BYTE rawIntensity = payload[18];
			if(version != WeatherProtocolVersion() ||
				buildId != WeatherBuildId() || sequence == 0 ||
				rawType > static_cast<BYTE>(WeatherType::Sand) ||
				rawIntensity > 100)
				return false;
			if(rawType == static_cast<BYTE>(WeatherType::None) &&
				(rawIntensity != 0 || payload[19] != 0 || payload[20] != 0))
				return false;

			command.sequence = sequence;
			command.type = static_cast<WeatherType>(rawType);
			command.intensity = rawIntensity;
			command.windX = static_cast<std::int8_t>(payload[19]);
			command.windY = static_cast<std::int8_t>(payload[20]);
			command.transitionMilliseconds = ReadU16LittleEndian(payload + 21);
			return true;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	__declspec(naked) void HookMapWindowDraw()
	{
		__asm {
			mov dword ptr [g_mapWindow], ecx
			inc dword ptr [g_mapDrawGeneration]
			push ebp
			mov ebp, esp
			push -1
			jmp dword ptr [g_mapWindowReturn]
		}
	}

	void __fastcall HookWeatherLoginMarker(void* writer, void*,
		DWORD challengeRandom) noexcept
	{
		if(!writer || !g_loginWriteByte)
			return;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			ResetWeatherLocked();
		}
		g_loginWriteByte(writer, challengeRandom);
		auto marker = TIBIA_OBF_BYTES(
			0x5A, 0x57, 0x44, 0x4C,
			0x01, 0x3C, 0xA5, 0x86);
		for(std::size_t index = 0; index < marker.size(); ++index)
			g_loginWriteByte(writer, marker[index]);
	}

	bool ReadMapRectangle(Rect& rectangle) noexcept
	{
		rectangle = Rect{};
		const DWORD mapWindow = g_mapWindow;
		if(!mapWindow)
			return false;
		std::int64_t originX = 0;
		std::int64_t originY = 0;
		__try {
			rectangle.width = *reinterpret_cast<const int*>(mapWindow + GUI_WIDTH);
			rectangle.height = *reinterpret_cast<const int*>(mapWindow + GUI_HEIGHT);

			// GUI offsets are relative to the parent. The weather is drawn on the
			// root surface, so reproduce the same parent transform used by the
			// native client instead of assuming that MapWindow starts at (0, 0).
			DWORD gui = mapWindow;
			std::size_t depth = 0;
			while(gui && depth < MAX_GUI_PARENT_DEPTH)
			{
				originX += *reinterpret_cast<const int*>(gui + GUI_OFFSET_X);
				originY += *reinterpret_cast<const int*>(gui + GUI_OFFSET_Y);
				gui = *reinterpret_cast<const DWORD*>(gui + GUI_PARENT_OFFSET);
				++depth;
			}
			if(gui)
				return false;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
		if(rectangle.width <= 0 || rectangle.height <= 0 ||
			rectangle.width > 4096 || rectangle.height > 4096 ||
			originX < -4096 || originX > 4096 ||
			originY < -4096 || originY > 4096)
			return false;
		rectangle.x = static_cast<int>(originX);
		rectangle.y = static_cast<int>(originY);

		HWND window = GetForegroundWindow();
		if(window)
		{
			DWORD processId = 0;
			GetWindowThreadProcessId(window, &processId);
			RECT client = {};
			if(processId == GetCurrentProcessId() && GetClientRect(window, &client))
			{
				const int left = (std::max)(rectangle.x,
					static_cast<int>(client.left));
				const int top = (std::max)(rectangle.y,
					static_cast<int>(client.top));
				const int right = (std::min)(rectangle.x + rectangle.width,
					static_cast<int>(client.right));
				const int bottom = (std::min)(rectangle.y + rectangle.height,
					static_cast<int>(client.bottom));
				rectangle.x = left;
				rectangle.y = top;
				rectangle.width = right - left;
				rectangle.height = bottom - top;
			}
		}
		return rectangle.width > 0 && rectangle.height > 0;
	}

	void SafeDrawClippedRectangle(DrawRectangleFn drawRectangle, DWORD engine,
		int surface, const Rect& clip, int x, int y, int width, int height,
		int red, int green, int blue) noexcept
	{
		const int left = (std::max)(x, clip.x);
		const int top = (std::max)(y, clip.y);
		const int right = (std::min)(x + width, clip.x + clip.width);
		const int bottom = (std::min)(y + height, clip.y + clip.height);
		if(left >= right || top >= bottom)
			return;
		__try {
			drawRectangle(engine, 0, static_cast<DWORD>(surface),
				static_cast<DWORD>(left), static_cast<DWORD>(top),
				static_cast<DWORD>(right - left), static_cast<DWORD>(bottom - top),
				static_cast<DWORD>(red), static_cast<DWORD>(green),
				static_cast<DWORD>(blue));
		} __except(EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	bool ResolveDrawRectangle(DWORD& engine,
		DrawRectangleFn& drawRectangle) noexcept
	{
		engine = 0;
		drawRectangle = nullptr;
		if(!g_getEngineAddr)
			return false;
		__try {
			engine = g_getEngineAddr();
			if(!engine)
				return false;
			const DWORD table = *reinterpret_cast<const DWORD*>(engine);
			if(!table)
				return false;
			const DWORD drawAddress =
				*reinterpret_cast<const DWORD*>(table + 0x14);
			if(!drawAddress)
				return false;
			drawRectangle = reinterpret_cast<DrawRectangleFn>(drawAddress);
			return true;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			engine = 0;
			drawRectangle = nullptr;
			return false;
		}
	}

	float WrapUnit(float value) noexcept
	{
		value -= std::floor(value);
		return value < 0.0F ? value + 1.0F : value;
	}

	bool UpdatePlayerInsideGameLocked(bool playerIdOnline) noexcept
	{
		const DWORD mapDrawGeneration = g_mapDrawGeneration;
		const bool mapDrawnThisFrame =
			mapDrawGeneration != g_observedMapDrawGeneration;
		g_observedMapDrawGeneration = mapDrawGeneration;

		if(playerIdOnline && mapDrawnThisFrame)
		{
			g_playerInsideGame = true;
			return true;
		}

		if(g_playerInsideGame)
			ResetWeatherLocked();
		return false;
	}

	bool CanRenderLocked() noexcept
	{
		return g_playerInsideGame &&
			g_weatherServerCheck == WeatherServerCheckState::Valid &&
			g_expectedWeatherSequence != 0 &&
			g_type != WeatherType::None &&
			g_intensity > MIN_VISIBLE_INTENSITY;
	}

	void AdvanceParticlesLocked(float seconds) noexcept
	{
		const float windX = static_cast<float>(g_windX) / 127.0F;
		const float windY = static_cast<float>(g_windY) / 127.0F;
		for(Particle& particle : g_particles)
		{
			switch(g_type)
			{
				case WeatherType::Rain:
					particle.x = WrapUnit(particle.x +
						windX * (0.10F + particle.speed * 0.03F) * seconds);
					particle.y = WrapUnit(particle.y +
						(0.75F + particle.speed * 0.45F + windY * 0.12F) * seconds);
					break;
				case WeatherType::Snow:
					particle.phase += seconds * (0.8F + particle.speed * 0.25F);
					particle.x = WrapUnit(particle.x +
						(windX * 0.055F + std::sin(particle.phase) * 0.012F) * seconds);
					particle.y = WrapUnit(particle.y +
						(0.08F + particle.speed * 0.075F + windY * 0.025F) * seconds);
					break;
				case WeatherType::Sand:
				{
					const float direction = windX < 0.0F ? -1.0F : 1.0F;
					particle.x = WrapUnit(particle.x + direction *
						(0.18F + particle.speed * 0.16F + std::fabs(windX) * 0.12F) * seconds);
					particle.y = WrapUnit(particle.y +
						(windY * 0.05F + std::sin(particle.phase) * 0.008F) * seconds);
					particle.phase += seconds * 0.65F;
					break;
				}
				case WeatherType::None:
					break;
			}
		}
	}

	void DrawParticle(DrawRectangleFn drawRectangle, DWORD engine, int surface,
		const Rect& clip, const Particle& particle, WeatherType type,
		std::int8_t windX) noexcept
	{
		const int x = clip.x + static_cast<int>(particle.x * clip.width);
		const int y = clip.y + static_cast<int>(particle.y * clip.height);
		switch(type)
		{
			case WeatherType::Rain:
			{
				const int slant = windX < 0 ? -1 : (windX > 0 ? 1 : 0);
				SafeDrawClippedRectangle(drawRectangle, engine, surface, clip,
					x, y, 1, 4, 115, 166, 225);
				SafeDrawClippedRectangle(drawRectangle, engine, surface, clip,
					x + slant, y + 4, 1, 3, 94, 145, 205);
				break;
			}
			case WeatherType::Snow:
				SafeDrawClippedRectangle(drawRectangle, engine, surface, clip,
					x, y, particle.size, particle.size, 235, 243, 252);
				break;
			case WeatherType::Sand:
				SafeDrawClippedRectangle(drawRectangle, engine, surface, clip,
					x, y, 3 + particle.size, 1, 204, 168, 94);
				break;
			case WeatherType::None:
				break;
		}
	}
}

namespace WeatherSystem
{
	BYTE ServerOpcode() noexcept
	{
		auto value = TIBIA_OBF_BYTES(0x3C);
		return value[0];
	}

	bool Install(DWORD clientBase, bool enableLoginMarker) noexcept
	{
		// A build without the authenticated login marker cannot safely accept
		// weather packets. Fail before installing any map hook so Init() can
		// restore a clean client state.
		if(!clientBase || !enableLoginMarker)
			return false;
		g_networkBufferAddress = clientBase + 0x3998AC;
		g_getEngineAddr = reinterpret_cast<WeatherGetEngineAddrFn>(
			clientBase + 0x122A90);
		g_mapWindowReturn = clientBase + 0xF4545;
		const DWORD mapDrawAddress = clientBase + 0xF4540;
		const BYTE expectedMapDraw[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
		if(!ClientMemory::MatchesCode(mapDrawAddress, expectedMapDraw,
			sizeof(expectedMapDraw)))
			return false;
		if(!HookJMP(mapDrawAddress, (DWORD)&HookMapWindowDraw))
			return false;

		const DWORD loginMarkerCall = clientBase + 0x16914;
		const BYTE expectedLoginCall[] = {0xE8, 0x27, 0x2C, 0x14, 0x00};
		if(!ClientMemory::MatchesCode(loginMarkerCall, expectedLoginCall,
			sizeof(expectedLoginCall)))
			return false;
		g_loginWriteByte = reinterpret_cast<LoginWriteByteFn>(
			clientBase + 0x159540);
		return HookCall(loginMarkerCall, (DWORD)&HookWeatherLoginMarker);
	}

	void HandleServerPacket() noexcept
	{
		NativePacket packet;
		if(!NativePacketBuffer::Capture(g_networkBufferAddress,
			NativePacketBuffer::MAXIMUM_SIZE, packet))
			FailWeatherServerCheck();

		WeatherCommand command;
		if(!ParseCommand(packet, command))
			FailWeatherServerCheck();
		if(!NativePacketBuffer::SetPosition(g_networkBufferAddress, packet,
			ZONE_WEATHER_PAYLOAD_SIZE))
			FailWeatherServerCheck();

		std::unique_lock<std::mutex> lock(g_mutex);
		if(g_weatherServerCheck == WeatherServerCheckState::Invalid)
		{
			lock.unlock();
			FailWeatherServerCheck();
		}
		if(g_weatherServerCheck == WeatherServerCheckState::Waiting)
		{
			g_expectedWeatherSequence = command.sequence;
			g_weatherServerCheck = WeatherServerCheckState::Valid;
			g_weatherCheckStartedAt = 0;
		}
		else if(g_expectedWeatherSequence == 0 ||
			command.sequence != g_expectedWeatherSequence)
		{
			g_weatherServerCheck = WeatherServerCheckState::Invalid;
			lock.unlock();
			FailWeatherServerCheck();
		}
		InitializeParticlesLocked();
		ApplyCommandLocked(command, getTimerTick());
	}

	bool Draw(int surface, bool online) noexcept
	{
		std::unique_lock<std::mutex> lock(g_mutex);
		if(!UpdatePlayerInsideGameLocked(online))
			return false;
		const DWORD now = getTimerTick();
		if(g_weatherServerCheck == WeatherServerCheckState::Waiting)
		{
			if(g_weatherCheckStartedAt == 0)
				g_weatherCheckStartedAt = now;
			else if(now - g_weatherCheckStartedAt >=
				WEATHER_SERVER_CHECK_TIMEOUT_MS)
			{
				g_weatherServerCheck = WeatherServerCheckState::Invalid;
				lock.unlock();
				FailWeatherServerCheck();
			}
		}
		else if(g_weatherServerCheck == WeatherServerCheckState::Invalid ||
			g_expectedWeatherSequence == 0)
		{
			g_weatherServerCheck = WeatherServerCheckState::Invalid;
			lock.unlock();
			FailWeatherServerCheck();
		}

		AdvanceTransitionLocked(now);
		if(!CanRenderLocked())
		{
			g_lastFrameAt = now;
			return true;
		}

		Rect mapRectangle;
		if(!ReadMapRectangle(mapRectangle))
			return true;

		DWORD engine = 0;
		DrawRectangleFn drawRectangle = nullptr;
		if(!ResolveDrawRectangle(engine, drawRectangle))
			return true;

		DWORD elapsed = 0;
		if(g_lastFrameAt != 0)
			elapsed = (std::min)(now - g_lastFrameAt, MAX_FRAME_TIME);
		g_lastFrameAt = now;
		AdvanceParticlesLocked(static_cast<float>(elapsed) / 1000.0F);

		const std::size_t visibleParticles = (std::min)(PARTICLE_COUNT,
			static_cast<std::size_t>(g_intensity *
				static_cast<float>(PARTICLE_COUNT) / 100.0F));
		for(std::size_t index = 0; index < visibleParticles; ++index)
			DrawParticle(drawRectangle, engine, surface, mapRectangle,
				g_particles[index], g_type, g_windX);
		return true;
	}

	void Shutdown() noexcept
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		ResetWeatherLocked();
		g_mapDrawGeneration = 0;
		g_observedMapDrawGeneration = 0;
		g_mapWindowReturn = 0;
		g_networkBufferAddress = 0;
		g_getEngineAddr = nullptr;
		g_loginWriteByte = nullptr;
	}
}
