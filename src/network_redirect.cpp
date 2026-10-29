#include "network_redirect.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include "config.h"
#include "hook.h"
#include "obfuscated_string.h"
#include "security_build_config.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{
	constexpr std::uint16_t LOCAL_LOGIN_PORT = 7171;
	constexpr std::uint16_t LOCAL_GAME_PORT = 7172;
	constexpr std::size_t MAX_CHARACTER_COUNT = 255;
	constexpr DWORD CHARACTER_LIST_CALL_OFFSET = 0x5C48E;
	constexpr DWORD CHARACTER_LIST_PARSER_OFFSET = 0x0CE70;
	constexpr DWORD NETWORK_MESSAGE_OFFSET = 0x3998AC;

	using ConnectFn = int (WSAAPI*)(SOCKET, const sockaddr*, int);
	using WSAConnectFn = int (WSAAPI*)(SOCKET, const sockaddr*, int,
		LPWSABUF, LPWSABUF, LPQOS, LPQOS);
	using CharacterListParserFn = void (__cdecl*)();

	SRWLOCK g_endpointLock = SRWLOCK_INIT;
	std::uint32_t g_gameAddress = 0;
	std::uint16_t g_gamePort = 0;
	bool g_gameEndpointValid = false;

	ConnectFn g_originalConnect = nullptr;
	WSAConnectFn g_originalWSAConnect = nullptr;
	CharacterListParserFn g_characterListParser = nullptr;
	DWORD g_networkMessageAddress = 0;

#if TIBIA_ENABLE_DEV_LOGS
	void DebugLog(const char* text) noexcept
	{
		OutputDebugStringA(text);
	}
#else
	void DebugLog(const char*) noexcept {}
