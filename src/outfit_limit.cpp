#include "outfit_limit.h"
#include "hook.h"

namespace OutfitLimit
{
	bool Install(DWORD clientBase, unsigned int limit)
	{
		if(!clientBase)
			return false;
		if(limit < 25)
			limit = 25;
		if(limit > 255)
			limit = 255;

		const DWORD addonsOffset = limit * 4;
		const DWORD namesOffset = limit * 8;

		bool success = true;
		auto apply = [&success](bool applied) noexcept {
			success = applied && success;
		};

		apply(OverWrite(clientBase + 0xA09FC, 0x28E + (limit * 38)));
		apply(OverWrite(clientBase + 0x13D9C, 0x1C + (limit * 8)));
		apply(OverWrite(clientBase + 0x13DB0, limit * 30));

		apply(OverWriteByte(clientBase + 0x98B83, 0xB8));
		apply(OverWrite(clientBase + 0x98B84, 1));
		apply(Nop(clientBase + 0x98B88, 8));

		apply(OverWriteByte(clientBase + 0x13E0B, 0xB8));
		apply(OverWrite(clientBase + 0x13E0C, 1));
		apply(Nop(clientBase + 0x13E10, 5));
		apply(OverWriteByte(clientBase + 0x13E17, 0x08));

		apply(OverWrite(clientBase + 0x880BB, 0x288 + addonsOffset));
		apply(OverWrite(clientBase + 0x8818D, 0x288 + addonsOffset));
		apply(OverWrite(clientBase + 0x881B8, 0x288 + addonsOffset));
		apply(OverWrite(clientBase + 0x98C68, 0x288 + addonsOffset));
		apply(OverWrite(clientBase + 0x98C7F, 0x288 + addonsOffset));
		apply(OverWrite(clientBase + 0x98C9A, 0x288 + addonsOffset));
		apply(OverWrite(clientBase + 0x9920D, 0x288 + addonsOffset));

		apply(OverWrite(clientBase + 0x8821F, 0x288 + namesOffset));
		apply(OverWrite(clientBase + 0x98CB1, 0x288 + namesOffset));
		apply(OverWrite(clientBase + 0x99274, 0x288 + namesOffset));
		return success;
	}
}
