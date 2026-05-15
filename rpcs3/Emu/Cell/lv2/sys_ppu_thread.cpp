#include "stdafx.h"
#include "sys_ppu_thread.h"

#include "Emu/System.h"
#include "Emu/multiproc_debug.h"
#include "Emu/IdManager.h"

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/PPUCallback.h"
#include "Emu/Cell/PPUOpcodes.h"
#include "Emu/Memory/vm_locking.h"
#include "sys_event.h"
#include "sys_process.h"
#include "sys_mmapper.h"
#include "sys_memory.h"

#include "util/asm.hpp"

#include <thread>

LOG_CHANNEL(sys_ppu_thread);

static bool vsh_child_process_active()
{
	return Emu.IsVshCoResident() && Emu.get_process_vm_base(2);
}

// Simple structure to cleanup previous thread, because can't remove its own thread
struct ppu_thread_cleaner
{
	shared_ptr<named_thread<ppu_thread>> old;

	shared_ptr<named_thread<ppu_thread>> clean(shared_ptr<named_thread<ppu_thread>> ptr)
	{
		return std::exchange(old, std::move(ptr));
	}

	ppu_thread_cleaner() = default;

	ppu_thread_cleaner(const ppu_thread_cleaner&) = delete;

	ppu_thread_cleaner& operator=(const ppu_thread_cleaner&) = delete;

	ppu_thread_cleaner& operator=(thread_state state) noexcept
	{
		reader_lock lock(id_manager::g_mutex);

		if (old)
		{
			// It is detached from IDM now so join must be done explicitly now
			*static_cast<named_thread<ppu_thread>*>(old.get()) = state;
		}

		return *this;
	}
};

void ppu_thread_exit(ppu_thread& ppu, ppu_opcode_t, be_t<u32>*, struct ppu_intrp_func*)
{
	ppu.state += cpu_flag::exit + cpu_flag::wait;

	// Deallocate Stack Area
	ensure(vm::dealloc_for_thread(ppu, ppu.stack_addr, vm::stack) == ppu.stack_size);

	if (auto dct = fxo::try_get<lv2_memory_container>())
	{
		dct->free(ppu.stack_size);
	}

	if (ppu.call_history.index)
	{
		ppu_log.notice("Calling history: %s", ppu.call_history);
		ppu.call_history.index = 0;
	}

	if (ppu.syscall_history.index)
	{
		ppu_log.notice("HLE/LV2 history: %s", ppu.syscall_history);
		ppu.syscall_history.index = 0;
	}
}

constexpr u32 c_max_ppu_name_size = 28;

