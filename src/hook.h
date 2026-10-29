#ifndef TIBIA_HOOK_H
#define TIBIA_HOOK_H

#include <cstddef>
#include <windows.h>

bool HookJMP(DWORD address, DWORD function, std::size_t overwriteSize = 5);
bool HookCall(DWORD address, DWORD function);
bool HookCallN(DWORD address, DWORD function);
bool Nop(DWORD address, int size);
bool OverWriteByte(DWORD address, BYTE value);
bool OverWriteWord(DWORD address, WORD value);
bool OverWrite(DWORD address, DWORD value);
bool HooksHealthy() noexcept;
void RestoreHooks();

#endif
