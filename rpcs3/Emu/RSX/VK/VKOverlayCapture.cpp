#include "stdafx.h"
#include "VKOverlayCapture.h"

#include "Emu/multiproc_debug.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/VK/vkutils/device.h"

namespace vk
{
	vsh_overlay_state::~vsh_overlay_state()
	{
		reset();
	}

	u64 vsh_overlay_state::latest_published_generation() const
	{
		return m_latest_published_generation.load();
	}

	const vsh_overlay_slot* vsh_overlay_state::latest_ready_slot() const
	{
		const u64 latest = m_latest_published_generation.load();
		if (!latest)
		{
			return nullptr;
		}

		for (const auto& slot : m_slots)
		{
			if (slot.generation.load() != latest)
			{
				continue;
			}

			if (!slot.ready.load())
			{
				continue;
			}

			if (slot.state.load() != vsh_overlay_slot_state::published)
			{
				continue;
			}

			return &slot;
		}

		return nullptr;
	}

	bool vsh_overlay_state::overlay_active() const
	{
		return m_overlay_active.load();
	}

	void vsh_overlay_state::set_overlay_active(bool active)
	{
		m_overlay_active.release(active);
	}

	u32 vsh_overlay_state::slot_index(const vsh_overlay_slot& slot) const
	{
		return ::narrow<u32>(&slot - m_slots.data());
	}

	bool vsh_overlay_state::ensure_command_resources(vk::render_device& dev)
	{
		if (m_command_pool_created)
		{
			return true;
		}

		m_command_pool.create(dev, dev.get_graphics_queue_family());
		m_command_pool_created = true;

		for (auto& slot : m_slots)
		{
			slot.command_buffer.create(m_command_pool);
			slot.command_buffer.access_hint = vk::command_buffer::access_type_hint::all;
		}

		return true;
	}

	bool vsh_overlay_state::ensure_slot_resources(vk::render_device& dev, vsh_overlay_slot& slot, u32 width, u32 height, VkFormat format)
	{
		if (slot.image && slot.ready_semaphore && slot.width == width && slot.height == height && slot.format == format)
		{
			return true;
		}

		slot.ready.release(false);
		slot.generation.release(0);
		slot.image.reset();
		slot.ready_semaphore.reset();

		const auto& memory_map = dev.get_memory_mapping();
		slot.image = std::make_unique<vk::viewable_image>(
			dev,
			memory_map.device_local,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			VK_IMAGE_TYPE_2D,
			format,
			width,
			height,
			1,
			1,
			1,
			VK_SAMPLE_COUNT_1_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			0,
			VMM_ALLOCATION_POOL_UNDEFINED,
			RSX_FORMAT_CLASS_COLOR);

		slot.ready_semaphore = std::make_unique<vk::semaphore>(dev);
		slot.width = width;
		slot.height = height;
		slot.format = format;
		return true;
	}

	vsh_overlay_slot* vsh_overlay_state::try_acquire_slot(vk::render_device& dev, u32 owner_pid, u32 width, u32 height, VkFormat format)
	{
		if (!width || !height || format == VK_FORMAT_UNDEFINED)
		{
			MPDBG_LOG(rsx_log, "VK_OVERLAY_CAPTURE_DROP: owner_pid=%u reason=invalid_extent size=%ux%u format=0x%x",
				owner_pid, width, height, static_cast<u32>(format));
			return nullptr;
		}

		if (!ensure_command_resources(dev))
		{
			MPDBG_LOG(rsx_log, "VK_OVERLAY_CAPTURE_DROP: owner_pid=%u reason=alloc_fail size=%ux%u format=0x%x",
				owner_pid, width, height, static_cast<u32>(format));
			return nullptr;
		}

		bool state_busy = false;
		bool fence_pending = false;
		bool alloc_fail = false;

		for (auto& slot : m_slots)
		{
			vsh_overlay_slot_state expected = vsh_overlay_slot_state::free;
			if (!slot.state.compare_and_swap_test(expected, vsh_overlay_slot_state::writing))
			{
				expected = vsh_overlay_slot_state::consumed;
				if (!slot.state.compare_and_swap_test(expected, vsh_overlay_slot_state::writing))
				{
					state_busy = true;
					continue;
				}
			}

			if (!slot.command_buffer.poke())
			{
				fence_pending = true;
				slot.state.release(vsh_overlay_slot_state::consumed);
				continue;
			}

			if (!ensure_slot_resources(dev, slot, width, height, format))
			{
				alloc_fail = true;
				slot.state.release(vsh_overlay_slot_state::consumed);
				continue;
			}

			slot.owner_pid = owner_pid;
			slot.ready.release(false);
			return &slot;
		}

		const u64 latest = m_latest_published_generation.load();
		for (auto& slot : m_slots)
		{
			if (slot.generation.load() == latest)
			{
				continue;
			}

			vsh_overlay_slot_state expected = vsh_overlay_slot_state::published;
			if (!slot.state.compare_and_swap_test(expected, vsh_overlay_slot_state::writing))
			{
				continue;
			}

			if (!slot.command_buffer.poke())
			{
				fence_pending = true;
				slot.state.release(vsh_overlay_slot_state::consumed);
				continue;
			}

			if (!ensure_slot_resources(dev, slot, width, height, format))
			{
				alloc_fail = true;
				slot.state.release(vsh_overlay_slot_state::consumed);
				continue;
			}

			slot.owner_pid = owner_pid;
			slot.ready.release(false);
			return &slot;
		}

		const char* reason = alloc_fail ? "alloc_fail" : fence_pending ? "fence_pending" : state_busy ? "state_writing" : "no_slot";
		MPDBG_LOG(rsx_log, "VK_OVERLAY_CAPTURE_DROP: owner_pid=%u reason=%s size=%ux%u format=0x%x",
			owner_pid, reason, width, height, static_cast<u32>(format));
		return nullptr;
	}

	void vsh_overlay_state::publish(vsh_overlay_slot& slot)
	{
		const u64 generation = m_next_generation.fetch_add(1);
		slot.generation.release(generation);
		slot.ready.release(true);
		slot.state.release(vsh_overlay_slot_state::published);
		m_latest_published_generation.release(generation);
	}

	void vsh_overlay_state::release(vsh_overlay_slot& slot)
	{
		slot.ready.release(false);
		slot.state.release(vsh_overlay_slot_state::consumed);
	}

	void vsh_overlay_state::reset_slot(vsh_overlay_slot& slot)
	{
		slot.state.release(vsh_overlay_slot_state::free);
		slot.ready.release(false);
		slot.generation.release(0);
		slot.owner_pid = 0;
		slot.width = 0;
		slot.height = 0;
		slot.format = VK_FORMAT_UNDEFINED;
		slot.image.reset();
		slot.ready_semaphore.reset();
	}

	void vsh_overlay_state::reset()
	{
		m_overlay_active.release(false);
		m_latest_published_generation.release(0);
		m_next_generation.release(1);

		for (auto& slot : m_slots)
		{
			reset_slot(slot);
		}

		if (m_command_pool_created.test_and_reset())
		{
			for (auto& slot : m_slots)
			{
				slot.command_buffer.destroy();
			}

			m_command_pool.destroy();
		}
	}
}
