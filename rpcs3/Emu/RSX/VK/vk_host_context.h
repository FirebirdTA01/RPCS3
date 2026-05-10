#pragma once

#include <memory>
#include <mutex>
#include <variant>

#include "vkutils/instance.h"

class GSFrameBase;

namespace vk
{
	class host_context
	{
		std::mutex m_init_mutex;
		bool m_initialized = false;
		instance m_instance;
		std::unique_ptr<swapchain_base> m_swapchain;
		display_handle_t m_display_handle{};
#ifdef HAVE_X11
		Display* m_x11_display = nullptr;
#endif

	public:
		host_context() noexcept = default;
		host_context(const host_context&) = delete;
		host_context& operator=(const host_context&) = delete;
		~host_context();

		// Lazy one-shot initializer. Idempotent — returns true if Vulkan
		// is available and the shared host-window resources are ready.
		bool initialize(GSFrameBase* frame);

		bool is_initialized() const noexcept { return m_initialized; }

		instance& vk_instance() { return m_instance; }
		swapchain_base* swapchain() { return m_swapchain.get(); }
		display_handle_t display_handle() const noexcept { return m_display_handle; }
	};
}
