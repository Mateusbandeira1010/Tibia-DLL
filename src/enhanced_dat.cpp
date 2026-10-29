#include "enhanced_dat.h"
#include "config.h"
#include "hook.h"
#include "security_build_config.h"
#include "timer.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
	enum DatThingCategory
	{
		DAT_CATEGORY_ITEM = 0,
		DAT_CATEGORY_OUTFIT = 1,
		DAT_CATEGORY_EFFECT = 2,
		DAT_CATEGORY_MISSILE = 3
	};

	struct FrameGroup86
	{
		BYTE type;
		DWORD width;
		DWORD height;
		DWORD exactSize;
		DWORD layers;
		DWORD patternX;
		DWORD patternY;
		DWORD patternZ;
		DWORD phases;
		std::vector<unsigned long long> cumulativeDurations;
		unsigned long long cycleDuration;
		std::vector<DWORD> sprites;
	};

	struct EnhancedThing86
	{
		DatThingCategory category = DAT_CATEGORY_ITEM;
		std::vector<FrameGroup86> groups;
	};

	struct DrawContext
	{
		DWORD creaturePtr;
		DWORD creatureId;
		DWORD thingType;
		LONG displacementX;
		LONG displacementY;
		DWORD tick;
		DWORD movementTick;
		DWORD stepStartTick;
		DWORD stepEndTick;
		DWORD stepBaseDuration;
		DWORD floorZ;
		DWORD groupStartTick;
		bool active;
		bool walking;
		bool stepTimingValid;
	};

	struct CreatureGroupState
	{
		DWORD creatureId = 0;
		DWORD startTick = 0;
		DWORD lastSeenTick = 0;
		DWORD stepStartTick = 0;
		DWORD stepEndTick = 0;
		DWORD movingPhaseTick = 0;
		DWORD movingStepStartTick = 0;
		DWORD movingStepEndTick = 0;
		DWORD movingPhase = 0;
		DWORD movingPhaseCount = 0;
		DWORD movingPhaseDuration = 0;
		DWORD floorZ = 0;
		BYTE groupType = 0xFF;
		bool hasStep = false;
		bool hasFloor = false;
		bool suppressWalking = false;
		bool hasMovingPhase = false;
		bool hasMovingStep = false;
	};

	using ParseDatThingFn = void (__cdecl*)(DWORD reader, DWORD thingType);
	using ReadDatU8Fn = BYTE (__thiscall*)(DWORD reader);
	using ReadDatU16Fn = WORD (__thiscall*)(DWORD reader);
	using ReadDatU32Fn = DWORD (__thiscall*)(DWORD reader);
	using ClientAllocFn = void* (__cdecl*)(size_t size);
	using CreateEffectSlotFn = int (__cdecl*)(DWORD kind, DWORD x, DWORD y,
		DWORD z);
	using CreateMagicEffectFn = void (__cdecl*)(DWORD x, DWORD y, DWORD z,
		DWORD type);
	using GetEffectTypeFn = DWORD (__cdecl*)(int slot);
	using GetCreatureAnimationPhaseFn = DWORD (__thiscall*)(DWORD creaturePtr,
		DWORD availablePhases);
	using NativeTimerFn = DWORD (__cdecl*)();

	const int EFFECT_SLOT_COUNT = 200;
	const size_t MAX_CREATURE_GROUP_STATES = 4096;
	const DWORD CREATURE_GROUP_STATE_TTL = 60000;
	const DWORD MAX_VALID_STEP_DURATION = 60000;
	const DWORD CREATURE_POSITION_Z_OFFSET = 0x2C;
	const DWORD CREATURE_WALK_BASE_DEADLINE_OFFSET = 0x38;
	const DWORD CREATURE_WALK_END_OFFSET = 0x3C;
	const DWORD CREATURE_WALK_BASE_DURATION_OFFSET = 0x48;
	const LONG CREATURE_WALK_CONTINUOUS_STEP_GAP = 50;
	const DWORD CREATURE_WALK_PHASE_MIN_DURATION = 20;
	const DWORD CREATURE_WALK_PHASE_DURATION_PADDING = 20;
	const DWORD CREATURE_WALK_PHASE_DELAY = 10;

	std::unordered_map<DWORD, EnhancedThing86> g_things;
	std::unordered_map<unsigned long long, CreatureGroupState> g_creatureGroups;
	DrawContext g_draw = {};
	bool g_extendedSprites = false;
	bool g_sessionOnline = false;
	volatile LONG g_category = DAT_CATEGORY_ITEM;
	DWORD g_parseThingAddr = 0;
	DWORD g_readU8Addr = 0;
	DWORD g_readU16Addr = 0;
	DWORD g_readU32Addr = 0;
	DWORD g_textureNativeContinue = 0;
	DWORD g_textureDone = 0;
	DWORD g_propertySupported = 0;
	DWORD g_propertyUnknown = 0;
	DWORD g_propertyLoop = 0;
	DWORD g_spriteLookupDone = 0;
	DWORD g_createEffectSlotAddr = 0;
	DWORD g_createMagicEffectAddr = 0;
	DWORD g_getEffectTypeAddr = 0;
	DWORD g_getCreatureAnimationPhaseAddr = 0;
	DWORD g_nativeTimerAddr = 0;
	ClientAllocFn g_clientAlloc = nullptr;
	DWORD g_effectStartTicks[EFFECT_SLOT_COUNT] = {};
	volatile LONG g_renderEffectSlot = -1;
	DWORD g_renderEffectTick = 0;
	volatile LONG g_parsedThings = 0;
	volatile LONG g_retainedThings = 0;
	volatile LONG g_effectThingCount = 0;

	#if TIBIA_ENABLE_DEV_LOGS
	void FrameGroupsLog(const char* format, ...) noexcept
	{
		if(!should_debug_frame_groups || !format)
			return;
		FILE* file = fopen("framegroups_debug.log", "ab");
		if(!file)
			return;
		SYSTEMTIME now = {};
		GetLocalTime(&now);
		fprintf(file, "[%02u:%02u:%02u.%03u] ",
			static_cast<unsigned int>(now.wHour),
			static_cast<unsigned int>(now.wMinute),
			static_cast<unsigned int>(now.wSecond),
			static_cast<unsigned int>(now.wMilliseconds));
		va_list arguments;
		va_start(arguments, format);
		vfprintf(file, format, arguments);
		va_end(arguments);
		fprintf(file, "\r\n");
		fclose(file);
	}

	void ResetFrameGroupsLog() noexcept
	{
		if(!should_debug_frame_groups)
			return;
		FILE* file = fopen("framegroups_debug.log", "wb");
		if(!file)
			return;
		fprintf(file, "Tibia-DLL-CIP frame groups diagnostics\r\n");
		fclose(file);
	}
	#else
	#define FrameGroupsLog(...) ((void)0)
	#define ResetFrameGroupsLog() ((void)0)
	#endif

	DWORD AnimationTick()
	{
		// Sprite lookup can run hundreds of times per frame. Query the high-
		// resolution clock only once per Windows tick on each render thread.
		static thread_local bool initialized = false;
		static thread_local ULONGLONG coarseTick = 0;
		static thread_local DWORD animationTick = 0;
		const ULONGLONG now = GetTickCount64();
		if(!initialized || now != coarseTick)
		{
			initialized = true;
			coarseTick = now;
			animationTick = getTimerTick();
		}
		return animationTick;
	}

	BYTE ReadU8(DWORD reader)
	{
		return ((ReadDatU8Fn)g_readU8Addr)(reader);
	}

	WORD ReadU16(DWORD reader)
	{
		return ((ReadDatU16Fn)g_readU16Addr)(reader);
	}

	DWORD ReadU32(DWORD reader)
	{
		return ((ReadDatU32Fn)g_readU32Addr)(reader);
	}

	DWORD ReadSpriteId(DWORD reader)
	{
		return g_extendedSprites ? ReadU32(reader) : ReadU16(reader);
	}

	bool RuntimeFrameGroups()
	{
	#ifdef __CONFIG__
		return should_use_frame_groups;
	#else
		return false;
	#endif
	}

	bool RuntimeAnimations()
	{
	#ifdef __CONFIG__
		return should_use_improved_animations;
	#else
		return false;
	#endif
	}

	bool LayoutFrameGroups()
	{
	#ifdef __CONFIG__
		return dat_has_frame_groups;
	#else
		return false;
	#endif
	}

	bool LayoutAnimations()
	{
	#ifdef __CONFIG__
		return dat_has_improved_animations;
	#else
		return false;
	#endif
	}

	bool __stdcall ShouldParseEnhanced()
	{
		return LayoutAnimations() ||
			(LayoutFrameGroups() && g_category == DAT_CATEGORY_OUTFIT);
	}

	bool __stdcall ConsumeExtendedProperty(DWORD reader, DWORD flag)
	{
		if(!reader)
			return false;
		if(flag == 0x20)
		{
			ReadU16(reader);
			return true;
		}
		if(flag == 0x21)
		{
			ReadU16(reader);
			ReadU16(reader);
			ReadU16(reader);
			const WORD nameLength = ReadU16(reader);
			for(WORD i = 0; i < nameLength; ++i)
				ReadU8(reader);
			ReadU16(reader);
			ReadU16(reader);
			return true;
		}
		if(flag == 0x27)
		{
			for(int i = 0; i < 8; ++i)
				ReadU16(reader);
			return true;
		}
		return false;
	}

	bool ReadGroup(DWORD reader, BYTE groupType, FrameGroup86& group)
	{
		group.type = groupType;
		group.width = ReadU8(reader);
		group.height = ReadU8(reader);
		if(group.width == 0 || group.height == 0)
			return false;
		group.exactSize = (group.width > 1 || group.height > 1) ? ReadU8(reader) : 32;
		group.layers = ReadU8(reader);
		group.patternX = ReadU8(reader);
		group.patternY = ReadU8(reader);
		group.patternZ = ReadU8(reader);
		group.phases = ReadU8(reader);
		if(group.layers == 0 || group.patternX == 0 || group.patternY == 0 ||
			group.patternZ == 0 || group.phases == 0)
			return false;

		group.cycleDuration = 0;
		if(group.phases > 1 && LayoutAnimations())
		{
			ReadU8(reader);  // animation mode; 8.60 playback is time-based
			ReadU32(reader); // loop count; native slots control their lifetime
			ReadU8(reader);  // asynchronous start frame
			group.cumulativeDurations.reserve(group.phases);
			for(DWORD i = 0; i < group.phases; ++i)
			{
				DWORD minimum = ReadU32(reader);
				DWORD maximum = ReadU32(reader);
				if(minimum == 0 && maximum == 0)
					minimum = maximum = 100;
				if(maximum < minimum)
					maximum = minimum;
				group.cycleDuration += minimum + (maximum - minimum) / 2;
				group.cumulativeDurations.push_back(group.cycleDuration);
			}
		}

		size_t count = group.width;
		const DWORD factors[] = { group.height, group.layers, group.patternX,
			group.patternY, group.patternZ, group.phases };
		for(DWORD factor : factors)
		{
			if(factor == 0 || count > 0x4000 / factor)
				return false;
			count *= factor;
		}

		group.sprites.resize(count);
		for(size_t i = 0; i < count; ++i)
			group.sprites[i] = ReadSpriteId(reader);
		return true;
	}

	bool ParseTexturePatternsImpl(DWORD reader, DWORD thingType)
	{
		if(!reader || !thingType || !g_clientAlloc)
			return false;

		const bool groupedOutfit = LayoutFrameGroups() && g_category == DAT_CATEGORY_OUTFIT;
		BYTE groupCount = groupedOutfit ? ReadU8(reader) : 1;
		if(groupCount == 0 || groupCount > 16)
		{
			FrameGroupsLog("invalid group count category=%ld count=%u",
				g_category, static_cast<unsigned int>(groupCount));
			return false;
		}

		EnhancedThing86 enhanced = {};
		enhanced.category = (DatThingCategory)g_category;
		enhanced.groups.reserve(groupCount);
		for(BYTE i = 0; i < groupCount; ++i)
		{
			FrameGroup86 group = {};
			const BYTE type = groupedOutfit ? ReadU8(reader) : 0;
			if(!ReadGroup(reader, type, group))
			{
				FrameGroupsLog("invalid group category=%ld index=%u",
					g_category, static_cast<unsigned int>(i));
				return false;
			}
			enhanced.groups.push_back(std::move(group));
		}

		const FrameGroup86& native = enhanced.groups.front();
		*(DWORD*)(thingType + 0x00) = native.width;
		*(DWORD*)(thingType + 0x04) = native.height;
		*(DWORD*)(thingType + 0x08) = native.exactSize;
		*(DWORD*)(thingType + 0x0C) = native.layers;
		*(DWORD*)(thingType + 0x10) = native.patternX;
		*(DWORD*)(thingType + 0x14) = native.patternY;
		*(DWORD*)(thingType + 0x18) = native.patternZ;
		*(DWORD*)(thingType + 0x1C) = native.phases;

		const size_t elementSize = g_extendedSprites ? sizeof(DWORD) : sizeof(WORD);
		void* nativeSprites = g_clientAlloc(native.sprites.size() * elementSize);
		if(!nativeSprites)
			return false;
		if(g_extendedSprites)
		{
			DWORD* output = (DWORD*)nativeSprites;
			for(size_t i = 0; i < native.sprites.size(); ++i)
				output[i] = native.sprites[i];
		}
		else
		{
			WORD* output = (WORD*)nativeSprites;
			for(size_t i = 0; i < native.sprites.size(); ++i)
				output[i] = (WORD)native.sprites[i];
		}
		*(void**)(thingType + 0x20) = nativeSprites;

		bool retain = enhanced.groups.size() > 1;
		for(size_t i = 0; i < enhanced.groups.size() && !retain; ++i)
			retain = !enhanced.groups[i].cumulativeDurations.empty();
		if(retain)
		{
			g_things[thingType] = std::move(enhanced);
			if(should_debug_frame_groups)
				InterlockedIncrement(&g_retainedThings);
		}
		if(should_debug_frame_groups)
		{
			const LONG parsed = InterlockedIncrement(&g_parsedThings);
			if(parsed == 1 || parsed % 1000 == 0)
			{
				FrameGroupsLog("parse progress things=%ld retained=%ld category=%ld",
					parsed, InterlockedCompareExchange(&g_retainedThings, 0, 0),
					g_category);
			}
		}
		return true;
	}

	bool ParseTexturePatternsNoexcept(DWORD reader, DWORD thingType) noexcept
	{
		try
		{
			return ParseTexturePatternsImpl(reader, thingType);
		}
		catch(...)
		{
			return false;
		}
	}

	bool __stdcall ParseTexturePatterns(DWORD reader, DWORD thingType)
	{
		__try {
			return ParseTexturePatternsNoexcept(reader, thingType);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	__declspec(noreturn) void __stdcall AbortInvalidDat()
	{
		MessageBoxA(nullptr,
			"Tibia.dat contains an invalid or incompatible frame group.",
			"Tibia DAT Error", MB_OK | MB_ICONERROR);
		ExitProcess(ERROR_BAD_FORMAT);
	}

	void ParseCategory(DWORD reader, DWORD thingType, LONG category)
	{
		const LONG previous = g_category;
		g_category = category;
		((ParseDatThingFn)g_parseThingAddr)(reader, thingType);
		g_category = previous;
	}

	void __cdecl ParseItem(DWORD reader, DWORD thingType) { ParseCategory(reader, thingType, DAT_CATEGORY_ITEM); }
	void __cdecl ParseOutfit(DWORD reader, DWORD thingType) { ParseCategory(reader, thingType, DAT_CATEGORY_OUTFIT); }
	void __cdecl ParseEffect(DWORD reader, DWORD thingType)
	{
		ParseCategory(reader, thingType, DAT_CATEGORY_EFFECT);
		InterlockedIncrement(&g_effectThingCount);
	}
	void __cdecl ParseMissile(DWORD reader, DWORD thingType) { ParseCategory(reader, thingType, DAT_CATEGORY_MISSILE); }

	bool ShouldSelectWalkingGroup(DWORD thingType) noexcept
	{
		if(!g_draw.active || !g_draw.walking ||
			(g_draw.thingType != 0 && g_draw.thingType != thingType))
		{
			return false;
		}

		const unsigned long long key =
			(static_cast<unsigned long long>(g_draw.creaturePtr) << 32) |
			thingType;
		const auto entry = g_creatureGroups.find(key);
		if(entry == g_creatureGroups.end() ||
			entry->second.creatureId != g_draw.creatureId)
		{
			return true;
		}

		const CreatureGroupState& state = entry->second;
		if(state.hasFloor && state.floorZ != g_draw.floorZ)
			return false;
		if(!state.suppressWalking)
			return true;
		if(!g_draw.stepTimingValid)
			return false;

		return !state.hasStep ||
			state.stepStartTick != g_draw.stepStartTick ||
			state.stepEndTick != g_draw.stepEndTick;
	}

	const FrameGroup86* SelectGroup(const EnhancedThing86& thing, DWORD thingType)
	{
		if(thing.groups.empty())
			return nullptr;
		if(!RuntimeFrameGroups())
			return &thing.groups.front();
		const BYTE wanted = ShouldSelectWalkingGroup(thingType) ? 1 : 0;
		for(size_t i = 0; i < thing.groups.size(); ++i)
			if(thing.groups[i].type == wanted)
				return &thing.groups[i];
		for(size_t i = 0; i < thing.groups.size(); ++i)
			if(thing.groups[i].type == 0)
				return &thing.groups[i];
		return &thing.groups.front();
	}

	void MakeRoomForCreatureGroup(DWORD tick) noexcept
	{
		if(g_creatureGroups.size() < MAX_CREATURE_GROUP_STATES)
			return;

		for(auto it = g_creatureGroups.begin(); it != g_creatureGroups.end();)
		{
			if(tick - it->second.lastSeenTick > CREATURE_GROUP_STATE_TTL)
				it = g_creatureGroups.erase(it);
			else
				++it;
		}

		while(g_creatureGroups.size() >= MAX_CREATURE_GROUP_STATES)
		{
			auto oldest = g_creatureGroups.end();
			DWORD oldestAge = 0;
			for(auto it = g_creatureGroups.begin();
				it != g_creatureGroups.end(); ++it)
			{
				const DWORD age = tick - it->second.lastSeenTick;
				if(oldest == g_creatureGroups.end() || age > oldestAge)
				{
					oldest = it;
					oldestAge = age;
				}
			}
			if(oldest == g_creatureGroups.end())
				break;
			g_creatureGroups.erase(oldest);
		}
	}

	void BindCreatureGroup(DWORD thingType, BYTE groupType) noexcept
	{
		g_draw.groupStartTick = 0;
		if(!g_draw.active || !g_draw.creaturePtr ||
			(g_draw.thingType != 0 && g_draw.thingType != thingType))
			return;

		try
		{
			const unsigned long long key =
				(static_cast<unsigned long long>(g_draw.creaturePtr) << 32) |
				thingType;
			auto entry = g_creatureGroups.find(key);
			if(entry == g_creatureGroups.end())
			{
				MakeRoomForCreatureGroup(g_draw.tick);
				entry = g_creatureGroups.emplace(key,
					CreatureGroupState{}).first;
			}
			CreatureGroupState& state = entry->second;
			if(state.creatureId != g_draw.creatureId)
			{
				state = {};
				state.creatureId = g_draw.creatureId;
			}
			const bool floorChanged = state.hasFloor &&
				state.floorZ != g_draw.floorZ;
			const bool sameStep = g_draw.stepTimingValid && state.hasStep &&
				state.stepStartTick == g_draw.stepStartTick &&
				state.stepEndTick == g_draw.stepEndTick;
			const bool stepChanged = groupType == 1 &&
				g_draw.stepTimingValid && state.hasStep && !sameStep;
			if(state.groupType != groupType || floorChanged || stepChanged)
			{
				// Tibia 10.x keeps an animator per creature and frame group. Switching
				// groups, starting another tile or changing floor resets the selected
				// group's phase once; it does not reuse the previous step.
				state.groupType = groupType;
				state.startTick = g_draw.tick;
			}
			if(floorChanged)
			{
				state.suppressWalking = true;
				state.hasMovingPhase = false;
			}
			else if(groupType == 1 && g_draw.stepTimingValid &&
				state.suppressWalking && !sameStep)
				state.suppressWalking = false;

			state.floorZ = g_draw.floorZ;
			state.hasFloor = true;
			if(g_draw.stepTimingValid &&
				(groupType == 1 || state.suppressWalking))
			{
				state.stepStartTick = g_draw.stepStartTick;
				state.stepEndTick = g_draw.stepEndTick;
				state.hasStep = true;
			}
			else
			{
				state.stepStartTick = 0;
				state.stepEndTick = 0;
				state.hasStep = false;
			}
			state.lastSeenTick = g_draw.tick;
			g_draw.groupStartTick = state.startTick;
		}
		catch(...)
		{
			// A render hook must never let an allocation failure unwind into the
			// original x86 client. The shared animation tick is a safe fallback.
			g_draw.groupStartTick = 0;
		}
	}

	// Pixels a creature travels per tile. Creature::startWalk seeds the native
	// displacement with -delta * 32 and it decays back to zero on arrival, so
	// every walk progress calculation is expressed in this scale.
	const DWORD CREATURE_STEP_PIXELS = 32;

	// Both walk clocks answer the same question: how far into the current tile
	// the creature already is, counted forward from 0 to CREATURE_STEP_PIXELS.
	DWORD ResolveWalkedPixels()
	{
		if(g_draw.stepTimingValid)
		{
			const DWORD duration = g_draw.stepEndTick - g_draw.stepStartTick;
			if(duration != 0)
			{
				DWORD elapsed = g_draw.movementTick - g_draw.stepStartTick;
				if(static_cast<LONG>(g_draw.movementTick -
					g_draw.stepStartTick) < 0)
				{
					elapsed = 0;
				}
				else if(elapsed > duration)
				{
					elapsed = duration;
				}

				// The native step duration already carries the 3x diagonal
				// factor applied by Creature::startWalk, so the pixel progress
				// stays linear for every direction.
				const unsigned long long scaled =
					static_cast<unsigned long long>(elapsed) *
					CREATURE_STEP_PIXELS;
				return static_cast<DWORD>(scaled / duration);
			}
		}

		const DWORD absoluteX = g_draw.displacementX < 0 ?
			(DWORD)(-(LONGLONG)g_draw.displacementX) : (DWORD)g_draw.displacementX;
		const DWORD absoluteY = g_draw.displacementY < 0 ?
			(DWORD)(-(LONGLONG)g_draw.displacementY) : (DWORD)g_draw.displacementY;
		// Creature::getAnimationPhase reads the displacement left until the
		// creature reaches the tile, which counts *down* from 32 to 0. Convert
		// it to travelled pixels so the walk cycle plays forward here too.
		const DWORD remainingPixels = std::min<DWORD>(CREATURE_STEP_PIXELS,
			std::max<DWORD>(absoluteX, absoluteY));
		return CREATURE_STEP_PIXELS - remainingPixels;
	}

	DWORD ResolveMovingFrame(const FrameGroup86& group, DWORD nativeFrame)
	{
		if(!g_draw.active || !g_draw.walking || group.type != 1 || group.phases <= 1)
			return nativeFrame % group.phases;

		// Keep the displacement-derived result as a safe fallback for malformed
		// native timing. Displacement counts down, so convert it to travelled
		// pixels before mapping all local Moving phases in ascending order.
		const DWORD walkedPixels = std::min<DWORD>(ResolveWalkedPixels(),
			CREATURE_STEP_PIXELS);
		DWORD phase = std::min<DWORD>(
			(walkedPixels * group.phases) / CREATURE_STEP_PIXELS,
			group.phases - 1);

		// OTClient does not restart the feet at every tile. It keeps a foot-step
		// counter across consecutive walks, advances it from the cardinal/base
		// duration (also while moving diagonally), and resets only when native
		// step deadlines show a real idle gap. The former elapsed/current-tile
		// calculation forced a complete cycle per tile and snapped back to phase 0
		// at every boundary.
		if(g_draw.stepTimingValid && g_draw.stepBaseDuration != 0 &&
			g_draw.stepBaseDuration <= MAX_VALID_STEP_DURATION)
		{
			const unsigned long long key =
				(static_cast<unsigned long long>(g_draw.creaturePtr) << 32) |
				g_draw.thingType;
			auto entry = g_creatureGroups.find(key);
			if(entry != g_creatureGroups.end() &&
				entry->second.creatureId == g_draw.creatureId)
			{
				CreatureGroupState& state = entry->second;
				const bool newStep = !state.hasMovingStep ||
					state.movingStepStartTick != g_draw.stepStartTick ||
					state.movingStepEndTick != g_draw.stepEndTick;
				const LONG stepGap = static_cast<LONG>(
					g_draw.stepStartTick - state.movingStepEndTick);
				const bool continuousStep = !newStep ||
					(state.hasMovingStep &&
						stepGap >= -CREATURE_WALK_CONTINUOUS_STEP_GAP &&
						stepGap <= CREATURE_WALK_CONTINUOUS_STEP_GAP);
				const DWORD phaseDuration = std::max<DWORD>(
					(g_draw.stepBaseDuration +
						CREATURE_WALK_PHASE_DURATION_PADDING) / group.phases +
						CREATURE_WALK_PHASE_DELAY,
					CREATURE_WALK_PHASE_MIN_DURATION);
				const bool resetPhase = !state.hasMovingPhase ||
					state.movingPhaseCount != group.phases ||
					!continuousStep;
				if(resetPhase)
				{
					state.movingPhase = 0;
					state.movingPhaseTick = g_draw.tick;
					state.movingPhaseCount = group.phases;
					state.movingPhaseDuration = phaseDuration;
					state.hasMovingPhase = true;
				}
				else if(state.movingPhaseDuration != phaseDuration)
				{
					// Preserve the current foot when speed/ground changes, then start
					// the next phase using the new cadence.
					state.movingPhaseTick = g_draw.tick;
					state.movingPhaseDuration = phaseDuration;
				}
				else
				{
					const DWORD phaseElapsed =
						g_draw.tick - state.movingPhaseTick;
					if(phaseElapsed >= phaseDuration)
					{
						const DWORD phasesElapsed = phaseElapsed / phaseDuration;
						state.movingPhase = (state.movingPhase + phasesElapsed) %
							group.phases;
						state.movingPhaseTick += phasesElapsed * phaseDuration;
					}
				}
				state.movingStepStartTick = g_draw.stepStartTick;
				state.movingStepEndTick = g_draw.stepEndTick;
				state.hasMovingStep = true;
				phase = state.movingPhase;
			}
		}

		FrameGroupsLog("walk thing=%08lX group=%u phases=%lu timing=%d "
			"movement=%lu start=%lu end=%lu base=%lu displacement=%ld,%ld "
			"remaining=%lu walked=%lu phase=%lu",
			g_draw.thingType, static_cast<unsigned int>(group.type),
			group.phases, g_draw.stepTimingValid ? 1 : 0, g_draw.movementTick,
			g_draw.stepStartTick, g_draw.stepEndTick, g_draw.stepBaseDuration,
			g_draw.displacementX, g_draw.displacementY,
			CREATURE_STEP_PIXELS - walkedPixels, walkedPixels, phase);

		return phase;
	}

	DWORD ResolveTimedFrame(const FrameGroup86& group, DWORD nativeFrame)
	{
		if(group.phases <= 1)
			return 0;
		// Moving animations follow the creature's native 32-pixel step. DAT
		// durations are for idle/asynchronous animation and make walking lag.
		if(g_draw.walking && group.type == 1)
			return ResolveMovingFrame(group, nativeFrame);
		if(!RuntimeAnimations() || group.cumulativeDurations.size() != group.phases ||
			group.cycleDuration == 0)
			return nativeFrame % group.phases;
		const unsigned long long tick = g_draw.active ?
			static_cast<DWORD>(g_draw.tick - g_draw.groupStartTick) %
				group.cycleDuration :
			AnimationTick() % group.cycleDuration;
		const auto frame = std::upper_bound(group.cumulativeDurations.begin(),
			group.cumulativeDurations.end(), tick);
		return frame == group.cumulativeDurations.end() ? group.phases - 1 :
			static_cast<DWORD>(frame - group.cumulativeDurations.begin());
	}

	DWORD ResolveEffectFrame(const FrameGroup86& group, DWORD nativeFrame)
	{
		if(!RuntimeAnimations() || group.cumulativeDurations.size() != group.phases ||
			group.cycleDuration == 0)
			return nativeFrame % group.phases;
		const LONG slot = g_renderEffectSlot;
		if(slot < 0 || slot >= EFFECT_SLOT_COUNT || g_effectStartTicks[slot] == 0)
			return nativeFrame % group.phases;

		const DWORD elapsed = (g_renderEffectTick ? g_renderEffectTick :
			getTimerTick()) - g_effectStartTicks[slot];
		if(elapsed < group.cycleDuration)
		{
			const auto frame = std::upper_bound(group.cumulativeDurations.begin(),
				group.cumulativeDurations.end(), elapsed);
			if(frame != group.cumulativeDurations.end())
				return static_cast<DWORD>(frame -
					group.cumulativeDurations.begin());
		}
		// Returning phases is the completion sentinel. The native 8.60 slot can
		// outlive the custom DAT duration, but the completed visual must vanish
		// instead of leaving its final sprite stuck on the map.
		return group.phases;
	}

	int __cdecl TrackEffectCreation(DWORD kind, DWORD x, DWORD y, DWORD z)
	{
		const int slot = ((CreateEffectSlotFn)g_createEffectSlotAddr)(kind, x,
			y, z);
		if(slot >= 0 && slot < EFFECT_SLOT_COUNT)
			g_effectStartTicks[slot] = getTimerTick();
		return slot;
	}

	void __cdecl CreateMagicEffectChecked(DWORD x, DWORD y, DWORD z, DWORD type)
	{
		const LONG effectCount =
			InterlockedCompareExchange(&g_effectThingCount, 0, 0);
		if(type == 0 || effectCount <= 0 ||
			type > static_cast<DWORD>(effectCount))
		{
			return;
		}
		((CreateMagicEffectFn)g_createMagicEffectAddr)(x, y, z, type);
	}

	DWORD __cdecl TrackRenderedEffect(int slot)
	{
		g_renderEffectSlot = slot >= 0 && slot < EFFECT_SLOT_COUNT ? slot : -1;
		g_renderEffectTick = AnimationTick();
		return ((GetEffectTypeFn)g_getEffectTypeAddr)(slot);
	}

	DWORD __stdcall ResolveSprite(DWORD thingType, DWORD oneBasedIndex)
	{
		if(!thingType || oneBasedIndex == 0)
			return 0;
		std::unordered_map<DWORD, EnhancedThing86>::const_iterator found = g_things.find(thingType);
		if(found != g_things.end())
		{
			if(g_draw.active && g_draw.thingType == 0)
				g_draw.thingType = thingType;
			const FrameGroup86* group = SelectGroup(found->second, thingType);
			if(group && !group->sprites.empty())
			{
				BindCreatureGroup(thingType, group->type);
				const FrameGroup86& native = found->second.groups.front();
				const DWORD nativePhases = native.phases ? native.phases : 1;
				const DWORD selectedPhases = group->phases ? group->phases : 1;
				const size_t nativePerPhase = native.sprites.size() / nativePhases;
				const size_t selectedPerPhase = group->sprites.size() / selectedPhases;
				const size_t zeroBased = oneBasedIndex - 1;
				const DWORD nativeFrame = nativePerPhase ? (DWORD)(zeroBased / nativePerPhase) : 0;
				const size_t pattern = nativePerPhase ? zeroBased % nativePerPhase : 0;
				if(pattern < selectedPerPhase)
				{
					// Effects use a slot-owned start time so their DAT durations stay
					// smooth without restarting. Missiles preserve the native flight
					// phase; other categories may use the shared idle timer.
					DWORD frame = 0;
					if(found->second.category == DAT_CATEGORY_EFFECT)
						frame = ResolveEffectFrame(*group, nativeFrame);
					else if(found->second.category == DAT_CATEGORY_MISSILE)
						frame = nativeFrame % selectedPhases;
					else
						frame = ResolveTimedFrame(*group, nativeFrame);
					if(frame >= selectedPhases)
						return 0;
					const size_t selectedIndex = frame * selectedPerPhase + pattern;
					if(selectedIndex < group->sprites.size())
						return group->sprites[selectedIndex];
				}
			}
		}
		const DWORD spriteArray = *(DWORD*)(thingType + 0x20);
		if(!spriteArray)
			return 0;
		return g_extendedSprites ? ((DWORD*)spriteArray)[oneBasedIndex - 1] :
			((WORD*)spriteArray)[oneBasedIndex - 1];
	}

	__declspec(naked) void HookTexturePatterns()
	{
		__asm {
			pushad
			call ShouldParseEnhanced
			test al, al
			popad
			jz nativeParser
			pushad
			push esi
			push ebx
			call ParseTexturePatterns
			test al, al
			jz invalidDat
			popad
			jmp dword ptr [g_textureDone]
		invalidDat:
			popad
			call AbortInvalidDat
		nativeParser:
			mov ecx, ebx
			call dword ptr [g_readU8Addr]
			jmp dword ptr [g_textureNativeContinue]
		}
	}

	__declspec(naked) void HookProperty()
	{
		__asm {
			cmp al, 20h
			jb nativeSupported
			pushad
			movzx eax, al
			push eax
			push ebx
			call ConsumeExtendedProperty
			test al, al
			popad
			jz nativeUnknown
			jmp dword ptr [g_propertyLoop]
		nativeSupported:
			jmp dword ptr [g_propertySupported]
		nativeUnknown:
			jmp dword ptr [g_propertyUnknown]
		}
	}

	__declspec(naked) void HookSpriteLookup()
	{
		__asm {
			pushad
			push ecx
			push esi
			call ResolveSprite
			mov dword ptr [esp+1Ch], eax
			popad
			jmp dword ptr [g_spriteLookupDone]
		}
	}
}

namespace EnhancedDat
{
	bool Install(DWORD clientBase, bool extendedSprites)
	{
	#ifdef __CONFIG__
		if constexpr (!dat_has_frame_groups && !dat_has_improved_animations)
			return true;
	#endif
		g_extendedSprites = extendedSprites;
		g_sessionOnline = false;
		g_parseThingAddr = clientBase + 0x100830;
		g_readU8Addr = clientBase + 0x1171A0;
		g_readU16Addr = clientBase + 0x117420;
		g_readU32Addr = clientBase + 0x1175D0;
		g_textureNativeContinue = clientBase + 0x1008A9;
		g_textureDone = clientBase + 0x10097C;
		g_propertySupported = clientBase + 0x100A5A;
		g_propertyUnknown = clientBase + 0x1009B1;
		g_propertyLoop = clientBase + 0x100890;
		g_spriteLookupDone = clientBase + 0x100811;
		g_createEffectSlotAddr = clientBase + 0xE4F30;
		g_createMagicEffectAddr = clientBase + 0xE52C0;
		g_getEffectTypeAddr = clientBase + 0xD9B50;
		g_getCreatureAnimationPhaseAddr = clientBase + 0x5E130;
		g_nativeTimerAddr = clientBase + 0x1414E0;
		g_clientAlloc = (ClientAllocFn)(clientBase + 0x171A12);
		g_creatureGroups.clear();
		memset(g_effectStartTicks, 0, sizeof(g_effectStartTicks));
		g_renderEffectSlot = -1;
		g_renderEffectTick = 0;
		g_parsedThings = 0;
		g_retainedThings = 0;
		g_effectThingCount = 0;
		ResetFrameGroupsLog();

		HookCall(clientBase + 0x1072BE, (DWORD)&ParseItem);
		HookCall(clientBase + 0x1072EA, (DWORD)&ParseOutfit);
		HookCall(clientBase + 0x107316, (DWORD)&ParseEffect);
		HookCall(clientBase + 0x107342, (DWORD)&ParseMissile);
		HookJMP(clientBase + 0x1008A2, (DWORD)&HookTexturePatterns);
		Nop(clientBase + 0x1008A7, 2);
		HookJMP(clientBase + 0x1009A9, (DWORD)&HookProperty);
		Nop(clientBase + 0x1009AE, 3);
		HookJMP(clientBase + 0x10080C, (DWORD)&HookSpriteLookup);
		HookCall(clientBase + 0x104CC, (DWORD)&CreateMagicEffectChecked);
		HookCall(clientBase + 0xE52F9, (DWORD)&TrackEffectCreation);
		HookCall(clientBase + 0xF32C7, (DWORD)&TrackRenderedEffect);
		FrameGroupsLog("installed extended=%d groups=%d animations=%d",
			extendedSprites ? 1 : 0, RuntimeFrameGroups() ? 1 : 0,
			RuntimeAnimations() ? 1 : 0);
		return true;
	}

	void Shutdown()
	{
		g_things.clear();
		g_creatureGroups.clear();
		g_draw = {};
		memset(g_effectStartTicks, 0, sizeof(g_effectStartTicks));
		g_renderEffectSlot = -1;
		g_renderEffectTick = 0;
		g_parsedThings = 0;
		g_retainedThings = 0;
		g_effectThingCount = 0;
		g_sessionOnline = false;
	}

	void UpdateSession(bool online)
	{
		if(g_sessionOnline == online)
			return;

		g_sessionOnline = online;
		if(online)
			return;

		g_creatureGroups.clear();
		g_draw = {};
		memset(g_effectStartTicks, 0, sizeof(g_effectStartTicks));
		g_renderEffectSlot = -1;
		g_renderEffectTick = 0;
	}

	void BeginCreatureDraw(DWORD creaturePtr)
	{
		g_draw.creaturePtr = creaturePtr;
		g_draw.creatureId = 0;
		g_draw.thingType = 0;
		g_draw.displacementX = 0;
		g_draw.displacementY = 0;
		g_draw.tick = AnimationTick();
		g_draw.movementTick = 0;
		g_draw.stepStartTick = 0;
		g_draw.stepEndTick = 0;
		g_draw.stepBaseDuration = 0;
		g_draw.floorZ = 0;
		g_draw.groupStartTick = 0;
		g_draw.active = creaturePtr != 0;
		g_draw.walking = false;
		g_draw.stepTimingValid = false;
		if(creaturePtr)
		{
			__try {
				g_draw.creatureId = *(DWORD*)creaturePtr;
				g_draw.displacementX = *(LONG*)(creaturePtr + 0x30);
				g_draw.displacementY = *(LONG*)(creaturePtr + 0x34);
				g_draw.floorZ = *(DWORD*)(creaturePtr +
					CREATURE_POSITION_Z_OFFSET);
				// Ask the original client whether its animation is Idle (phase 1) or
				// Moving (phase 2). Unlike the persistent walking byte, this native
				// decision accounts for displacement and the cardinal/diagonal
				// deadlines and returns to Idle on the exact finishing draw.
				g_draw.walking = g_getCreatureAnimationPhaseAddr &&
					((GetCreatureAnimationPhaseFn)g_getCreatureAnimationPhaseAddr)(
						creaturePtr, 2) > 1;
				if(g_draw.walking)
				{
					const DWORD baseDeadline = *(DWORD*)(creaturePtr +
						CREATURE_WALK_BASE_DEADLINE_OFFSET);
					const DWORD baseDuration = *(DWORD*)(creaturePtr +
						CREATURE_WALK_BASE_DURATION_OFFSET);
					g_draw.stepBaseDuration = baseDuration;
					g_draw.stepStartTick = baseDeadline - baseDuration;
					g_draw.stepEndTick = *(DWORD*)(creaturePtr +
						CREATURE_WALK_END_OFFSET);
					g_draw.movementTick = g_nativeTimerAddr ?
						((NativeTimerFn)g_nativeTimerAddr)() : 0;
					const DWORD stepDuration =
						g_draw.stepEndTick - g_draw.stepStartTick;
					g_draw.stepTimingValid = g_nativeTimerAddr &&
						baseDuration != 0 && stepDuration >= baseDuration &&
						stepDuration <= MAX_VALID_STEP_DURATION;
				}
			} __except(EXCEPTION_EXECUTE_HANDLER) {
				g_draw.displacementX = 0;
				g_draw.displacementY = 0;
				g_draw.walking = false;
				g_draw.stepTimingValid = false;
			}
		}
	}

	void EndCreatureDraw()
	{
		g_draw = {};
	}
}
