#ifndef TIBIA_CHAT_INPUT_H
#define TIBIA_CHAT_INPUT_H

#include <cstdint>
#include <windows.h>

namespace ChatInput
{
	enum class Mode : std::uint8_t
	{
		Off,
		On,
		Temporary
	};

	using ReleaseMovementKeysFn = void (*)();
	using ToggleSmartWalkingFn = void (*)();
	using IsSmartWalkingEnabledFn = bool (*)();

	bool Install(DWORD clientBase,
		ReleaseMovementKeysFn releaseMovementKeys,
		ToggleSmartWalkingFn toggleSmartWalking,
		IsSmartWalkingEnabledFn isSmartWalkingEnabled) noexcept;
	void Shutdown() noexcept;

	bool IsEnabled() noexcept;
	bool IsTemporary() noexcept;
	bool CanUseWasdWalking() noexcept;
	void SetEnabled(bool enabled) noexcept;
	void Toggle() noexcept;
	void EnableTemporary() noexcept;
	void Reset() noexcept;
	void ResetTransientInput() noexcept;

	bool HandleKeyboardControl(WPARAM message,
		const KBDLLHOOKSTRUCT& keyboard) noexcept;
	bool ShouldSuppressTextKey(WPARAM message,
		const KBDLLHOOKSTRUCT& keyboard) noexcept;
	void HandleMouseMessage(WPARAM message,
		const MSLLHOOKSTRUCT& mouse) noexcept;

	void Update(bool online) noexcept;
}

#endif
