#include "stdafx.h"
#include "sys_process.h"
#include "Emu/multiproc_debug.h"
#include "Emu/Memory/vm.h"
#include "Emu/Memory/vm_ptr.h"
#include "Emu/Memory/vm_var.h"
#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/VFS.h"
#include "Emu/vfs_config.h"
#include "Emu/IdManager.h"

#include "Crypto/unedat.h"
#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUInterpreter.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/PPUCallback.h"
#include "sys_lwmutex.h"
#include "sys_lwcond.h"
#include "sys_mutex.h"
#include "sys_cond.h"
#include "sys_event.h"
#include "sys_event_flag.h"
#include "sys_interrupt.h"
#include "sys_memory.h"
#include "sys_mmapper.h"
#include "sys_prx.h"
#include "sys_overlay.h"
#include "sys_ppu_thread.h"
#include "sys_rwlock.h"
#include "sys_semaphore.h"
#include "sys_timer.h"
#include "sys_fs.h"
#include "sys_vm.h"
#include "sys_spu.h"

// Check all flags known to be related to extended permissions (TODO)
// It's possible anything which has root flags implicitly has debug perm as well
// But I haven't confirmed it.
bool ps3_process_info_t::debug_or_root() const
{
	return (ctrl_flags1 & (0xe << 28)) != 0;
}

bool ps3_process_info_t::has_root_perm() const
{
	return (ctrl_flags1 & (0xc << 28)) != 0;
}

bool ps3_process_info_t::has_debug_perm() const
{
	return (ctrl_flags1 & (0xa << 28)) != 0;
}

// If a SELF file is of CellOS return its filename, otheriwse return an empty string
std::string_view ps3_process_info_t::get_cellos_appname() const
{
	if (!has_root_perm() || !Emu.GetTitleID().empty())
	{
		return {};
	}

	return std::string_view(Emu.GetBoot()).substr(Emu.GetBoot().find_last_of('/') + 1);
}

LOG_CHANNEL(sys_process);

ps3_process_info_t g_ps3_process_info;

namespace
{
	std::mutex g_mcore_spawn_mutex;
	std::deque<u64> g_mcore_pending_base_keys;

	u64 find_mcore_base_key_in_buffer(u32 addr, u32 size)
	{
		if (!addr || !size)
		{
			return 0;
		}

		const u8* data = static_cast<const u8*>(vm::base(addr));

		for (u32 i = 0; i + 18 <= size; i++)
		{
			if (data[i] != '0' || (data[i + 1] != 'x' && data[i + 1] != 'X'))
			{
				continue;
			}

			u64 value = 0;
			bool valid = true;

			for (u32 n = 0; n < 16; n++)
			{
				const u8 c = data[i + 2 + n];
				u8 nybble = 0;

				if (c >= '0' && c <= '9')
				{
					nybble = c - '0';
				}
				else if (c >= 'a' && c <= 'f')
				{
					nybble = c - 'a' + 10;
				}
				else if (c >= 'A' && c <= 'F')
				{
					nybble = c - 'A' + 10;
				}
				else
				{
					valid = false;
					break;
				}

				value = (value << 4) | nybble;
			}

			if (valid && (value & 0xffffffff00000000ull) == 0x8005911000000000ull && (value & 0xf) == 0)
			{
				return value;
			}
		}

		return 0;
	}

	u64 pop_mcore_pending_base_key()
	{
		std::lock_guard lock(g_mcore_spawn_mutex);

		if (g_mcore_pending_base_keys.empty())
		{
			return 0;
		}

		const u64 key = g_mcore_pending_base_keys.front();
		g_mcore_pending_base_keys.pop_front();
		return key;
	}

