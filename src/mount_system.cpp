#include "mount_system.h"
#include "config.h"
#include "enhanced_dat.h"
#include "hook.h"
#include "obfuscated_string.h"
#include "security_build_config.h"
#include "timer.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string.h>

namespace
{
	struct CreatureMountState
	{
		DWORD creatureId;
		WORD mountClientId;
		DWORD lastSeenTick;
	};

	struct MountEntry
	{
		WORD clientId;
		char name[64];
	};

	struct NativeRect
	{
		DWORD x;
		DWORD y;
		DWORD width;
		DWORD height;
	};

	struct NativeTextStyle
	{
		DWORD values[5];
	};

	using ReadU16Fn = WORD (__cdecl*)();
	using ReadU8Fn = BYTE (__cdecl*)();
	using ReadStringFn = void (__cdecl*)(char* output, DWORD maximumLength);
	using WriteValueFn = void (__cdecl*)(DWORD value);
	using DrawCreatureFn = void (__thiscall*)(DWORD renderContext, DWORD x,
		DWORD y, DWORD surface, DWORD creaturePtr);
	using GetThingTypeFn = DWORD (__thiscall*)(DWORD manager, DWORD thingId);
	using HasThingPropertyFn = BYTE (__cdecl*)(DWORD thingId, DWORD property);
	using GetDisplacementFn = DWORD (__cdecl*)(DWORD thingId);
	using ClientAllocFn = void* (__cdecl*)(size_t size);
	using ClientFreeFn = void (__cdecl*)(void* memory);
	using PreviewCtorFn = DWORD (__thiscall*)(DWORD self, DWORD lookType,
		DWORD direction, DWORD colors, DWORD addons, DWORD layout);
	using PanelCtorFn = DWORD (__thiscall*)(DWORD self, DWORD child,
		DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6, DWORD a7,
		DWORD a8, DWORD a9, DWORD layout);
	using BuildCallbackFn = DWORD* (__thiscall*)(DWORD output,
		DWORD owner, DWORD eventId);
	using ButtonCtorFn = DWORD (__thiscall*)(DWORD self, DWORD a1,
		DWORD a2, DWORD normalSkin, DWORD pressedSkin, DWORD callbackObject,
		DWORD callbackMethod, DWORD text, DWORD a8);
	using AddChildFn = void (__thiscall*)(DWORD parent, DWORD child);
	using SetSizeFn = void (__thiscall*)(DWORD object, DWORD width,
		DWORD height);
	using PlaceChildFn = void (__thiscall*)(DWORD parent, DWORD child,
		DWORD x, DWORD y);
	using PlaceChildRectFn = void (__thiscall*)(DWORD parent, DWORD child,
		NativeRect rectangle);
	using SetPreviewLookTypeFn = void (__thiscall*)(DWORD preview,
		DWORD lookType);
	using LabelCtorFn = DWORD (__thiscall*)(DWORD self,
		NativeTextStyle style, const char* text);
	using SetLabelTextFn = void (__thiscall*)(DWORD label,
		const char* text, DWORD unknown);
	using ContextMenuAddEntryFn = DWORD (__thiscall*)(DWORD context,
		DWORD eventId, const char* caption, DWORD flags);

	CreatureMountState g_states[512] = {};
	MountEntry g_mountEntries[256] = {};
	DWORD g_mountEntryCount = 0;
	DWORD g_skippedOutfitCount = 0;
	int g_mountSelection = -1;
	WORD g_currentMount = 0;
	WORD g_selectedMount = 0;
	bool g_protocolEnabled = false;
	bool g_drawingMount = false;
	volatile DWORD g_patternZ = 1;
	volatile DWORD g_mountedRider = 0;
	volatile DWORD g_mountDisplacementX = 0;
	volatile DWORD g_mountDisplacementY = 0;
	DWORD g_playerIdAddr = 0;
	DWORD g_readU8Addr = 0;
	DWORD g_readU16Addr = 0;
	DWORD g_readStringAddr = 0;
	DWORD g_writePacketStartAddr = 0;
	DWORD g_writeByteAddr = 0;
	DWORD g_writeU16Addr = 0;
	DWORD g_sendPacketAddr = 0;
	DWORD g_drawCreatureAddr = 0;
	DWORD g_getThingTypeAddr = 0;
	DWORD g_thingTypeManagerPtrAddr = 0;
	DWORD g_hasThingPropertyAddr = 0;
	DWORD g_getDisplacementXAddr = 0;
	DWORD g_getDisplacementYAddr = 0;

