#ifndef TIBIA_MOUNT_SYSTEM_H
#define TIBIA_MOUNT_SYSTEM_H

#include "main.h"

namespace MountSystem
{
	bool Install(DWORD clientBase, bool protocolEnabled);
	void Shutdown();
	bool ToggleLocalMount();
}

#endif
