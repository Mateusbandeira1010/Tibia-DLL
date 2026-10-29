#include "hook.h"

#include <cstddef>
#include <cstring>

namespace
{
	constexpr std::size_t MAX_PATCHES = 256;
	constexpr std::size_t MAX_PATCH_SIZE = 32;

	struct PatchRecord
	{
		DWORD address;
		std::size_t size;
		DWORD protection;
		BYTE original[MAX_PATCH_SIZE];
	};

	PatchRecord g_patches[MAX_PATCHES] = {};
	std::size_t g_patchCount = 0;
	bool g_patchFailure = false;

	bool RecordOriginal(DWORD address, std::size_t size,
		DWORD protection) noexcept
	{
		if(size > MAX_PATCH_SIZE)
			return false;
		for(std::size_t i = 0; i < g_patchCount; ++i)
		{
			if(g_patches[i].address == address && g_patches[i].size == size)
				return true;
		}
		if(g_patchCount >= MAX_PATCHES)
			return false;
		PatchRecord& patch = g_patches[g_patchCount];
		patch.address = address;
		patch.size = size;
		patch.protection = protection;
		__try {
			memcpy(patch.original, reinterpret_cast<const void*>(address), size);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			patch = {};
			return false;
		}
		++g_patchCount;
		return true;
	}

	bool WriteCode(DWORD address, const void* data, std::size_t size) noexcept
	{
		if(!address || !data || size == 0 || size > MAX_PATCH_SIZE)
		{
			g_patchFailure = true;
			return false;
		}

		DWORD oldProtection = 0;
		if(!VirtualProtect(reinterpret_cast<void*>(address), size,
			PAGE_EXECUTE_READWRITE, &oldProtection))
		{
			g_patchFailure = true;
			return false;
		}
		if(!RecordOriginal(address, size, oldProtection))
		{
			DWORD ignoredProtection = 0;
			VirtualProtect(reinterpret_cast<void*>(address), size, oldProtection,
				&ignoredProtection);
			g_patchFailure = true;
			return false;
		}

		__try {
			memcpy(reinterpret_cast<void*>(address), data, size);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			DWORD ignoredProtection = 0;
			VirtualProtect(reinterpret_cast<void*>(address), size, oldProtection,
				&ignoredProtection);
			g_patchFailure = true;
			return false;
		}
		const bool flushed = FlushInstructionCache(GetCurrentProcess(),
			reinterpret_cast<void*>(address), size) != FALSE;

		DWORD ignoredProtection = 0;
		const bool protectionRestored = VirtualProtect(
			reinterpret_cast<void*>(address), size, oldProtection,
			&ignoredProtection) != FALSE;
		if(!flushed || !protectionRestored)
			g_patchFailure = true;
		return flushed && protectionRestored;
	}

	bool WriteRelativeBranch(DWORD address, DWORD function, BYTE opcode,
		std::size_t overwriteSize) noexcept
	{
		if(!address || !function || overwriteSize < 5 ||
			overwriteSize > MAX_PATCH_SIZE)
		{
			g_patchFailure = true;
			return false;
		}
		BYTE patch[MAX_PATCH_SIZE] = {};
		memset(patch, 0x90, overwriteSize);
		patch[0] = opcode;
		const DWORD relative = function - address - 5;
		memcpy(patch + 1, &relative, sizeof(relative));
		return WriteCode(address, patch, overwriteSize);
	}
}

bool HookJMP(DWORD address, DWORD function, std::size_t overwriteSize)
{
	return WriteRelativeBranch(address, function, 0xE9, overwriteSize);
}

bool HookCall(DWORD address, DWORD function)
{
	return WriteRelativeBranch(address, function, 0xE8, 5);
}

bool HookCallN(DWORD address, DWORD function)
{
	return WriteRelativeBranch(address, function, 0xE8, 6);
}

bool Nop(DWORD address, int size)
{
	if(!address || size <= 0 ||
		static_cast<std::size_t>(size) > MAX_PATCH_SIZE)
	{
		g_patchFailure = true;
		return false;
	}
	BYTE patch[MAX_PATCH_SIZE] = {};
	memset(patch, 0x90, static_cast<std::size_t>(size));
	return WriteCode(address, patch, static_cast<std::size_t>(size));
}

bool OverWriteByte(DWORD address, BYTE value)
{
	return WriteCode(address, &value, sizeof(value));
}

bool OverWriteWord(DWORD address, WORD value)
{
	return WriteCode(address, &value, sizeof(value));
}

bool OverWrite(DWORD address, DWORD value)
{
	return WriteCode(address, &value, sizeof(value));
}

bool HooksHealthy() noexcept
{
	return !g_patchFailure;
}

void RestoreHooks()
{
	while(g_patchCount != 0)
	{
		PatchRecord& patch = g_patches[--g_patchCount];
		DWORD oldProtection = 0;
		if(VirtualProtect(reinterpret_cast<void*>(patch.address), patch.size,
			PAGE_EXECUTE_READWRITE, &oldProtection))
		{
			memcpy(reinterpret_cast<void*>(patch.address), patch.original,
				patch.size);
			FlushInstructionCache(GetCurrentProcess(),
				reinterpret_cast<void*>(patch.address), patch.size);
			DWORD ignoredProtection = 0;
			VirtualProtect(reinterpret_cast<void*>(patch.address), patch.size,
				patch.protection, &ignoredProtection);
		}
		patch = {};
	}
	g_patchFailure = false;
}