void _sys_ppu_thread_exit(ppu_thread& ppu, u64 errorcode)
{
	ppu.state += cpu_flag::wait;
	u64 writer_mask = 0;

	sys_ppu_thread.trace("_sys_ppu_thread_exit(errorcode=0x%llx)", errorcode);

	if (vsh_child_process_active() && ppu.owner_pid == 1 &&
		std::string_view(ppu.get_name()).find("SceVshPower") != umax)
	{
		sys_ppu_thread.warning("_sys_ppu_thread_exit(errorcode=0x%llx): preserving VSH monitor thread '%s' while child app is active",
			errorcode, ppu.get_name());
		MPDBG_LOG(sys_ppu_thread, "VSH_NATIVE_MONITOR_REFUSE_THREAD_EXIT: id=0x%x name=%s errorcode=0x%llx active_pid=%u cia=0x%x lr=0x%x",
			ppu.id, ppu.get_name(), errorcode, Emu.current_process().pid(), ppu.cia, ppu.lr);
		ppu.state -= cpu_flag::wait;
		return;
	}

	ppu_join_status old_status;

	// Avoid cases where cleaning causes the destructor to be called inside IDM lock scope (for performance)
	shared_ptr<named_thread<ppu_thread>> old_ppu;
	{
		lv2_obj::notify_all_t notify;
		lv2_obj::prepare_for_sleep(ppu);

		std::lock_guard lock(id_manager::g_mutex);

		// Get joiner ID
		old_status = ppu.joiner.fetch_op([](ppu_join_status& status)
		{
			if (status == ppu_join_status::joinable)
			{
				// Joinable, not joined
				status = ppu_join_status::zombie;
				return;
			}

			// Set deleted thread status
			status = ppu_join_status::exited;
		});

		if (old_status >= ppu_join_status::max)
		{
			lv2_obj::append(idm::check_unlocked<named_thread<ppu_thread>>(static_cast<u32>(old_status)));
		}

		if (old_status != ppu_join_status::joinable)
		{
			// Remove self ID from IDM, move owning ptr
			old_ppu = g_fxo->get<ppu_thread_cleaner>().clean(idm::withdraw<named_thread<ppu_thread>>(ppu.id, 0, std::false_type{}));
		}

		// Get writers mask (wait for all current writers to quit)
		writer_mask = vm::g_range_lock_bits[1];

		// Unqueue
		lv2_obj::sleep(ppu);
		notify.cleanup();

		// Remove suspend state (TODO)
		ppu.state -= cpu_flag::suspend;
	}

	while (ppu.joiner == ppu_join_status::zombie)
	{
		if (ppu.is_stopped() && ppu.joiner.compare_and_swap_test(ppu_join_status::zombie, ppu_join_status::joinable))
		{
			// Abort
			ppu.state += cpu_flag::again;
			return;
		}

		// Wait for termination
		thread_ctrl::wait_on(ppu.joiner, ppu_join_status::zombie);
	}

	ppu_thread_exit(ppu, {}, nullptr, nullptr);

	if (old_ppu)
	{
		// It is detached from IDM now so join must be done explicitly now
		*old_ppu = thread_state::finished;
	}

	// Need to wait until the current writers finish
	if (ppu.state & cpu_flag::memory)
	{
		for (; writer_mask; writer_mask &= vm::g_range_lock_bits[1])
		{
			busy_wait(200);
		}
	}
}

s32 sys_ppu_thread_yield(ppu_thread& ppu)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.trace("sys_ppu_thread_yield()");

	const s32 success = lv2_obj::yield(ppu) ? CELL_OK : CELL_CANCEL;

	if (success == CELL_CANCEL)
	{
		// Do other work in the meantime
		lv2_obj::notify_all();
	}

	// Return 0 on successful context switch, 1 otherwise
	return success;
}

error_code sys_ppu_thread_join(ppu_thread& ppu, u32 thread_id, vm::ptr<u64> vptr)
{
	lv2_obj::prepare_for_sleep(ppu);

	sys_ppu_thread.trace("sys_ppu_thread_join(thread_id=0x%x, vptr=*0x%x)", thread_id, vptr);

	if (thread_id == ppu.id)
	{
		return CELL_EDEADLK;
	}

	auto thread = idm::get<named_thread<ppu_thread>>(thread_id, [&, notify = lv2_obj::notify_all_t()](ppu_thread& thread) -> CellError
	{
		if (vsh_child_process_active() && ppu.owner_pid == 1 &&
			std::string_view(thread.get_name()).find("SceVshPower") != umax)
		{
			MPDBG_LOG(sys_ppu_thread, "VSH_NATIVE_MONITOR_REFUSE_JOIN: caller_id=0x%x caller_name=%s target_id=0x%x target_name=%s target_cia=0x%x target_state=0x%x target_func=%s target_last=%s active_pid=%u",
				ppu.id, ppu.get_name(), thread.id, thread.get_name(), thread.cia, +thread.state,
				thread.current_function ? thread.current_function : "", thread.last_function ? thread.last_function : "", Emu.current_process().pid());
			return CELL_EBUSY;
		}

		CellError result = thread.joiner.atomic_op([&](ppu_join_status& value) -> CellError
		{
			switch (value)
			{
			case ppu_join_status::joinable:
				value = ppu_join_status{ppu.id};
				return {};
			case ppu_join_status::zombie:
				value = ppu_join_status::exited;
				return CELL_EAGAIN;
			case ppu_join_status::exited:
				return CELL_ESRCH;
			case ppu_join_status::detached:
			default:
				return CELL_EINVAL;
			}
		});

		if (!result)
		{
			lv2_obj::prepare_for_sleep(ppu);
			lv2_obj::sleep(ppu);
		}

		notify.cleanup();
		return result;
	});

	if (!thread)
	{
		return CELL_ESRCH;
	}

	if (thread.ret && thread.ret != CELL_EAGAIN)
	{
		return thread.ret;
	}

	if (thread.ret == CELL_EAGAIN)
	{
		// Notify thread if waiting for a joiner
		thread->joiner.notify_one();
	}

	// Wait for cleanup
	(*thread.ptr)();

	if (thread->joiner != ppu_join_status::exited)
	{
		// Thread aborted, log it later
		ppu.state += cpu_flag::again;
		return {};
	}

	static_cast<void>(ppu.test_stopped());

	// Get the exit status from the register
	const u64 vret = thread->gpr[3];

	if (thread.ret == CELL_EAGAIN)
	{
		// Cleanup
		ensure(idm::remove_verify<named_thread<ppu_thread>>(thread_id, std::move(thread.ptr)));
	}

	if (!vptr)
	{
		return not_an_error(CELL_EFAULT);
	}

	*vptr = vret;
	return CELL_OK;
}

