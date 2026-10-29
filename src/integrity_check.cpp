#include "integrity_check.h"

#include "security_build_config.h"

#include <windows.h>

#include <array>
#include <bcrypt.h>
#include <cstdint>

namespace
{
	INIT_ONCE g_integrityOnce = INIT_ONCE_STATIC_INIT;
	IntegrityCheck::Status g_integrityStatus = IntegrityCheck::Status::Invalid;
	constexpr std::array<BYTE, 8> STAMP_MAGIC = {
		0xD4, 0x91, 0x7A, 0x2C, 0xE8, 0x35, 0x6B, 0xF0
	};
	constexpr DWORD STAMP_VERSION = 1;
	constexpr DWORD STAMP_SIZE = 56;
	constexpr std::size_t HASH_BUFFER_SIZE = 8 * 1024;
	constexpr std::uint64_t MAX_IMAGE_FILE_SIZE = 64ull * 1024ull * 1024ull;

	bool IsExecutableProtection(DWORD protection) noexcept
	{
		const DWORD base = protection & 0xFFu;
		return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ;
	}

	std::uint32_t ReadU32(const BYTE* data) noexcept
	{
		return static_cast<std::uint32_t>(data[0]) |
			(static_cast<std::uint32_t>(data[1]) << 8) |
			(static_cast<std::uint32_t>(data[2]) << 16) |
			(static_cast<std::uint32_t>(data[3]) << 24);
	}

	std::uint64_t ReadU64(const BYTE* data) noexcept
	{
		std::uint64_t value = 0;
		for(std::size_t index = 0; index < 8; ++index)
			value |= static_cast<std::uint64_t>(data[index]) << (index * 8);
		return value;
	}

	void ZeroOverlap(BYTE* data, DWORD dataSize, std::uint64_t dataOffset,
		std::uint64_t fieldOffset, DWORD fieldSize) noexcept
	{
		for(DWORD index = 0; index < fieldSize; ++index)
		{
			const std::uint64_t position = fieldOffset + index;
			if(position >= dataOffset && position < dataOffset + dataSize)
				data[position - dataOffset] = 0;
		}
	}

