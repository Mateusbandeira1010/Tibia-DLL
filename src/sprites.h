#ifndef TIBIA_SPRITES_H
#define TIBIA_SPRITES_H

#include <cstddef>
#include <vector>

class Sprites final
{
	public:
		Sprites(const char *filename, const char* readType);
		~Sprites() = default;

		Sprites(const Sprites&) = delete;
		Sprites& operator=(const Sprites&) = delete;

		bool read(std::size_t offset, void* buffer,
			std::size_t size) const noexcept;
		bool loaded() const noexcept { return !data_.empty(); }
		std::size_t size() const noexcept { return data_.size(); }

	private:
		std::vector<unsigned char> data_;
};

#endif
