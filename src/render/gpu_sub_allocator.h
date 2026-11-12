// Copied from Andromeda-Lib/Andromeda/Graphics/GXM/GpuSubAllocator.h, without its namespace.
#ifndef OPENCMR2_GPU_SUB_ALLOCATOR_H
#define OPENCMR2_GPU_SUB_ALLOCATOR_H

#include <vector>
#include <cstddef>

// Sub-allocates byte ranges out of arenas that the owner provides.
//
// WHY THIS EXISTS. The Vita dies when a process holds more than about
// two thousand GPU-mapped memblocks. Measured on device 2026-09-04: a
// probe that allocated and mapped 4 KB blocks one at a time killed the
// console between block 1,024 and block 2,048, and no call returned an
// error first. One memblock per vertex buffer put a radius-8 VitaCraft
// world over that line a few seconds after boot. So vertex and index
// buffers now come out of a few large arenas, and this class hands out
// the ranges.
//
// It knows nothing about GXM. The owner maps the arenas and passes the
// base pointers in, so the PC build can test the bookkeeping.
//
// Layout: every block starts with a 16-byte Header, then the payload,
// so Free() needs only the payload pointer. Payloads are 16-byte
// aligned. The free list of an arena is sorted by offset and never
// holds two adjacent ranges, because Free() merges with both
// neighbours.
//
// Not thread safe. Only the render thread allocates GPU memory.
class GpuSubAllocator
{
public:
	static constexpr unsigned int kAlign  = 16u;
	static constexpr unsigned int kHeader = 16u;
	static constexpr unsigned int kMagic  = 0x504F4F4Cu;   // 'POOL'

	struct Header
	{
		unsigned int magic;
		unsigned int size;    // header + payload, aligned
		unsigned int arena;   // index into the arena list
		unsigned int pad;
	};

	struct Range
	{
		unsigned int offset;
		unsigned int size;
	};

	struct Arena
	{
		unsigned char*     base;
		unsigned int       size;
		int                handle;   // whatever the owner needs to free it
		std::vector<Range> free;
	};

	static unsigned int Align(unsigned int size)
	{
		return (size + kAlign - 1u) & ~(kAlign - 1u);
	}

	// Bytes an allocation of `size` takes out of an arena.
	static unsigned int Total(unsigned int size)
	{
		return Align(size) + kHeader;
	}

	void AddArena(unsigned char* base, unsigned int size, int handle)
	{
		Arena arena;
		arena.base   = base;
		arena.size   = size;
		arena.handle = handle;
		arena.free.push_back(Range{0u, size});
		_arenas.push_back(arena);
		_capacity += size;
	}

	// First fit across the arenas in order. Returns the payload
	// pointer, or nullptr when no arena has a range large enough. The
	// owner then adds an arena and calls again.
	void* Alloc(unsigned int size)
	{
		const unsigned int total = Total(size);

		for (size_t a = 0; a < _arenas.size(); ++a)
		{
			Arena& arena = _arenas[a];

			for (size_t i = 0; i < arena.free.size(); ++i)
			{
				if (arena.free[i].size < total)
					continue;

				const unsigned int offset = arena.free[i].offset;

				if (arena.free[i].size == total)
				{
					arena.free.erase(arena.free.begin() + (std::ptrdiff_t)i);
				}
				else
				{
					arena.free[i].offset += total;
					arena.free[i].size   -= total;
				}

				Header* header = reinterpret_cast<Header*>(arena.base + offset);
				header->magic = kMagic;
				header->size  = total;
				header->arena = (unsigned int)a;
				header->pad   = 0u;

				_used += total;
				++_live;
				return arena.base + offset + kHeader;
			}
		}

		return nullptr;
	}

	// Returns the index of the arena the block went back to, or -1 for
	// a pointer this allocator did not hand out. The header is cleared,
	// so a second Free of the same pointer also returns -1.
	int Free(void* payload)
	{
		if (payload == nullptr)
			return -1;

		Header* header = reinterpret_cast<Header*>(
			static_cast<unsigned char*>(payload) - kHeader);

		if (header->magic != kMagic || header->arena >= _arenas.size())
			return -1;

		Arena& arena = _arenas[header->arena];
		const unsigned int offset = (unsigned int)(
			reinterpret_cast<unsigned char*>(header) - arena.base);
		const unsigned int size  = header->size;
		const int          index = (int)header->arena;

		if (offset + size > arena.size)
			return -1;

		header->magic = 0u;
		_used -= size;
		--_live;
		Release(arena, offset, size);
		return index;
	}

	// True when the arena's free list is one range that covers all of
	// it, which is what every Free() in any order must leave behind.
	bool ArenaIsWhole(size_t index) const
	{
		if (index >= _arenas.size())
			return false;

		const Arena& arena = _arenas[index];
		return arena.free.size() == 1
		    && arena.free[0].offset == 0u
		    && arena.free[0].size == arena.size;
	}

	size_t       ArenaCount() const           { return _arenas.size(); }
	const Arena& GetArena(size_t index) const { return _arenas[index]; }
	long         Live() const                 { return _live; }
	long         Used() const                 { return _used; }
	long         Capacity() const             { return _capacity; }

private:
	// Put a range back, merging with the neighbour on either side.
	void Release(Arena& arena, unsigned int offset, unsigned int size)
	{
		std::vector<Range>& free = arena.free;

		size_t i = 0;
		while (i < free.size() && free[i].offset < offset)
			++i;

		const bool joinsPrev = (i > 0)
			&& (free[i - 1].offset + free[i - 1].size == offset);
		const bool joinsNext = (i < free.size())
			&& (offset + size == free[i].offset);

		if (joinsPrev && joinsNext)
		{
			free[i - 1].size += size + free[i].size;
			free.erase(free.begin() + (std::ptrdiff_t)i);
		}
		else if (joinsPrev)
		{
			free[i - 1].size += size;
		}
		else if (joinsNext)
		{
			free[i].offset = offset;
			free[i].size  += size;
		}
		else
		{
			free.insert(free.begin() + (std::ptrdiff_t)i, Range{offset, size});
		}
	}

	std::vector<Arena> _arenas;
	long _live     = 0;
	long _used     = 0;
	long _capacity = 0;
};

#endif
