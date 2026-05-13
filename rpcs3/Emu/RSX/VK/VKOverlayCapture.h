#pragma once

#include "VKGSRenderTypes.hpp"
#include "vkutils/image.h"
#include "vkutils/sync.h"

#include "util/atomic.hpp"

#include <array>
#include <memory>

namespace vk
{
	class render_device;

	enum class vsh_overlay_slot_state : u32
	{
		free = 0,
		writing,
		published,
		consumed
	};

	struct vsh_overlay_slot
	{
		atomic_t<vsh_overlay_slot_state> state = vsh_overlay_slot_state::free;
		atomic_t<bool> ready = false;
		atomic_t<u64> generation = 0;

		u32 owner_pid = 0;
		u32 width = 0;
		u32 height = 0;
		VkFormat format = VK_FORMAT_UNDEFINED;

		std::unique_ptr<vk::viewable_image> image;
		std::unique_ptr<vk::semaphore> ready_semaphore;
		vk::command_buffer_chunk command_buffer;
	};

	class vsh_overlay_state
	{
		static constexpr usz slot_count = 2;

		atomic_t<u64> m_latest_published_generation = 0;
		atomic_t<u64> m_next_generation = 1;
		atomic_t<bool> m_overlay_active = false;
		std::array<vsh_overlay_slot, slot_count> m_slots;
		vk::command_pool m_command_pool;
		atomic_t<bool> m_command_pool_created = false;

		void reset_slot(vsh_overlay_slot& slot);
		bool ensure_command_resources(vk::render_device& dev);
		bool ensure_slot_resources(vk::render_device& dev, vsh_overlay_slot& slot, u32 width, u32 height, VkFormat format);

	public:
		vsh_overlay_state() = default;
		vsh_overlay_state(const vsh_overlay_state&) = delete;
		vsh_overlay_state& operator=(const vsh_overlay_state&) = delete;
		~vsh_overlay_state();

		u64 latest_published_generation() const;
		bool overlay_active() const;
		void set_overlay_active(bool active);
		u32 slot_index(const vsh_overlay_slot& slot) const;

		vsh_overlay_slot* try_acquire_slot(vk::render_device& dev, u32 owner_pid, u32 width, u32 height, VkFormat format);
		void publish(vsh_overlay_slot& slot);
		void release(vsh_overlay_slot& slot);
		void reset();
	};
}
