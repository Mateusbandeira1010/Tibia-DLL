#include "chat_input.h"

#include "hook.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>

namespace ChatInput
{
	namespace
	{
		constexpr DWORD CONSOLE_VTABLE_RVA = 0x1BB878;
		constexpr DWORD CONSOLE_RENDER_RVA = 0x228E0;
		constexpr DWORD CONSOLE_EVENT_RVA = 0x2AE00;
		constexpr DWORD CONSOLE_RENDER_SLOT = 0x48;
		constexpr DWORD CONSOLE_EVENT_SLOT = 0x70;
		constexpr DWORD CONSOLE_TEXTBOX_OFFSET = 0x40;

		constexpr DWORD TEXT_BUTTON_VTABLE_RVA = 0x1C0188;
		constexpr DWORD CLIENT_ALLOC_RVA = 0x1719E2;
		constexpr DWORD CLIENT_FREE_RVA = 0x171A0D;
		constexpr DWORD BUILD_CALLBACK_RVA = 0xB76C0;
		constexpr DWORD TEXT_BUTTON_CTOR_RVA = 0x7C650;
		constexpr DWORD ADD_CHILD_RVA = 0xC0C60;
		constexpr DWORD PLACE_CHILD_RVA = 0xC45F0;

		constexpr DWORD ELEMENT_PARENT_OFFSET = 0x0C;
		constexpr DWORD ELEMENT_X_OFFSET = 0x14;
		constexpr DWORD ELEMENT_Y_OFFSET = 0x18;
		constexpr DWORD ELEMENT_WIDTH_OFFSET = 0x1C;
		constexpr DWORD ELEMENT_HEIGHT_OFFSET = 0x20;

		constexpr DWORD CHAT_TOGGLE_EVENT = 0x7FFF0001;
		constexpr DWORD SMART_WALKING_TOGGLE_EVENT = 0x7FFF0002;
		constexpr int CHAT_BUTTON_WIDTH = 75;
		constexpr int SMART_WALKING_BUTTON_WIDTH = 75;
		constexpr int BUTTON_HEIGHT = 20;
		constexpr int BUTTON_RIGHT_MARGIN = 7;
		constexpr int BUTTON_BOTTOM_MARGIN = 2;
		constexpr int BUTTON_SPACING = 4;
		constexpr int TEXTBOX_BUTTON_SPACING = 4;
		constexpr int MINIMUM_TEXTBOX_WIDTH = 20;
		constexpr int MAXIMUM_UI_COORDINATE = 16384;
		constexpr DWORD BUTTON_SKIN_UP = 18;
		constexpr DWORD BUTTON_SKIN_DOWN = 19;

		struct Rect
		{
			int x = 0;
			int y = 0;
			int width = 0;
			int height = 0;

			bool Contains(int pointX, int pointY) const noexcept
			{
				return pointX >= x && pointY >= y &&
					pointX < x + width && pointY < y + height;
			}
		};

		using ConsoleRenderFn = void (__thiscall*)(void* console);
		using ConsoleEventFn = void (__thiscall*)(void* console, DWORD eventId);
		using ClientAllocFn = void* (__cdecl*)(std::size_t size);
		using ClientFreeFn = void (__cdecl*)(void* memory);
		using BuildCallbackFn = DWORD* (__thiscall*)(DWORD output,
			DWORD owner, DWORD eventId);
		using TextButtonCtorFn = DWORD (__thiscall*)(DWORD self, DWORD x,
			DWORD y, DWORD text, DWORD normalSkin, DWORD pressedSkin,
			DWORD callbackObject, DWORD callbackMethod, DWORD tooltip);
		using AddChildFn = void (__thiscall*)(DWORD parent, DWORD child);
		using PlaceChildFn = void (__thiscall*)(DWORD parent, DWORD child,
			DWORD x, DWORD y);