error_code sys_ppu_thread_detach(ppu_thread& ppu, u32 thread_id)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.trace("sys_ppu_thread_detach(thread_id=0x%x)", thread_id);

	CellError result = CELL_ESRCH;

	auto [ptr, _] = idm::withdraw<named_thread<ppu_thread>>(thread_id, [&](ppu_thread& thread)
	{
		result = thread.joiner.atomic_op([](ppu_join_status& value) -> CellError
		{
			switch (value)
			{
			case ppu_join_status::joinable:
				value = ppu_join_status::detached;
				return {};
			case ppu_join_status::detached:
				return CELL_EINVAL;
			case ppu_join_status::zombie:
				value = ppu_join_status::exited;
				return CELL_EAGAIN;
			case ppu_join_status::exited:
				return CELL_ESRCH;
			default:
				return CELL_EBUSY;
			}
		});

		// Remove ID on EAGAIN
		return result != CELL_EAGAIN;
	});

	if (result)
	{
		if (result == CELL_EAGAIN)
		{
			// Join and notify thread (it is detached from IDM now so it must be done explicitly now)
			*ptr = thread_state::finished;
			return CELL_OK;
		}

		return result;
	}

	return CELL_OK;
}

error_code sys_ppu_thread_get_join_state(ppu_thread& ppu, vm::ptr<s32> isjoinable)
{
	sys_ppu_thread.trace("sys_ppu_thread_get_join_state(isjoinable=*0x%x)", isjoinable);

	if (!isjoinable)
	{
		return CELL_EFAULT;
	}

	*isjoinable = ppu.joiner != ppu_join_status::detached;
	return CELL_OK;
}

error_code sys_ppu_thread_set_priority(ppu_thread& ppu, u32 thread_id, s32 prio)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.trace("sys_ppu_thread_set_priority(thread_id=0x%x, prio=%d)", thread_id, prio);

	if (prio < (g_ps3_process_info.debug_or_root() ? -512 : 0) || prio > 3071)
	{
		return CELL_EINVAL;
	}

	if (thread_id == ppu.id)
	{
		// Fast path for self
		if (ppu.prio.load().prio != prio)
		{
			lv2_obj::set_priority(ppu, prio);
		}

		return CELL_OK;
	}

	const auto thread = idm::check<named_thread<ppu_thread>>(thread_id, [&, notify = lv2_obj::notify_all_t()](ppu_thread& thread)
	{
		lv2_obj::set_priority(thread, prio);
	});

	if (!thread)
	{
		return CELL_ESRCH;
	}

	return CELL_OK;
}

