#include "config.h"
#include "chat_input.h"
#include "client_memory.h"
#include "extdx9.h"
#include "extogl.h"
#include "sprites.h"
#include "timer.h"
#include "hook.h"
#include "enhanced_dat.h"
#include "integrity_check.h"
#include "mount_system.h"
#include "network_redirect.h"
#include "obfuscated_string.h"
#include "outfit_limit.h"
#include "ping_overlay.h"
#include "security_build_config.h"
#include "store_window.h"
#include "weather_system.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

//There aren't hardware acceleration in DirectDraw so even if you use transparency it won't work on DirectDraw(correct me if I wrong and send me some examples)(maybe later I rewrite usage of DirectDraw to some other api)
//Cipsoft Client draws sprites different than OTClient so there can be some glitches using transparency on DX9, OGL

struct Render_NEW
{
	DWORD padds1[5];
	void (__stdcall *DrawRectangle) (DWORD nSurface, DWORD X, DWORD Y, DWORD W, DWORD H, DWORD nRed, DWORD nGreen, DWORD nBlue);
	DWORD padds2[4];
	void  (__stdcall *LoadSprite) (int surface, int x, int y, int w, int h, void* data);
};

Render_NEW g_rendererStorage = {};
Render_NEW* newRenderer = &g_rendererStorage;

HMODULE orig_ddraw;
const char* prjInfo = nullptr;

DWORD client_Version;
DWORD client_BaseAddr;
DWORD client_pointerTransPixels;
bool client_extended;
bool client_transparent;

HHOOK keyboardHook = nullptr;
HHOOK mouseHook = nullptr;
HMODULE selfModule = nullptr;

static int InitMain();

namespace
{
	DWORD g_entryPointContinue = 0;
	DWORD g_entryPointExceptionHandler = 0;
	constexpr DWORD TIBIA_860_MULTICLIENT_FILE_OFFSET = 0x10BCA4;
	constexpr BYTE TIBIA_860_MULTICLIENT_ORIGINAL_OPCODE = 0x75;
	constexpr BYTE TIBIA_860_MULTICLIENT_PATCHED_OPCODE = 0xEB;
	constexpr std::uint32_t PROJECT_INFO_SEED =
		tibia::security::MixSeed(0x4D617465u ^
			tibia::security::HashText(__FILE__));
	constexpr auto g_encryptedProjectInfo =
		tibia::security::MakeEncrypted<PROJECT_INFO_SEED>(PROJECT_INFO);
	char g_projectInfo[sizeof(PROJECT_INFO)] = {};

	bool ApplyMulticlientPatch(DWORD imageBase) noexcept
	{
		if(!imageBase)
			return false;

		__try {
			const auto dosHeader =
				reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
			if(dosHeader->e_magic != IMAGE_DOS_SIGNATURE ||
				dosHeader->e_lfanew <= 0 || dosHeader->e_lfanew > 0x100000)
				return false;

			const auto ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
				imageBase + dosHeader->e_lfanew);
			if(ntHeaders->Signature != IMAGE_NT_SIGNATURE ||
				ntHeaders->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
				ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
				ntHeaders->FileHeader.NumberOfSections == 0 ||
				ntHeaders->FileHeader.NumberOfSections > 96)
				return false;

			const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(ntHeaders);
			for(WORD index = 0;
				index < ntHeaders->FileHeader.NumberOfSections; ++index)
			{
				const DWORD rawStart = section[index].PointerToRawData;
				const DWORD rawSize = section[index].SizeOfRawData;
				if(TIBIA_860_MULTICLIENT_FILE_OFFSET < rawStart ||
					TIBIA_860_MULTICLIENT_FILE_OFFSET - rawStart >= rawSize)
					continue;

				const DWORD rva = section[index].VirtualAddress +
					(TIBIA_860_MULTICLIENT_FILE_OFFSET - rawStart);
				if(rva >= ntHeaders->OptionalHeader.SizeOfImage)
					return false;

				BYTE* opcode = reinterpret_cast<BYTE*>(imageBase + rva);
				// Validate the surrounding TEST/Jcc bytes as well as the patch
				// byte, so a different Tibia build is never modified by accident.
				if(opcode[-2] != 0x84 || opcode[-1] != 0xC0 ||
					opcode[1] != 0x52)
					return false;
				if(*opcode == TIBIA_860_MULTICLIENT_PATCHED_OPCODE)
					return true;
				if(*opcode != TIBIA_860_MULTICLIENT_ORIGINAL_OPCODE)
					return false;

				DWORD oldProtection = 0;
				if(!VirtualProtect(opcode, 1, PAGE_EXECUTE_READWRITE,
					&oldProtection))
					return false;

				*opcode = TIBIA_860_MULTICLIENT_PATCHED_OPCODE;
				FlushInstructionCache(GetCurrentProcess(), opcode, 1);

				DWORD restoredProtection = 0;
				const BOOL protectionRestored = VirtualProtect(opcode, 1,
					oldProtection, &restoredProtection);
				return protectionRestored &&
					*opcode == TIBIA_860_MULTICLIENT_PATCHED_OPCODE;
			}
		}
		__except(EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}

