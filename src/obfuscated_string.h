#ifndef TIBIA_OBFUSCATED_STRING_H
#define TIBIA_OBFUSCATED_STRING_H

#include "security_build_config.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace tibia::security
{
	constexpr std::uint32_t MixSeed(std::uint32_t value) noexcept
	{
		value ^= value >> 16;
		value *= 0x7FEB352Du;
		value ^= value >> 15;
		value *= 0x846CA68Bu;
		value ^= value >> 16;
		return value ? value : 0xA341316Cu;
	}

	constexpr std::uint32_t HashText(const char* text) noexcept
	{
		std::uint32_t hash = 2166136261u;
		for(std::size_t index = 0; text[index] != '\0'; ++index)
			hash = (hash ^ static_cast<unsigned char>(text[index])) * 16777619u;
		return hash;
	}

	constexpr unsigned char StreamByte(std::uint32_t seed,
		std::size_t index) noexcept
	{
		return static_cast<unsigned char>(MixSeed(seed +
			static_cast<std::uint32_t>(index * 0x9E3779B9u)) & 0xFFu);
	}

	template<std::size_t Size>
	class DecryptedString final
	{
	public:
		__declspec(noinline) DecryptedString(
			const std::array<unsigned char, Size>& encrypted,
			std::uint32_t seed) noexcept
		{
			const volatile unsigned char* source = encrypted.data();
			for(std::size_t index = 0; index < Size; ++index)
			{
				buffer_[index] = static_cast<char>(source[index] ^
					StreamByte(seed, index));
			}
		}

		~DecryptedString() noexcept
		{
			SecureZeroMemory(buffer_.data(), buffer_.size());
		}

		DecryptedString(const DecryptedString&) = delete;
		DecryptedString& operator=(const DecryptedString&) = delete;
		DecryptedString(DecryptedString&&) = delete;
		DecryptedString& operator=(DecryptedString&&) = delete;

		const char* c_str() const noexcept { return buffer_.data(); }

	private:
		std::array<char, Size> buffer_{};
	};

	template<std::size_t Size, std::uint32_t Seed>
	class EncryptedLiteral final
	{
	public:
		constexpr explicit EncryptedLiteral(const char (&text)[Size]) noexcept :
			encrypted_{}
		{
			for(std::size_t index = 0; index < Size; ++index)
			{
				encrypted_[index] = static_cast<unsigned char>(text[index]) ^
					StreamByte(Seed, index);
			}
		}

		__declspec(noinline) DecryptedString<Size> Decrypt() const noexcept
		{
			return DecryptedString<Size>(encrypted_, Seed);
		}

	private:
		std::array<unsigned char, Size> encrypted_;
	};

	template<std::uint32_t Seed, std::size_t Size>
	constexpr EncryptedLiteral<Size, Seed> MakeEncrypted(
		const char (&text)[Size]) noexcept
	{
		return EncryptedLiteral<Size, Seed>(text);
	}

	template<std::size_t Size>
	class PlainLiteral final
	{
	public:
		constexpr explicit PlainLiteral(const char (&text)[Size]) noexcept :
			text_(text) {}

		const char* c_str() const noexcept { return text_; }

	private:
		const char* text_;
	};

	template<std::size_t Size>
	class DecryptedBytes final
	{
	public:
		__declspec(noinline) DecryptedBytes(
			const std::array<unsigned char, Size>& encrypted,
			std::uint32_t seed) noexcept
		{
			const volatile unsigned char* source = encrypted.data();
			for(std::size_t index = 0; index < Size; ++index)
				buffer_[index] = static_cast<unsigned char>(source[index] ^
					StreamByte(seed, index));
		}

		~DecryptedBytes() noexcept
		{
			SecureZeroMemory(buffer_.data(), buffer_.size());
		}

		DecryptedBytes(const DecryptedBytes&) = delete;
		DecryptedBytes& operator=(const DecryptedBytes&) = delete;
		DecryptedBytes(DecryptedBytes&&) = delete;
		DecryptedBytes& operator=(DecryptedBytes&&) = delete;

		const unsigned char* data() const noexcept { return buffer_.data(); }
		std::size_t size() const noexcept { return buffer_.size(); }
		unsigned char operator[](std::size_t index) const noexcept
		{
			return buffer_[index];
		}

	private:
		std::array<unsigned char, Size> buffer_{};
	};

	template<std::size_t Size, std::uint32_t Seed>
	class EncryptedBytes final
	{
	public:
		constexpr explicit EncryptedBytes(
			const std::array<unsigned char, Size>& bytes) noexcept :
			encrypted_{}
		{
			for(std::size_t index = 0; index < Size; ++index)
				encrypted_[index] = bytes[index] ^ StreamByte(Seed, index);
		}

		__declspec(noinline) DecryptedBytes<Size> Decrypt() const noexcept
		{
			return DecryptedBytes<Size>(encrypted_, Seed);
		}

	private:
		std::array<unsigned char, Size> encrypted_;
	};

	template<std::uint32_t Seed, typename... Values>
	constexpr auto MakeEncryptedBytes(Values... values) noexcept
	{
		static_assert(sizeof...(Values) > 0, "Encrypted byte list is empty");
		const std::array<unsigned char, sizeof...(Values)> bytes = {
			static_cast<unsigned char>(values)...
		};
		return EncryptedBytes<sizeof...(Values), Seed>(bytes);
	}

	template<std::size_t Size>
	class PlainBytes final
	{
	public:
		constexpr explicit PlainBytes(
			const std::array<unsigned char, Size>& bytes) noexcept :
			bytes_(bytes) {}

		const unsigned char* data() const noexcept { return bytes_.data(); }
		std::size_t size() const noexcept { return bytes_.size(); }
		unsigned char operator[](std::size_t index) const noexcept
		{
			return bytes_[index];
		}

	private:
		std::array<unsigned char, Size> bytes_;
	};

	template<typename... Values>
	constexpr auto MakePlainBytes(Values... values) noexcept
	{
		static_assert(sizeof...(Values) > 0, "Plain byte list is empty");
		return PlainBytes<sizeof...(Values)>(
			std::array<unsigned char, sizeof...(Values)>{
				static_cast<unsigned char>(values)...
			});
	}
}