error_code sys_ppu_thread_get_priority(ppu_thread& ppu, u32 thread_id, vm::ptr<s32> priop)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.trace("sys_ppu_thread_get_priority(thread_id=0x%x, priop=*0x%x)", thread_id, priop);
	u32 prio{};

	if (thread_id == ppu.id)
	{
		// Fast path for self
		for (; !ppu.is_stopped(); std::this_thread::yield())
		{
			if (reader_lock lock(lv2_obj::g_mutex); cpu_flag::suspend - ppu.state)
			{
				prio = ppu.prio.load().prio;
				break;
			}

			ppu.check_state();
			ppu.state += cpu_flag::wait;
		}

		ppu.check_state();
		*priop = prio;
		return CELL_OK;
	}

	for (; !ppu.is_stopped(); std::this_thread::yield())
	{
		bool check_state = false;
		const auto thread = idm::check<named_thread<ppu_thread>>(thread_id, [&](ppu_thread& thread)
		{
			if (reader_lock lock(lv2_obj::g_mutex); cpu_flag::suspend - ppu.state)
			{
				prio = thread.prio.load().prio;
			}
			else
			{
				check_state = true;
			}
		});

		if (check_state)
		{
			ppu.check_state();
			ppu.state += cpu_flag::wait;
			continue;
		}

		if (!thread)
		{
			return CELL_ESRCH;
		}

		ppu.check_state();
		*priop = prio;
		break;
	}

	return CELL_OK;
}

error_code sys_ppu_thread_get_stack_information(ppu_thread& ppu, vm::ptr<sys_ppu_thread_stack_t> sp)
{
	sys_ppu_thread.trace("sys_ppu_thread_get_stack_information(sp=*0x%x)", sp);

	sp->pst_addr = ppu.stack_addr;
	sp->pst_size = ppu.stack_size;

	return CELL_OK;
}

error_code sys_ppu_thread_stop(ppu_thread& ppu, u32 thread_id)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.todo("sys_ppu_thread_stop(thread_id=0x%x)", thread_id);

	if (!g_ps3_process_info.has_root_perm())
	{
		return CELL_ENOSYS;
	}

	const auto thread = idm::check<named_thread<ppu_thread>>(thread_id, [](named_thread<ppu_thread>&) {});

	if (!thread)
	{
		return CELL_ESRCH;
	}

	return CELL_OK;
}

error_code sys_ppu_thread_restart(ppu_thread& ppu)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.todo("sys_ppu_thread_restart()");

	if (!g_ps3_process_info.has_root_perm())
	{
		return CELL_ENOSYS;
	}

	return CELL_OK;
}