		return false;
	}

	bool InitializeProjectInfo() noexcept
	{
		auto projectInfo = g_encryptedProjectInfo.Decrypt();
		if(strcpy_s(g_projectInfo, projectInfo.c_str()) != 0)
			return false;
		prjInfo = g_projectInfo;
		return true;
	}

	int __stdcall InitializeAfterLoaderLock()
	{
		return InitMain();
	}

	__declspec(noreturn) void __stdcall AbortClientInitialization()
	{
		ExitProcess(ERROR_DLL_INIT_FAILED);
	}

	__declspec(naked) void HookClientEntryPoint()
	{
		__asm {
			pushfd
			pushad
			call InitializeAfterLoaderLock
			test eax, eax
			jnz initialized
			popad
			popfd
			call AbortClientInitialization
		initialized:
			popad
			popfd
			// Restore the exact seven-byte Tibia 8.60 entry-point prologue.
			push 60h
			push dword ptr [g_entryPointExceptionHandler]
			jmp dword ptr [g_entryPointContinue]
		}
	}

	bool InstallDeferredInitialization() noexcept
	{
		const DWORD imageBase = reinterpret_cast<DWORD>(GetModuleHandleA(nullptr));
		if(!imageBase)
			return false;

		__try {
			const IMAGE_DOS_HEADER* dos =
				reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
			if(dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
				dos->e_lfanew > 0x100000)
				return false;
			const IMAGE_NT_HEADERS32* nt =
				reinterpret_cast<const IMAGE_NT_HEADERS32*>(imageBase + dos->e_lfanew);
			if(nt->Signature != IMAGE_NT_SIGNATURE ||
				nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
				nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
				return false;

			const DWORD entryRva = nt->OptionalHeader.AddressOfEntryPoint;
			if(nt->OptionalHeader.SizeOfImage < 7 || entryRva == 0 ||
				entryRva > nt->OptionalHeader.SizeOfImage - 7)
				return false;
			const DWORD entryPoint = imageBase + entryRva;
			const BYTE* prologue = reinterpret_cast<const BYTE*>(entryPoint);
			if(prologue[0] != 0x6A || prologue[1] != 0x60 ||
				prologue[2] != 0x68)
				return false;

			memcpy(&g_entryPointExceptionHandler, prologue + 3,
				sizeof(g_entryPointExceptionHandler));
			g_entryPointContinue = entryPoint + 7;
			if(!HookJMP(entryPoint,
				reinterpret_cast<DWORD>(&HookClientEntryPoint), 7))
			{
				RestoreHooks();
				return false;
			}
			return true;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool PinModuleLifetime() noexcept
	{
		HMODULE pinnedModule = nullptr;
		return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_PIN,
			reinterpret_cast<LPCSTR>(&PinModuleLifetime), &pinnedModule) != FALSE;
	}
}

namespace
{
	constexpr DWORD DISPATCH_COMMAND_RVA = 0x058200;
	constexpr DWORD GET_CREATURE_BY_ID_RVA = 0x05E720;
	constexpr DWORD PLAYER_ID_RVA = 0x23FE98;
	constexpr DWORD CREATURE_POSITION_X_OFFSET = 0x24;
	constexpr DWORD CREATURE_POSITION_Y_OFFSET = 0x28;
	constexpr DWORD CREATURE_POSITION_Z_OFFSET = 0x2C;
	constexpr DWORD CREATURE_WALK_START_OFFSET = 0x38;
	constexpr DWORD CREATURE_WALK_END_OFFSET = 0x3C;
	constexpr DWORD CREATURE_WALKING_OFFSET = 0x4C;

	constexpr DWORD COMMAND_WALK_NORTH = 0x10C;
	constexpr DWORD COMMAND_WALK_WEST = 0x10D;
	constexpr DWORD COMMAND_WALK_EAST = 0x10E;
	constexpr DWORD COMMAND_WALK_SOUTH = 0x10F;
	constexpr DWORD COMMAND_WALK_NORTH_WEST = 0x112;
	constexpr DWORD COMMAND_WALK_SOUTH_WEST = 0x113;
	constexpr DWORD COMMAND_WALK_NORTH_EAST = 0x114;
	constexpr DWORD COMMAND_WALK_SOUTH_EAST = 0x115;
	constexpr DWORD ELFBOT_KEY_TABLE_POLL_CALL_RVA = 0x02AF98;
	constexpr BYTE ELFBOT_KEY_TABLE_POLL_CALL[] = {
		0xE8, 0xEB, 0x56, 0x01, 0x00
	};

	using DispatchCommandFn = void (__cdecl*)(DWORD command);
	using GetCreatureByIdFn = DWORD (__cdecl*)(DWORD creatureId);

	enum class MovementDirection : BYTE
	{
		None,
		North,
		East,
		South,
		West,
		NorthEast,
		SouthEast,
		SouthWest,
		NorthWest
	};

	struct MovementBinding
	{
		DWORD sourceKey;
		int vertical;
		int horizontal;
		bool wasdOnly;
		bool pressed;
		ULONGLONG pressSequence;
	};

	struct PlayerMovementState
	{
		DWORD x;
		DWORD y;
		DWORD z;
		DWORD walkStartTick;
		DWORD walkEndTick;
		bool walking;
	};

	#if TIBIA_ENABLE_DEV_LOGS
	struct LogicalMovementState
	{
		bool up;
		bool down;
		bool left;
		bool right;
	};
	#endif

	MovementBinding g_movementBindings[] = {
		{ 'W', -1, 0, true, false, 0 },
		{ 'S', 1, 0, true, false, 0 },
		{ 'A', 0, -1, true, false, 0 },
		{ 'D', 0, 1, true, false, 0 },
		{ VK_UP, -1, 0, false, false, 0 },
		{ VK_DOWN, 1, 0, false, false, 0 },
		{ VK_LEFT, 0, -1, false, false, 0 },
		{ VK_RIGHT, 0, 1, false, false, 0 }
	};
	SRWLOCK g_movementLock = SRWLOCK_INIT;
	std::atomic<bool> g_smartWalkingEnabled(should_use_smart_walking);
	std::atomic<bool> g_movementInputActive(false);
	ULONGLONG g_movementSequence = 0;
	DWORD g_activeMovementCommand = 0;
	bool g_waitingForWalk = false;
	bool g_movementStateKnown = false;
	PlayerMovementState g_lastMovementState = {};
	DWORD g_preDispatchedWalkEndTick = 0;
	const DWORD MOVEMENT_PREQUEUE_LEAD_MS = 40;
	const DWORD MOVEMENT_PREQUEUE_RETRY_GRACE_MS = 20;
	bool g_storeShortcutHeld = false;
	std::atomic<HWND> g_clientWindow(nullptr);
	std::atomic<DWORD> g_elfBotCompatibilityModule(0);

	struct ClientWindowCandidate
	{
		HWND window;
		ULONGLONG clientArea;
	};

	bool IsClientWindowCandidate(HWND window) noexcept
	{
		if(!window || !IsWindowVisible(window) ||
			GetWindow(window, GW_OWNER) != nullptr)
		{
			return false;
		}
		const LONG_PTR extendedStyle = GetWindowLongPtrA(window, GWL_EXSTYLE);
		if((extendedStyle & WS_EX_TOOLWINDOW) != 0)
			return false;

		DWORD processId = 0;
		GetWindowThreadProcessId(window, &processId);
		if(processId != GetCurrentProcessId())
			return false;

		char className[64] = {};
		if(GetClassNameA(window, className, sizeof(className)) == 0 ||
			strcmp(className, "#32770") == 0)
		{
			return false;
		}
		return true;
	}

	BOOL CALLBACK FindClientWindow(HWND window, LPARAM parameter) noexcept
	{
		if(!IsClientWindowCandidate(window))
			return TRUE;

		RECT clientRect = {};
		if(!GetClientRect(window, &clientRect))
			return TRUE;
		const LONG width = clientRect.right - clientRect.left;
		const LONG height = clientRect.bottom - clientRect.top;
		if(width <= 0 || height <= 0)
			return TRUE;

		ClientWindowCandidate& candidate =
			*reinterpret_cast<ClientWindowCandidate*>(parameter);
		const ULONGLONG area = static_cast<ULONGLONG>(width) *
			static_cast<ULONGLONG>(height);
		if(!candidate.window || area > candidate.clientArea)
		{
			candidate.window = window;
			candidate.clientArea = area;
		}
		return TRUE;
	}

	HWND ClientWindow() noexcept
	{
		HWND window = g_clientWindow.load(std::memory_order_acquire);
		if(IsClientWindowCandidate(window))
			return window;

		ClientWindowCandidate candidate = {};
		EnumWindows(&FindClientWindow,
			reinterpret_cast<LPARAM>(&candidate));
		g_clientWindow.store(candidate.window, std::memory_order_release);
		return candidate.window;
	}

	MovementBinding* FindMovementBinding(DWORD sourceKey) noexcept
	{
		for(MovementBinding& binding : g_movementBindings)
			if(binding.sourceKey == sourceKey)
				return &binding;
		return nullptr;
	}

	SHORT WINAPI ElfBotGetAsyncKeyState(int virtualKey) noexcept
	{
		SHORT state = GetAsyncKeyState(virtualKey);
		if((state & 0x8000) != 0 ||
			(virtualKey != VK_LEFT && virtualKey != VK_UP &&
			 virtualKey != VK_RIGHT && virtualKey != VK_DOWN))
		{
			return state;
		}

		bool pressed = false;
		AcquireSRWLockShared(&g_movementLock);
		MovementBinding* binding = FindMovementBinding(
			static_cast<DWORD>(virtualKey));
		if(binding)
			pressed = binding->pressed;
		ReleaseSRWLockShared(&g_movementLock);

		if(pressed)
			state = static_cast<SHORT>(
				static_cast<unsigned short>(state) | 0x8000u);
		return state;
	}

	void InstallElfBotArrowStateCompatibility() noexcept
	{
		if(g_elfBotCompatibilityModule.load(std::memory_order_acquire) != 0)
			return;

		auto elfBotModuleName = TIBIA_OBF("elfbot.dll");
		const DWORD module = reinterpret_cast<DWORD>(
			GetModuleHandleA(elfBotModuleName.c_str()));
		if(module == 0)
			return;

		DWORD expectedModule = 0;
		if(!g_elfBotCompatibilityModule.compare_exchange_strong(expectedModule,
			module, std::memory_order_acq_rel))
		{
			return;
		}

		const DWORD callSite = module + ELFBOT_KEY_TABLE_POLL_CALL_RVA;
		if(!ClientMemory::MatchesCode(callSite,
			ELFBOT_KEY_TABLE_POLL_CALL,
			sizeof(ELFBOT_KEY_TABLE_POLL_CALL)))
		{
			return;
		}

		HookCall(callSite,
			reinterpret_cast<DWORD>(&ElfBotGetAsyncKeyState));
	}

	bool IsClientForeground() noexcept
	{
		HWND activeWindow = GetForegroundWindow();
		return activeWindow != nullptr && activeWindow == ClientWindow();
	}

	bool IsPlayerOnline() noexcept
	{
		if(!client_BaseAddr)
			return false;
		__try {
			return *reinterpret_cast<const DWORD*>(
				client_BaseAddr + PLAYER_ID_RVA) != 0;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool TryReadPlayerMovementState(PlayerMovementState& state) noexcept
	{
		state = {};
		if(!client_BaseAddr)
			return false;
		__try {
			const DWORD playerId = *reinterpret_cast<const DWORD*>(
				client_BaseAddr + PLAYER_ID_RVA);
			if(playerId == 0)
				return false;
			const DWORD creature = reinterpret_cast<GetCreatureByIdFn>(
				client_BaseAddr + GET_CREATURE_BY_ID_RVA)(playerId);
			if(creature == 0)
				return false;
			state.x = *reinterpret_cast<const DWORD*>(
				creature + CREATURE_POSITION_X_OFFSET);
			state.y = *reinterpret_cast<const DWORD*>(
				creature + CREATURE_POSITION_Y_OFFSET);
			state.z = *reinterpret_cast<const DWORD*>(
				creature + CREATURE_POSITION_Z_OFFSET);
			state.walkStartTick = *reinterpret_cast<const DWORD*>(
				creature + CREATURE_WALK_START_OFFSET);
			state.walkEndTick = *reinterpret_cast<const DWORD*>(
				creature + CREATURE_WALK_END_OFFSET);
			state.walking = *reinterpret_cast<const BYTE*>(
				creature + CREATURE_WALKING_OFFSET) != 0;
			return true;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			state = {};
			return false;
		}
	}

	bool IsSameMovementCycle(const PlayerMovementState& left,
		const PlayerMovementState& right) noexcept
	{
		return left.x == right.x && left.y == right.y && left.z == right.z &&
			left.walkStartTick == right.walkStartTick &&
			left.walkEndTick == right.walkEndTick;
	}

	bool HasReachedTick(DWORD now, DWORD target) noexcept
	{
		return static_cast<LONG>(now - target) >= 0;
	}

	DWORD MovementCommand(MovementDirection direction) noexcept
	{
		switch(direction)
		{
			case MovementDirection::North: return COMMAND_WALK_NORTH;
			case MovementDirection::East: return COMMAND_WALK_EAST;
			case MovementDirection::South: return COMMAND_WALK_SOUTH;
			case MovementDirection::West: return COMMAND_WALK_WEST;
			case MovementDirection::NorthEast: return COMMAND_WALK_NORTH_EAST;
			case MovementDirection::SouthEast: return COMMAND_WALK_SOUTH_EAST;
			case MovementDirection::SouthWest: return COMMAND_WALK_SOUTH_WEST;
			case MovementDirection::NorthWest: return COMMAND_WALK_NORTH_WEST;
			default: return 0;
		}
	}

	#if TIBIA_ENABLE_DEV_LOGS
	LogicalMovementState ReadLogicalMovementStateLocked() noexcept
	{
		LogicalMovementState state = {};
		for(const MovementBinding& binding : g_movementBindings)
		{
			if(!binding.pressed)
				continue;
			state.up = state.up || binding.vertical < 0;
			state.down = state.down || binding.vertical > 0;
			state.left = state.left || binding.horizontal < 0;
			state.right = state.right || binding.horizontal > 0;
		}
		return state;
	}

	const char* MovementKeyName(DWORD key) noexcept
	{
		switch(key)
		{
			case 'W': return "W";
			case 'S': return "S";
			case 'A': return "A";
			case 'D': return "D";
			case VK_UP: return "Up";
			case VK_DOWN: return "Down";
			case VK_LEFT: return "Left";
			case VK_RIGHT: return "Right";
			default: return "Unknown";
		}
	}

	const char* MovementCommandName(DWORD command) noexcept
	{
		switch(command)
		{
			case COMMAND_WALK_NORTH: return "North";
			case COMMAND_WALK_EAST: return "East";
			case COMMAND_WALK_SOUTH: return "South";
			case COMMAND_WALK_WEST: return "West";
			case COMMAND_WALK_NORTH_EAST: return "NorthEast";
			case COMMAND_WALK_SOUTH_EAST: return "SouthEast";
			case COMMAND_WALK_SOUTH_WEST: return "SouthWest";
			case COMMAND_WALK_NORTH_WEST: return "NorthWest";
			default: return "None";
		}
	}

	BYTE MovementOpcode(DWORD command) noexcept
	{
		switch(command)
		{
			case COMMAND_WALK_NORTH: return 0x65;
			case COMMAND_WALK_EAST: return 0x66;
			case COMMAND_WALK_SOUTH: return 0x67;
			case COMMAND_WALK_WEST: return 0x68;
			case COMMAND_WALK_NORTH_EAST: return 0x6A;
			case COMMAND_WALK_SOUTH_EAST: return 0x6B;
			case COMMAND_WALK_SOUTH_WEST: return 0x6C;
			case COMMAND_WALK_NORTH_WEST: return 0x6D;
			default: return 0;
		}
	}

	void LogMovementInput(DWORD key, bool keyDown, bool repeated, bool handled,
		bool smartWalkingEnabled, const LogicalMovementState& state,
		DWORD previousCommand, DWORD resolvedCommand,
		bool dispatchRequested) noexcept
	{
		char message[320] = {};
		_snprintf_s(message, sizeof(message), _TRUNCATE,
			"[SmartWalk] t=%lu key=%s state=%s repeat=%d chat=%s handled=%d "
			"mode=%s up=%d down=%d left=%d right=%d previous=%s result=%s "
			"dispatch=%d\n",
			getTimerTick(), MovementKeyName(key), keyDown ? "down" : "up",
			repeated ? 1 : 0,
			ChatInput::IsTemporary() ? "temporary" :
				(ChatInput::IsEnabled() ? "on" : "off"),
			handled ? 1 : 0, smartWalkingEnabled ? "ON" : "OFF",
			state.up ? 1 : 0, state.down ? 1 : 0, state.left ? 1 : 0,
			state.right ? 1 : 0, MovementCommandName(previousCommand),
			MovementCommandName(resolvedCommand), dispatchRequested ? 1 : 0);
		OutputDebugStringA(message);
	}

	void LogMovementDispatch(DWORD command, const char* source) noexcept
	{
		char message[192] = {};
		_snprintf_s(message, sizeof(message), _TRUNCATE,
			"[Walk] t=%lu command=0x%lX opcode=0x%02X source=%s\n",
			getTimerTick(), command, MovementOpcode(command),
			source ? source : "unknown");
		OutputDebugStringA(message);
	}

	void LogMovementState(const PlayerMovementState& state, DWORD now,
		bool pending, const char* event) noexcept
	{
		char message[256] = {};
		_snprintf_s(message, sizeof(message), _TRUNCATE,
			"[WalkState] t=%lu event=%s pos=%lu,%lu,%lu walking=%d "
			"start=%lu end=%lu pending=%d\n",
			now, event ? event : "unknown", state.x, state.y, state.z,
			state.walking ? 1 : 0, state.walkStartTick, state.walkEndTick,
			pending ? 1 : 0);
		OutputDebugStringA(message);
	}
	#endif

	bool DispatchMovementCommand(DWORD command, const char* source) noexcept
	{
		if(!client_BaseAddr || command == 0)
			return false;
		__try {
			reinterpret_cast<DispatchCommandFn>(
				client_BaseAddr + DISPATCH_COMMAND_RVA)(command);
			#if TIBIA_ENABLE_DEV_LOGS
			LogMovementDispatch(command, source);
			#else
			(void)source;
			#endif
			return true;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	MovementDirection ResolveSmartWalkingDirectionLocked(
		bool smartWalkingEnabled) noexcept
	{
		if(!smartWalkingEnabled)
		{
			const MovementBinding* latest = nullptr;
			for(const MovementBinding& binding : g_movementBindings)
			{
				if(binding.pressed &&
					(!latest || binding.pressSequence > latest->pressSequence))
				{
					latest = &binding;
				}
			}
			if(!latest)
				return MovementDirection::None;
			if(latest->vertical < 0)
				return MovementDirection::North;
			if(latest->vertical > 0)
				return MovementDirection::South;
			if(latest->horizontal < 0)
				return MovementDirection::West;
			return MovementDirection::East;
		}

		int vertical = 0;
		int horizontal = 0;
		ULONGLONG verticalSequence = 0;
		ULONGLONG horizontalSequence = 0;
		for(const MovementBinding& binding : g_movementBindings)
		{
			if(!binding.pressed)
				continue;
			if(binding.vertical != 0 &&
				binding.pressSequence >= verticalSequence)
			{
				vertical = binding.vertical;
				verticalSequence = binding.pressSequence;
			}
			if(binding.horizontal != 0 &&
				binding.pressSequence >= horizontalSequence)
			{
				horizontal = binding.horizontal;
				horizontalSequence = binding.pressSequence;
			}
		}

		if(vertical < 0 && horizontal < 0)
			return MovementDirection::NorthWest;
		if(vertical > 0 && horizontal < 0)
			return MovementDirection::SouthWest;
		if(vertical < 0 && horizontal > 0)
			return MovementDirection::NorthEast;
		if(vertical > 0 && horizontal > 0)
			return MovementDirection::SouthEast;
		if(vertical < 0)
			return MovementDirection::North;
		if(vertical > 0)
			return MovementDirection::South;
		if(horizontal < 0)
			return MovementDirection::West;
		if(horizontal > 0)
			return MovementDirection::East;
		return MovementDirection::None;
	}

	void ApplyResolvedMovementCommandLocked(DWORD resolvedCommand,
		bool movementStateKnown, const PlayerMovementState& movementState,
		DWORD now, DWORD& commandToDispatch) noexcept
	{
		if(resolvedCommand == g_activeMovementCommand)
			return;

		g_activeMovementCommand = resolvedCommand;
		g_movementInputActive.store(resolvedCommand != 0,
			std::memory_order_release);
		if(resolvedCommand == 0)
		{
			g_waitingForWalk = false;
			g_movementStateKnown = false;
			g_lastMovementState = {};
			g_preDispatchedWalkEndTick = 0;
			return;
		}

		g_movementStateKnown = movementStateKnown;
		g_lastMovementState = movementStateKnown ? movementState :
			PlayerMovementState{};

		// Keep a changed direction pending while the native step is active. The
		// held-direction updater pre-dispatches near its deadline, then retries if
		// the client ignored the early command.
		const bool currentStepActive = movementStateKnown &&
			movementState.walking && movementState.walkEndTick != 0 &&
			!HasReachedTick(now, movementState.walkEndTick);
		g_waitingForWalk = !currentStepActive;
		if(!currentStepActive)
			commandToDispatch = resolvedCommand;
	}

	void ClearHeldMovementKeys() noexcept
	{
		AcquireSRWLockExclusive(&g_movementLock);
		g_movementInputActive.store(false, std::memory_order_release);
		for(MovementBinding& binding : g_movementBindings)
		{
			binding.pressed = false;
			binding.pressSequence = 0;
		}
		g_movementSequence = 0;
		g_activeMovementCommand = 0;
		g_waitingForWalk = false;
		g_movementStateKnown = false;
		g_lastMovementState = {};
		g_preDispatchedWalkEndTick = 0;
		ReleaseSRWLockExclusive(&g_movementLock);
	}

	bool IsSmartWalkingEnabled() noexcept
	{
		return g_smartWalkingEnabled.load(std::memory_order_acquire);
	}

	void SetSmartWalkingEnabled(bool enabled) noexcept
	{
		const bool previous = g_smartWalkingEnabled.exchange(enabled,
			std::memory_order_acq_rel);
		if(previous == enabled)
			return;

		PlayerMovementState movementState = {};
		const bool movementStateKnown =
			TryReadPlayerMovementState(movementState);
		DWORD commandToDispatch = 0;
		DWORD resolvedCommand = 0;
		#if TIBIA_ENABLE_DEV_LOGS
		DWORD previousCommand = 0;
		#endif
		AcquireSRWLockExclusive(&g_movementLock);
		#if TIBIA_ENABLE_DEV_LOGS
		previousCommand = g_activeMovementCommand;
		#endif
		resolvedCommand = MovementCommand(
			ResolveSmartWalkingDirectionLocked(enabled));
		ApplyResolvedMovementCommandLocked(resolvedCommand, movementStateKnown,
			movementState, getTimerTick(), commandToDispatch);
		ReleaseSRWLockExclusive(&g_movementLock);

		#if TIBIA_ENABLE_DEV_LOGS
		char message[192] = {};
		_snprintf_s(message, sizeof(message), _TRUNCATE,
			"[SmartWalk] t=%lu mode=%s previous=%s result=%s dispatch=%d\n",
			getTimerTick(), enabled ? "ON" : "OFF",
			MovementCommandName(previousCommand),
			MovementCommandName(resolvedCommand),
			commandToDispatch != 0 ? 1 : 0);
		OutputDebugStringA(message);
		#endif

		if(commandToDispatch != 0 &&
			!DispatchMovementCommand(commandToDispatch, "smart-walk-toggle"))
		{
			ClearHeldMovementKeys();
		}
	}

	void ToggleSmartWalkingState() noexcept
	{
		SetSmartWalkingEnabled(!IsSmartWalkingEnabled());
	}

	bool HandleMovementKey(WPARAM wParam, const KBDLLHOOKSTRUCT& keyboard,
		bool online, bool commandModifier) noexcept
	{
		const bool keyDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
		const bool keyUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
		MovementBinding* binding = FindMovementBinding(keyboard.vkCode);
		// The CIP client already repeats arrow-key movement natively. Let its
		// arrows through so held directions and mid-step turns do not depend on
		// the DLL's deadline-based repeat loop; keep custom movement for WASD.
		if(!binding || !binding->wasdOnly || (!keyDown && !keyUp))
			return false;

		if(commandModifier)
		{
			// Ctrl+Arrow is handled natively by Tibia as turn-in-place.
			// Release every walk still owned by the DLL so UpdateHeldMovement
			// cannot overwrite that turn, then pass both key-down and key-up.
			ClearHeldMovementKeys();
			return false;
		}

		PlayerMovementState movementState = {};
		const bool movementStateKnown =
			TryReadPlayerMovementState(movementState);
		DWORD commandToDispatch = 0;
		const char* dispatchSource = "key-change";
		bool handled = false;
		#if TIBIA_ENABLE_DEV_LOGS
		bool repeated = false;
		bool smartWalkingEnabled = false;
		LogicalMovementState logicalState = {};
		DWORD previousCommand = 0;
		DWORD resolvedCommandForLog = 0;
		#endif

		AcquireSRWLockExclusive(&g_movementLock);
		#if TIBIA_ENABLE_DEV_LOGS
		repeated = keyDown && binding->pressed;
		previousCommand = g_activeMovementCommand;
		#endif
		if(keyUp && binding->pressed)
		{
			binding->pressed = false;
			binding->pressSequence = 0;
			handled = true;
		}
		else if(keyDown && online && !commandModifier &&
			(!binding->wasdOnly || ChatInput::CanUseWasdWalking()))
		{
			handled = true;
			if(!binding->pressed)
			{
				binding->pressed = true;
				binding->pressSequence = ++g_movementSequence;
			}
			else if(g_waitingForWalk && movementStateKnown &&
				!movementState.walking)
			{
				// Preserve native retries against blocked tiles, but do not use
				// Windows repeat to pace ordinary walking.
				g_lastMovementState = movementState;
				g_movementStateKnown = true;
				commandToDispatch = g_activeMovementCommand;
				dispatchSource = "os-retry";
			}
		}

		if(handled)
		{
			const bool smartWalking = g_smartWalkingEnabled.load(
				std::memory_order_acquire);
			const DWORD resolvedCommand = MovementCommand(
				ResolveSmartWalkingDirectionLocked(smartWalking));
			ApplyResolvedMovementCommandLocked(resolvedCommand,
				movementStateKnown, movementState, getTimerTick(),
				commandToDispatch);
		}
		#if TIBIA_ENABLE_DEV_LOGS
		smartWalkingEnabled = g_smartWalkingEnabled.load(
			std::memory_order_acquire);
		logicalState = ReadLogicalMovementStateLocked();
		resolvedCommandForLog = g_activeMovementCommand;
		#endif
		ReleaseSRWLockExclusive(&g_movementLock);
		#if TIBIA_ENABLE_DEV_LOGS
		LogMovementInput(keyboard.vkCode, keyDown, repeated, handled,
			smartWalkingEnabled, logicalState, previousCommand,
			resolvedCommandForLog, commandToDispatch != 0);
		#endif

		if(commandToDispatch != 0 &&
			!DispatchMovementCommand(commandToDispatch, dispatchSource))
		{
			ClearHeldMovementKeys();
		}
		return handled;
	}

	void UpdateHeldMovement(bool online) noexcept
	{
		if(!online)
		{
			ClearHeldMovementKeys();
			return;
		}
		if(!g_movementInputActive.load(std::memory_order_acquire))
			return;
		if(!IsClientForeground())
		{
			ClearHeldMovementKeys();
			ChatInput::ResetTransientInput();
			return;
		}

		PlayerMovementState movementState = {};
		if(!TryReadPlayerMovementState(movementState))
			return;

		DWORD commandToDispatch = 0;
		const DWORD now = getTimerTick();
		#if TIBIA_ENABLE_DEV_LOGS
		bool movementCycleChanged = false;
		bool waitingAfterUpdate = false;
		#endif
		AcquireSRWLockExclusive(&g_movementLock);
		if(g_activeMovementCommand != 0)
		{
			if(!g_movementStateKnown)
			{
				g_lastMovementState = movementState;
				g_movementStateKnown = true;
			}
			else if(!IsSameMovementCycle(g_lastMovementState, movementState))
			{
				// Position and native animation deadlines persist across frames,
				// unlike the brief walking=false transition between steps.
				g_lastMovementState = movementState;
				g_waitingForWalk = false;
				g_preDispatchedWalkEndTick = 0;
				#if TIBIA_ENABLE_DEV_LOGS
				movementCycleChanged = true;
				#endif
			}

			if(!g_waitingForWalk && !movementState.walking)
			{
				const DWORD walkEndTick = movementState.walkEndTick;
				const bool earlyCommandWasSent = walkEndTick != 0 &&
					g_preDispatchedWalkEndTick == walkEndTick;
				const bool retryGraceElapsed = walkEndTick != 0 &&
					HasReachedTick(now, walkEndTick) &&
					now - walkEndTick >= MOVEMENT_PREQUEUE_RETRY_GRACE_MS;
				if(!earlyCommandWasSent || retryGraceElapsed)
				{
					// Resume a still-held direction as soon as the client reports an
					// idle step, including spell actions that leave a stale deadline.
					commandToDispatch = g_activeMovementCommand;
					g_waitingForWalk = true;
					g_preDispatchedWalkEndTick = 0;
				}
			}
			else if(!g_waitingForWalk && movementState.walkEndTick != 0)
			{
				const DWORD walkEndTick = movementState.walkEndTick;
				if(!HasReachedTick(now, walkEndTick))
				{
					const DWORD remaining = walkEndTick - now;
					if(movementState.walking &&
						remaining <= MOVEMENT_PREQUEUE_LEAD_MS &&
						g_preDispatchedWalkEndTick != walkEndTick)
					{
						commandToDispatch = g_activeMovementCommand;
						g_preDispatchedWalkEndTick = walkEndTick;
					}
				}
				else
				{
					const DWORD elapsedAfterEnd = now - walkEndTick;
					const bool earlyCommandWasSent =
						g_preDispatchedWalkEndTick == walkEndTick;
					if(!earlyCommandWasSent ||
						elapsedAfterEnd >= MOVEMENT_PREQUEUE_RETRY_GRACE_MS)
					{
						commandToDispatch = g_activeMovementCommand;
						g_waitingForWalk = true;
						g_preDispatchedWalkEndTick = 0;
					}
				}
			}
			#if TIBIA_ENABLE_DEV_LOGS
			waitingAfterUpdate = g_waitingForWalk;
			#endif
		}
		ReleaseSRWLockExclusive(&g_movementLock);
		#if TIBIA_ENABLE_DEV_LOGS
		if(movementCycleChanged)
			LogMovementState(movementState, now, waitingAfterUpdate,
				"server-cycle");
		if(commandToDispatch != 0)
			LogMovementState(movementState, now, waitingAfterUpdate,
				"walk-step");
		#endif

		if(commandToDispatch != 0 &&
			!DispatchMovementCommand(commandToDispatch, "walk-deadline"))
		{
			ClearHeldMovementKeys();
		}
	}
}

LRESULT CALLBACK KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if(nCode >= 0 && lParam)
	{
		const bool keyDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
		const bool keyUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
		const KBDLLHOOKSTRUCT* keyboard =
			reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
		if((keyDown || keyUp) && !(keyboard->flags & LLKHF_INJECTED))
		{
			if(!IsClientForeground())
			{
				g_storeShortcutHeld = false;
				ClearHeldMovementKeys();
				ChatInput::ResetTransientInput();
				return CallNextHookEx(keyboardHook, nCode, wParam, lParam);
			}

			if(StoreWindow::IsVisible())
			{
				if(keyDown && keyboard->vkCode == VK_ESCAPE)
				{
					StoreWindow::Close();
					return 1;
				}
				// The Store is an overlay, not a native modal control. Do not
				// swallow unrelated keys or leave the client keyboard stuck.
			}

			if(keyDown && should_use_mount_system && keyboard->vkCode == 'M' &&
				(GetKeyState(VK_CONTROL) & 0x8000)) {
				static DWORD lastMountToggle = 0;
				const DWORD now = getTimerTick();
				if(now - lastMountToggle > 300) {
					lastMountToggle = now;
					MountSystem::ToggleLocalMount();
				}
				return 1;
			}

			if constexpr (should_use_store)
			{
				if(keyboard->vkCode == 'S')
				{
					if(keyUp)
					{
						const bool consume = g_storeShortcutHeld;
						g_storeShortcutHeld = false;
						if(consume)
							return 1;
					}
					else if(g_storeShortcutHeld)
					{
						return 1;
					}
					else if((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
						(GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 &&
						(keyboard->flags & LLKHF_ALTDOWN) == 0 &&
						(GetAsyncKeyState(VK_LWIN) & 0x8000) == 0 &&
						(GetAsyncKeyState(VK_RWIN) & 0x8000) == 0 &&
						IsPlayerOnline())
					{
						g_storeShortcutHeld = true;
						StoreWindow::Toggle();
						return 1;
					}
				}
			}

			if(ChatInput::HandleKeyboardControl(wParam, *keyboard))
				return 1;

			const bool commandModifier =
				(GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
				(GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ||
				(keyboard->flags & LLKHF_ALTDOWN) != 0 ||
				(GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
				(GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
			const bool online = IsPlayerOnline();
			if(HandleMovementKey(wParam, *keyboard, online, commandModifier))
				return 1;
			if(ChatInput::ShouldSuppressTextKey(wParam, *keyboard))
				return 1;
		}
	}
	
	return CallNextHookEx(keyboardHook, nCode, wParam, lParam);
}

LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if(nCode >= 0 && lParam)
	{
		if(!IsClientForeground())
			return CallNextHookEx(mouseHook, nCode, wParam, lParam);

		const MOUSEHOOKSTRUCT& threadMouse =
			*reinterpret_cast<const MOUSEHOOKSTRUCT*>(lParam);
		MSLLHOOKSTRUCT mouse = {};
		mouse.pt = threadMouse.pt;
		if(wParam == WM_MOUSEWHEEL || wParam == WM_MOUSEHWHEEL ||
			wParam == WM_XBUTTONDOWN || wParam == WM_XBUTTONUP ||
			wParam == WM_XBUTTONDBLCLK)
		{
			mouse.mouseData = reinterpret_cast<const MOUSEHOOKSTRUCTEX*>(
				lParam)->mouseData;
		}
		if(StoreWindow::HandleMouseMessage(wParam, mouse))
			return 1;
		ChatInput::HandleMouseMessage(wParam, mouse);
	}
	return CallNextHookEx(mouseHook, nCode, wParam, lParam);
}

bool SetupInputHooks()
{
	if(!keyboardHook && selfModule)
		keyboardHook = SetWindowsHookExA(WH_KEYBOARD_LL, KeyboardHookProc,
			selfModule, 0);
	if(!mouseHook)
		mouseHook = SetWindowsHookExA(WH_MOUSE, MouseHookProc, nullptr,
			GetCurrentThreadId());
	if(keyboardHook && mouseHook)
		return true;
	if(keyboardHook)
	{
		UnhookWindowsHookEx(keyboardHook);
		keyboardHook = nullptr;
	}
	if(mouseHook)
	{
		UnhookWindowsHookEx(mouseHook);
		mouseHook = nullptr;
	}
	return false;
}

std::unique_ptr<ExtendedEngine> transEngine;
std::unique_ptr<Sprites> spritesFile;
std::size_t spritePointerOffset = 0;
DWORD spriteCount = 0;

using GetFileNameFn = const char* (*)(int fileId, bool backup);
static GetFileNameFn GetFileName;

DWORD PlayerHealthAddr;
DWORD PlayerHealthMaxAddr;
DWORD PlayerManaAddr;
DWORD PlayerManaMaxAddr;
DWORD PlayerIDAddr;

DWORD CreateGlContext;
DWORD DeleteGlContext;
DWORD CreateDX9Context;
DWORD DeleteDX9Context;
DWORD CreateDX7Context;
DWORD DeleteDX7Context;
DWORD SpriteContext;
BYTE SpriteContextPos;
BYTE SpriteContextNeg;

GetEngineAddrFn GetEngineAddr;
DrawSkinFn DrawSkin;
PrintTextFn PrintText;

namespace
{
	bool TryReadDword(DWORD address, DWORD& value) noexcept
	{
		if(!address)
			return false;
		__try {
			value = *reinterpret_cast<const DWORD*>(address);
			return true;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			value = 0;
			return false;
		}
	}

	bool TryReadByte(DWORD address, BYTE& value) noexcept
	{
		if(!address)
			return false;
		__try {
			value = *reinterpret_cast<const BYTE*>(address);
			return true;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			value = 0;
			return false;
		}
	}

	bool TryWriteDword(DWORD address, DWORD value) noexcept
	{
		if(!address)
			return false;
		__try {
			*reinterpret_cast<DWORD*>(address) = value;
			return true;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	template<typename T>
	bool ReadSpriteValue(std::size_t& offset, T& value) noexcept
	{
		if(!spritesFile || !spritesFile->read(offset, &value, sizeof(value)))
			return false;
		offset += sizeof(value);
		return true;
	}

	bool GetSpriteEntryAddress(uint32_t sprite, DWORD& entryAddress) noexcept
	{
		entryAddress = 0;
		if(!client_pointerTransPixels || sprite == 0 || sprite > spriteCount)
			return false;

		DWORD tableAddress = 0;
		if(!TryReadDword(client_pointerTransPixels, tableAddress) ||
			tableAddress < 0x10)
			return false;

		const unsigned long long address =
			static_cast<unsigned long long>(tableAddress - 0x10) +
			static_cast<unsigned long long>(sprite) * 0x10;
		if(address > (std::numeric_limits<DWORD>::max)())
			return false;
		entryAddress = static_cast<DWORD>(address);
		return true;
	}

	void FillColorKey(unsigned char* pixels, unsigned char red,
		unsigned char green, unsigned char blue) noexcept
	{
		for(std::size_t i = 0; i < 1024; ++i)
		{
			pixels[i * 3] = red;
			pixels[i * 3 + 1] = green;
			pixels[i * 3 + 2] = blue;
		}
	}
}

SpritePixels LoadSpriteAlpha(uint32_t sprite)
{
	SpritePixels pixels(new(std::nothrow) unsigned char[4096]());
	if(!pixels)
		return {};

	DWORD entryAddress = 0;
	DWORD dataOffset = 0;
	if(!GetSpriteEntryAddress(sprite, entryAddress) ||
		!TryReadDword(entryAddress, dataOffset) || dataOffset == 0)
		return pixels;

	std::size_t cursor = dataOffset;
	unsigned char colorKey[3] = {};
	WORD encodedSize = 0;
	if(!spritesFile->read(cursor, colorKey, sizeof(colorKey)))
		return pixels;
	cursor += sizeof(colorKey);
	if(!ReadSpriteValue(cursor, encodedSize) || encodedSize == 0)
		return pixels;

	std::size_t consumed = 0;
	std::size_t pixelIndex = 0;
	bool coloredRun = false;
	while(consumed < encodedSize)
	{
		WORD runLength = 0;
		if(encodedSize - consumed < sizeof(runLength) ||
			!ReadSpriteValue(cursor, runLength))
			return pixels;
		consumed += sizeof(runLength);
		if(runLength > 1024 - pixelIndex)
			return pixels;

		if(!coloredRun)
		{
			pixelIndex += runLength;
		}
		else
		{
			const std::size_t bytesPerPixel = client_transparent ? 4 : 3;
			const std::size_t runBytes =
				static_cast<std::size_t>(runLength) * bytesPerPixel;
			if(runBytes > encodedSize - consumed)
				return pixels;
			for(WORD i = 0; i < runLength; ++i, ++pixelIndex)
			{
				unsigned char rgba[4] = { 0, 0, 0, 0xFF };
				if(!spritesFile->read(cursor, rgba, bytesPerPixel))
					return pixels;
				cursor += bytesPerPixel;
				const std::size_t output = pixelIndex * 4;
				pixels[output] = rgba[0];
				pixels[output + 1] = rgba[1];
				pixels[output + 2] = rgba[2];
				pixels[output + 3] = rgba[3];
			}
			consumed += runBytes;
		}
		coloredRun = !coloredRun;
	}
	return pixels;
}

uint32_t HookPointers()
{
	uint32_t value = 0;
	if(!ReadSpriteValue(spritePointerOffset, value))
		return 0;
	return value;
}

uint32_t HookSignature()
{
	return 0x44545845; //'EXTD'
}

uint32_t HookNumSprites()
{
	std::unique_ptr<Sprites> candidate(
		new(std::nothrow) Sprites(GetFileName(4, false), "rb"));
	if(!candidate || !candidate->loaded())
	{
		MessageBox(NULL, "Cannot read client .spr file.", PROJECT_NAME,
			MB_OK | MB_ICONERROR);
		return 0;
	}

	std::size_t offset = 4;
	uint32_t count = 0;
	if(client_extended)
	{
		if(!candidate->read(offset, &count, sizeof(count)))
			return 0;
		offset += sizeof(count);
	}
	else
	{
		WORD shortCount = 0;
		if(!candidate->read(offset, &shortCount, sizeof(shortCount)))
			return 0;
		count = shortCount;
		offset += sizeof(shortCount);
	}

	if(count == 0 || offset > candidate->size() ||
		static_cast<std::size_t>(count) >
			(candidate->size() - offset) / sizeof(DWORD))
		return 0;
	spritesFile = std::move(candidate);
	spritePointerOffset = offset;
	spriteCount = count;
	return count;
}

void HookLoadSprite(uint32_t sprite, unsigned char* pixels)
{
	if(!pixels || !spritesFile)
		return;

	DWORD entryAddress = 0;
	DWORD dataOffset = 0;
	if(!GetSpriteEntryAddress(sprite, entryAddress) ||
		!TryReadDword(entryAddress, dataOffset))
		return;

	if(dataOffset == 0)
	{
		TryWriteDword(entryAddress + 4, 0xFF);
		TryWriteDword(entryAddress + 8, 0x00);
		TryWriteDword(entryAddress + 12, 0xFF);
		FillColorKey(pixels, 0xFF, 0x00, 0xFF);
		return;
	}

	std::size_t cursor = dataOffset;
	unsigned char colorKey[3] = {};
	WORD encodedSize = 0;
	if(!spritesFile->read(cursor, colorKey, sizeof(colorKey)))
		return;
	cursor += sizeof(colorKey);
	TryWriteDword(entryAddress + 4, colorKey[0]);
	TryWriteDword(entryAddress + 8, colorKey[1]);
	TryWriteDword(entryAddress + 12, colorKey[2]);
	FillColorKey(pixels, colorKey[0], colorKey[1], colorKey[2]);
	if(!ReadSpriteValue(cursor, encodedSize) || encodedSize == 0)
		return;

	std::size_t consumed = 0;
	std::size_t pixelIndex = 0;
	bool coloredRun = false;
	while(consumed < encodedSize)
	{
		WORD runLength = 0;
		if(encodedSize - consumed < sizeof(runLength) ||
			!ReadSpriteValue(cursor, runLength))
			return;
		consumed += sizeof(runLength);
		if(runLength > 1024 - pixelIndex)
			return;

		if(!coloredRun)
		{
			pixelIndex += runLength;
		}
		else
		{
			const std::size_t bytesPerPixel = client_transparent ? 4 : 3;
			const std::size_t runBytes =
				static_cast<std::size_t>(runLength) * bytesPerPixel;
			if(runBytes > encodedSize - consumed)
				return;
			for(WORD i = 0; i < runLength; ++i, ++pixelIndex)
			{
				unsigned char rgb[4] = {};
				if(!spritesFile->read(cursor, rgb, bytesPerPixel))
					return;
				cursor += bytesPerPixel;
				const std::size_t output = pixelIndex * 3;
				pixels[output] = rgb[0];
				pixels[output + 1] = rgb[1];
				pixels[output + 2] = rgb[2];
			}
			consumed += runBytes;
		}
		coloredRun = !coloredRun;
	}
}

void __stdcall MyLoadSprite(int surface, int x, int y, int w, int h, void* data)
{
	if(transEngine)
		transEngine->LoadSprite(surface, x, y, w, h, data);
}

DWORD HookExtendedEngine()
{
	if(transEngine)
		return (DWORD)&newRenderer;
	else
		return GetEngineAddr();
}

bool __stdcall HookCreateGLContext()
{
	bool created = ((bool (__stdcall *)())CreateGlContext)();
	if(created)
	{
		if(transEngine && transEngine->getRenderId() != OGL_RENDER)
			transEngine.reset();
		std::unique_ptr<ExtendedEngine> candidate;
		if(!transEngine)
		{
			candidate.reset(new(std::nothrow) ExtendedEngineOGL());
			if(!candidate)
				return created;
		}
		if(!OverWriteByte(SpriteContext, SpriteContextPos))
		{
			transEngine.reset();
			return created;
		}
		if(candidate)
			transEngine = std::move(candidate);
	}

	return created;
}

void __stdcall HookDeleteGLContext()
{
	OverWriteByte(SpriteContext, SpriteContextNeg);
	if(transEngine && transEngine->getRenderId() == OGL_RENDER)
		transEngine.reset();

	((void (__stdcall *)())DeleteGlContext)();
}

void __stdcall HookGLEnableAlpha(GLenum cap)
{
	if(transEngine)
		transEngine->enableAlpha();

	glEnable(cap);
}

void __stdcall HookGLDisableAlpha(void)
{
	glEnd();
	if(transEngine)
		transEngine->disableAlpha();
}

void __fastcall HookCreateDX9Context(DWORD dx9engine, int)
{
	((void (__fastcall *)(DWORD, int))CreateDX9Context)(dx9engine, 0);
	if(transEngine && transEngine->getRenderId() != DX9_RENDER)
		transEngine.reset();
	std::unique_ptr<ExtendedEngine> candidate;
	if(!transEngine)
	{
		candidate.reset(new(std::nothrow) ExtendedEngineDX9());
		if(!candidate)
			return;
	}
	if(!OverWriteByte(SpriteContext, SpriteContextPos))
	{
		transEngine.reset();
		return;
	}
	if(candidate)
		transEngine = std::move(candidate);
}

void __fastcall HookDeleteDX9Context(DWORD dx9engine, int)
{
	OverWriteByte(SpriteContext, SpriteContextNeg);
	if(transEngine && transEngine->getRenderId() == DX9_RENDER)
		transEngine.reset();

	((void (__fastcall *)(DWORD, int))DeleteDX9Context)(dx9engine, 0);
}

void __fastcall HookCreateDX7Context(DWORD dx7engine, int)
{
	((void (__fastcall *)(DWORD, int))CreateDX7Context)(dx7engine, 0);
	OverWriteByte(SpriteContext, SpriteContextNeg);
}

void __fastcall HookDeleteDX7Context(DWORD dx7engine, int)
{
	OverWriteByte(SpriteContext, SpriteContextNeg);
	((void (__fastcall *)(DWORD, int))DeleteDX7Context)(dx7engine, 0);
}

void HookDrawHealthBar(int nSurface, int X, int Y, int W, int H, int SkinId, int dX, int dY)
{
	DrawSkin(nSurface, X, Y, W, H, SkinId, dX, dY);
	DWORD maximum = 0;
	DWORD current = 0;
	if(!TryReadDword(PlayerHealthMaxAddr, maximum) ||
		!TryReadDword(PlayerHealthAddr, current))
		return;

	const double ratio = maximum == 0 ? 0.0 :
		std::clamp(static_cast<double>(current) / maximum, 0.0, 1.0);
	char tmpBuff[8] = {};
	_snprintf_s(tmpBuff, sizeof(tmpBuff), _TRUNCATE, "%d%%",
		static_cast<int>(100 * ratio));
	PrintText(nSurface, X+45, Y, 2, 180, 180, 180, tmpBuff, 1);
}

void HookDrawManaBar(int nSurface, int X, int Y, int W, int H, int SkinId, int dX, int dY)
{
	DrawSkin(nSurface, X, Y, W, H, SkinId, dX, dY);
	DWORD maximum = 0;
	DWORD current = 0;
	if(!TryReadDword(PlayerManaMaxAddr, maximum) ||
		!TryReadDword(PlayerManaAddr, current))
		return;

	const double ratio = maximum == 0 ? 0.0 :
		std::clamp(static_cast<double>(current) / maximum, 0.0, 1.0);
	char tmpBuff[8] = {};
	_snprintf_s(tmpBuff, sizeof(tmpBuff), _TRUNCATE, "%d%%",
		static_cast<int>(100 * ratio));
	PrintText(nSurface, X+45, Y, 2, 180, 180, 180, tmpBuff, 1);
}

#if (!defined(__CONFIG__) && defined(__MANABAR__)) || defined(__CONFIG__)
void __stdcall MyDrawHPBar(DWORD nSurface, DWORD X, DWORD Y, DWORD W, DWORD creaturePointer/*this should be height but we don't need it so we hack our way'*/, DWORD nRed, DWORD nGreen, DWORD nBlue)
{
	BYTE clientState = 0;
	if(!TryReadByte(client_BaseAddr + 0x39C314, clientState) ||
		clientState < 2)
		return;

	if(nRed == 0 && nGreen == 0 && nBlue == 0)//skip drawing black bar
		return;

	DWORD creatureId = 0;
	if(!TryReadDword(creaturePointer, creatureId))
		return;

	const DWORD engineAddr = GetEngineAddr ? GetEngineAddr() : 0;
	DWORD virtualTable = 0;
	DWORD drawRectAddr = 0;
	if(!TryReadDword(engineAddr, virtualTable) ||
		!TryReadDword(virtualTable + 0x14, drawRectAddr) || !drawRectAddr)
		return;
		using DrawRectangleFn = void (__fastcall*)(DWORD, DWORD, DWORD, DWORD,
			DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
	const DrawRectangleFn drawRectangle =
		reinterpret_cast<DrawRectangleFn>(drawRectAddr);
	drawRectangle(engineAddr, 0, nSurface, X-1, Y-1, 27, 4, 0, 0, 0);
	drawRectangle(engineAddr, 0, nSurface, X, Y, W, 2, nRed, nGreen, nBlue);
	DWORD playerId = 0;
	if(TryReadDword(PlayerIDAddr, playerId) && creatureId == playerId
	#ifdef __CONFIG__
		&& should_draw_manabar
	#endif
	)
	{
		DWORD maximum = 0;
		DWORD current = 0;
		if(!TryReadDword(PlayerManaMaxAddr, maximum) ||
			!TryReadDword(PlayerManaAddr, current))
			return;

		const double ratio = maximum == 0 ? 0.0 :
			std::clamp(static_cast<double>(current) / maximum, 0.0, 1.0);
		drawRectangle(engineAddr, 0, nSurface, X-1, Y+4, 27, 4, 0, 0, 0);
		drawRectangle(engineAddr, 0, nSurface, X, Y+5,
			static_cast<DWORD>(25 * ratio), 2, 0, 108, 255);
	}
}
#endif

DWORD HPBarRenderHandle()
{
	return (DWORD)&newRenderer;
}

void HookPrintFps(int nSurface, int nX, int nY, int nFont, int nRed,
	int nGreen, int nBlue, const char* text, int nAlign)
{
	PrintText(nSurface, nX, nY + 14, nFont, nRed, nGreen, nBlue, text,
		nAlign);

	DWORD playerId = 0;
	const bool online = TryReadDword(PlayerIDAddr, playerId) && playerId != 0;
	EnhancedDat::UpdateSession(online);
	const bool insideGame = should_use_weather ?
		WeatherSystem::Draw(nSurface, online) : online;
	PingOverlay::Draw(nSurface, insideGame);
	ChatInput::Update(online);
	UpdateHeldMovement(online);
	InstallElfBotArrowStateCompatibility();
	StoreWindow::Draw(nSurface, online);
}

static HRESULT WINAPI Init( bool extended, bool transparent)
{
	HRESULT result = S_OK;
	HMODULE baseHandle = GetModuleHandle(NULL);
	client_BaseAddr = reinterpret_cast<DWORD>(baseHandle);
	if(!baseHandle)
	{
		result = E_HANDLE;
		return result;
	}

	const DWORD entryPoint = g_entryPointContinue >= client_BaseAddr + 7 ?
		g_entryPointContinue - client_BaseAddr - 7 : 0;
	if(entryPoint == 0x1625EB)
		client_Version = 860;
	else
		client_Version = 0;

	switch(client_Version)
	{


		#ifdef __INCLUDE_860_VERSION__
		case 860:
		{
			if(!InitializeProjectInfo())
			{
				result = E_FAIL;
				break;
			}
			if(should_use_custom_client_os)
			{
				// Identify both login packets as the custom 8.60 DLL client.
				OverWriteByte(client_BaseAddr + 0x15FCD, 30);
				OverWriteByte(client_BaseAddr + 0x1695C, 30);
			}

			transEngine.reset();
			client_extended = extended;
			client_transparent = transparent;
			client_pointerTransPixels = (client_BaseAddr+0x24A9A8);
			GetFileName = reinterpret_cast<GetFileNameFn>(client_BaseAddr + 0x108870);
			GetEngineAddr = reinterpret_cast<GetEngineAddrFn>(client_BaseAddr + 0x122A90);
			DrawSkin = reinterpret_cast<DrawSkinFn>(client_BaseAddr + 0xB96E0);
			PrintText = reinterpret_cast<PrintTextFn>(client_BaseAddr + 0xB4DD0);
			if(!ChatInput::Install(client_BaseAddr,
					&ClearHeldMovementKeys, &ToggleSmartWalkingState,
					&IsSmartWalkingEnabled) ||
				!NetworkRedirect::Install(client_BaseAddr,
					should_use_dynamic_endpoint) ||
				!StoreWindow::Install(client_BaseAddr) ||
				(should_use_weather && !WeatherSystem::Install(client_BaseAddr,
					should_use_custom_client_os)) ||
				!PingOverlay::Install(client_BaseAddr, should_use_custom_ping,
					should_consume_impact_tracker))
			{
				result = E_FAIL;
				break;
			}
			HookCall(client_BaseAddr + 0x5A258, (DWORD)&HookPrintFps);
			if constexpr (should_use_custom_ping || should_use_custom_client_os)
				Nop(client_BaseAddr + 0x5A194, 6);
			HookCall((client_BaseAddr+0xABA36), (DWORD)&HookPointers);
			HookCall((client_BaseAddr+0xAB9C9), (DWORD)&HookSignature);
			HookCall((client_BaseAddr+0xAB9D9), (DWORD)&HookNumSprites);
			HookCall((client_BaseAddr+0xADCC9), (DWORD)&HookLoadSprite);
			HookCall((client_BaseAddr+0x340C4), (DWORD)&HookDrawHealthBar);
			HookCall((client_BaseAddr+0x34276), (DWORD)&HookDrawManaBar);
			OverWrite((client_BaseAddr+0x1B85A0), (DWORD)&getTimerTick);
			Nop((client_BaseAddr+0xAB9DE), 3);

			OverWrite((client_BaseAddr+0xC7BD5), (DWORD)&prjInfo[0]);
			OverWrite((client_BaseAddr+0xBD9FB), 0xD2);//Info Window Width - 0xBE+20 to make more space for text

			//Use DirectDraw7 interface instead of DirectDraw4
			//DEFINE_GUID( IID_IDirectDraw4,          0x9c59509a,0x39bd,0x11d1,0x8c,0x4a,0x00,0xc0,0x4f,0xd9,0x30,0xc5 );
			//DEFINE_GUID( IID_IDirectDraw7,          0x15e65ec0,0x3b9c,0x11d2,0xb9,0x2f,0x00,0x60,0x97,0x97,0xea,0x5b );
			OverWrite((client_BaseAddr+0x1D8840), 0x15E65EC0);
			OverWrite((client_BaseAddr+0x1D8840+4), 0x11D23B9C);
			OverWrite((client_BaseAddr+0x1D8840+8), 0x60002FB9);
			OverWrite((client_BaseAddr+0x1D8840+12), 0x5BEA9797);
			OverWriteByte((client_BaseAddr+0x1C1DB7), 0x37);

			#ifdef __MAGIC_EFFECTS_U16__
			if(should_use_magic_effects_u16)
			{
				HookCall((client_BaseAddr+0x104B4), (client_BaseAddr+0xF9C00));
				OverWriteByte((client_BaseAddr+0x104BA), 0xB7);
			}
			#endif

			#ifdef __DISTANCE_SHOOT_U16__
			if(should_use_distance_shoots_u16)
			{
				HookCall((client_BaseAddr+0x108F6), (client_BaseAddr+0xF9C00));
				OverWriteByte((client_BaseAddr+0x108FC), 0xB7);
			}
			#endif

			//if you want to use this remember it's limited to 0x7FFFFFFF because its handled as signed int32_t
			#ifdef __PLAYER_HEALTH_U32__
			if(should_use_player_health_u32)
			{
				HookCall((client_BaseAddr+0x11D2B), (client_BaseAddr+0xF9DA0));
				OverWrite((client_BaseAddr+0x11D30), 0x8990F08B);
				HookCall((client_BaseAddr+0x11D36), (client_BaseAddr+0xF9DA0));
				OverWrite((client_BaseAddr+0x11D3B), 0x8990F88B);
			}
			#endif

			#ifdef __PLAYER_MANA_U32__
			if(should_use_player_mana_u32)
			{
				HookCall((client_BaseAddr+0x11D69), (client_BaseAddr+0xF9DA0));
				OverWrite((client_BaseAddr+0x11D6E), 0x8990D08B);
				HookCall((client_BaseAddr+0x11D74), (client_BaseAddr+0xF9DA0));
				OverWrite((client_BaseAddr+0x11D79), 0x89909090);
			}
			#endif

			//Might be usefull if you want to use custom skills system
			#ifdef __PLAYER_SKILLS_U16__
			HookCall((client_BaseAddr+0x11FA4), (client_BaseAddr+0xF9C00));
			OverWriteByte((client_BaseAddr+0x11FAA), 0xB7);
			#endif

			PlayerHealthAddr = (client_BaseAddr+0x23FE94);
			PlayerHealthMaxAddr = (client_BaseAddr+0x23FE90);
			PlayerManaAddr = (client_BaseAddr+0x23FE78);
			PlayerManaMaxAddr = (client_BaseAddr+0x23FE74);
			PlayerIDAddr = (client_BaseAddr+0x23FE98);

			#if (!defined(__CONFIG__) && defined(__MANABAR__)) || defined(__CONFIG__)
			newRenderer->DrawRectangle = MyDrawHPBar;
			HookCall((client_BaseAddr+0xF5675), (DWORD)&HPBarRenderHandle);
			HookCall((client_BaseAddr+0xF573E), (DWORD)&HPBarRenderHandle);
			HookCall((client_BaseAddr+0xF5875), (DWORD)&HPBarRenderHandle);
			HookCall((client_BaseAddr+0xF5922), (DWORD)&HPBarRenderHandle);
			OverWriteWord((client_BaseAddr+0xF57D8), 0xFFD0);//hack our way to creaturepointer
			OverWriteWord((client_BaseAddr+0xF5912), 0xFFD0);//hack our way to creaturepointer
			#endif
			if(transparent)
			{
				newRenderer->LoadSprite = MyLoadSprite;

				CreateGlContext = (client_BaseAddr+0x139F50);
				DeleteGlContext = (client_BaseAddr+0x139CF0);
				CreateDX9Context = (client_BaseAddr+0x131FD0);
				DeleteDX9Context = (client_BaseAddr+0x131D30);
				CreateDX7Context = (client_BaseAddr+0x129A10);
				DeleteDX7Context = (client_BaseAddr+0x1297D0);
				SpriteContext = (client_BaseAddr+0xAF686);
				SpriteContextPos = 0x57;
				SpriteContextNeg = 0x51;

				HookCall((client_BaseAddr+0xAF67A), (DWORD)&HookExtendedEngine);
				HookCall((client_BaseAddr+0x1403AB), (DWORD)&HookCreateGLContext);
				HookCall((client_BaseAddr+0x1400A6), (DWORD)&HookDeleteGLContext);
				HookCallN((client_BaseAddr+0x13E948), (DWORD)&HookGLEnableAlpha);
				HookCallN((client_BaseAddr+0x13EA44), (DWORD)&HookGLDisableAlpha);
				OverWrite((client_BaseAddr+0x1D2214), (DWORD)&HookCreateDX9Context);
				OverWrite((client_BaseAddr+0x1D2210), (DWORD)&HookDeleteDX9Context);
				OverWrite((client_BaseAddr+0x1D1DF4), (DWORD)&HookCreateDX7Context);
				OverWrite((client_BaseAddr+0x1D1DF0), (DWORD)&HookDeleteDX7Context);
			}

			if(extended)
			{
				HookCall((client_BaseAddr+0x100967), (client_BaseAddr+0x1175D0));
				HookJMP((client_BaseAddr+0x100944), (client_BaseAddr+0x1B7AA0));
				OverWrite((client_BaseAddr+0x100972), 0x908A0489);

				OverWrite((client_BaseAddr+0x1B7AA0), 0xBD048D3E);
				OverWriteByte((client_BaseAddr+0x1B7AA8), 0x50);
				HookCall((client_BaseAddr+0x1B7AA9), (client_BaseAddr+0x171A12));
				HookJMP((client_BaseAddr+0x1B7AAE), (client_BaseAddr+0x10094D));

				OverWrite((client_BaseAddr+0x10080C), 0xFC8A448B);
				Nop((client_BaseAddr+0x10080C+4), 1);
			}

			if(!OutfitLimit::Install(client_BaseAddr, should_outfit_limit) ||
				!EnhancedDat::Install(client_BaseAddr, client_extended))
			{
				result = E_FAIL;
				break;
			}
			if(!MountSystem::Install(client_BaseAddr, should_use_mount_system))
			{
				result = E_FAIL;
				break;
			}

		}
		break;
		#endif

		default:
			result = E_INVALIDARG;
			break;
	}

	return result;
}

static int InitMain()
{
	if(IntegrityCheck::GetStatus() != IntegrityCheck::Status::Valid)
	{
		MessageBoxA(nullptr, "Client component unavailable.", PROJECT_NAME,
			MB_OK | MB_ICONERROR);
		RestoreHooks();
		return 0;
	}
	if(should_use_multiclient &&
		!ApplyMulticlientPatch(reinterpret_cast<DWORD>(GetModuleHandleA(nullptr))))
	{
		MessageBoxA(nullptr,
			"The multiclient patch does not match this Tibia 8.60 executable.",
			PROJECT_NAME, MB_OK | MB_ICONERROR);
		RestoreHooks();
		return 0;
	}

	char systemDirectory[MAX_PATH] = {};
	char systemDDrawDllPath[MAX_PATH] = {};
	const UINT systemLength = GetSystemDirectoryA(systemDirectory, MAX_PATH);
	if(systemLength == 0 || systemLength >= MAX_PATH ||
		_snprintf_s(systemDDrawDllPath, sizeof(systemDDrawDllPath), _TRUNCATE,
			"%s\\ddraw.dll", systemDirectory) < 0)
	{
		RestoreHooks();
		return 0;
	}
	orig_ddraw = LoadLibraryA(systemDDrawDllPath);
	if(!orig_ddraw)
	{
		MessageBox(NULL, "Cannot load system 'ddraw.dll'.", PROJECT_NAME, MB_OK|MB_ICONERROR);
		RestoreHooks();
		return 0;
	}

	#ifdef __CONFIG__
	HRESULT result = Init(should_use_extended, should_use_alpha);
	#else
	HRESULT result = Init(
	#ifdef __EXTENDED_FILE__
	true,
	#else
	false,
	#endif
	#ifdef __ALPHA_SPRITES__
	true
	#else
	false
	#endif
	);
	#endif
	if(result != S_OK || !HooksHealthy())
	{
		MessageBox(NULL,
			result != S_OK ? "This version of client is unsupported." :
				"A required client hook could not be installed.",
			PROJECT_NAME, MB_OK | MB_ICONERROR);
		RestoreHooks();
		return 0;
	}

	if(!SetupInputHooks())
	{
		MessageBoxA(nullptr, "Cannot install the input hooks.", PROJECT_NAME,
			MB_OK | MB_ICONERROR);
		RestoreHooks();
		return 0;
	}
	if(!PinModuleLifetime())
	{
		ClearHeldMovementKeys();
		if(keyboardHook)
		{
			UnhookWindowsHookEx(keyboardHook);
			keyboardHook = nullptr;
		}
		if(mouseHook)
		{
			UnhookWindowsHookEx(mouseHook);
			mouseHook = nullptr;
		}
		MessageBoxA(nullptr, "Cannot pin the hook DLL in memory.", PROJECT_NAME,
			MB_OK | MB_ICONERROR);
		RestoreHooks();
		return 0;
	}

	return 1;
}

extern "C"
{
	__declspec(dllexport) HRESULT WINAPI DirectDrawCreate(void* lpGUID, void* lplpDD, void* pUnkOuter)
	{
		if(!orig_ddraw)
			return E_HANDLE;
		FARPROC ddcreate = GetProcAddress(orig_ddraw, "DirectDrawCreate");
		if(!ddcreate)
			return E_INVALIDARG;

		using DirectDrawCreateFn = HRESULT (WINAPI*)(void*, void*, void*);
		return reinterpret_cast<DirectDrawCreateFn>(ddcreate)(lpGUID, lplpDD,
			pUnkOuter);
	}
}

extern "C"
{
	BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID reserved)
	{
		switch(dwReason)
		{
			case DLL_PROCESS_ATTACH:
				selfModule = hModule;
				DisableThreadLibraryCalls(hModule);
				return InstallDeferredInitialization() ? TRUE : FALSE;

			case DLL_THREAD_ATTACH:
			case DLL_THREAD_DETACH:
				break;
			case DLL_PROCESS_DETACH:
				// During process termination Windows reclaims all address-space
				// resources. Avoid calling hooks, UI or heap code under loader lock.
				if(reserved != nullptr)
					break;
				if (keyboardHook) {
					ClearHeldMovementKeys();
					UnhookWindowsHookEx(keyboardHook);
					keyboardHook = nullptr;
				}
				if(mouseHook) {
					UnhookWindowsHookEx(mouseHook);
					mouseHook = nullptr;
				}
				RestoreHooks();
				g_clientWindow.store(nullptr, std::memory_order_release);
				NetworkRedirect::Shutdown();
				ChatInput::Shutdown();
				PingOverlay::Shutdown();
				WeatherSystem::Shutdown();
				StoreWindow::Shutdown();
				MountSystem::Shutdown();
				EnhancedDat::Shutdown();
				transEngine.reset();
				spritesFile.reset();
				spritePointerOffset = 0;
				spriteCount = 0;
				SecureZeroMemory(g_projectInfo, sizeof(g_projectInfo));
				prjInfo = nullptr;
				selfModule = nullptr;
				break;
		}

		return 1;
	}
}