	DWORD g_knownCreatureReturn = 0;
	DWORD g_newCreatureReturn = 0;
	DWORD g_creatureOutfitReturn = 0;
	DWORD g_outfitWindowReturn = 0;
	DWORD g_outfitWindowListReturn = 0;
	DWORD g_setOutfitSendReturn = 0;
	DWORD g_drawBeforeOutfitReturn = 0;
	DWORD g_drawCreatureExitReturn = 0;
	DWORD g_spritePatternZReturn = 0;
	DWORD g_drawOutfitYOffsetReturn = 0;
	DWORD g_drawOutfitXOffsetReturn = 0;
	DWORD g_contextMenuAddEntryAddr = 0;
	DWORD g_contextMenuNativeContinuationAddr = 0;
	DWORD g_contextMenuReturn = 0;
	DWORD g_outfitWindowFinalizeAddr = 0;
	DWORD g_outfitWindowCreatedReturn = 0;
	DWORD g_outfitEventContinue = 0;
	DWORD g_outfitEventPrevious = 0;
	DWORD g_outfitEventDone = 0;
	DWORD g_contextCommandReturn = 0;
	DWORD g_nativeOutfitObject = 0;
	DWORD g_nativeMountPreview = 0;
	DWORD g_nativeMountLabel = 0;
	DWORD g_clientAllocAddr = 0;
	DWORD g_clientFreeAddr = 0;
	DWORD g_previewCtorAddr = 0;
	DWORD g_panelCtorAddr = 0;
	DWORD g_buildCallbackAddr = 0;
	DWORD g_buttonCtorAddr = 0;
	DWORD g_addChildAddr = 0;
	DWORD g_setSizeAddr = 0;
	DWORD g_placeChildAddr = 0;
	DWORD g_placeChildRectAddr = 0;
	DWORD g_setPreviewLookTypeAddr = 0;
	DWORD g_labelCtorAddr = 0;
	DWORD g_previewLayoutAddr = 0;
	DWORD g_panelLayoutValueAddr = 0;
	DWORD g_nameTextStyleAddr = 0;
	const DWORD g_mountPreviewXOffset = 12;
	const DWORD g_mountPreviewYOffset = 6;
	const char g_outfitDescription[] =
		"Select an outfit by clicking on the arrows below the\n"
		"character box. Individualise the single parts by\n"
		"using the colour palette. If you are premium and\n"
		"have earned an outfit addon, you can activate it by\n"
		"checking the corresponding box. Mounts earned in\n"
		"the game can be selected from the right-hand box.";
	DWORD g_previewColors[4] = {};