		DWORD g_clientBase = 0;
		ConsoleRenderFn g_originalConsoleRender = nullptr;
		ConsoleEventFn g_originalConsoleEvent = nullptr;
		ReleaseMovementKeysFn g_releaseMovementKeys = nullptr;
		DWORD g_clientAllocAddr = 0;
		DWORD g_clientFreeAddr = 0;
		DWORD g_buildCallbackAddr = 0;
		DWORD g_textButtonCtorAddr = 0;
		DWORD g_addChildAddr = 0;
		DWORD g_placeChildAddr = 0;
		DWORD g_nativeConsole = 0;
		DWORD g_nativeChatButton = 0;
		DWORD g_nativeSmartWalkingButton = 0;
		bool g_nativeButtonsUnavailable = false;
		ToggleSmartWalkingFn g_toggleSmartWalking = nullptr;
		IsSmartWalkingEnabledFn g_isSmartWalkingEnabled = nullptr;
		std::atomic<Mode> g_mode(Mode::On);
		std::atomic<bool> g_online(false);
		std::atomic<bool> g_installed(false);
		SRWLOCK g_textboxLock = SRWLOCK_INIT;
		SRWLOCK g_inputLock = SRWLOCK_INIT;
		Rect g_textbox;
		bool g_textboxValid = false;
		bool g_ctrlTabHeld = false;
		bool g_smartWalkingKeyHeld = false;
		bool g_smartWalkingShortcutHeld = false;
		bool g_enterHeld = false;
		bool g_consumeEnterKeyUp = false;
		bool g_suppressedKeys[256] = {};
		alignas(LONG) char g_buttonCaption[12] = "Chat On";
		alignas(LONG) char g_smartWalkingCaption[12] = "Smart: On";

		bool IsSaneRect(const Rect& rect) noexcept;

		bool ReadDword(DWORD address, DWORD& value) noexcept
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

		bool ReadRect(DWORD object, Rect& rect) noexcept
		{
			if(!object)
				return false;
			__try {
				rect.x = *reinterpret_cast<const int*>(object + ELEMENT_X_OFFSET);
				rect.y = *reinterpret_cast<const int*>(object + ELEMENT_Y_OFFSET);
				rect.width = *reinterpret_cast<const int*>(
					object + ELEMENT_WIDTH_OFFSET);
				rect.height = *reinterpret_cast<const int*>(
					object + ELEMENT_HEIGHT_OFFSET);
				return true;
			} __except(EXCEPTION_EXECUTE_HANDLER) {
				rect = {};
				return false;
			}
		}

		bool ReadClientRect(DWORD object, Rect& rect) noexcept
		{
			if(!ReadRect(object, rect) || !IsSaneRect(rect))
				return false;

			DWORD current = object;
			for(int depth = 0; depth < 16; ++depth)
			{
				DWORD parent = 0;
				if(!ReadDword(current + ELEMENT_PARENT_OFFSET, parent))
					return false;
				if(!parent)
					return IsSaneRect(rect);
				if(parent == current)
					return false;

				Rect parentRect;
				if(!ReadRect(parent, parentRect) || !IsSaneRect(parentRect))
					return false;
				rect.x += parentRect.x;
				rect.y += parentRect.y;
				if(!IsSaneRect(rect))
					return false;
				current = parent;
			}
			return false;
		}

