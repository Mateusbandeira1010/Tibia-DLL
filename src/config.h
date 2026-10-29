#ifndef TIBIA_CONFIG_H
#define TIBIA_CONFIG_H

#ifdef __CONFIG__
// Bootstrap host used for the initial login connection. Public builds encrypt
// this literal in the binary. Override TIBIA_BOOTSTRAP_DOMAIN in the project
// settings for production without changing the source file.
#ifndef TIBIA_BOOTSTRAP_DOMAIN
#define TIBIA_BOOTSTRAP_DOMAIN "127.0.0.1"
#endif

// Fixed production profile. Keeping the packet layout inside the binary avoids
// an external file changing protocol widths after the DLL was built.
inline constexpr bool should_use_hirestimer = true;
inline constexpr bool should_use_extended = true;
inline constexpr bool should_use_alpha = false;
inline constexpr bool should_draw_manabar = true;
inline constexpr bool should_use_frame_groups = true;
inline constexpr bool should_use_improved_animations = true;
inline constexpr bool dat_has_frame_groups = true;
inline constexpr bool dat_has_improved_animations = true;
inline constexpr bool should_use_mount_system = true;
inline constexpr bool should_use_magic_effects_u16 = true;
inline constexpr bool should_use_distance_shoots_u16 = true;
inline constexpr bool should_use_player_health_u32 = true;
inline constexpr bool should_use_player_mana_u32 = true;
inline constexpr bool should_use_outfit_count_u16 = true;
// This server sends the native 8.60 ping (0x1E without a payload).
inline constexpr bool should_use_custom_ping = false;
inline constexpr bool should_consume_impact_tracker = true;
inline constexpr bool should_use_custom_client_os = false;
// Enable only with a server implementing the authenticated weather protocol.
inline constexpr bool should_use_weather = false;
inline constexpr bool should_debug_mount = false;
inline constexpr bool should_debug_frame_groups = false;
inline constexpr bool should_debug_store = false;
inline constexpr bool should_use_store = false;
inline constexpr bool should_use_smart_walking = true;
inline constexpr bool should_use_dynamic_endpoint = true;
// Allow multiple 8.60 client processes by applying the known MC branch patch
// to each process image during the DLL's early startup.
inline constexpr bool should_use_multiclient = true;
inline constexpr unsigned int should_outfit_limit = 255;
#endif

#endif