error_code _sys_ppu_thread_create(ppu_thread& ppu, vm::ptr<u64> thread_id, vm::ptr<ppu_thread_param_t> param, u64 arg, u64 unk, s32 prio, u64 _stacksz, u64 flags, vm::cptr<char> threadname)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.warning("_sys_ppu_thread_create(thread_id=*0x%x, param=*0x%x, arg=0x%llx, unk=0x%llx, prio=%d, stacksize=0x%llx, flags=0x%llx, threadname=*0x%x)",
		thread_id, param, arg, unk, prio, _stacksz, flags, threadname);

	// thread_id is checked for null in stub -> CELL_ENOMEM
	// unk is set to 0 in sys_ppu_thread_create stub

	if (!param || !param->entry)
	{
		return CELL_EFAULT;
	}

	if (prio < (g_ps3_process_info.debug_or_root() ? -512 : 0) || prio > 3071)
	{
		return CELL_EINVAL;
	}

	if ((flags & 3) == 3) // Check two flags: joinable + interrupt not allowed
	{
		return CELL_EPERM;
	}

	const ppu_func_opd_t entry = param->entry.opd();
	const u32 tls = param->tls;

	// Compute actual stack size and allocate
	// 0 and UINT64_MAX both convert to 4096
	const u64 stack_size = FN(x ? x : 4096)(utils::align<u64>(_stacksz, 4096));

	auto& dct = fxo::get<lv2_memory_container>();

	// Try to obtain "physical memory" from the default container
	if (!dct.take(stack_size))
	{
		return {CELL_ENOMEM, dct.size - dct.used};
	}

	const vm::addr_t stack_base{vm::alloc(static_cast<u32>(stack_size), vm::stack, 4096)};

	if (!stack_base)
	{
		dct.free(stack_size);
		return CELL_ENOMEM;
	}

	std::string ppu_name;

	if (threadname)
	{
		constexpr u32 max_size = c_max_ppu_name_size - 1; // max size excluding null terminator

		if (!vm::read_string(threadname.addr(), max_size, ppu_name, true))
		{
			dct.free(stack_size);
			return CELL_EFAULT;
		}
	}

	const u32 tid = idm::import<named_thread<ppu_thread>>([&]()
	{
		ppu_thread_params p;
		p.stack_addr = stack_base;
		p.stack_size = static_cast<u32>(stack_size);
		p.tls_addr = tls;
		p.entry = entry;
		p.arg0 = arg;
		p.arg1 = unk;

		return stx::make_shared<named_thread<ppu_thread>>(p, ppu_name, prio, 1 - static_cast<int>(flags & 3));
	});

	if (!tid)
	{
		vm::dealloc(stack_base);
		dct.free(stack_size);
		return CELL_EAGAIN;
	}

	sys_ppu_thread.warning("_sys_ppu_thread_create(): Thread \"%s\" created (id=0x%x, func=*0x%x, rtoc=0x%x, user-tls=0x%x)", ppu_name, tid, entry.addr, entry.rtoc, tls);

	if (ppu.owner_pid == 1)
	{
		u64 arg0 = 0;
		u32 arg8 = 0;
		u32 arg16 = 0;
		const bool arg0_ok = vm::check_addr<8>(static_cast<u32>(arg));
		const bool arg8_ok = vm::check_addr<4>(static_cast<u32>(arg + 8));
		const bool arg16_ok = vm::check_addr<4>(static_cast<u32>(arg + 16));
		if (arg0_ok)
		{
			arg0 = vm::read64(static_cast<u32>(arg));
		}

		if (arg8_ok)
		{
			arg8 = vm::read32(static_cast<u32>(arg + 8));
		}

		if (arg16_ok)
		{
			arg16 = vm::read32(static_cast<u32>(arg + 16));
		}

		MPDBG_LOG(sys_ppu_thread, "VSH_PPU_THREAD_CREATE: thread_id=0x%x name=%s entry=0x%x rtoc=0x%x arg=0x%llx arg0=0x%llx arg0_ok=%d arg8=0x%x arg8_ok=%d arg16=0x%x arg16_ok=%d owner_pid=%u",
			tid, ppu_name, entry.addr, entry.rtoc, arg, arg0, arg0_ok, arg8, arg8_ok, arg16, arg16_ok, ppu.owner_pid);

		if (ppu_name == "view_module_load" && vm::check_addr<0x80>(static_cast<u32>(arg)))
		{
			u32 words[32]{};
			for (u32 i = 0; i < 32; i++)
			{
				words[i] = vm::read32(static_cast<u32>(arg + i * 4));
			}

			MPDBG_LOG(sys_ppu_thread, "VSH_VIEW_MODULE_ARG_DUMP: thread_id=0x%x arg=0x%llx w00=0x%x w04=0x%x w08=0x%x w0c=0x%x w10=0x%x w14=0x%x w18=0x%x w1c=0x%x w20=0x%x w24=0x%x w28=0x%x w2c=0x%x w30=0x%x w34=0x%x w38=0x%x w3c=0x%x w40=0x%x w44=0x%x w48=0x%x w4c=0x%x w50=0x%x w54=0x%x w58=0x%x w5c=0x%x w60=0x%x w64=0x%x w68=0x%x w6c=0x%x w70=0x%x w74=0x%x w78=0x%x w7c=0x%x",
				tid, arg,
				words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7],
				words[8], words[9], words[10], words[11], words[12], words[13], words[14], words[15],
				words[16], words[17], words[18], words[19], words[20], words[21], words[22], words[23],
				words[24], words[25], words[26], words[27], words[28], words[29], words[30], words[31]);
		}
	}

	ppu.check_state();
	*thread_id = tid;
	return CELL_OK;
}