	static void sys_process_mcore_host_spawn_callback(ppu_thread& ppu, ppu_opcode_t, be_t<u32>*, ppu_intrp_func*)
	{
		const u64 base_key = pop_mcore_pending_base_key();

		if (!base_key)
		{
			MPDBG_LOG(sys_process, "VSH_MCORE_HOST_SPAWN: result=0x%x reason=no_pending_key active=%u",
				CELL_ESRCH, Emu.current_process().pid());
			return;
		}

		const u64 parent_queue_key = base_key + 2;
		const u64 child_queue_key = base_key + 4;
		const u64 child_port_name = base_key + 5;

		MPDBG_LOG(sys_process, "VSH_MCORE_HOST_SPAWN: phase=start base_key=0x%llx parent_queue_key=0x%llx child_queue_key=0x%llx child_port_name=0x%llx active=%u",
			base_key, parent_queue_key, child_queue_key, child_port_name, Emu.current_process().pid());

		bool parent_queue_seen = false;

		for (u32 attempt = 0; attempt < 1000; attempt++)
		{
			if (lv2_event_queue::find(parent_queue_key))
			{
				parent_queue_seen = true;
				break;
			}

			thread_ctrl::wait_for(1000);
		}

		if (!parent_queue_seen)
		{
			MPDBG_LOG(sys_process, "VSH_MCORE_HOST_SPAWN: result=0x%x reason=parent_queue_timeout base_key=0x%llx parent_queue_key=0x%llx active=%u",
				CELL_ETIMEDOUT, base_key, parent_queue_key, Emu.current_process().pid());
			return;
		}

		vm::var<sys_event_queue_attribute_t> attr;
		attr->protocol = SYS_SYNC_PRIORITY;
		attr->type = SYS_PPU_QUEUE;
		attr->name_u64 = 0x766d335f;

		vm::var<u32> child_queue_id;
		error_code result = sys_event_queue_create(ppu, child_queue_id, attr, child_queue_key, 4);
		if (result < 0 && !lv2_event_queue::find(child_queue_key))
		{
			MPDBG_LOG(sys_process, "VSH_MCORE_HOST_SPAWN: result=0x%x reason=child_queue_create base_key=0x%llx child_queue_key=0x%llx active=%u",
				static_cast<u32>(static_cast<s32>(result)), base_key, child_queue_key, Emu.current_process().pid());
			return;
		}

		vm::var<u32> child_port_id;
		result = sys_event_port_create(ppu, child_port_id, SYS_EVENT_PORT_IPC, child_port_name);
		if (result < 0)
		{
			MPDBG_LOG(sys_process, "VSH_MCORE_HOST_SPAWN: result=0x%x reason=child_port_create base_key=0x%llx child_port_name=0x%llx active=%u",
				static_cast<u32>(static_cast<s32>(result)), base_key, child_port_name, Emu.current_process().pid());
			return;
		}

		result = sys_event_port_connect_ipc(ppu, *child_port_id, parent_queue_key);
		if (result < 0)
		{
			sys_event_port_destroy(ppu, *child_port_id);
			MPDBG_LOG(sys_process, "VSH_MCORE_HOST_SPAWN: result=0x%x reason=connect_parent base_key=0x%llx port_id=0x%x parent_queue_key=0x%llx active=%u",
				static_cast<u32>(static_cast<s32>(result)), base_key, *child_port_id, parent_queue_key, Emu.current_process().pid());
			return;
		}

		result = sys_event_port_send(*child_port_id, 0, 0, 0);
		if (result < 0)
		{
			sys_event_port_disconnect(ppu, *child_port_id);
			sys_event_port_destroy(ppu, *child_port_id);
			MPDBG_LOG(sys_process, "VSH_MCORE_HOST_SPAWN: result=0x%x reason=send_parent base_key=0x%llx port_id=0x%x parent_queue_key=0x%llx active=%u",
				static_cast<u32>(static_cast<s32>(result)), base_key, *child_port_id, parent_queue_key, Emu.current_process().pid());
			return;
		}

		MPDBG_LOG(sys_process, "VSH_MCORE_HOST_SPAWN: phase=handshake_sent base_key=0x%llx child_queue_id=0x%x child_queue_key=0x%llx child_port_id=0x%x child_port_name=0x%llx active=%u",
			base_key, *child_queue_id, child_queue_key, *child_port_id, child_port_name, Emu.current_process().pid());

		MPDBG_LOG(sys_process, "VSH_MCORE_HOST_SPAWN: phase=post_handshake_no_active_switch active_pid=%u input_pid=%u present_pid=%u",
			Emu.current_process().pid(), Emu.GetInputForegroundPid(), Emu.GetForegroundPresentPid());

		MPDBG_LOG(sys_process, "VSH_MCORE_HOST_SPAWN: result=0x0 reason=event_driven_listener base_key=0x%llx child_queue_key=0x%llx child_port_id=0x%x child_port_name=0x%llx active=%u",
			base_key, child_queue_key, *child_port_id, child_port_name, Emu.current_process().pid());
	}

	error_code sys_process_spawn_mcore_host_helper(u64 base_key, u32 prio)
	{
		constexpr u32 helper_stack_size = 64 * 1024;

		auto& dct = fxo::get<lv2_memory_container>();
		if (!dct.take(helper_stack_size))
		{
			return CELL_ENOMEM;
		}

		const vm::addr_t stack_base{vm::alloc(helper_stack_size, vm::stack, 4096)};
		if (!stack_base)
		{
			dct.free(helper_stack_size);
			return CELL_ENOMEM;
		}

		ppu_thread_params p{};
		p.stack_addr = stack_base;
		p.stack_size = helper_stack_size;
		p.entry = ppu_func_opd_t{};

		const auto helper = idm::make_ptr<named_thread<ppu_thread>>(p, "mcore_host_spawn", prio, 1);
		if (!helper)
		{
			vm::dealloc(stack_base);
			dct.free(helper_stack_size);
			return CELL_EAGAIN;
		}

		{
			std::lock_guard lock(g_mcore_spawn_mutex);
			g_mcore_pending_base_keys.emplace_back(base_key);
		}

		helper->cmd_list({
			{ppu_cmd::ptr_call, 0}, std::bit_cast<u64>(&sys_process_mcore_host_spawn_callback),
			{ppu_cmd::sleep, 0},
		});

		helper->state.test_and_reset(cpu_flag::stop);
		ensure(lv2_obj::awake(helper.get()));
		helper->cmd_notify.store(1);
		helper->cmd_notify.notify_one();

		MPDBG_LOG(sys_process, "VSH_MCORE_HOST_SPAWN: phase=queued helper_id=0x%x base_key=0x%llx stack=0x%x active=%u",
			helper->id, base_key, static_cast<u32>(stack_base), Emu.current_process().pid());

		return CELL_OK;
	}
}

