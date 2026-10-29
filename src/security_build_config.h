#ifndef TIBIA_SECURITY_BUILD_CONFIG_H
#define TIBIA_SECURITY_BUILD_CONFIG_H

// Build policy is centralized here. Individual modules must not invent their
// own public-release or diagnostics switches.
#ifndef TIBIA_PUBLIC_RELEASE
#define TIBIA_PUBLIC_RELEASE 0
#endif

#ifndef TIBIA_ENABLE_DEV_LOGS
#if defined(_DEBUG) && !TIBIA_PUBLIC_RELEASE
#define TIBIA_ENABLE_DEV_LOGS 1
#else
#define TIBIA_ENABLE_DEV_LOGS 0
#endif
#endif

#ifndef TIBIA_ENABLE_DIAGNOSTIC_STRINGS
#if TIBIA_PUBLIC_RELEASE
#define TIBIA_ENABLE_DIAGNOSTIC_STRINGS 0
#else
#define TIBIA_ENABLE_DIAGNOSTIC_STRINGS 1
#endif
#endif

#if TIBIA_ENABLE_DIAGNOSTIC_STRINGS
#define TIBIA_DIAGNOSTIC_TEXT(text) (text)
#else
#define TIBIA_DIAGNOSTIC_TEXT(text) ""
#endif

#ifndef TIBIA_ENABLE_INTEGRITY_CHECK
#define TIBIA_ENABLE_INTEGRITY_CHECK TIBIA_PUBLIC_RELEASE
#endif

#ifndef TIBIA_ENABLE_PROTECT_MARKERS
#define TIBIA_ENABLE_PROTECT_MARKERS 0
#endif

#ifndef TIBIA_ENABLE_STRING_OBFUSCATION
#define TIBIA_ENABLE_STRING_OBFUSCATION TIBIA_PUBLIC_RELEASE
#endif

#if TIBIA_PUBLIC_RELEASE && TIBIA_ENABLE_DEV_LOGS
#error ReleasePublic cannot contain development logs
#endif

#if TIBIA_ENABLE_PROTECT_MARKERS
#include "VMProtectSDK.h"
#define TIBIA_PROTECT_BEGIN(name) VMProtectBeginUltra(name)
#define TIBIA_PROTECT_MUTATION(name) VMProtectBeginMutation(name)
#define TIBIA_PROTECT_END() VMProtectEnd()
#else
#define TIBIA_PROTECT_BEGIN(name) ((void)0)
#define TIBIA_PROTECT_MUTATION(name) ((void)0)
#define TIBIA_PROTECT_END() ((void)0)
#endif

#endif