	bool VerifyStampedFile(HMODULE module) noexcept
	{
		wchar_t path[MAX_PATH] = {};
		const DWORD pathLength = GetModuleFileNameW(module, path, MAX_PATH);
		if(pathLength == 0 || pathLength >= MAX_PATH)
			return false;
		HANDLE file = CreateFileW(path, GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
			nullptr);
		if(file == INVALID_HANDLE_VALUE)
			return false;

		bool valid = false;
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		BCRYPT_HASH_HANDLE hash = nullptr;
		std::array<BYTE, 512> hashObject{};
		std::array<BYTE, HASH_BUFFER_SIZE> buffer{};
		std::array<BYTE, STAMP_SIZE> trailer{};
		std::array<BYTE, 32> digest{};
		do
		{
			LARGE_INTEGER size = {};
			if(!GetFileSizeEx(file, &size) || size.QuadPart <= STAMP_SIZE ||
				static_cast<std::uint64_t>(size.QuadPart) > MAX_IMAGE_FILE_SIZE)
				break;
			LARGE_INTEGER start = {};
			if(!SetFilePointerEx(file, start, nullptr, FILE_BEGIN))
				break;
			DWORD bytesRead = 0;
			if(!ReadFile(file, buffer.data(), 512, &bytesRead, nullptr) ||
				bytesRead != 512 || ReadU32(buffer.data() + 0x3C) > 0x100000)
				break;
			const DWORD peOffset = ReadU32(buffer.data() + 0x3C);
			const DWORD optionalOffset = peOffset + 24;
			if(optionalOffset + 136 > bytesRead ||
				ReadU32(buffer.data() + peOffset) != IMAGE_NT_SIGNATURE ||
				static_cast<WORD>(buffer[optionalOffset] |
					(buffer[optionalOffset + 1] << 8)) !=
					IMAGE_NT_OPTIONAL_HDR32_MAGIC)
				break;
			const DWORD checksumOffset = optionalOffset + 64;
			const DWORD securityEntryOffset = optionalOffset + 128;
			const DWORD certificateOffset = ReadU32(
				buffer.data() + securityEntryOffset);
			const DWORD certificateSize = ReadU32(
				buffer.data() + securityEntryOffset + 4);
			LARGE_INTEGER trailerOffset = {};
			if(certificateOffset != 0 || certificateSize != 0)
			{
				if(certificateOffset < STAMP_SIZE || certificateSize == 0 ||
					static_cast<std::uint64_t>(certificateOffset) +
					certificateSize > static_cast<std::uint64_t>(size.QuadPart))
					break;
				trailerOffset.QuadPart = certificateOffset - STAMP_SIZE;
			}
			else
			{
				trailerOffset.QuadPart = size.QuadPart - STAMP_SIZE;
			}
			if(!SetFilePointerEx(file, trailerOffset, nullptr, FILE_BEGIN))
				break;
			if(!ReadFile(file, trailer.data(), STAMP_SIZE, &bytesRead, nullptr) ||
				bytesRead != STAMP_SIZE)
				break;
			BYTE magicDifference = 0;
			for(std::size_t index = 0; index < STAMP_MAGIC.size(); ++index)
				magicDifference |= trailer[index] ^ STAMP_MAGIC[index];
			const std::uint64_t coveredSize = ReadU64(trailer.data() + 12);
			if(magicDifference != 0 || ReadU32(trailer.data() + 8) !=
				STAMP_VERSION || coveredSize !=
				static_cast<std::uint64_t>(trailerOffset.QuadPart))
				break;

			DWORD objectSize = 0;
			DWORD resultSize = 0;
			NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm,
				BCRYPT_SHA256_ALGORITHM, nullptr, 0);
			if(BCRYPT_SUCCESS(status))
				status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
					reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
					&resultSize, 0);
			if(!BCRYPT_SUCCESS(status) || objectSize > hashObject.size())
				break;
			status = BCryptCreateHash(algorithm, &hash, hashObject.data(),
				objectSize, nullptr, 0, 0);
			if(!BCRYPT_SUCCESS(status))
				break;
			if(!SetFilePointerEx(file, start, nullptr, FILE_BEGIN))
				break;
			std::uint64_t remaining = coveredSize;
			for(; remaining != 0;)
			{
				const DWORD requested = remaining < buffer.size() ?
					static_cast<DWORD>(remaining) :
					static_cast<DWORD>(buffer.size());
				bytesRead = 0;
				if(!ReadFile(file, buffer.data(), requested, &bytesRead, nullptr) ||
					bytesRead != requested)
				{
					status = -1;
					break;
				}
				const std::uint64_t dataOffset = coveredSize - remaining;
				ZeroOverlap(buffer.data(), bytesRead, dataOffset,
					checksumOffset, 4);
				ZeroOverlap(buffer.data(), bytesRead, dataOffset,
					securityEntryOffset, 8);
				status = BCryptHashData(hash, buffer.data(), bytesRead, 0);
				if(!BCRYPT_SUCCESS(status))
					break;
				remaining -= bytesRead;
			}
			if(!BCRYPT_SUCCESS(status) || remaining != 0)
				break;
			status = BCryptFinishHash(hash, digest.data(),
				static_cast<ULONG>(digest.size()), 0);
			if(!BCRYPT_SUCCESS(status))
				break;
			BYTE difference = 0;
			for(std::size_t index = 0; index < digest.size(); ++index)
				difference |= digest[index] ^ trailer[20 + index];
			valid = difference == 0;
		} while(false);

		if(hash)
			BCryptDestroyHash(hash);
		if(algorithm)
			BCryptCloseAlgorithmProvider(algorithm, 0);
		SecureZeroMemory(hashObject.data(), hashObject.size());
		SecureZeroMemory(buffer.data(), buffer.size());
		SecureZeroMemory(digest.data(), digest.size());
		CloseHandle(file);
		return valid;
	}

	bool VerifyLoadedImage() noexcept
	{
		#if !TIBIA_ENABLE_INTEGRITY_CHECK
		return true;
		#else
		HMODULE module = nullptr;
		if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&VerifyLoadedImage), &module) || !module)
			return false;

		bool valid = false;
		__try {
			const auto* base = reinterpret_cast<const BYTE*>(module);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if(dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
				dos->e_lfanew > 0x100000)
				return false;

			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
				base + dos->e_lfanew);
			if(nt->Signature != IMAGE_NT_SIGNATURE ||
				nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
				(nt->FileHeader.Characteristics & IMAGE_FILE_DLL) == 0 ||
				nt->FileHeader.NumberOfSections == 0 ||
				nt->FileHeader.NumberOfSections > 32 ||
				nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
				nt->OptionalHeader.SizeOfImage < 0x1000 ||
				nt->OptionalHeader.SizeOfImage > 64u * 1024u * 1024u)
				return false;

			const auto* section = IMAGE_FIRST_SECTION(nt);
			const ULONG_PTR functionAddress = reinterpret_cast<ULONG_PTR>(
				&VerifyLoadedImage);
			const ULONG_PTR imageBase = reinterpret_cast<ULONG_PTR>(base);
			for(WORD index = 0; index < nt->FileHeader.NumberOfSections;
				++index, ++section)
			{
				const ULONG_PTR start = imageBase + section->VirtualAddress;
				const ULONG_PTR length = section->Misc.VirtualSize ?
					section->Misc.VirtualSize : section->SizeOfRawData;
				if(functionAddress < start || functionAddress >= start + length)
					continue;
				const DWORD required = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
				valid = (section->Characteristics & required) == required &&
					(section->Characteristics & IMAGE_SCN_MEM_WRITE) == 0;
				break;
			}
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
		if(!valid)
			return false;

		MEMORY_BASIC_INFORMATION memory = {};
		if(VirtualQuery(reinterpret_cast<const void*>(&VerifyLoadedImage),
			&memory, sizeof(memory)) != sizeof(memory))
			return false;
		return memory.State == MEM_COMMIT &&
			(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
			IsExecutableProtection(memory.Protect) && VerifyStampedFile(module);
		#endif
	}

	BOOL CALLBACK InitializeIntegrity(PINIT_ONCE, PVOID, PVOID*) noexcept
	{
		TIBIA_PROTECT_MUTATION("B1");
		g_integrityStatus = VerifyLoadedImage() ? IntegrityCheck::Status::Valid :
			IntegrityCheck::Status::Invalid;
		TIBIA_PROTECT_END();
		return TRUE;
	}
}

IntegrityCheck::Status IntegrityCheck::GetStatus() noexcept
{
	InitOnceExecuteOnce(&g_integrityOnce, InitializeIntegrity, nullptr, nullptr);
	return g_integrityStatus;
}