s32 process_getpid()
{
	return ::narrow<s32>(Emu.current_process().pid());
}

s32 sys_process_getpid()
{
	const s32 pid = process_getpid();
	sys_process.trace("sys_process_getpid() -> %d", pid);
	return pid;
}

s32 sys_process_getppid()
{
	sys_process.todo("sys_process_getppid() -> 0");
	return 0;
}

template <typename T, typename Get>
u32 idm_get_count()
{
	return idm::select<T, Get>([&](u32, Get&) {});
}

error_code sys_process_get_number_of_object(u32 object, vm::ptr<u32> nump)
{
	sys_process.error("sys_process_get_number_of_object(object=0x%x, nump=*0x%x)", object, nump);

	switch(object)
	{
	case SYS_MEM_OBJECT: *nump = idm_get_count<lv2_obj, lv2_memory>(); break;
	case SYS_MUTEX_OBJECT: *nump = idm_get_count<lv2_obj, lv2_mutex>(); break;
	case SYS_COND_OBJECT: *nump = idm_get_count<lv2_obj, lv2_cond>(); break;
	case SYS_RWLOCK_OBJECT: *nump = idm_get_count<lv2_obj, lv2_rwlock>(); break;
	case SYS_INTR_TAG_OBJECT: *nump = idm_get_count<lv2_obj, lv2_int_tag>(); break;
	case SYS_INTR_SERVICE_HANDLE_OBJECT: *nump = idm_get_count<lv2_obj, lv2_int_serv>(); break;
	case SYS_EVENT_QUEUE_OBJECT: *nump = idm_get_count<lv2_obj, lv2_event_queue>(); break;
	case SYS_EVENT_PORT_OBJECT: *nump = idm_get_count<lv2_obj, lv2_event_port>(); break;
	case SYS_TRACE_OBJECT: sys_process.error("sys_process_get_number_of_object: object = SYS_TRACE_OBJECT"); *nump = 0; break;
	case SYS_SPUIMAGE_OBJECT: *nump = idm_get_count<lv2_obj, lv2_spu_image>(); break;
	case SYS_PRX_OBJECT: *nump = idm_get_count<lv2_obj, lv2_prx>(); break;
	case SYS_SPUPORT_OBJECT: sys_process.error("sys_process_get_number_of_object: object = SYS_SPUPORT_OBJECT"); *nump = 0; break;
	case SYS_OVERLAY_OBJECT: *nump = idm_get_count<lv2_obj, lv2_overlay>(); break;
	case SYS_LWMUTEX_OBJECT: *nump = idm_get_count<lv2_obj, lv2_lwmutex>(); break;
	case SYS_TIMER_OBJECT: *nump = idm_get_count<lv2_obj, lv2_timer>(); break;
	case SYS_SEMAPHORE_OBJECT: *nump = idm_get_count<lv2_obj, lv2_sema>(); break;
	case SYS_FS_FD_OBJECT: *nump = idm_get_count<lv2_fs_object, lv2_fs_object>(); break;
	case SYS_LWCOND_OBJECT: *nump = idm_get_count<lv2_obj, lv2_lwcond>(); break;
	case SYS_EVENT_FLAG_OBJECT: *nump = idm_get_count<lv2_obj, lv2_event_flag>(); break;

	default:
	{
		return CELL_EINVAL;
	}
	}

	return CELL_OK;
}

#include <set>

template <typename T, typename Get>
void idm_get_set(std::set<u32>& out)
{
	idm::select<T, Get>([&](u32 id, Get&)
	{
		out.emplace(id);
	});
}

static error_code process_get_id(u32 object, vm::ptr<u32> buffer, u32 size, vm::ptr<u32> set_size)
{
	std::set<u32> objects;

	switch (object)
	{
	case SYS_MEM_OBJECT: idm_get_set<lv2_obj, lv2_memory>(objects); break;
	case SYS_MUTEX_OBJECT: idm_get_set<lv2_obj, lv2_mutex>(objects); break;
	case SYS_COND_OBJECT: idm_get_set<lv2_obj, lv2_cond>(objects); break;
	case SYS_RWLOCK_OBJECT: idm_get_set<lv2_obj, lv2_rwlock>(objects); break;
	case SYS_INTR_TAG_OBJECT: idm_get_set<lv2_obj, lv2_int_tag>(objects); break;
	case SYS_INTR_SERVICE_HANDLE_OBJECT: idm_get_set<lv2_obj, lv2_int_serv>(objects); break;
	case SYS_EVENT_QUEUE_OBJECT: idm_get_set<lv2_obj, lv2_event_queue>(objects); break;
	case SYS_EVENT_PORT_OBJECT: idm_get_set<lv2_obj, lv2_event_port>(objects); break;
	case SYS_TRACE_OBJECT: fmt::throw_exception("SYS_TRACE_OBJECT");
	case SYS_SPUIMAGE_OBJECT: idm_get_set<lv2_obj, lv2_spu_image>(objects); break;
	case SYS_PRX_OBJECT: idm_get_set<lv2_obj, lv2_prx>(objects); break;
	case SYS_OVERLAY_OBJECT: idm_get_set<lv2_obj, lv2_overlay>(objects); break;
	case SYS_LWMUTEX_OBJECT: idm_get_set<lv2_obj, lv2_lwmutex>(objects); break;
	case SYS_TIMER_OBJECT: idm_get_set<lv2_obj, lv2_timer>(objects); break;
	case SYS_SEMAPHORE_OBJECT: idm_get_set<lv2_obj, lv2_sema>(objects); break;
	case SYS_FS_FD_OBJECT: idm_get_set<lv2_fs_object, lv2_fs_object>(objects); break;
	case SYS_LWCOND_OBJECT: idm_get_set<lv2_obj, lv2_lwcond>(objects); break;
	case SYS_EVENT_FLAG_OBJECT: idm_get_set<lv2_obj, lv2_event_flag>(objects); break;
	case SYS_SPUPORT_OBJECT: fmt::throw_exception("SYS_SPUPORT_OBJECT");
	default:
	{
		return CELL_EINVAL;
	}
	}

	u32 i = 0;

	// NOTE: Treats negative and 0 values as 1 due to signed checks and "do-while" behavior of fw
	for (auto id = objects.begin(); i < std::max<s32>(size, 1) + 0u && id != objects.end(); id++, i++)
	{
		buffer[i] = *id;
	}

	*set_size = i;

	return CELL_OK;
}

