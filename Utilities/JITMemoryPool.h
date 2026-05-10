#pragma once

#include "util/types.hpp"
#include "Utilities/mutex.h"

#include <memory>
#include <vector>

// One reserved-and-eventually-decommitted host VA region used by a single
// MemoryManager1 in pool-backed mode. Owned by ppu_jit_memory_pool;
// outlives the MM1 instance that allocates from it.
struct ppu_jit_memory_region
{
	static constexpr u64 c_total_size = 0x3000'0000;  // c_max_size * 3, mirrored from MM1
	void* m_code_mems = nullptr;

	ppu_jit_memory_region();
	~ppu_jit_memory_region();

	ppu_jit_memory_region(const ppu_jit_memory_region&) = delete;
	ppu_jit_memory_region& operator=(const ppu_jit_memory_region&) = delete;
};

// Owns a vector of regions. Lives inside lv2_process. clear() decommits and
// releases every region; called from lv2_process::reset() and from the dtor.
class ppu_jit_memory_pool
{
	shared_mutex m_mtx;
	std::vector<std::unique_ptr<ppu_jit_memory_region>> m_regions;

public:
	ppu_jit_memory_pool();
	~ppu_jit_memory_pool();

	ppu_jit_memory_pool(const ppu_jit_memory_pool&) = delete;
	ppu_jit_memory_pool& operator=(const ppu_jit_memory_pool&) = delete;

	// Reserves a fresh 768 MiB region, retains ownership, returns a raw
	// pointer the caller (MM1) holds for its lifetime.
	ppu_jit_memory_region* take_region();

	// Decommits + releases every region. Safe to call multiple times.
	void clear();
};