#endif

	bool AddWouldOverflow(std::size_t left, std::size_t right) noexcept
	{
		return left > static_cast<std::size_t>(-1) - right;
	}

	bool CanRead(std::size_t position, std::size_t count,
		std::size_t size) noexcept
	{
		return !AddWouldOverflow(position, count) &&
			position + count <= size;
	}

	bool ReadU16(const unsigned char* data, std::size_t size,
		std::size_t& position, std::uint16_t& value) noexcept
	{
		if(!CanRead(position, sizeof(value), size))
			return false;
		value = static_cast<std::uint16_t>(data[position]) |
			(static_cast<std::uint16_t>(data[position + 1]) << 8);
		position += sizeof(value);
		return true;
	}

	bool SkipString(const unsigned char* data, std::size_t size,
		std::size_t& position) noexcept
	{
		std::uint16_t length = 0;
		if(!ReadU16(data, size, position, length) ||
			!CanRead(position, length, size))
		{
			return false;
		}
		position += length;
		return true;
	}

	void ClearGameEndpoint() noexcept
	{
		AcquireSRWLockExclusive(&g_endpointLock);
		g_gameAddress = 0;
		g_gamePort = 0;
		g_gameEndpointValid = false;
		ReleaseSRWLockExclusive(&g_endpointLock);
	}

	void StoreGameEndpoint(std::uint32_t address,
		std::uint16_t port) noexcept
	{
		AcquireSRWLockExclusive(&g_endpointLock);
		g_gameAddress = address;
		g_gamePort = port;
		g_gameEndpointValid = address != 0 && port != 0;
		ReleaseSRWLockExclusive(&g_endpointLock);
	}

	bool LoadGameEndpoint(std::uint32_t& address,
		std::uint16_t& port) noexcept
	{
		AcquireSRWLockShared(&g_endpointLock);
		const bool valid = g_gameEndpointValid;
		address = g_gameAddress;
		port = g_gamePort;
		ReleaseSRWLockShared(&g_endpointLock);
		return valid;
	}

	std::uint32_t ResolveBootstrapAddress() noexcept
	{
		auto domain = TIBIA_OBF(TIBIA_BOOTSTRAP_DOMAIN);
		if(domain.c_str()[0] == '\0')
		{
			DebugLog("[NetworkRedirect] Bootstrap host is empty.\n");
			return 0;
		}

		IN_ADDR numericAddress = {};
		if(InetPtonA(AF_INET, domain.c_str(), &numericAddress) == 1)
			return numericAddress.s_addr;

		addrinfo hints = {};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		addrinfo* addresses = nullptr;
		if(getaddrinfo(domain.c_str(), nullptr, &hints, &addresses) != 0)
		{
			DebugLog("[NetworkRedirect] Bootstrap DNS resolution failed.\n");
			return 0;
		}

		std::uint32_t resolved = 0;
		for(const addrinfo* entry = addresses; entry; entry = entry->ai_next)
		{
			if(entry->ai_family != AF_INET ||
				entry->ai_addrlen < sizeof(sockaddr_in) ||
				!entry->ai_addr)
			{
				continue;
			}
			const auto* ipv4 =
				reinterpret_cast<const sockaddr_in*>(entry->ai_addr);
			if(ipv4->sin_addr.s_addr != 0)
			{
				resolved = ipv4->sin_addr.s_addr;
				break;
			}
		}
		freeaddrinfo(addresses);

		return resolved;
	}

	bool IsLocalEndpoint(const sockaddr* name, int nameLength,
		std::uint16_t port) noexcept
	{
		if(!name || nameLength < static_cast<int>(sizeof(sockaddr_in)) ||
			name->sa_family != AF_INET)
		{
			return false;
		}
		const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(name);
		return ipv4->sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
			ntohs(ipv4->sin_port) == port;
	}

	const sockaddr* RedirectEndpoint(const sockaddr* name, int nameLength,
		sockaddr_in& replacement, int& rejectionError) noexcept
	{
		rejectionError = 0;
		std::uint32_t address = 0;
		std::uint16_t port = 0;
		if(IsLocalEndpoint(name, nameLength, LOCAL_LOGIN_PORT))
		{
			ClearGameEndpoint();
			address = ResolveBootstrapAddress();
			port = LOCAL_LOGIN_PORT;
			if(address == 0)
			{
				rejectionError = WSAHOST_NOT_FOUND;
				return name;
			}
		}
		else if(IsLocalEndpoint(name, nameLength, LOCAL_GAME_PORT))
		{
			if(!LoadGameEndpoint(address, port))
			{
				rejectionError = WSAENOTCONN;
				return name;
			}
		}
		else
		{
			return name;
		}

		replacement = *reinterpret_cast<const sockaddr_in*>(name);
		replacement.sin_addr.s_addr = address;
		replacement.sin_port = htons(port);
		return reinterpret_cast<const sockaddr*>(&replacement);
	}

	int WSAAPI HookConnect(SOCKET socket, const sockaddr* name,
		int nameLength) noexcept
	{
		if(!g_originalConnect)
		{
			WSASetLastError(WSAEINVAL);
			return SOCKET_ERROR;
		}
		sockaddr_in replacement = {};
		int rejectionError = 0;
		const sockaddr* destination = RedirectEndpoint(name, nameLength,
			replacement, rejectionError);
		if(rejectionError != 0)
		{
			WSASetLastError(rejectionError);
			return SOCKET_ERROR;
		}
		return g_originalConnect(socket, destination, nameLength);
	}

	int WSAAPI HookWSAConnect(SOCKET socket, const sockaddr* name,
		int nameLength, LPWSABUF callerData, LPWSABUF calleeData,
		LPQOS sqos, LPQOS gqos) noexcept
	{
		if(!g_originalWSAConnect)
		{
			WSASetLastError(WSAEINVAL);
			return SOCKET_ERROR;
		}
		sockaddr_in replacement = {};
		int rejectionError = 0;
		const sockaddr* destination = RedirectEndpoint(name, nameLength,
			replacement, rejectionError);
		if(rejectionError != 0)
		{
			WSASetLastError(rejectionError);
			return SOCKET_ERROR;
		}
		return g_originalWSAConnect(socket, destination, nameLength,
			callerData, calleeData, sqos, gqos);
	}

	bool RvaFits(DWORD rva, std::size_t bytes, DWORD imageSize) noexcept
	{
		return rva < imageSize && bytes <= imageSize - rva;
	}

	bool PatchWinsockImports(DWORD imageBase) noexcept
	{
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
		if(!dos || dos->e_magic != IMAGE_DOS_SIGNATURE ||
			dos->e_lfanew <= 0 ||
			!RvaFits(static_cast<DWORD>(dos->e_lfanew),
				sizeof(IMAGE_NT_HEADERS32), 0x10000000))
		{
			return false;
		}

		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
			imageBase + static_cast<DWORD>(dos->e_lfanew));
		if(nt->Signature != IMAGE_NT_SIGNATURE ||
			nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
		{
			return false;
		}

		const DWORD imageSize = nt->OptionalHeader.SizeOfImage;
		const auto& imports =
			nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if(!RvaFits(imports.VirtualAddress,
			sizeof(IMAGE_IMPORT_DESCRIPTOR), imageSize))
		{
			return false;
		}

		bool connectPatched = false;
		auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
			imageBase + imports.VirtualAddress);
		const DWORD descriptorLimit = imports.Size /
			static_cast<DWORD>(sizeof(IMAGE_IMPORT_DESCRIPTOR));

		for(DWORD descriptorIndex = 0;
			descriptorIndex < descriptorLimit && descriptor->Name != 0;
			++descriptorIndex, ++descriptor)
		{
			if(!RvaFits(descriptor->FirstThunk, sizeof(IMAGE_THUNK_DATA32),
				imageSize))
			{
				return false;
			}

			auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(
				imageBase + descriptor->FirstThunk);
			const DWORD maxThunks =
				(imageSize - descriptor->FirstThunk) /
				static_cast<DWORD>(sizeof(IMAGE_THUNK_DATA32));

			for(DWORD thunkIndex = 0;
				thunkIndex < maxThunks && thunk->u1.Function != 0;
				++thunkIndex, ++thunk)
			{
				const DWORD current = thunk->u1.Function;
				if(current == reinterpret_cast<DWORD>(g_originalConnect))
				{
					if(!OverWrite(reinterpret_cast<DWORD>(&thunk->u1.Function),
						reinterpret_cast<DWORD>(&HookConnect)))
					{
						return false;
					}
					connectPatched = true;
				}
				else if(current ==
					reinterpret_cast<DWORD>(g_originalWSAConnect))
				{
					if(!OverWrite(reinterpret_cast<DWORD>(&thunk->u1.Function),
						reinterpret_cast<DWORD>(&HookWSAConnect)))
					{
						return false;
					}
				}
			}
		}
		return connectPatched;
	}

	bool ParseAndRewriteCharacterList() noexcept
	{
		if(!g_networkMessageAddress)
			return false;

		std::size_t ipOffsets[MAX_CHARACTER_COUNT] = {};
		std::size_t portOffsets[MAX_CHARACTER_COUNT] = {};
		std::uint32_t realAddress = 0;
		std::uint16_t realPort = 0;
		unsigned char* data = nullptr;
		std::size_t size = 0;
		std::size_t position = 0;
		std::size_t characterCount = 0;
		bool valid = false;

		__try
		{
			const auto* message =
				reinterpret_cast<const DWORD*>(g_networkMessageAddress);
			data = reinterpret_cast<unsigned char*>(message[0]);
			size = message[1];
			position = message[2];

			if(!data || size == 0 || size > 0xFFFF ||
				!CanRead(position, 1, size))
			{
				__leave;
			}

			characterCount = data[position++];
			if(characterCount == 0 ||
				characterCount > MAX_CHARACTER_COUNT)
			{
				__leave;
			}

			// Even empty names need two length fields, IPv4, port and the
			// final premium-days field.
			const std::size_t minimumRecordSize = 10;
			if(characterCount > (size - position) / minimumRecordSize ||
				!CanRead(position,
					characterCount * minimumRecordSize + 2, size))
			{
				__leave;
			}

			for(std::size_t index = 0; index < characterCount; ++index)
			{
				if(!SkipString(data, size, position) ||
					!SkipString(data, size, position) ||
					!CanRead(position, 6, size))
				{
					__leave;
				}

				ipOffsets[index] = position;
				std::uint32_t address = 0;
				memcpy(&address, data + position, sizeof(address));
				position += sizeof(address);

				portOffsets[index] = position;
				std::uint16_t port = 0;
				if(!ReadU16(data, size, position, port) ||
					address == 0 || port == 0)
				{
					__leave;
				}

				if(index == 0)
				{
					realAddress = address;
					realPort = port;
				}
				else if(address != realAddress || port != realPort)
				{
					// A single local game endpoint cannot distinguish worlds.
					__leave;
				}
			}

			if(!CanRead(position, sizeof(std::uint16_t), size))
				__leave;

			const std::uint32_t localAddress = htonl(INADDR_LOOPBACK);
			const std::uint16_t localPort = LOCAL_GAME_PORT;
			for(std::size_t index = 0; index < characterCount; ++index)
			{
				memcpy(data + ipOffsets[index], &localAddress,
					sizeof(localAddress));
				data[portOffsets[index]] =
					static_cast<unsigned char>(localPort & 0xFF);
				data[portOffsets[index] + 1] =
					static_cast<unsigned char>(localPort >> 8);
			}
			valid = true;
		}
		__except(EXCEPTION_EXECUTE_HANDLER)
		{
			valid = false;
		}

		if(valid)
		{
			StoreGameEndpoint(realAddress, realPort);
			DebugLog("[NetworkRedirect] Character endpoint captured.\n");
		}
		else
		{
			ClearGameEndpoint();
			DebugLog("[NetworkRedirect] Invalid character-list payload.\n");
		}
		return valid;
	}

	void __cdecl HookCharacterListParser()
	{
		ParseAndRewriteCharacterList();
		g_characterListParser();
	}

	bool InstallCharacterListHook(DWORD clientBase) noexcept
	{
		const DWORD callAddress = clientBase + CHARACTER_LIST_CALL_OFFSET;
		const DWORD parserAddress = clientBase + CHARACTER_LIST_PARSER_OFFSET;
		bool valid = false;
		__try {
			const unsigned char* call =
				reinterpret_cast<const unsigned char*>(callAddress);
			std::int32_t relative = 0;
			if(call[0] == 0xE8)
			{
				memcpy(&relative, call + 1, sizeof(relative));
				const unsigned char* parser =
					reinterpret_cast<const unsigned char*>(parserAddress);
				valid = callAddress + 5 + relative == parserAddress &&
					parser[0] == 0x55 && parser[1] == 0x8B &&
					parser[2] == 0xEC;
			}
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			valid = false;
		}
		if(!valid)
			return false;

		g_characterListParser =
			reinterpret_cast<CharacterListParserFn>(parserAddress);
		g_networkMessageAddress = clientBase + NETWORK_MESSAGE_OFFSET;
		return HookCall(callAddress,
			reinterpret_cast<DWORD>(&HookCharacterListParser));
	}
}