#if TIBIA_ENABLE_STRING_OBFUSCATION
#define TIBIA_OBF_IMPL(text, unique_id) ([]() noexcept { \
	static constexpr std::uint32_t seed = ::tibia::security::MixSeed( \
		0xC001D00Du ^ static_cast<std::uint32_t>(__LINE__ * 131u) ^ \
		static_cast<std::uint32_t>(unique_id * 977u) ^ \
		::tibia::security::HashText(__FILE__)); \
	static constexpr auto encrypted = \
		::tibia::security::MakeEncrypted<seed>(text); \
	return encrypted.Decrypt(); \
}())
#define TIBIA_OBF(text) TIBIA_OBF_IMPL(text, __COUNTER__)
#define TIBIA_OBF_BYTES_IMPL(unique_id, ...) ([]() noexcept { \
	static constexpr std::uint32_t seed = ::tibia::security::MixSeed( \
		0x7A39C51Bu ^ static_cast<std::uint32_t>(__LINE__ * 193u) ^ \
		static_cast<std::uint32_t>(unique_id * 1237u) ^ \
		::tibia::security::HashText(__FILE__)); \
	static constexpr auto encrypted = \
		::tibia::security::MakeEncryptedBytes<seed>(__VA_ARGS__); \
	return encrypted.Decrypt(); \
}())
#define TIBIA_OBF_BYTES(...) TIBIA_OBF_BYTES_IMPL(__COUNTER__, __VA_ARGS__)
#else
#define TIBIA_OBF(text) ::tibia::security::PlainLiteral(text)
#define TIBIA_OBF_BYTES(...) \
	::tibia::security::MakePlainBytes(__VA_ARGS__)
#endif

#endif
