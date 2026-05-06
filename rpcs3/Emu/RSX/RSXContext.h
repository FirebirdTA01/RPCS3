#pragma once

#include "util/types.hpp"

namespace rsx
{
	// Per-process RSX state — saved/restored on process switch.
	// Only one process drives the GPU at a time; the other's state is parked.
	// Switching costs one frame (pause + wait_idle + save + load + unpause).
	struct rsx_context_state
	{
		// FIFO control
		u32 ctrl = 0;
		u32 dma_address = 0;
		// TODO: iomap_table, method_registers, pipeline state, surface info,
		// sampler descriptors, display flip queue — filled in sub-task B
	};
} // namespace rsx