namespace NetworkRedirect
{
	bool Install(std::uintptr_t clientBaseValue, bool enabled) noexcept
	{
		Shutdown();
		if(!enabled)
			return true;
		const DWORD clientBase = static_cast<DWORD>(clientBaseValue);

		auto winsock = TIBIA_OBF("ws2_32.dll");
		HMODULE module = GetModuleHandleA(winsock.c_str());
		if(!module)
			return false;

		auto connectName = TIBIA_OBF("connect");
		auto wsaConnectName = TIBIA_OBF("WSAConnect");
		g_originalConnect = reinterpret_cast<ConnectFn>(
			GetProcAddress(module, connectName.c_str()));
		g_originalWSAConnect = reinterpret_cast<WSAConnectFn>(
			GetProcAddress(module, wsaConnectName.c_str()));
		if(!g_originalConnect || !g_originalWSAConnect ||
			!PatchWinsockImports(clientBase) ||
			!InstallCharacterListHook(clientBase))
		{
			Shutdown();
			return false;
		}
		return true;
	}

	void Shutdown() noexcept
	{
		ClearGameEndpoint();
		g_originalConnect = nullptr;
		g_originalWSAConnect = nullptr;
		g_characterListParser = nullptr;
		g_networkMessageAddress = 0;
	}
}