error_code sys_ppu_thread_start(ppu_thread& ppu, u32 thread_id)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.trace("sys_ppu_thread_start(thread_id=0x%x)", thread_id);

	const auto thread = idm::get<named_thread<ppu_thread>>(thread_id, [&, notify = lv2_obj::notify_all_t()](ppu_thread& thread) -> CellError
	{
		if (!thread.state.test_and_reset(cpu_flag::stop))
		{
			// Already started
			return CELL_EBUSY;
		}

		ensure(lv2_obj::awake(&thread));

		thread.cmd_list
		({
			{ppu_cmd::entry_call, 0},
		});

		return {};
	});

	if (!thread)
	{
		return CELL_ESRCH;
	}

	if (thread.ret)
	{
		return thread.ret;
	}
	else
	{
		thread->cmd_notify.store(1);
		thread->cmd_notify.notify_one();
	}

	return CELL_OK;
}

error_code sys_ppu_thread_rename(ppu_thread& ppu, u32 thread_id, vm::cptr<char> name)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.warning("sys_ppu_thread_rename(thread_id=0x%x, name=*0x%x)", thread_id, name);

	const auto thread = idm::get_unlocked<named_thread<ppu_thread>>(thread_id);

	if (!thread)
	{
		return CELL_ESRCH;
	}

	if (!name)
	{
		return CELL_EFAULT;
	}

	constexpr u32 max_size = c_max_ppu_name_size - 1; // max size excluding null terminator

	// Make valid name
	std::string out_str;
	if (!vm::read_string(name.addr(), max_size, out_str, true))
	{
		return CELL_EFAULT;
	}

	auto _name = make_single<std::string>(std::move(out_str));

	// thread_ctrl name is not changed (TODO)
	sys_ppu_thread.warning("sys_ppu_thread_rename(): Thread renamed to \"%s\"", *_name);
	thread->ppu_tname.store(std::move(_name));
	thread_ctrl::set_name(*thread, thread->thread_name); // TODO: Currently sets debugger thread name only for local thread

	return CELL_OK;
}

error_code sys_ppu_thread_recover_page_fault(ppu_thread& ppu, u32 thread_id)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.warning("sys_ppu_thread_recover_page_fault(thread_id=0x%x)", thread_id);

	const auto thread = idm::get_unlocked<named_thread<ppu_thread>>(thread_id);

	if (!thread)
	{
		return CELL_ESRCH;
	}

	return mmapper_thread_recover_page_fault(thread.get());
}

error_code sys_ppu_thread_get_page_fault_context(ppu_thread& ppu, u32 thread_id, vm::ptr<sys_ppu_thread_icontext_t> ctxt)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.todo("sys_ppu_thread_get_page_fault_context(thread_id=0x%x, ctxt=*0x%x)", thread_id, ctxt);

	const auto thread = idm::get_unlocked<named_thread<ppu_thread>>(thread_id);

	if (!thread)
	{
		return CELL_ESRCH;
	}

	// We can only get a context if the thread is being suspended for a page fault.
	auto& pf_events = g_fxo->get<page_fault_event_entries>();
	reader_lock lock(pf_events.pf_mutex);

	const auto evt = pf_events.events.find(thread.get());
	if (evt == pf_events.events.end())
	{
		return CELL_EINVAL;
	}

	// TODO: Fill ctxt with proper information.

	return CELL_OK;
}