		bool WriteWidth(DWORD object, int width) noexcept
		{
			if(!object || width <= 0)
				return false;
			__try {
				*reinterpret_cast<int*>(object + ELEMENT_WIDTH_OFFSET) = width;
				return true;
			} __except(EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool IsSaneRect(const Rect& rect) noexcept
		{
			return rect.x >= -MAXIMUM_UI_COORDINATE &&
				rect.y >= -MAXIMUM_UI_COORDINATE && rect.width > 0 &&
				rect.height > 0 && rect.width <= MAXIMUM_UI_COORDINATE &&
				rect.height <= MAXIMUM_UI_COORDINATE &&
				rect.x <= MAXIMUM_UI_COORDINATE - rect.width &&
				rect.y <= MAXIMUM_UI_COORDINATE - rect.height;
		}

		void InvalidateTextbox() noexcept
		{
			AcquireSRWLockExclusive(&g_textboxLock);
			g_textbox = {};
			g_textboxValid = false;
			ReleaseSRWLockExclusive(&g_textboxLock);
		}

		void ClearTransientInput() noexcept
		{
			AcquireSRWLockExclusive(&g_inputLock);
			g_ctrlTabHeld = false;
			g_smartWalkingKeyHeld = false;
			g_smartWalkingShortcutHeld = false;
			g_enterHeld = false;
			g_consumeEnterKeyUp = false;
			std::memset(g_suppressedKeys, 0, sizeof(g_suppressedKeys));
			ReleaseSRWLockExclusive(&g_inputLock);
		}

		void ReleaseMovementKeys() noexcept
		{
			if(g_releaseMovementKeys)
				g_releaseMovementKeys();
		}

		void UpdateCaption(Mode mode) noexcept
		{
			DWORD suffix = 0x006E4F20; // " On\0"
			if(mode == Mode::Off)
				suffix = 0x66664F20; // " Off"
			else if(mode == Mode::Temporary)
				suffix = 0x2A6E4F20; // " On*"
			InterlockedExchange(reinterpret_cast<volatile LONG*>(
				g_buttonCaption + 4), static_cast<LONG>(suffix));

			const DWORD button = g_nativeChatButton;
			DWORD vtable = 0;
			if(button && ReadDword(button, vtable) &&
				vtable == g_clientBase + TEXT_BUTTON_VTABLE_RVA)
			{
				__try {
					// Native text buttons keep their visible caption inline at +0x48.
					// "Chat" never changes; update only the aligned suffix atomically.
					InterlockedExchange(reinterpret_cast<volatile LONG*>(
						button + 0x4C), static_cast<LONG>(suffix));
					*reinterpret_cast<char*>(button + 0x50) = '\0';
				} __except(EXCEPTION_EXECUTE_HANDLER) {
				}
			}
		}

		void UpdateSmartWalkingCaption(bool enabled) noexcept
		{
			// Only the aligned tail differs: "n\0\0\0" or "ff\0\0".
			const DWORD suffix = enabled ? 0x0000006E : 0x00006666;
			InterlockedExchange(reinterpret_cast<volatile LONG*>(
				g_smartWalkingCaption + 8), static_cast<LONG>(suffix));

			const DWORD button = g_nativeSmartWalkingButton;
			DWORD vtable = 0;
			if(button && ReadDword(button, vtable) &&
				vtable == g_clientBase + TEXT_BUTTON_VTABLE_RVA)
			{
				__try {
					InterlockedExchange(reinterpret_cast<volatile LONG*>(
						button + 0x50), static_cast<LONG>(suffix));
				} __except(EXCEPTION_EXECUTE_HANDLER) {
				}
			}
		}

		void ToggleSmartWalking() noexcept
		{
			if(!g_toggleSmartWalking || !g_isSmartWalkingEnabled)
				return;
			g_toggleSmartWalking();
			UpdateSmartWalkingCaption(g_isSmartWalkingEnabled());
		}

		void SetMode(Mode mode) noexcept
		{
			g_mode.store(mode, std::memory_order_release);
			UpdateCaption(mode);
			if(mode != Mode::Off)
				ReleaseMovementKeys();
		}

		bool IsNativeButton(DWORD button, DWORD console) noexcept
		{
			DWORD vtable = 0;
			DWORD parent = 0;
			return button && console &&
				ReadDword(button, vtable) &&
				vtable == g_clientBase + TEXT_BUTTON_VTABLE_RVA &&
				ReadDword(button + ELEMENT_PARENT_OFFSET, parent) &&
				parent == console;
		}

		void ReleaseUnowned(void* memory) noexcept
		{
			if(memory && g_clientFreeAddr)
				reinterpret_cast<ClientFreeFn>(g_clientFreeAddr)(memory);
		}

		DWORD CreateNativeButton(DWORD console, const char* caption,
			DWORD eventId) noexcept
		{
			void* allocation = nullptr;
			DWORD button = 0;
			bool attached = false;
			__try {
				allocation = reinterpret_cast<ClientAllocFn>(
					g_clientAllocAddr)(0x7C);
				if(!allocation)
					return 0;

				DWORD callback[2] = {};
				reinterpret_cast<BuildCallbackFn>(g_buildCallbackAddr)(
					reinterpret_cast<DWORD>(callback), console, eventId);
				button = reinterpret_cast<TextButtonCtorFn>(
					g_textButtonCtorAddr)(reinterpret_cast<DWORD>(allocation),
					0, 0, reinterpret_cast<DWORD>(caption),
					BUTTON_SKIN_UP, BUTTON_SKIN_DOWN, callback[0], callback[1], 0);
				if(!button)
				{
					ReleaseUnowned(allocation);
					return 0;
				}
				reinterpret_cast<AddChildFn>(g_addChildAddr)(console, button);
				attached = true;
				return button;
			} __except(EXCEPTION_EXECUTE_HANDLER) {
				if(!attached)
					ReleaseUnowned(allocation);
				return 0;
			}
		}

		void CacheNativeLayout(void* consolePointer) noexcept
		{
			const DWORD console = reinterpret_cast<DWORD>(consolePointer);
			DWORD consoleVtable = 0;
			DWORD textbox = 0;
			Rect consoleRect;
			Rect textboxRect;
			Rect textboxClientRect;
			if(!console || !ReadDword(console, consoleVtable) ||
				consoleVtable != g_clientBase + CONSOLE_VTABLE_RVA ||
				!ReadDword(console + CONSOLE_TEXTBOX_OFFSET, textbox) || !textbox ||
				!ReadRect(console, consoleRect) || !IsSaneRect(consoleRect) ||
				!ReadRect(textbox, textboxRect) || !IsSaneRect(textboxRect) ||
				!ReadClientRect(textbox, textboxClientRect))
			{
				InvalidateTextbox();
				return;
			}

			const int chatButtonX = consoleRect.width - CHAT_BUTTON_WIDTH -
				BUTTON_RIGHT_MARGIN;
			const int buttonY = consoleRect.height - BUTTON_HEIGHT -
				BUTTON_BOTTOM_MARGIN;
			const int smartWalkingButtonX = chatButtonX - BUTTON_SPACING -
				SMART_WALKING_BUTTON_WIDTH;
			const int availableTextboxWidth = smartWalkingButtonX -
				TEXTBOX_BUTTON_SPACING - textboxRect.x;
			if(smartWalkingButtonX < 0 || buttonY < 0 ||
				availableTextboxWidth < MINIMUM_TEXTBOX_WIDTH)
			{
				InvalidateTextbox();
				return;
			}

			if(g_nativeConsole != console)
			{
				g_nativeConsole = console;
				g_nativeChatButton = 0;
				g_nativeSmartWalkingButton = 0;
				g_nativeButtonsUnavailable = false;
			}
			if(!g_nativeButtonsUnavailable)
			{
				// Once a button belongs to the native UI tree, this DLL must not
				// free or replace it. If the console invalidates either child,
				// disable our controls until a new console object is observed.
				if((g_nativeChatButton &&
						!IsNativeButton(g_nativeChatButton, console)) ||
					(g_nativeSmartWalkingButton &&
						!IsNativeButton(g_nativeSmartWalkingButton, console)))
				{
					g_nativeButtonsUnavailable = true;
				}
				else
				{
					if(!g_nativeChatButton)
						g_nativeChatButton = CreateNativeButton(console,
							g_buttonCaption, CHAT_TOGGLE_EVENT);
					if(!g_nativeSmartWalkingButton)
						g_nativeSmartWalkingButton = CreateNativeButton(console,
							g_smartWalkingCaption,
							SMART_WALKING_TOGGLE_EVENT);
					if(!g_nativeChatButton || !g_nativeSmartWalkingButton)
						g_nativeButtonsUnavailable = true;
				}
			}
			if(g_nativeButtonsUnavailable)
			{
				InvalidateTextbox();
				return;
			}
			const int narrowedWidth = (std::min)(textboxRect.width,
				availableTextboxWidth);
			__try {
				reinterpret_cast<PlaceChildFn>(g_placeChildAddr)(console,
					g_nativeChatButton, static_cast<DWORD>(chatButtonX),
					static_cast<DWORD>(buttonY));
				reinterpret_cast<PlaceChildFn>(g_placeChildAddr)(console,
					g_nativeSmartWalkingButton,
					static_cast<DWORD>(smartWalkingButtonX),
					static_cast<DWORD>(buttonY));
			} __except(EXCEPTION_EXECUTE_HANDLER) {
				InvalidateTextbox();
				return;
			}

			if(narrowedWidth != textboxRect.width &&
				!WriteWidth(textbox, narrowedWidth))
			{
				InvalidateTextbox();
				return;
			}
			textboxClientRect.width = narrowedWidth;
			AcquireSRWLockExclusive(&g_textboxLock);
			g_textbox = textboxClientRect;
			g_textboxValid = true;
			ReleaseSRWLockExclusive(&g_textboxLock);
		}

		void __fastcall HookConsoleRender(void* console, void*)
		{
			CacheNativeLayout(console);
			if(g_originalConsoleRender)
				g_originalConsoleRender(console);
		}

		void __fastcall HookConsoleEvent(void* console, void*, DWORD eventId)
		{
			if(eventId == CHAT_TOGGLE_EVENT ||
				eventId == SMART_WALKING_TOGGLE_EVENT)
			{
				if(reinterpret_cast<DWORD>(console) == g_nativeConsole &&
					g_online.load(std::memory_order_acquire))
				{
					if(eventId == CHAT_TOGGLE_EVENT)
						Toggle();
					else
						ToggleSmartWalking();
				}
				return;
			}
			if(g_originalConsoleEvent)
				g_originalConsoleEvent(console, eventId);
		}

		HWND ClientWindow() noexcept
		{
			HWND window = GetForegroundWindow();
			if(!window)
				return nullptr;
			DWORD processId = 0;
			GetWindowThreadProcessId(window, &processId);
			return processId == GetCurrentProcessId() ? window : nullptr;
		}

		bool IsControlPressed() noexcept
		{
			return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
				(GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0 ||
				(GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
		}

		bool HasCommandModifier(const KBDLLHOOKSTRUCT& keyboard) noexcept
		{
			return IsControlPressed() || (keyboard.flags & LLKHF_ALTDOWN) != 0 ||
				(GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
				(GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
		}

		bool IsChatEditingShortcut(DWORD key) noexcept
		{
			return key == 'A' || key == 'C' || key == 'V' || key == 'X';
		}

		bool IsTextOrEditingKey(DWORD key) noexcept
		{
			if((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z') ||
				(key >= VK_NUMPAD0 && key <= VK_DIVIDE))
				return true;
			switch(key)
			{
				case VK_SPACE:
				case VK_BACK:
				case VK_DELETE:
				case VK_HOME:
				case VK_END:
				case VK_OEM_1:
				case VK_OEM_PLUS:
				case VK_OEM_COMMA:
				case VK_OEM_MINUS:
				case VK_OEM_PERIOD:
				case VK_OEM_2:
				case VK_OEM_3:
				case VK_OEM_4:
				case VK_OEM_5:
				case VK_OEM_6:
				case VK_OEM_7:
				case VK_OEM_8:
				case VK_OEM_102:
				case VK_PACKET:
					return true;
				default:
					return false;
			}
		}
	}

	bool Install(DWORD clientBase,
		ReleaseMovementKeysFn releaseMovementKeys,
		ToggleSmartWalkingFn toggleSmartWalking,
		IsSmartWalkingEnabledFn isSmartWalkingEnabled) noexcept
	{
		static_assert(sizeof(void*) == sizeof(DWORD),
			"The Tibia 8.60 hooks require a Win32 build.");
		if(!clientBase || !releaseMovementKeys || !toggleSmartWalking ||
			!isSmartWalkingEnabled ||
			g_installed.load(std::memory_order_acquire))
			return false;

		const DWORD renderSlot = clientBase + CONSOLE_VTABLE_RVA +
			CONSOLE_RENDER_SLOT;
		const DWORD eventSlot = clientBase + CONSOLE_VTABLE_RVA +
			CONSOLE_EVENT_SLOT;
		DWORD currentRender = 0;
		DWORD currentEvent = 0;
		if(!ReadDword(renderSlot, currentRender) ||
			currentRender != clientBase + CONSOLE_RENDER_RVA ||
			!ReadDword(eventSlot, currentEvent) ||
			currentEvent != clientBase + CONSOLE_EVENT_RVA)
		{
			return false;
		}

		g_clientBase = clientBase;
		g_originalConsoleRender =
			reinterpret_cast<ConsoleRenderFn>(currentRender);
		g_originalConsoleEvent = reinterpret_cast<ConsoleEventFn>(currentEvent);
		g_releaseMovementKeys = releaseMovementKeys;
		g_toggleSmartWalking = toggleSmartWalking;
		g_isSmartWalkingEnabled = isSmartWalkingEnabled;
		g_clientAllocAddr = clientBase + CLIENT_ALLOC_RVA;
		g_clientFreeAddr = clientBase + CLIENT_FREE_RVA;
		g_buildCallbackAddr = clientBase + BUILD_CALLBACK_RVA;
		g_textButtonCtorAddr = clientBase + TEXT_BUTTON_CTOR_RVA;
		g_addChildAddr = clientBase + ADD_CHILD_RVA;
		g_placeChildAddr = clientBase + PLACE_CHILD_RVA;

		if(!OverWrite(renderSlot,
			reinterpret_cast<DWORD>(&HookConsoleRender)) ||
			!OverWrite(eventSlot, reinterpret_cast<DWORD>(&HookConsoleEvent)))
		{
			return false;
		}

		g_nativeConsole = 0;
		g_nativeChatButton = 0;
		g_nativeSmartWalkingButton = 0;
		g_nativeButtonsUnavailable = false;
		g_mode.store(Mode::On, std::memory_order_release);
		UpdateCaption(Mode::On);
		UpdateSmartWalkingCaption(g_isSmartWalkingEnabled());
		g_online.store(false, std::memory_order_release);
		ClearTransientInput();
		InvalidateTextbox();
		g_installed.store(true, std::memory_order_release);
		return true;
	}

	void Shutdown() noexcept
	{
		Reset();
		g_installed.store(false, std::memory_order_release);
		g_nativeConsole = 0;
		g_nativeChatButton = 0;
		g_nativeSmartWalkingButton = 0;
		g_nativeButtonsUnavailable = false;
		g_originalConsoleRender = nullptr;
		g_originalConsoleEvent = nullptr;
		g_releaseMovementKeys = nullptr;
		g_toggleSmartWalking = nullptr;
		g_isSmartWalkingEnabled = nullptr;
		g_clientAllocAddr = 0;
		g_clientFreeAddr = 0;
		g_buildCallbackAddr = 0;
		g_textButtonCtorAddr = 0;
		g_addChildAddr = 0;
		g_placeChildAddr = 0;
		g_clientBase = 0;
	}

	bool IsEnabled() noexcept
	{
		return g_mode.load(std::memory_order_acquire) != Mode::Off;
	}

	bool IsTemporary() noexcept
	{
		return g_mode.load(std::memory_order_acquire) == Mode::Temporary;
	}

	bool CanUseWasdWalking() noexcept
	{
		return g_online.load(std::memory_order_acquire) && !IsEnabled();
	}

	void SetEnabled(bool enabled) noexcept
	{
		SetMode(enabled ? Mode::On : Mode::Off);
	}

	void Toggle() noexcept
	{
		SetMode(IsEnabled() ? Mode::Off : Mode::On);
	}

	void EnableTemporary() noexcept
	{
		SetMode(Mode::Temporary);
	}

	void Reset() noexcept
	{
		g_online.store(false, std::memory_order_release);
		SetMode(Mode::On);
		ClearTransientInput();
		InvalidateTextbox();
	}

	void ResetTransientInput() noexcept
	{
		ClearTransientInput();
	}

	bool HandleKeyboardControl(WPARAM message,
		const KBDLLHOOKSTRUCT& keyboard) noexcept
	{
		const bool keyDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
		const bool keyUp = message == WM_KEYUP || message == WM_SYSKEYUP;
		if(!keyDown && !keyUp)
			return false;

		if(keyboard.vkCode == 'W')
		{
			if(keyUp)
			{
				AcquireSRWLockExclusive(&g_inputLock);
				const bool consume = g_smartWalkingShortcutHeld;
				g_smartWalkingKeyHeld = false;
				g_smartWalkingShortcutHeld = false;
				ReleaseSRWLockExclusive(&g_inputLock);
				if(consume)
					return true;
			}
			else
			{
				AcquireSRWLockExclusive(&g_inputLock);
				const bool firstPress = !g_smartWalkingKeyHeld;
				g_smartWalkingKeyHeld = true;
				const bool alreadyConsumed = g_smartWalkingShortcutHeld;
				ReleaseSRWLockExclusive(&g_inputLock);
				if(alreadyConsumed)
					return true;

				const bool shortcut = firstPress && IsControlPressed() &&
					(GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 &&
					(keyboard.flags & LLKHF_ALTDOWN) == 0 &&
					(GetAsyncKeyState(VK_LWIN) & 0x8000) == 0 &&
					(GetAsyncKeyState(VK_RWIN) & 0x8000) == 0 &&
					g_online.load(std::memory_order_acquire);
				if(shortcut)
				{
					AcquireSRWLockExclusive(&g_inputLock);
					g_smartWalkingShortcutHeld = true;
					ReleaseSRWLockExclusive(&g_inputLock);
					ToggleSmartWalking();
					return true;
				}
			}
		}

		if(keyboard.vkCode == VK_TAB)
		{
			if(keyUp)
			{
				AcquireSRWLockExclusive(&g_inputLock);
				const bool consume = g_ctrlTabHeld;
				g_ctrlTabHeld = false;
				ReleaseSRWLockExclusive(&g_inputLock);
				return consume;
			}
			if(IsControlPressed() && g_online.load(std::memory_order_acquire))
			{
				AcquireSRWLockExclusive(&g_inputLock);
				const bool firstPress = !g_ctrlTabHeld;
				g_ctrlTabHeld = true;
				ReleaseSRWLockExclusive(&g_inputLock);
				if(firstPress)
					Toggle();
				return true;
			}
		}

		if(keyboard.vkCode != VK_RETURN)
			return false;

		if(keyUp)
		{
			AcquireSRWLockExclusive(&g_inputLock);
			const bool consume = g_enterHeld && g_consumeEnterKeyUp;
			g_enterHeld = false;
			g_consumeEnterKeyUp = false;
			ReleaseSRWLockExclusive(&g_inputLock);
			return consume;
		}
		if(!g_online.load(std::memory_order_acquire))
			return false;

		AcquireSRWLockExclusive(&g_inputLock);
		if(g_enterHeld)
		{
			ReleaseSRWLockExclusive(&g_inputLock);
			return true;
		}
		g_enterHeld = true;
		ReleaseSRWLockExclusive(&g_inputLock);

		const Mode mode = g_mode.load(std::memory_order_acquire);
		if(mode == Mode::Off)
		{
			AcquireSRWLockExclusive(&g_inputLock);
			g_consumeEnterKeyUp = true;
			ReleaseSRWLockExclusive(&g_inputLock);
			EnableTemporary();
			return true;
		}
		if(mode == Mode::Temporary)
			SetEnabled(false);
		return false;
	}

	bool ShouldSuppressTextKey(WPARAM message,
		const KBDLLHOOKSTRUCT& keyboard) noexcept
	{
		const bool keyDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
		const bool keyUp = message == WM_KEYUP || message == WM_SYSKEYUP;
		if((!keyDown && !keyUp) || keyboard.vkCode >= 256)
			return false;

		AcquireSRWLockExclusive(&g_inputLock);
		if(keyUp && g_suppressedKeys[keyboard.vkCode])
		{
			g_suppressedKeys[keyboard.vkCode] = false;
			ReleaseSRWLockExclusive(&g_inputLock);
			return true;
		}
		const bool alreadySuppressed = g_suppressedKeys[keyboard.vkCode];
		ReleaseSRWLockExclusive(&g_inputLock);
		if(alreadySuppressed)
			return true;
		if(!keyDown || !g_online.load(std::memory_order_acquire) || IsEnabled())
			return false;
		const bool chatEditingShortcut = IsControlPressed() &&
			IsChatEditingShortcut(keyboard.vkCode) &&
			(keyboard.flags & LLKHF_ALTDOWN) == 0 &&
			(GetAsyncKeyState(VK_LWIN) & 0x8000) == 0 &&
			(GetAsyncKeyState(VK_RWIN) & 0x8000) == 0;
		if(!chatEditingShortcut && (HasCommandModifier(keyboard) ||
			!IsTextOrEditingKey(keyboard.vkCode)))
		{
			return false;
		}

		AcquireSRWLockExclusive(&g_inputLock);
		g_suppressedKeys[keyboard.vkCode] = true;
		ReleaseSRWLockExclusive(&g_inputLock);
		return true;
	}

	void HandleMouseMessage(WPARAM message,
		const MSLLHOOKSTRUCT& mouse) noexcept
	{
		if(message != WM_LBUTTONDOWN ||
			!g_online.load(std::memory_order_acquire))
		{
			return;
		}

		HWND window = ClientWindow();
		if(!window)
			return;
		POINT point = mouse.pt;
		if(!ScreenToClient(window, &point))
			return;

		bool insideTextbox = false;
		AcquireSRWLockShared(&g_textboxLock);
		if(g_textboxValid)
			insideTextbox = g_textbox.Contains(point.x, point.y);
		ReleaseSRWLockShared(&g_textboxLock);
		if(insideTextbox && (IsTemporary() || !IsEnabled()))
			SetEnabled(true);
	}

	void Update(bool online) noexcept
	{
		const bool wasOnline = g_online.exchange(online,
			std::memory_order_acq_rel);
		if(!online)
		{
			if(wasOnline)
			{
				SetMode(Mode::On);
				ClearTransientInput();
				InvalidateTextbox();
			}
			return;
		}
		if(!wasOnline)
		{
			SetMode(Mode::On);
			ClearTransientInput();
		}
	}
}
