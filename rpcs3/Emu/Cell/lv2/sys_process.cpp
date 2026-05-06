#include "stdafx.h"
#include "sys_process.h"
#include "Emu/Memory/vm_ptr.h"
#include "Emu/System.h"
#include "Emu/VFS.h"
#include "Emu/vfs_config.h"
#include "Emu/IdManager.h"

#include "Crypto/unedat.h"
#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"
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
			// Clear the flag so the VSH boot chain restarts cleanly. BootGame() also clears it
			// for non-continuous boots, but be explicit here.
			Emu.SetLaunchedFromVsh(false);

			Emu.current_process().RefAfterKillCallback() = []()
			{
				sys_process.success("[VSH] after_kill_callback firing — booting VSH");

				Emu.current_process().RefArgv().clear();
				Emu.current_process().RefEnvp().clear();
				Emu.current_process().RefData().clear();
				Emu.current_process().RefDisc().clear();
				Emu.current_process().RefHdd1().clear();
				Emu.current_process().RefKlic().clear();
				Emu.current_process().RefInitMemContainers() = nullptr;

				Emu.SetForceBoot(true);

				const std::string vsh_path = g_cfg_vfs.get_dev_flash() + "/vsh/module/vsh.self";
				const auto res = Emu.BootGame(vsh_path, "", true, cfg_mode::continuous);

				sys_process.success("[VSH] BootGame(vsh.self) returned: %s", res);

				if (res != game_boot_result::no_errors)
				{
					sys_process.fatal("Failed to relaunch VSH after game exit (path=\"%s\", error=%s)", vsh_path, res);
				}
			};

			Emu.SetContinuousMode(true);
			Emu.Kill(false);
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

		const u128 klic = g_fxo->get<loaded_npdrm_keys>().last_key();

		using namespace id_manager;

		shared_ptr<utils::serial> idm_capture = make_shared<utils::serial>();

		if (!is_real_reboot)
		{
			reader_lock rlock{id_manager::g_mutex};
			g_fxo->get<id_map<lv2_memory_container>>().save(*idm_capture);
			stx::serial_breathe_and_tag(*idm_capture, "id_map<lv2_memory_container>", false);
		}

		idm_capture->set_reading_state();

		auto func = [is_real_reboot, old_size = g_fxo->get<lv2_memory_container>().size, idm_capture](u32 sdk_suggested_mem) mutable
		{
			if (is_real_reboot)
			{
				// Do not save containers on actual reboot
				ensure(g_fxo->init<id_map<lv2_memory_container>>());
			}
			else
			{
				// Save LV2 memory containers
				ensure(g_fxo->init<id_map<lv2_memory_container>>(*idm_capture));
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
			ensure(g_fxo->init<lv2_memory_container>(std::min(old_size - total_size, sdk_suggested_mem) + total_size));
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