error_code sys_process_get_id(u32 object, vm::ptr<u32> buffer, u32 size, vm::ptr<u32> set_size)
{
	sys_process.error("sys_process_get_id(object=0x%x, buffer=*0x%x, size=%d, set_size=*0x%x)", object, buffer, size, set_size);

	if (object == SYS_SPUPORT_OBJECT)
	{
		// Unallowed for this syscall
		return CELL_EINVAL;
	}

	return process_get_id(object, buffer, size, set_size);
}

error_code sys_process_get_id2(u32 object, vm::ptr<u32> buffer, u32 size, vm::ptr<u32> set_size)
{
	sys_process.error("sys_process_get_id2(object=0x%x, buffer=*0x%x, size=%d, set_size=*0x%x)", object, buffer, size, set_size);

	if (!g_ps3_process_info.has_root_perm())
	{
		// This syscall is more capable than sys_process_get_id but also needs a root perm check
		return CELL_ENOSYS;
	}

	return process_get_id(object, buffer, size, set_size);
}

CellError process_is_spu_lock_line_reservation_address(u32 addr, u64 flags)
{
	if (!flags || flags & ~(SYS_MEMORY_ACCESS_RIGHT_SPU_THR | SYS_MEMORY_ACCESS_RIGHT_RAW_SPU))
	{
		return CELL_EINVAL;
	}

	// TODO: respect sys_mmapper region's access rights
	switch (addr >> 28)
	{
	case 0x0: // Main memory
	case 0x1: // Main memory
	case 0x2: // User 64k (sys_memory)
	case 0xc: // RSX Local memory
	case 0xe: // RawSPU MMIO
		break;

	case 0xf: // Private SPU MMIO
	{
		if (flags & SYS_MEMORY_ACCESS_RIGHT_RAW_SPU)
		{
			// Cannot be accessed by RawSPU
			return CELL_EPERM;
		}

		break;
	}

	case 0xd: // PPU Stack area
		return CELL_EPERM;
	default:
	{
		if (auto vm0 = idm::get_unlocked<sys_vm_t>(sys_vm_t::find_id(addr)))
		{
			// sys_vm area was not covering the address specified but made a reservation on the entire 256mb region
			if (vm0->addr + vm0->size - 1 < addr)
			{
				return CELL_EINVAL;
			}

			// sys_vm memory is not allowed
			return CELL_EPERM;
		}

		if (!vm::get(vm::any, addr & -0x1000'0000))
		{
			return CELL_EINVAL;
		}

		break;
	}
	}

	return {};
}

error_code sys_process_is_spu_lock_line_reservation_address(u32 addr, u64 flags)
{
	sys_process.warning("sys_process_is_spu_lock_line_reservation_address(addr=0x%x, flags=0x%llx)", addr, flags);

	if (auto err = process_is_spu_lock_line_reservation_address(addr, flags))
	{
		return err;
	}

	return CELL_OK;
}

error_code _sys_process_get_paramsfo(vm::ptr<char> buffer)
{
	sys_process.warning("_sys_process_get_paramsfo(buffer=0x%x)", buffer);

	if (Emu.GetTitleID().empty())
	{
		return CELL_ENOENT;
	}

	memset(buffer.get_ptr(), 0, 0x40);
	memcpy(buffer.get_ptr() + 1, Emu.GetTitleID().c_str(), std::min<usz>(Emu.GetTitleID().length(), 9));

	return CELL_OK;
}

s32 process_get_sdk_version(u32 /*pid*/, s32& ver)
{
	// get correct SDK version for selected pid
	ver = g_ps3_process_info.sdk_ver;

	return CELL_OK;
}

error_code sys_process_get_sdk_version(u32 pid, vm::ptr<s32> version)
{
	sys_process.warning("sys_process_get_sdk_version(pid=0x%x, version=*0x%x)", pid, version);

	s32 sdk_ver;
	const s32 ret = process_get_sdk_version(pid, sdk_ver);
	if (ret != CELL_OK)
	{
		return CellError{ret + 0u}; // error code
	}
	else
	{
		*version = sdk_ver;
		return CELL_OK;
	}
}

error_code sys_process_kill(u32 pid)
{
	sys_process.todo("sys_process_kill(pid=0x%x)", pid);
	return CELL_OK;
}

error_code sys_process_wait_for_child(u32 pid, vm::ptr<u32> status, u64 unk)
{
	sys_process.todo("sys_process_wait_for_child(pid=0x%x, status=*0x%x, unk=0x%llx", pid, status, unk);

	return CELL_OK;
}

error_code sys_process_wait_for_child2(u64 unk1, u64 unk2, u64 unk3, u64 unk4, u64 unk5, u64 unk6)
{
	sys_process.todo("sys_process_wait_for_child2(unk1=0x%llx, unk2=0x%llx, unk3=0x%llx, unk4=0x%llx, unk5=0x%llx, unk6=0x%llx)",
		unk1, unk2, unk3, unk4, unk5, unk6);
	return CELL_OK;
}

error_code sys_process_get_status(u64 unk)
{
	sys_process.todo("sys_process_get_status(unk=0x%llx)", unk);
	if (auto ppu = cpu_thread::get_current<ppu_thread>(); ppu && ppu->owner_pid == 1)
	{
		MPDBG_LOG(sys_process, "PROCESS_GET_STATUS: owner_pid=%u id=0x%x name=%s unk=0x%llx active_pid=%u ret=0x%x",
			ppu->owner_pid, ppu->id, ppu->get_name(), unk, Emu.current_process().pid(), static_cast<u32>(CELL_OK));
	}
	//vm::write32(CPU.gpr[4], GetPPUThreadStatus(CPU));
	return CELL_OK;
}

error_code sys_process_detach_child(u64 unk)
{
	sys_process.todo("sys_process_detach_child(unk=0x%llx)", unk);
	return CELL_OK;
}

extern void signal_system_cache_can_stay();

void _sys_process_exit(ppu_thread& ppu, s32 status, u32 arg2, u32 arg3)
{
	ppu.state += cpu_flag::wait;

	sys_process.warning("_sys_process_exit(status=%d, arg2=0x%x, arg3=0x%x)", status, arg2, arg3);

	// One-shot consumption: if this process belongs to a VSH-rooted boot chain, relaunch VSH
	// after the kill instead of stopping emulation. VSH itself never sets the flag for itself
	// (it's set by lv2_exitspawn for downstream processes), so VSH shutting down still stops.
	const bool boot_vsh_after_exit = Emu.IsLaunchedFromVsh();

	sys_process.success("[VSH] _sys_process_exit: launched_from_vsh=%d, will_boot_vsh=%d", static_cast<int>(boot_vsh_after_exit), static_cast<int>(boot_vsh_after_exit));

	Emu.CallFromMainThread([boot_vsh_after_exit]()
	{
		sys_process.success("Process finished");
		signal_system_cache_can_stay();

		sys_process.success("[VSH] _sys_process_exit (main thread): boot_vsh_after_exit=%d", static_cast<int>(boot_vsh_after_exit));

		if (boot_vsh_after_exit)
		{
			// Non-destructive return to VSH: destroy game process, switch back to VSH
			const u32 current_pid = Emu.current_process().pid();
			sys_process.success("[VSH] non-destructive exit: destroying pid=%u, returning to VSH (pid=1)", current_pid);

			Emu.destroy_process(current_pid);
			Emu.set_active_process(1); // Switch back to VSH
			Emu.resume_process(1);     // Unpark VSH threads
		}
		else
		{
			Emu.Kill();
		}
	});

	// Wait for GUI thread
	while (auto state = +ppu.state)
	{
		if (is_stopped(state))
		{
			break;
		}

		ppu.state.wait(state);
	}
}

void _sys_process_exit2(ppu_thread& ppu, s32 status, vm::ptr<sys_exit2_param> arg, u32 arg_size, u32 arg4)
{
	ppu.state += cpu_flag::wait;

	sys_process.warning("_sys_process_exit2(status=%d, arg=*0x%x, arg_size=0x%x, arg4=0x%x)", status, arg, arg_size, arg4);

	auto pstr = +arg->args;

	std::vector<std::string> argv;
	std::vector<std::string> envp;

	while (auto ptr = *pstr++)
	{
		argv.emplace_back(ptr.get_ptr());
		sys_process.notice(" *** arg: %s", ptr);
	}

	while (auto ptr = *pstr++)
	{
		envp.emplace_back(ptr.get_ptr());
		sys_process.notice(" *** env: %s", ptr);
	}

	std::vector<u8> data;

	if (arg_size > 0x1030)
	{
		data.resize(0x1000);
		std::memcpy(data.data(), vm::base(arg.addr() + arg_size - 0x1000), 0x1000);
	}

	if (argv.empty())
	{
		return _sys_process_exit(ppu, status, 0, 0);
	}

	// TODO: set prio, flags

	lv2_exitspawn(ppu, argv, envp, data);
}

void lv2_exitspawn(ppu_thread& ppu, std::vector<std::string>& argv, std::vector<std::string>& envp, std::vector<u8>& data)
{
	ppu.state += cpu_flag::wait;

	// sys_sm_shutdown
	const bool is_real_reboot = (ppu.gpr[11] == 379);

	// Capture VSH-rooted state before Kill destroys per-process state. Either we are VSH itself
	// launching a downstream process (e.g. a game from XMB), or we are already in a VSH-rooted
	// chain (game→game exitspawn) and need to keep the flag sticky.
	const bool is_from_vsh = Emu.IsVsh() || Emu.IsLaunchedFromVsh();

	Emu.CallFromMainThread([is_real_reboot, is_from_vsh, argv = std::move(argv), envp = std::move(envp), data = std::move(data)]() mutable
	{
		sys_process.success("Process finished -> %s", argv[0]);

		std::string disc;

		if (Emu.GetCat() == "DG" || Emu.GetCat() == "GD")
			disc = vfs::get("/dev_bdvd/");
		if (disc.empty() && !Emu.GetTitleID().empty())
			disc = vfs::get(Emu.GetDir());

		std::string path = vfs::get(argv[0]);
		std::string hdd1 = vfs::get("/dev_hdd1/");

		const u128 klic = fxo::get<loaded_npdrm_keys>().last_key();

		// Non-destructive VSH→game launch: create a second process and switch to it
		if (is_from_vsh && !is_real_reboot)
		{
			u32 game_pid = Emu.create_process();
			if (!game_pid)
			{
				sys_process.fatal("Failed to create game process — falling back to destructive launch");
			}
			else
			{
				sys_process.success("Created game process pid=%u, switching from VSH (pid=1)", game_pid);

				// Dispatch to main thread — the current VSH PPU thread cannot suspend itself
				Emu.CallFromMainThread([game_pid, disc = std::move(disc), path = std::move(path),
					hdd1 = std::move(hdd1), klic, argv = std::move(argv), envp = std::move(envp),
					data = std::move(data)]() mutable
				{
					const bool use_vsh_native_overlay = static_cast<bool>(g_cfg.misc.use_vsh_native_overlay);
					Emu.SetUseVshNativeOverlay(use_vsh_native_overlay);
					Emu.set_active_process(game_pid, /*suspend_outgoing=*/false); // Co-resident: keep VSH alive while game runs

					Emu.current_process().RefArgv() = std::move(argv);
					Emu.current_process().RefEnvp() = std::move(envp);
					Emu.current_process().RefData() = std::move(data);
					Emu.current_process().RefDisc() = std::move(disc);
					Emu.current_process().RefHdd1() = std::move(hdd1);

					if (klic)
					{
						Emu.current_process().RefKlic().emplace_back(klic);
					}

					Emu.SetForceBoot(true);
					Emu.SetLaunchedFromVsh(true);

					// Co-resident load: skip destructive Init/Load to keep VSH alive
					Emu.EnterCoResidentLoad();
					Emu.BootGame(path, "", true, cfg_mode::continuous, Emu.GetUsedConfig(), Emu.GetUsedDatabaseConfig());
					Emu.ExitCoResidentLoad();

					// Co-resident boot does not get the standard FinalizeRunRequest
					// fire path because lv2_obj::sleep_unlocked's "Final Thread"
					// branch is gated on global g_to_sleep being empty — pid 1
					// threads keep it non-empty under co-resident, so the
					// transition from starting to running never fires
					// automatically. Trigger it explicitly here so this process
					// exits the starting state and rsx::thread can leave its
					// initial wait loop.
					if (Emu.IsStarting())
					{
						// Clear cpu_flag::stop on this process's PPU threads BEFORE we transition
						// state to running. The standard RunPPU fire path goes through Run()->
						// line 2735 which is gated on rsx->is_initialized — that flip is a race
						// with rsx::thread's worker calling on_task. Lose the race and the
						// fallback (rsx::thread on_task line 996 queuing RunPPU via
						// CallFromMainThread) runs AFTER FinalizeRunRequest below has already
						// transitioned state, at which point RunPPU's graceful early-return
						// skips the work and main_thread parks with stop still set.
						Emu.RunPPU();
						Emu.FinalizeRunRequest();
					}

					sys_process.success("[VSH] non-destructive launch: booted %s", path);
				});
				return;
			}
		}

		using namespace id_manager;

		shared_ptr<utils::serial> idm_capture = make_shared<utils::serial>();

		if (!is_real_reboot)
		{
			reader_lock rlock{id_manager::g_mutex};
			fxo::get<id_map<lv2_memory_container>>().save(*idm_capture);
			stx::serial_breathe_and_tag(*idm_capture, "id_map<lv2_memory_container>", false);
		}

		idm_capture->set_reading_state();

		auto func = [is_real_reboot, old_size = fxo::get<lv2_memory_container>().size, idm_capture](u32 sdk_suggested_mem) mutable
		{
			if (is_real_reboot)
			{
				// Do not save containers on actual reboot
				ensure(fxo::init<id_map<lv2_memory_container>>());
			}
			else
			{
				// Save LV2 memory containers
				ensure(fxo::init<id_map<lv2_memory_container>>(*idm_capture));
			}

			// Empty the containers, accumulate their total size
			u32 total_size = 0;
			idm::select<lv2_memory_container>([&](u32, lv2_memory_container& ctr)
			{
				ctr.used = 0;
				total_size += ctr.size;
			});

			// The default memory container capacity can only decrease after exitspawn
			// 1. If newer SDK version suggests higher memory capacity - it is ignored
			// 2. If newer SDK version suggests lower memory capacity - it is lowered
			// And if 2. happens while user memory containers exist, the left space can be spent on user memory containers
			ensure(fxo::init<lv2_memory_container>(std::min(old_size - total_size, sdk_suggested_mem) + total_size));
		};

		Emu.current_process().RefAfterKillCallback() = [is_from_vsh, func = std::move(func), argv = std::move(argv), envp = std::move(envp), data = std::move(data),
			disc = std::move(disc), path = std::move(path), hdd1 = std::move(hdd1), old_config = Emu.GetUsedConfig(), old_db_config = Emu.GetUsedDatabaseConfig(), klic]() mutable
		{
			Emu.current_process().RefArgv() = std::move(argv);
			Emu.current_process().RefEnvp() = std::move(envp);
			Emu.current_process().RefData() = std::move(data);
			Emu.current_process().RefDisc() = std::move(disc);
			Emu.current_process().RefHdd1() = std::move(hdd1);
			Emu.current_process().RefInitMemContainers() = std::move(func);

			if (klic)
			{
				Emu.current_process().RefKlic().emplace_back(klic);
			}

			Emu.SetForceBoot(true);

			auto res = Emu.BootGame(path, "", true, cfg_mode::continuous, old_config, old_db_config);

			if (res != game_boot_result::no_errors)
			{
				sys_process.fatal("Failed to boot from exitspawn! (path=\"%s\", error=%s)", path, res);
			}
			else if (is_from_vsh)
			{
				// BootGame() resets the flag on non-continuous boots only; we are continuous,
				// but set explicitly to make the propagation obvious and avoid coupling.
				Emu.SetLaunchedFromVsh(true);
				sys_process.success("[VSH] lv2_exitspawn: marked %s as launched-from-VSH (IsLaunchedFromVsh=%d)", path, static_cast<int>(Emu.IsLaunchedFromVsh()));
			}
			else
			{
				sys_process.success("[VSH] lv2_exitspawn: NOT marking %s as VSH-rooted (is_from_vsh=false)", path);
			}
		};

		signal_system_cache_can_stay();

		// Make sure we keep the game window opened
		Emu.SetContinuousMode(true);
		Emu.Kill(false);
	});

	if (is_from_vsh && !is_real_reboot)
	{
		// Co-resident VSH launches keep the caller process alive. Do not enter
		// the destructive exitspawn wait path, which waits for VSH's caller PPU
		// to be stopped and leaves PAF locks held forever.
		ppu.check_state();
		return;
	}

	// Wait for GUI thread
	while (auto state = +ppu.state)
	{
		if (is_stopped(state))
		{
			break;
		}

		ppu.state.wait(state);
	}
}

void sys_process_exit3(ppu_thread& ppu, s32 status)
{
	ppu.state += cpu_flag::wait;

	sys_process.warning("_sys_process_exit3(status=%d)", status);

	return _sys_process_exit(ppu, status, 0, 0);
}

error_code sys_process_spawns_a_self2(ppu_thread& ppu, vm::ptr<u32> pid, u32 primary_prio, u64 flags, vm::ptr<void> stack, u32 stack_size, u32 mem_id, vm::ptr<void> param_sfo, vm::ptr<void> dbg_data)
{
	sys_process.warning("sys_process_spawns_a_self2(pid=*0x%x, primary_prio=0x%x, flags=0x%llx, stack=*0x%x, stack_size=0x%x, mem_id=0x%x, param_sfo=*0x%x, dbg_data=*0x%x)"
		, pid, primary_prio, flags, stack, stack_size, mem_id, param_sfo, dbg_data);

	// Hex+ASCII dump of an opaque buffer to discover the SELF path encoding.
	auto dump_buffer = [](const char* name, u32 addr, u32 size)
	{
		if (!addr || !size)
		{
			sys_process.warning("  %s: <null or empty>", name);
			return;
		}

		const u8* data = static_cast<const u8*>(vm::base(addr));

		for (u32 row = 0; row < size; row += 16)
		{
			std::string hex;
			std::string ascii;
			const u32 row_len = std::min<u32>(16, size - row);

			for (u32 i = 0; i < row_len; i++)
			{
				const u8 b = data[row + i];
				fmt::append(hex, "%02x ", b);
				ascii += (b >= 0x20 && b < 0x7f) ? static_cast<char>(b) : '.';
			}

			while (hex.size() < 48) hex += ' ';
			sys_process.warning("  %s @ 0x%08x +0x%04x: %s| %s", name, addr, row, hex, ascii);
		}
	};

	dump_buffer("stack    ", stack.addr(), stack_size);
	dump_buffer("param_sfo", param_sfo.addr(), 0x200);
	dump_buffer("dbg_data ", dbg_data.addr(), 0x200);

	// Heuristic path extraction: scan all three buffers for a NUL-terminated ASCII string
	// that begins with "/dev_" and looks like a SELF/EBOOT path. Real PS3 firmware encodes
	// the spawn target inside one of these structs; without official docs we look for it.
	auto find_path = [](u32 addr, u32 size) -> std::string
	{
		if (!addr || !size) return {};

		const u8* data = static_cast<const u8*>(vm::base(addr));

		for (u32 i = 0; i + 5 < size; i++)
		{
			if (std::memcmp(data + i, "/dev_", 5) != 0) continue;

			// Found a candidate; copy until NUL or first non-printable.
			std::string s;
			for (u32 j = i; j < size && data[j] && data[j] >= 0x20 && data[j] < 0x7f && s.size() < 0x200; j++)
			{
				s += static_cast<char>(data[j]);
			}

			// Plausibility filter: must look like a SELF/EBOOT/SPRX path.
			if (s.ends_with(".BIN") || s.ends_with(".bin") || s.ends_with(".SELF") || s.ends_with(".self") || s.ends_with(".SPRX") || s.ends_with(".sprx"))
			{
				return s;
			}
		}

		return {};
	};

	std::string vfs_path = find_path(stack.addr(), stack_size);
	if (vfs_path.empty()) vfs_path = find_path(dbg_data.addr(), 0x200);
	if (vfs_path.empty()) vfs_path = find_path(param_sfo.addr(), 0x200);

	if (vfs_path.empty())
	{
		sys_process.warning("sys_process_spawns_a_self2: no SELF path found in any buffer");
		if (pid) *pid = 0;
		return CELL_OK;
	}

	sys_process.success("sys_process_spawns_a_self2: target path = %s", vfs_path);

	// sys_process_spawns_a_self2 has two very different real-world uses on PS3:
	//   1. VSH spawns *helper* SELFs (/dev_flash/vsh/module/mcore.self etc.) as child
	//      processes that run alongside VSH. We can't model these without true multi-process
	//      support — exitspawn'ing them kills VSH and creates an infinite respawn loop. Stub.
	//   2. VSH spawns a *game* (/dev_hdd0/game/<TID>/USRDIR/EBOOT.BIN or the disc EBOOT).
	//      Semantically the game replaces VSH from the user's perspective, so we route this
	//      through lv2_exitspawn — same machinery as sys_game_process_exitspawn.
	//
	// The path is the most reliable discriminator. Helper prio is 0x7d6, game prio is 0x3e9,
	// but trusting the path keeps the heuristic robust against unrelated low-prio game spawns.
	const bool is_game_launch =
		(vfs_path.starts_with("/dev_hdd0/game/") && vfs_path.ends_with("/USRDIR/EBOOT.BIN")) ||
		vfs_path == "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN";

	if (!is_game_launch)
	{
		if (vfs_path == "/dev_flash/vsh/module/mcore.self")
		{
			u64 base_key = find_mcore_base_key_in_buffer(stack.addr(), stack_size);
			if (!base_key) base_key = find_mcore_base_key_in_buffer(dbg_data.addr(), 0x200);
			if (!base_key) base_key = find_mcore_base_key_in_buffer(param_sfo.addr(), 0x200);

			if (!base_key)
			{
				sys_process.warning("sys_process_spawns_a_self2: mcore.self helper spawn found no 0x800591... base key");
				MPDBG_LOG(sys_process, "VSH_MCORE_HOST_SPAWN: result=0x%x reason=no_base_key path=%s active=%u",
					CELL_ESRCH, vfs_path, Emu.current_process().pid());
				if (pid) *pid = 0;
				return CELL_OK;
			}

			if (base_key != 0x8005911000000020ull)
			{
				MPDBG_LOG(sys_process, "VSH_MCORE_HOST_SPAWN: result=0x0 reason=ignored_probe path=%s base_key=0x%llx active=%u",
					vfs_path, base_key, Emu.current_process().pid());
				if (pid) *pid = 0;
				return CELL_OK;
			}

			const error_code helper_result = sys_process_spawn_mcore_host_helper(base_key, primary_prio ? primary_prio : 2003);
			MPDBG_LOG(sys_process, "VSH_MCORE_HOST_SPAWN: path=%s base_key=0x%llx helper_result=0x%x active=%u",
				vfs_path, base_key, static_cast<u32>(static_cast<s32>(helper_result)), Emu.current_process().pid());
			if (pid) *pid = 0;
			return CELL_OK;
		}

		sys_process.warning("sys_process_spawns_a_self2: not a game launch path, ignoring (helper SELF — would need multi-process support to spawn as child)");
		if (pid) *pid = 0;
		return CELL_OK;
	}

	const std::string host_path = vfs::get(vfs_path);

	if (host_path.empty() || !fs::is_file(host_path))
	{
		sys_process.error("sys_process_spawns_a_self2: game EBOOT.BIN does not exist (vfs=%s, host=%s)", vfs_path, host_path);
		if (pid) *pid = 0;
		return CELL_OK;
	}

	sys_process.success("sys_process_spawns_a_self2: launching game %s (replacing VSH)", vfs_path);

	std::vector<std::string> argv = { vfs_path };
	std::vector<std::string> envp;
	std::vector<u8> data;

	if (pid) *pid = 1; // Caller may pass this to wait_for_child; placeholder is fine.

	lv2_exitspawn(ppu, argv, envp, data);

	return CELL_OK;
}
