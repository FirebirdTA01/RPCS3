#include "stdafx.h"
#include "vk_host_context.h"

#include "VKHelpers.h"
#include "vkutils/device.h"

#include "Emu/RSX/GSFrameBase.h"
#include "Emu/system_config.h"

#include "vkutils/swapchain.h"

#ifdef HAVE_X11
#include <X11/Xlib.h>
#endif

namespace vk
{
	bool host_context::initialize(GSFrameBase* frame)
	{
		std::lock_guard lock(m_init_mutex);

		if (m_initialized)
		{
			return true;
		}

		if (!m_instance.create("RPCS3"))
		{
			return false;
		}

		m_instance.bind();

		auto& gpus = m_instance.enumerate_devices();

		if (gpus.empty())
		{
			return false;
		}

		m_display_handle = frame->handle();

#ifdef HAVE_X11
		std::visit([this](auto&& p)
		{
			using T = std::decay_t<decltype(p)>;
			if constexpr (std::is_same_v<T, std::pair<Display*, Window>>)
			{
				m_x11_display = p.first;
				XFlush(m_x11_display);
			}
		}, m_display_handle);
#endif

		bool gpu_found = false;
		std::string adapter_name = g_cfg.video.vk.adapter;

		for (auto& gpu : gpus)
		{
			if (gpu.get_name() == adapter_name)
			{
				m_swapchain.reset(m_instance.create_swapchain(m_display_handle, gpu));
				gpu_found = true;
				break;
			}
		}

		if (!gpu_found || adapter_name.empty())
		{
			m_swapchain.reset(m_instance.create_swapchain(m_display_handle, gpus[0]));
		}

		if (!m_swapchain)
		{
			return false;
		}

		if (!m_swapchain->init(frame->client_width(), frame->client_height()))
		{
			m_swapchain.reset();
			return false;
		}

		vk::set_current_renderer(m_swapchain->get_device());
		vk::init();

		m_initialized = true;
		return true;
	}

	host_context::~host_context()
	{
		if (m_swapchain)
		{
			m_swapchain->destroy();
		}
		m_instance.destroy();

#if defined(HAVE_X11) && defined(HAVE_VULKAN)
		if (m_x11_display)
		{
			XCloseDisplay(m_x11_display);
		}
#endif

		vk::destroy_global_resources();
	}
}