	#if TIBIA_ENABLE_DEV_LOGS
	void MountLog(const char* format, ...) noexcept
	{
		if(!should_debug_mount || !format)
			return;
		FILE* file = fopen("mount_debug.log", "ab");
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

	void ResetMountLog() noexcept
	{
		if(!should_debug_mount)
			return;
		FILE* file = fopen("mount_debug.log", "wb");
		if(!file)
			return;
		fprintf(file, "Tibia-DLL-CIP mount diagnostics\r\n");
		fclose(file);
	}
	#else
	#define MountLog(...) ((void)0)
	#define ResetMountLog() ((void)0)
	#endif

	DWORD CreatureId(DWORD creaturePtr)
	{
		if(!creaturePtr)
			return 0;
		__try {
			return *(DWORD*)creaturePtr;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}

	void StoreMount(DWORD creaturePtr, WORD mountClientId)
	{
		const DWORD creatureId = CreatureId(creaturePtr);
		if(!creatureId)
			return;
		const DWORD now = getTimerTick();
		int freeSlot = -1;
		int oldestSlot = 0;
		DWORD oldestTick = 0xFFFFFFFF;
		const unsigned int start = (creatureId * 2654435761u) & 511u;
		for(unsigned int probe = 0; probe < 512; ++probe)
		{
			const int i = static_cast<int>((start + probe) & 511u);
			if(g_states[i].creatureId == creatureId)
			{
				g_states[i].mountClientId = mountClientId;
				g_states[i].lastSeenTick = now;
				return;
			}
			if(g_states[i].creatureId == 0 && freeSlot < 0)
				freeSlot = i;
			if(g_states[i].lastSeenTick < oldestTick)
			{
				oldestTick = g_states[i].lastSeenTick;
				oldestSlot = i;
			}
		}
		const int slot = freeSlot >= 0 ? freeSlot : oldestSlot;
		g_states[slot].creatureId = creatureId;
		g_states[slot].mountClientId = mountClientId;
		g_states[slot].lastSeenTick = now;
	}

	WORD StoredMount(DWORD creaturePtr)
	{
		const DWORD creatureId = CreatureId(creaturePtr);
		if(!creatureId)
			return 0;
		const unsigned int start = (creatureId * 2654435761u) & 511u;
		for(unsigned int probe = 0; probe < 512; ++probe)
		{
			const int i = static_cast<int>((start + probe) & 511u);
			if(g_states[i].creatureId == 0)
				return 0;
			if(g_states[i].creatureId == creatureId)
			{
				g_states[i].lastSeenTick = getTimerTick();
				return g_states[i].mountClientId;
			}
		}
		return 0;
	}

	bool IsLocalPlayer(DWORD creaturePtr)
	{
		if(!creaturePtr || !g_playerIdAddr)
			return false;
		__try {
			return *(DWORD*)creaturePtr == *(DWORD*)g_playerIdAddr;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	void __stdcall ApplyMount(DWORD creaturePtr, DWORD mountClientId)
	{
		StoreMount(creaturePtr, (WORD)mountClientId);
		if(IsLocalPlayer(creaturePtr))
		{
			g_currentMount = (WORD)mountClientId;
			MountLog("local creature=%08lX mount=%u", creaturePtr,
				static_cast<unsigned int>(g_currentMount));
		}
	}

	DWORD LookTypePatternZ(DWORD lookType)
	{
		if(!lookType || !g_getThingTypeAddr || !g_thingTypeManagerPtrAddr)
			return 1;
		__try {
			const DWORD manager = *(DWORD*)g_thingTypeManagerPtrAddr;
			if(!manager)
				return 1;
			const DWORD thingType = ((GetThingTypeFn)g_getThingTypeAddr)(manager, lookType);
			if(!thingType)
				return 1;
			const DWORD patternZ = *(DWORD*)(thingType + 0x18);
			return patternZ >= 1 && patternZ <= 4 ? patternZ : 1;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			return 1;
		}
	}

	void LookTypeDisplacement(DWORD lookType, DWORD& x, DWORD& y)
	{
		x = 0;
		y = 0;
		if(!lookType || !g_hasThingPropertyAddr || !g_getDisplacementXAddr ||
			!g_getDisplacementYAddr)
			return;
		__try {
			// This is the exact 8.60 equivalent of the 8.70 mount path:
			// flag 0x18 guards the displacementX/displacementY lookups.
			if(!((HasThingPropertyFn)g_hasThingPropertyAddr)(lookType, 0x18))
				return;
			x = ((GetDisplacementFn)g_getDisplacementXAddr)(lookType);
			y = ((GetDisplacementFn)g_getDisplacementYAddr)(lookType);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			x = 0;
			y = 0;
		}
	}

	void __stdcall PrepareRider(DWORD creaturePtr)
	{
		EnhancedDat::BeginCreatureDraw(creaturePtr);
		g_patternZ = 1;
		g_mountedRider = 0;
		g_mountDisplacementX = 0;
		g_mountDisplacementY = 0;
		if(g_drawingMount || !creaturePtr)
			return;
		const WORD mountClientId = StoredMount(creaturePtr);
		if(!mountClientId)
			return;

		DWORD displacementX = 0;
		DWORD displacementY = 0;
		LookTypeDisplacement(mountClientId, displacementX, displacementY);
		g_mountDisplacementX = displacementX;
		g_mountDisplacementY = displacementY;
		g_mountedRider = 1;
		__try {
			if(LookTypePatternZ(*(DWORD*)(creaturePtr + 0x60)) >= 2)
				g_patternZ = 2;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			g_patternZ = 1;
		}
	}

	void __stdcall FinishRider()
	{
		EnhancedDat::EndCreatureDraw();
		g_patternZ = 1;
		g_mountedRider = 0;
		g_mountDisplacementX = 0;
		g_mountDisplacementY = 0;
	}

	void RestoreAppearance(DWORD creaturePtr, const DWORD* appearance) noexcept
	{
		if(!creaturePtr || !appearance)
			return;
		__try {
			for(int i = 0; i < 6; ++i)
				*(DWORD*)(creaturePtr + 0x60 + i * 4) = appearance[i];
		} __except(EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	void __stdcall DrawMountLayer(DWORD renderContext, DWORD x, DWORD y,
		DWORD surface, DWORD creaturePtr)
	{
		if(g_drawingMount || !g_drawCreatureAddr || !creaturePtr)
			return;
		const WORD mountClientId = StoredMount(creaturePtr);
		if(!mountClientId)
			return;

		DWORD appearance[6] = {};
		bool captured = false;
		__try {
			__try {
				for(int i = 0; i < 6; ++i)
					appearance[i] = *(DWORD*)(creaturePtr + 0x60 + i * 4);
				captured = true;
				g_drawingMount = true;
				*(DWORD*)(creaturePtr + 0x60) = mountClientId;
				for(int i = 1; i < 6; ++i)
					*(DWORD*)(creaturePtr + 0x60 + i * 4) = 0;
				// Keep the native base position. Moving this recursive draw call
				// outside its tile origin lets later map tiles clip the mount.
				((DrawCreatureFn)g_drawCreatureAddr)(renderContext, x, y,
					surface, creaturePtr);
			} __except(EXCEPTION_EXECUTE_HANDLER) {
			}
		} __finally {
			if(captured)
				RestoreAppearance(creaturePtr, appearance);
			g_drawingMount = false;
		}
	}

	BYTE ClientReadU8()
	{
		return ((ReadU8Fn)g_readU8Addr)();
	}

	WORD ClientReadU16()
	{
		return ((ReadU16Fn)g_readU16Addr)();
	}

	void ClientReadString(char* output, DWORD outputSize)
	{
		if(!output || outputSize == 0)
			return;
		output[0] = 0;
		((ReadStringFn)g_readStringAddr)(output, outputSize - 1);
		output[outputSize - 1] = 0;
	}

	DWORD __stdcall ReadOutfitWindowHeader(DWORD outfitStruct)
	{
		(void)outfitStruct;
		g_skippedOutfitCount = 0;
		__try {
			g_currentMount = ClientReadU16();
			g_selectedMount = g_currentMount;
			const DWORD outfitCount = should_use_outfit_count_u16 ?
				(DWORD)ClientReadU16() : (DWORD)ClientReadU8();
			MountLog("outfit window currentMount=%u outfits=%lu countBytes=%u",
				static_cast<unsigned int>(g_currentMount), outfitCount,
				should_use_outfit_count_u16 ? 2U : 1U);
			// TFS sends a uint16 count, but the native dialog has at most
			// 255 slots. Consume the remaining entries before the mount list.
			const DWORD limit = std::clamp(should_outfit_limit, 25U, 255U);
			const DWORD visibleCount = (std::min)(outfitCount, limit);
			g_skippedOutfitCount = outfitCount - visibleCount;
			return visibleCount;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			MountLog("outfit window header exception");
			return 0;
		}
	}

	void __stdcall SkipOutfitWindowMountList()
	{
		__try {
			char skippedName[256] = {};
			for(DWORD i = 0; i < g_skippedOutfitCount; ++i)
			{
				ClientReadU16(); // lookType
				ClientReadString(skippedName, sizeof(skippedName));
				ClientReadU8(); // addons
			}
			g_skippedOutfitCount = 0;
			const BYTE count = ClientReadU8();
			g_mountEntryCount = 0;
			g_mountSelection = -1;
			memset(g_mountEntries, 0, sizeof(g_mountEntries));
			for(DWORD i = 0; i < count; ++i)
			{
				MountEntry& entry = g_mountEntries[g_mountEntryCount++];
				entry.clientId = ClientReadU16();
				ClientReadString(entry.name, sizeof(entry.name));
				if(entry.clientId == g_currentMount)
					g_mountSelection = (int)i;
			}
			// The 8.70 client always shows a valid mount preview. If the
			// player is currently dismounted (or the active mount is absent
			// from the list), it selects the first available mount locally.
			if(g_mountSelection < 0 && g_mountEntryCount > 0)
				g_mountSelection = 0;
			g_selectedMount = g_mountSelection >= 0 ?
				g_mountEntries[g_mountSelection].clientId : 0;
			MountLog("outfit window mounts stored=%u current=%u selected=%d",
				static_cast<unsigned int>(count),
				static_cast<unsigned int>(g_currentMount), g_mountSelection);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			MountLog("outfit window mount list exception");
			g_skippedOutfitCount = 0;
			g_mountEntryCount = 0;
			g_mountSelection = -1;
		}
	}

	void __stdcall ToggleMountFromContext()
	{
		// Keep mouse and keyboard behavior on the same implementation. The
		// confirmed state is updated later by ApplyMount when the server sends
		// the creature outfit containing the resulting mountClientId.
		MountSystem::ToggleLocalMount();
	}

	void ReleaseUnowned(void* memory)
	{
		if(memory && g_clientFreeAddr)
			((ClientFreeFn)g_clientFreeAddr)(memory);
	}

	DWORD CreateNativeArrowWithCaption(DWORD parent, DWORD eventId, bool left,
		DWORD x, DWORD y, const char* caption)
	{
		void* allocation = ((ClientAllocFn)g_clientAllocAddr)(0x48);
		if(!allocation)
			return 0;

		DWORD callback[2] = {};
		((BuildCallbackFn)g_buildCallbackAddr)((DWORD)callback, parent, eventId);
		// Use the arrow pairs present in the original 8.60 Tibia.pic.
		const DWORD normalSkin = left ? 0x46 : 0x48;
		const DWORD pressedSkin = left ? 0x47 : 0x49;
		DWORD button = 0;
		__try {
			button = ((ButtonCtorFn)g_buttonCtorAddr)((DWORD)allocation,
				0, 0, normalSkin, pressedSkin, callback[0], callback[1],
				(DWORD)caption, 0);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			button = 0;
		}
		if(!button)
		{
			ReleaseUnowned(allocation);
			return 0;
		}

		((AddChildFn)g_addChildAddr)(parent, button);
		// Match the native 8.60/8.70 arrow flow. The button constructor has
		// already taken the exact clickable size from its Tibia.pic skin.
		((PlaceChildFn)g_placeChildAddr)(parent, button, x, y);
		return button;
	}

	DWORD CreateNativeArrow(DWORD parent, DWORD eventId, bool left,
		DWORD x, DWORD y)
	{
		if(left)
		{
			auto caption = TIBIA_OBF("Previous mount");
			return CreateNativeArrowWithCaption(parent, eventId, true, x, y,
				caption.c_str());
		}
		auto caption = TIBIA_OBF("Next mount");
		return CreateNativeArrowWithCaption(parent, eventId, false, x, y,
			caption.c_str());
	}

	DWORD CreateNativeMountPreview(DWORD parent, WORD mountClientId)
	{
		// Look type zero means "no mount" in the protocol, but it is not a
		// valid appearance index for the native preview constructor.
		if(!parent || mountClientId == 0)
			return 0;

		void* previewAllocation = ((ClientAllocFn)g_clientAllocAddr)(0x4C);
		if(!previewAllocation)
			return 0;

		DWORD preview = 0;
		__try {
			preview = ((PreviewCtorFn)g_previewCtorAddr)((DWORD)previewAllocation,
				mountClientId, 2, (DWORD)g_previewColors, 0,
				g_previewLayoutAddr);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			preview = 0;
		}
		if(!preview)
		{
			ReleaseUnowned(previewAllocation);
			return 0;
		}

		void* panelAllocation = ((ClientAllocFn)g_clientAllocAddr)(0x68);
		if(!panelAllocation)
		{
			ReleaseUnowned(previewAllocation);
			return 0;
		}

		DWORD panel = 0;
		__try {
			const DWORD layoutValue = *(DWORD*)g_panelLayoutValueAddr;
			// Preserve the exact 8.60 preview-panel contract. The 8.70
			// constructor uses different layout IDs that do not exist in an
			// arbitrary 8.60 Tibia.pic.
			panel = ((PanelCtorFn)g_panelCtorAddr)((DWORD)panelAllocation,
				preview, 0, 9, 0, 0x0B, 0x0A, 0, 8, 0, layoutValue);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			panel = 0;
		}
		if(!panel)
		{
			ReleaseUnowned(panelAllocation);
			ReleaseUnowned(previewAllocation);
			return 0;
		}

		((AddChildFn)g_addChildAddr)(parent, panel);
		const NativeRect mountRectangle = { 318, 102, 140, 140 };
		((PlaceChildRectFn)g_placeChildRectAddr)(parent, panel,
			mountRectangle);
		// Keep the 140x140 panel and the sprite scale untouched. Move only
		// the preview child inside its panel for visual centering.
		__try {
			const DWORD previewX = *(DWORD*)(preview + 0x14);
			const DWORD previewY = *(DWORD*)(preview + 0x18);
			((PlaceChildFn)g_placeChildAddr)(panel, preview,
				previewX + g_mountPreviewXOffset,
				previewY + g_mountPreviewYOffset);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
		}
		return preview;
	}

	DWORD CreateNativeMountLabel(DWORD parent, const char* text)
	{
		void* labelAllocation = ((ClientAllocFn)g_clientAllocAddr)(0x4C);
		if(!labelAllocation)
			return 0;

		DWORD label = 0;
		__try {
			NativeTextStyle style = {};
			memcpy(style.values, (const void*)g_nameTextStyleAddr,
				sizeof(style.values));
			label = ((LabelCtorFn)g_labelCtorAddr)((DWORD)labelAllocation,
				style, text);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			label = 0;
		}
		if(!label)
		{
			ReleaseUnowned(labelAllocation);
			return 0;
		}

		void* panelAllocation = ((ClientAllocFn)g_clientAllocAddr)(0x68);
		if(!panelAllocation)
		{
			ReleaseUnowned(labelAllocation);
			return 0;
		}

		DWORD panel = 0;
		__try {
			const DWORD layoutValue = *(DWORD*)g_panelLayoutValueAddr;
			panel = ((PanelCtorFn)g_panelCtorAddr)((DWORD)panelAllocation,
				label, 0, 9, 0, 0x0B, 0x0A, 0, 8, 0, layoutValue);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			panel = 0;
		}
		if(!panel)
		{
			ReleaseUnowned(panelAllocation);
			ReleaseUnowned(labelAllocation);
			return 0;
		}

		((AddChildFn)g_addChildAddr)(parent, panel);
		const NativeRect labelRectangle = { 342, 247, 92, 20 };
		((PlaceChildRectFn)g_placeChildRectAddr)(parent, panel,
			labelRectangle);
		return label;
	}

	void SetNativeLabelText(DWORD label, const char* text)
	{
		if(!label || !text)
			return;
		__try {
			const DWORD virtualTable = *(DWORD*)label;
			const DWORD setter = *(DWORD*)(virtualTable + 0x84);
			((SetLabelTextFn)setter)(label, text, 0);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	DWORD CreateNativeMountLabelForSelection(DWORD parent)
	{
		if(g_mountSelection >= 0)
			return CreateNativeMountLabel(parent,
				g_mountEntries[g_mountSelection].name);
		auto caption = TIBIA_OBF("No Mount");
		return CreateNativeMountLabel(parent, caption.c_str());
	}

	void UpdateNativeMountLabelForSelection()
	{
		if(g_mountSelection >= 0)
		{
			SetNativeLabelText(g_nativeMountLabel,
				g_mountEntries[g_mountSelection].name);
			return;
		}
		auto caption = TIBIA_OBF("No Mount");
		SetNativeLabelText(g_nativeMountLabel, caption.c_str());
	}

	void __stdcall OnNativeOutfitWindowCreated(DWORD outfitObject,
		DWORD dialogObject)
	{
		g_nativeOutfitObject = 0;
		g_nativeMountPreview = 0;
		g_nativeMountLabel = 0;
		if(!outfitObject || !g_protocolEnabled)
			return;
		// A regular player may own no mounts. Keep the stock 8.60 outfit
		// window in that case; constructing a preview with look type zero
		// triggers Container.h's appearance-index assertion.
		if(g_mountEntryCount == 0 || g_selectedMount == 0)
			return;
		__try {
			const DWORD previousContentHeight = *(DWORD*)(outfitObject + 0x20);
			const DWORD dialogWidth = dialogObject ?
				*(DWORD*)(dialogObject + 0x1C) : 0;
			const DWORD dialogHeight = dialogObject ?
				*(DWORD*)(dialogObject + 0x20) : 0;
			// Exact logical size used by the native 8.70 outfit content. The
			// 8.60 content is 461x251; mounts add 16 pixels vertically.
			((SetSizeFn)g_setSizeAddr)(outfitObject, 461, 267);
			// The outer dialog was constructed before this hook, while the
			// content was still 251 pixels high. Grow its existing bounds by
			// the same delta; otherwise hit-testing clips the mount arrows at
			// the old bottom edge even though their skins are still rendered.
			if(dialogObject && previousContentHeight < 267)
			{
				((SetSizeFn)g_setSizeAddr)(dialogObject, dialogWidth,
					dialogHeight + (267 - previousContentHeight));
			}
			g_nativeMountPreview = CreateNativeMountPreview(outfitObject,
				g_selectedMount);
			if(!g_nativeMountPreview)
				return;
			g_nativeMountLabel = CreateNativeMountLabelForSelection(
				outfitObject);
			// These are the native mount event IDs used by Tibia 8.70.
			CreateNativeArrow(outfitObject, 3, true, 318, 247);
			CreateNativeArrow(outfitObject, 4, false, 438, 247);
			g_nativeOutfitObject = outfitObject;
			MountLog("native mount panel object=%08lX dialog=%08lX preview=%08lX label=%08lX",
				outfitObject, dialogObject, g_nativeMountPreview,
				g_nativeMountLabel);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			g_nativeOutfitObject = 0;
			g_nativeMountPreview = 0;
			g_nativeMountLabel = 0;
			MountLog("native mount panel exception");
		}
	}

	void __stdcall ChangeNativeMount(DWORD outfitObject, int delta)
	{
		if(outfitObject != g_nativeOutfitObject || !g_nativeMountPreview)
			return;
		const int optionCount = (int)g_mountEntryCount + 1;
		if(optionCount <= 0)
			return;
		int option = g_mountSelection + 1;
		option = (option + delta + optionCount) % optionCount;
		g_mountSelection = option - 1;
		g_selectedMount = g_mountSelection >= 0 ?
			g_mountEntries[g_mountSelection].clientId : 0;
		__try {
			// Selecting "No Mount" must only affect the value sent on Ok.
			// Passing zero to the native appearance preview is invalid, so keep
			// the last valid sprite visible until another mount is selected.
			if(g_selectedMount != 0)
				((SetPreviewLookTypeFn)g_setPreviewLookTypeAddr)(
					g_nativeMountPreview, g_selectedMount);
			UpdateNativeMountLabelForSelection();
			MountLog("native mount selected index=%d clientId=%u",
				g_mountSelection, static_cast<unsigned int>(g_selectedMount));
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			MountLog("native mount selection exception");
		}
	}

	WORD __stdcall ActiveMount()
	{
		return g_selectedMount;
	}

	void __stdcall AddMountContextMenuEntry(DWORD context)
	{
		if(!context || !g_contextMenuAddEntryAddr)
			return;
		if(g_currentMount != 0)
		{
			auto caption = TIBIA_OBF("Dismount (Ctrl+M)");
			((ContextMenuAddEntryFn)g_contextMenuAddEntryAddr)(context,
				0x2719, caption.c_str(), 0);
			return;
		}
		auto caption = TIBIA_OBF("Mount (Ctrl+M)");
		((ContextMenuAddEntryFn)g_contextMenuAddEntryAddr)(context,
			0x2719, caption.c_str(), 0);
	}

	__declspec(naked) void HookKnownCreature()
	{
		__asm {
			call dword ptr [g_readU16Addr]
			movzx eax, ax
			push eax
			push esi
			call ApplyMount
			call dword ptr [g_readU8Addr]
			jmp dword ptr [g_knownCreatureReturn]
		}
	}

	__declspec(naked) void HookNewCreature()
	{
		__asm {
			call dword ptr [g_readU16Addr]
			movzx eax, ax
			push eax
			push esi
			call ApplyMount
			call dword ptr [g_readU8Addr]
			jmp dword ptr [g_newCreatureReturn]
		}
	}

	__declspec(naked) void HookCreatureOutfit()
	{
		__asm {
			call dword ptr [g_readU16Addr]
			movzx eax, ax
			push eax
			push esi
			call ApplyMount
			mov dword ptr [ebp-4], 0FFFFFFFFh
			jmp dword ptr [g_creatureOutfitReturn]
		}
	}

	__declspec(naked) void HookOutfitWindowHeader()
	{
		__asm {
			push esi
			call ReadOutfitWindowHeader
			mov edi, eax
			mov dword ptr [ebp-24h], edi
			jmp dword ptr [g_outfitWindowReturn]
		}
	}

	__declspec(naked) void HookOutfitWindowMountList()
	{
		__asm {
			pushad
			call SkipOutfitWindowMountList
			popad
			mov eax, dword ptr [ebp-14h]
			push eax
			push ebx
			jmp dword ptr [g_outfitWindowListReturn]
		}
	}

	__declspec(naked) void HookSetOutfitSend()
	{
		__asm {
			pushad
			call ActiveMount
			movzx eax, ax
			push eax
			call dword ptr [g_writeU16Addr]
			add esp, 4
			popad
			push 1
			call dword ptr [g_sendPacketAddr]
			// The native continuation at 0x40A82B executes `add esp, 20h`
			// and releases every argument accumulated by the D3 sender,
			// including this final `push 1`. Do not release it twice here.
			jmp dword ptr [g_setOutfitSendReturn]
		}
	}

	__declspec(naked) void HookInsertMountContextMenu()
	{
		__asm {
			push esi
			call AddMountContextMenuEntry
			call dword ptr [g_contextMenuNativeContinuationAddr]
			jmp dword ptr [g_contextMenuReturn]
		}
	}

	__declspec(naked) void HookPlaceOutfitAddon()
	{
		__asm {
			cmp dword ptr [g_mountEntryCount], 0
			je nativePlacement
			// NativeRect is passed by value after the child pointer.
			// Convert 8.60 (0, 177+n*26, 140, 22) to the 8.70 layout
			// (150, 102+n*26, 158, 22).
			mov dword ptr [esp+8], 150
			sub dword ptr [esp+0Ch], 75
			mov dword ptr [esp+10h], 158
			mov dword ptr [esp+14h], 22
		nativePlacement:
			jmp dword ptr [g_placeChildRectAddr]
		}
	}

	__declspec(naked) void HookOutfitDescriptionText()
	{
		__asm {
			cmp dword ptr [g_mountEntryCount], 0
			je nativeText
			// Label constructor receives a 20-byte style followed by text.
			mov dword ptr [esp+18h], offset g_outfitDescription
		nativeText:
			jmp dword ptr [g_labelCtorAddr]
		}
	}

	__declspec(naked) void HookPlaceOutfitDescription()
	{
		__asm {
			cmp dword ptr [g_mountEntryCount], 0
			je nativePlacement
			// Move the long description below the outfit preview, matching
			// the 8.70 coordinates (0, 186).
			mov dword ptr [esp+8], 0
			mov dword ptr [esp+0Ch], 186
		nativePlacement:
			jmp dword ptr [g_placeChildAddr]
		}
	}

	__declspec(naked) void HookNativeOutfitWindowCreated()
	{
		__asm {
			pushad
			push esi
			push edi
			call OnNativeOutfitWindowCreated
			popad
			call dword ptr [g_outfitWindowFinalizeAddr]
			jmp dword ptr [g_outfitWindowCreatedReturn]
		}
	}

	__declspec(naked) void HookContextCommand()
	{
		__asm {
			cmp dword ptr [esp+4], 2719h
			jne nativeCommand
			pushad
			call ToggleMountFromContext
			popad
			xor eax, eax
			ret 4
		nativeCommand:
			push ebp
			mov ebp, esp
			push 0FFFFFFFFh
			jmp dword ptr [g_contextCommandReturn]
		}
	}

	__declspec(naked) void HookNativeOutfitEvent()
	{
		__asm {
			cmp edi, 3
			je previousMount
			cmp edi, 4
			je nextMount
			cmp edi, 5
			je previousOutfit
			jmp dword ptr [g_outfitEventContinue]
		previousMount:
			push -1
			push esi
			call ChangeNativeMount
			jmp dword ptr [g_outfitEventDone]
		nextMount:
			push 1
			push esi
			call ChangeNativeMount
			jmp dword ptr [g_outfitEventDone]
		previousOutfit:
			jmp dword ptr [g_outfitEventPrevious]
		}
	}

	__declspec(naked) void HookDrawMountAndBeginRider()
	{
		__asm {
			mov dword ptr [ebp-4], 0Fh
			pushad
			push esi
			push dword ptr [ebp+10h]
			push dword ptr [ebp+0Ch]
			push dword ptr [ebp+8]
			push dword ptr [ebp-38h]
			call DrawMountLayer
			popad
			pushad
			push esi
			call PrepareRider
			popad
			jmp dword ptr [g_drawBeforeOutfitReturn]
		}
	}

	__declspec(naked) void HookPatternZ()
	{
		__asm {
			push dword ptr [g_patternZ]
			mov ecx, dword ptr [ebp+1Ch]
			jmp dword ptr [g_spritePatternZReturn]
		}
	}

	__declspec(naked) void HookDrawCreatureExit()
	{
		__asm {
			call FinishRider
			mov dword ptr [ebp-4], 0FFFFFFFFh
			jmp dword ptr [g_drawCreatureExitReturn]
		}
	}

	__declspec(naked) void HookRiderYOffset()
	{
		__asm {
			mov edx, dword ptr [ebp+0Ch]
			sub edx, ecx
			cmp dword ptr [g_mountedRider], 0
			je nativeDisplacement
			// Tibia 8.70 deliberately reuses the mount's DAT displacement
			// for every rider layer in the mounted composition.
			sub edx, dword ptr [g_mountDisplacementY]
			jmp positionReady
		nativeDisplacement:
			sub edx, dword ptr [ebp-1Ch]
		positionReady:
			add edx, 20h
			push edx
			mov eax, edi
			jmp dword ptr [g_drawOutfitYOffsetReturn]
		}
	}

	__declspec(naked) void HookRiderXOffset()
	{
		__asm {
			mov ecx, dword ptr [ebp+8]
			sub ecx, eax
			cmp dword ptr [g_mountedRider], 0
			je nativeDisplacement
			sub ecx, dword ptr [g_mountDisplacementX]
			jmp positionReady
		nativeDisplacement:
			sub ecx, dword ptr [ebp-24h]
		positionReady:
			add ecx, 20h
			push ecx
			jmp dword ptr [g_drawOutfitXOffsetReturn]
		}
	}
}

namespace MountSystem
{
	bool Install(DWORD clientBase, bool protocolEnabled)
	{
		if(!clientBase)
			return false;

		g_protocolEnabled = protocolEnabled;
		g_playerIdAddr = clientBase + 0x23FE98;
		g_readU8Addr = clientBase + 0xF9A60;
		g_readU16Addr = clientBase + 0xF9C00;
		g_readStringAddr = clientBase + 0xF9F40;
		g_writePacketStartAddr = clientBase + 0xF8290;
		g_writeByteAddr = clientBase + 0xF8560;
		g_writeU16Addr = clientBase + 0xF8700;
		g_sendPacketAddr = clientBase + 0xF8E40;
		g_drawCreatureAddr = clientBase + 0xF0F30;
		g_getThingTypeAddr = clientBase + 0x100CE0;
		g_thingTypeManagerPtrAddr = clientBase + 0x3998D8;
		g_hasThingPropertyAddr = clientBase + 0x102E30;
		g_getDisplacementXAddr = clientBase + 0x103340;
		g_getDisplacementYAddr = clientBase + 0x1034F0;
		g_knownCreatureReturn = clientBase + 0xE0F3;
		g_newCreatureReturn = clientBase + 0xE3BB;
		g_creatureOutfitReturn = clientBase + 0x110B9;
		g_outfitWindowReturn = clientBase + 0x13E08;
		g_outfitWindowListReturn = clientBase + 0x13F04;
		g_setOutfitSendReturn = clientBase + 0xA82B;
		g_drawBeforeOutfitReturn = clientBase + 0xF126D;
		g_drawCreatureExitReturn = clientBase + 0xF14A7;
		g_spritePatternZReturn = clientBase + 0x104474;
		g_drawOutfitYOffsetReturn = clientBase + 0xF146F;
		g_drawOutfitXOffsetReturn = clientBase + 0xF1480;
		g_contextMenuAddEntryAddr = clientBase + 0x52320;
		g_contextMenuNativeContinuationAddr = clientBase + 0x5FC80;
		g_contextMenuReturn = clientBase + 0x5328D;
		g_outfitWindowFinalizeAddr = clientBase + 0x82E80;
		g_outfitWindowCreatedReturn = clientBase + 0xA0A85;
		g_outfitEventContinue = clientBase + 0x880EA;
		g_outfitEventPrevious = clientBase + 0x8813A;
		g_outfitEventDone = clientBase + 0x8822A;
		g_contextCommandReturn = clientBase + 0x4E965;
		g_clientAllocAddr = clientBase + 0x1719E2;
		g_clientFreeAddr = clientBase + 0x171A0D;
		g_previewCtorAddr = clientBase + 0x7C130;
		g_panelCtorAddr = clientBase + 0x67CD0;
		g_buildCallbackAddr = clientBase + 0xB76C0;
		g_buttonCtorAddr = clientBase + 0x7C310;
		g_addChildAddr = clientBase + 0xC0C60;
		g_setSizeAddr = clientBase + 0xC09E0;
		g_placeChildAddr = clientBase + 0xC45F0;
		g_placeChildRectAddr = clientBase + 0xC4110;
		g_setPreviewLookTypeAddr = clientBase + 0x64EA0;
		g_labelCtorAddr = clientBase + 0x7B420;
		g_previewLayoutAddr = clientBase + 0x24B708;
		g_panelLayoutValueAddr = clientBase + 0x24B704;
		g_nameTextStyleAddr = clientBase + 0x24B774;

		memset(g_states, 0, sizeof(g_states));
		memset(g_mountEntries, 0, sizeof(g_mountEntries));
		g_currentMount = 0;
		g_selectedMount = 0;
		g_mountEntryCount = 0;
		g_mountSelection = -1;
		g_nativeOutfitObject = 0;
		g_skippedOutfitCount = 0;
		g_nativeMountPreview = 0;
		g_nativeMountLabel = 0;
		g_mountedRider = 0;
		g_mountDisplacementX = 0;
		g_mountDisplacementY = 0;
		ResetMountLog();
		bool success = true;
		auto apply = [&success](bool applied) noexcept {
			success = applied && success;
		};
		if(protocolEnabled)
		{
			apply(HookJMP(clientBase + 0xE0EE, (DWORD)&HookKnownCreature));
			apply(HookJMP(clientBase + 0xE3B6, (DWORD)&HookNewCreature));
			apply(HookJMP(clientBase + 0x110B2, (DWORD)&HookCreatureOutfit));
			apply(Nop(clientBase + 0x110B7, 2));
			apply(HookJMP(clientBase + 0x13E00,
				(DWORD)&HookOutfitWindowHeader));
			apply(HookJMP(clientBase + 0x13EFF,
				(DWORD)&HookOutfitWindowMountList));
			apply(HookJMP(clientBase + 0xA824, (DWORD)&HookSetOutfitSend));
			apply(Nop(clientBase + 0xA829, 2));
			apply(HookJMP(clientBase + 0x53288,
				(DWORD)&HookInsertMountContextMenu));
			apply(HookJMP(clientBase + 0x4E960, (DWORD)&HookContextCommand));
			apply(HookCall(clientBase + 0x991F6, (DWORD)&HookPlaceOutfitAddon));
			apply(HookCall(clientBase + 0x9938C,
				(DWORD)&HookOutfitDescriptionText));
			apply(HookCall(clientBase + 0x993BF,
				(DWORD)&HookPlaceOutfitDescription));
			apply(HookJMP(clientBase + 0xA0A80,
				(DWORD)&HookNativeOutfitWindowCreated));
			apply(HookJMP(clientBase + 0x880E5,
				(DWORD)&HookNativeOutfitEvent));
		}

		if(protocolEnabled || should_use_frame_groups || should_use_improved_animations)
		{
			apply(HookJMP(clientBase + 0xF1266,
				(DWORD)&HookDrawMountAndBeginRider));
			apply(Nop(clientBase + 0xF126B, 2));
			apply(HookJMP(clientBase + 0x10446F, (DWORD)&HookPatternZ));
			apply(HookJMP(clientBase + 0xF14A0,
				(DWORD)&HookDrawCreatureExit));
			apply(Nop(clientBase + 0xF14A5, 2));
			apply(HookJMP(clientBase + 0xF1463, (DWORD)&HookRiderYOffset));
			apply(HookJMP(clientBase + 0xF1474, (DWORD)&HookRiderXOffset));
			apply(Nop(clientBase + 0xF1479, 7));
		}
		MountLog("installed protocol=%d outfitCountBytes=%u",
			protocolEnabled ? 1 : 0, should_use_outfit_count_u16 ? 2U : 1U);
		return success;
	}

	void Shutdown()
	{
		memset(g_states, 0, sizeof(g_states));
		memset(g_mountEntries, 0, sizeof(g_mountEntries));
		g_currentMount = 0;
		g_selectedMount = 0;
		g_mountEntryCount = 0;
		g_mountSelection = -1;
		g_nativeOutfitObject = 0;
		g_skippedOutfitCount = 0;
		g_nativeMountPreview = 0;
		g_nativeMountLabel = 0;
		g_protocolEnabled = false;
		g_drawingMount = false;
		g_patternZ = 1;
		g_mountedRider = 0;
		g_mountDisplacementX = 0;
		g_mountDisplacementY = 0;
	}

	bool ToggleLocalMount()
	{
		if(!g_protocolEnabled || !g_writePacketStartAddr || !g_writeByteAddr ||
			!g_sendPacketAddr)
			return false;
		const bool enable = g_currentMount == 0;
		__try {
			((WriteValueFn)g_writePacketStartAddr)(0xD4);
			((WriteValueFn)g_writeByteAddr)(enable ? 1 : 0);
			((WriteValueFn)g_sendPacketAddr)(1);
			MountLog("toggle requested enable=%d", enable ? 1 : 0);
			return true;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			MountLog("toggle exception");
			return false;
		}
	}
}
