#include "sprites.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

namespace
{
	struct FileCloser
	{
		void operator()(FILE* file) const noexcept
		{
			if(file)
				fclose(file);
		}
	};
}

Sprites::Sprites(const char* filename, const char* readType)
{
	if(!filename || !readType)
		return;

	std::unique_ptr<FILE, FileCloser> file(fopen(filename, readType));
	if(!file || _fseeki64(file.get(), 0, SEEK_END) != 0)
		return;

	const __int64 fileSize = _ftelli64(file.get());
	if(fileSize <= 0 ||
		static_cast<unsigned long long>(fileSize) >
			(std::numeric_limits<std::size_t>::max)() ||
		_fseeki64(file.get(), 0, SEEK_SET) != 0)
		return;

	try
	{
		data_.resize(static_cast<std::size_t>(fileSize));
	}
	catch(const std::bad_alloc&)
	{
		return;
	}

	if(fread(data_.data(), 1, data_.size(), file.get()) != data_.size())
		data_.clear();
}

bool Sprites::read(std::size_t offset, void* buffer,
	std::size_t size) const noexcept
{
	if(!buffer || offset > data_.size() || size > data_.size() - offset)
	{
		if(buffer && size != 0)
			memset(buffer, 0, size);
		return false;
	}
	if(size != 0)
		memcpy(buffer, data_.data() + offset, size);
	return true;
}
