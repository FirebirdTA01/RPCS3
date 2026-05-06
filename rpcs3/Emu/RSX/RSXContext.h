#pragma once

#include "util/types.hpp"

namespace rsx
{
	// Forward declarations — full types in RSXThread.h
	struct gcm_framebuffer_info;
	struct framebuffer_layout;

	// Per-process RSX state — saved/restored on process switch.
	// Fields are pointers into the actual storage (currently on rsx::thread;
	// later phases swap them to per-process storage).
	struct rsx_context_state
	{
		// FIFO control — values saved/restored on process switch
		u32 ctrl = 0;
		u32 dma_address = 0;
		u32 driver_info = 0;
		u32 device_addr = 0;
		u32 label_addr = 0;
		u32 main_mem_size = 0;
		u32 local_mem_size = 0;
		u32 rsx_event_port = 0;

		// Framebuffer setup — pointers to avoid type dependency issues
		gcm_framebuffer_info* m_surface_info = nullptr;
		gcm_framebuffer_info* m_depth_surface_info = nullptr;
		framebuffer_layout* m_framebuffer_layout = nullptr;

		// Pipeline state (pointer to rsx::thread::m_graphics_state)
		void* m_graphics_state = nullptr;
	};
} // namespace rsx
