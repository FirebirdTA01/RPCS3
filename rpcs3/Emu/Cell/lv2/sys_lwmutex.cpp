#include "stdafx.h"
#include "sys_lwmutex.h"

#include "Emu/IdManager.h"
#include "Emu/Memory/vm.h"
#include "Emu/System.h"

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/multiproc_debug.h"

#include "util/asm.hpp"

LOG_CHANNEL(sys_lwmutex);

static atomic_t<u32> g_vsh_view_module_lwmutex_watch_id = 0;

static bool is_vsh_view_module_lwmutex_trace(const ppu_thread& ppu, u32 lwmutex_id)
{
	if (ppu.owner_pid != 1)
	{
		return false;
	}

	const std::string name = ppu.get_name();
	return name.find("view_module_load") != umax || ppu.cia == 0x61527c || ppu.lr == 0x620538 || (lwmutex_id && lwmutex_id == g_vsh_view_module_lwmutex_watch_id.load());
}

static void log_vsh_view_module_lwmutex_state(const char* phase, const ppu_thread& ppu, u32 lwmutex_id, const lv2_lwmutex* mutex = nullptr, u32 result = 0xffff'ffff)
{
	u32 control_addr = 0;
	u32 control_owner = 0xffff'ffff;
	u32 control_waiter = 0xffff'ffff;
	u32 control_attr = 0xffff'ffff;
	u32 control_recursive = 0xffff'ffff;
	u32 control_sleep_queue = 0xffff'ffff;
	u64 lwmutex_name = 0;
	s32 signaled = 0;
	u32 sq_head_id = 0;
	u32 sq_head_cia = 0;
	u64 sq_head_lr = 0;
	u64 sq_head_state = 0;
	std::string sq_head_name;

	if (mutex)
	{
		control_addr = mutex->control.addr();
		lwmutex_name = static_cast<u64>(mutex->name);
		signaled = atomic_storage<s32>::load(mutex->lv2_control.raw().signaled);

		if (mutex->control && vm::check_addr<sizeof(sys_lwmutex_t)>(control_addr))
		{
			control_owner = mutex->control->vars.owner.load();
			control_waiter = mutex->control->vars.waiter.load();
			control_attr = mutex->control->attribute;
			control_recursive = mutex->control->recursive_count;
			control_sleep_queue = mutex->control->sleep_queue;
		}

		if (const ppu_thread* head = mutex->load_sq())
		{
			sq_head_id = head->id;
			sq_head_cia = head->cia;
			sq_head_lr = head->lr;
			sq_head_state = static_cast<u32>(+head->state.load());
			sq_head_name = head->get_name();
		}
	}

	MPDBG_LOG(sys_lwmutex, "VSH_VIEW_MODULE_LWMUTEX: phase=%s pid=%u tid=0x%x name=%s cia=0x%x lr=0x%llx lwmutex_id=0x%x result=0x%x control=0x%x owner=0x%x waiter=0x%x attr=0x%x recursive=0x%x sleep_queue=0x%x lwname=0x%llx signaled=%d sq_head=0x%x sq_head_name=%s sq_head_cia=0x%x sq_head_lr=0x%llx sq_head_state=0x%llx active_pid=%u",
		phase, ppu.owner_pid, ppu.id, ppu.get_name(), ppu.cia, ppu.lr, lwmutex_id, result,
		control_addr, control_owner, control_waiter, control_attr, control_recursive, control_sleep_queue,
		lwmutex_name, signaled, sq_head_id, sq_head_name, sq_head_cia, sq_head_lr, sq_head_state, Emu.current_process().pid());
}

lv2_lwmutex::lv2_lwmutex(utils::serial& ar)
	: protocol(ar)
	, control(ar.pop<decltype(control)>())
	, name(ar.pop<be_t<u64>>())
{
	ar(lv2_control.raw().signaled);
}

void lv2_lwmutex::save(utils::serial& ar)
{
	ar(protocol, control, name, lv2_control.raw().signaled);
}

error_code _sys_lwmutex_create(ppu_thread& ppu, vm::ptr<u32> lwmutex_id, u32 protocol, vm::ptr<sys_lwmutex_t> control, s32 has_name, u64 name)
{
	ppu.state += cpu_flag::wait;

	sys_lwmutex.trace(u8"_sys_lwmutex_create(lwmutex_id=*0x%x, protocol=0x%x, control=*0x%x, has_name=0x%x, name=0x%llx (“%s”))", lwmutex_id, protocol, control, has_name, name, lv2_obj::name_64{std::bit_cast<be_t<u64>>(name)});

	if (protocol != SYS_SYNC_FIFO && protocol != SYS_SYNC_RETRY && protocol != SYS_SYNC_PRIORITY)
	{
		sys_lwmutex.error("_sys_lwmutex_create(): unknown protocol (0x%x)", protocol);
		return CELL_EINVAL;
	}

	if (!(has_name < 0))
	{
		name = 0;
	}

	if (const u32 id = idm::make<lv2_obj, lv2_lwmutex>(protocol, control, name))
	{
		ppu.check_state();
		*lwmutex_id = id;
		return CELL_OK;
	}

	return CELL_EAGAIN;
}

error_code _sys_lwmutex_destroy(ppu_thread& ppu, u32 lwmutex_id)
{
	ppu.state += cpu_flag::wait;

	sys_lwmutex.trace("_sys_lwmutex_destroy(lwmutex_id=0x%x)", lwmutex_id);

	shared_ptr<lv2_lwmutex> _mutex;

	while (true)
	{
		s32 old_val = 0;

		auto [ptr, ret] = idm::withdraw<lv2_obj, lv2_lwmutex>(lwmutex_id, [&](lv2_lwmutex& mutex) -> CellError
		{
			// Ignore check on first iteration
			if (_mutex && std::addressof(mutex) != _mutex.get())
			{
				// Other thread has destroyed the lwmutex earlier
				return CELL_ESRCH;
			}

			std::lock_guard lock(mutex.mutex);

			if (mutex.load_sq())
			{
				return CELL_EBUSY;
			}

			old_val = mutex.lwcond_waiters.or_fetch(smin);

			if (old_val != smin)
			{
				// Deschedule if waiters were found
				lv2_obj::sleep(ppu);

				// Repeat loop: there are lwcond waiters
				return CELL_EAGAIN;
			}

			return {};
		});

		if (!ptr)
		{
			return CELL_ESRCH;
		}

		if (ret)
		{
			if (ret != CELL_EAGAIN)
			{
				return ret;
			}
		}
		else
		{
			break;
		}

		_mutex = std::move(ptr);

		// Wait for all lwcond waiters to quit
		while (old_val + 0u > 1u << 31)
		{
			thread_ctrl::wait_on(_mutex->lwcond_waiters, old_val);

			if (ppu.is_stopped())
			{
				ppu.state += cpu_flag::again;
				return {};
			}

			old_val = _mutex->lwcond_waiters;
		}

		// Wake up from sleep
		ppu.check_state();
	}

	return CELL_OK;
}

error_code _sys_lwmutex_lock(ppu_thread& ppu, u32 lwmutex_id, u64 timeout)
{
	ppu.state += cpu_flag::wait;

	sys_lwmutex.trace("_sys_lwmutex_lock(lwmutex_id=0x%x, timeout=0x%llx)", lwmutex_id, timeout);

	ppu.gpr[3] = CELL_OK;
	const bool vsh_trace = is_vsh_view_module_lwmutex_trace(ppu, lwmutex_id);

	if (vsh_trace)
	{
		g_vsh_view_module_lwmutex_watch_id.release(lwmutex_id);
		log_vsh_view_module_lwmutex_state("lock_enter", ppu, lwmutex_id);
	}

	const auto mutex = idm::get<lv2_obj, lv2_lwmutex>(lwmutex_id, [&, notify = lv2_obj::notify_all_t()](lv2_lwmutex& mutex)
	{
		if (vsh_trace)
		{
			log_vsh_view_module_lwmutex_state("lock_inspect", ppu, lwmutex_id, &mutex);
		}

		if (s32 signal = mutex.lv2_control.fetch_op([](lv2_lwmutex::control_data_t& data)
		{
			if (data.signaled)
			{
				data.signaled = 0;
				return true;
			}

			return false;
		}).first.signaled)
		{
			if (~signal & 1)
			{
				ppu.gpr[3] = CELL_EBUSY;
			}

			if (vsh_trace)
			{
				log_vsh_view_module_lwmutex_state("lock_signal", ppu, lwmutex_id, &mutex, static_cast<u32>(ppu.gpr[3]));
			}

			return true;
		}

		lv2_obj::prepare_for_sleep(ppu);

		ppu.cancel_sleep = 1;

		if (s32 signal = mutex.try_own(&ppu))
		{
			if (~signal & 1)
			{
				ppu.gpr[3] = CELL_EBUSY;
			}

			ppu.cancel_sleep = 0;
			if (vsh_trace)
			{
				log_vsh_view_module_lwmutex_state("lock_owned_after_try", ppu, lwmutex_id, &mutex, static_cast<u32>(ppu.gpr[3]));
			}
			return true;
		}

		if (vsh_trace)
		{
			log_vsh_view_module_lwmutex_state("lock_wait", ppu, lwmutex_id, &mutex);
		}

		const bool finished = !mutex.sleep(ppu, timeout);
		notify.cleanup();

		if (vsh_trace && finished)
		{
			log_vsh_view_module_lwmutex_state("lock_finished_in_sleep", ppu, lwmutex_id, &mutex, static_cast<u32>(ppu.gpr[3]));
		}

		return finished;
	});

	if (!mutex)
	{
		if (vsh_trace)
		{
			log_vsh_view_module_lwmutex_state("lock_invalid", ppu, lwmutex_id, nullptr, static_cast<u32>(CELL_ESRCH));
		}

		if (lwmutex_id >> 24 == lv2_lwmutex::id_base >> 24)
		{
			return { CELL_ESRCH, lwmutex_id };
		}

		return { CELL_ESRCH, "Invalid ID" };
	}

	if (mutex.ret)
	{
		if (vsh_trace)
		{
			log_vsh_view_module_lwmutex_state("lock_return_immediate", ppu, lwmutex_id, mutex.ptr.get(), static_cast<u32>(ppu.gpr[3]));
		}

		return not_an_error(ppu.gpr[3]);
	}

	while (auto state = +ppu.state)
	{
		if (state & cpu_flag::signal && ppu.state.test_and_reset(cpu_flag::signal))
		{
			break;
		}

		if (is_stopped(state))
		{
			std::lock_guard lock(mutex->mutex);

			for (auto cpu = mutex->load_sq(); cpu; cpu = cpu->next_cpu)
			{
				if (cpu == &ppu)
				{
					ppu.state += cpu_flag::again;
					return {};
				}
			}

			break;
		}

		for (usz i = 0; cpu_flag::signal - ppu.state && i < 50; i++)
		{
			busy_wait(500);
		}

		if (ppu.state & cpu_flag::signal)
 		{
			continue;
		}

		if (timeout)
		{
			if (lv2_obj::wait_timeout(timeout, &ppu))
			{
				// Wait for rescheduling
				if (ppu.check_state())
				{
					continue;
				}

				ppu.state += cpu_flag::wait;

				if (!mutex->load_sq())
				{
					// Sleep queue is empty, so the thread must have been signaled
					mutex->mutex.lock_unlock();
					break;
				}

				std::lock_guard lock(mutex->mutex);

				bool success = false;

				mutex->lv2_control.fetch_op([&](lv2_lwmutex::control_data_t& data)
				{
					success = false;

					ppu_thread* sq = static_cast<ppu_thread*>(data.sq);

					const bool retval = &ppu == sq;

					if (!mutex->unqueue<false>(sq, &ppu))
					{
						return false;
					}

					success = true;

					if (!retval)
					{
						return false;
					}

					data.sq = sq;
					return true;
				});

				if (success)
				{
					ppu.next_cpu = nullptr;
					ppu.gpr[3] = CELL_ETIMEDOUT;
				}

				break;
			}
		}
		else
		{
			ppu.state.wait(state);
		}
	}

	if (vsh_trace)
	{
		log_vsh_view_module_lwmutex_state("lock_return_wait", ppu, lwmutex_id, mutex.ptr.get(), static_cast<u32>(ppu.gpr[3]));
	}

	return not_an_error(ppu.gpr[3]);
}

error_code _sys_lwmutex_trylock(ppu_thread& ppu, u32 lwmutex_id)
{
	ppu.state += cpu_flag::wait;

	sys_lwmutex.trace("_sys_lwmutex_trylock(lwmutex_id=0x%x)", lwmutex_id);

	const auto mutex = idm::check<lv2_obj, lv2_lwmutex>(lwmutex_id, [&](lv2_lwmutex& mutex)
	{
		auto [_, ok] = mutex.lv2_control.fetch_op([](lv2_lwmutex::control_data_t& data)
		{
			if (data.signaled & 1)
			{
				data.signaled = 0;
				return true;
			}

			return false;
		});

		return ok;
	});

	if (!mutex)
	{
		if (lwmutex_id >> 24 == lv2_lwmutex::id_base >> 24)
		{
			return { CELL_ESRCH, lwmutex_id };
		}

		return { CELL_ESRCH, "Invalid ID" };
	}

	if (!mutex.ret)
	{
		return not_an_error(CELL_EBUSY);
	}

	return CELL_OK;
}

error_code _sys_lwmutex_unlock(ppu_thread& ppu, u32 lwmutex_id)
{
	ppu.state += cpu_flag::wait;

	sys_lwmutex.trace("_sys_lwmutex_unlock(lwmutex_id=0x%x)", lwmutex_id);
	const bool vsh_trace = is_vsh_view_module_lwmutex_trace(ppu, lwmutex_id);

	if (vsh_trace)
	{
		log_vsh_view_module_lwmutex_state("unlock_enter", ppu, lwmutex_id);
	}

	const auto mutex = idm::check<lv2_obj, lv2_lwmutex>(lwmutex_id, [&, notify = lv2_obj::notify_all_t()](lv2_lwmutex& mutex)
	{
		if (vsh_trace)
		{
			log_vsh_view_module_lwmutex_state("unlock_inspect", ppu, lwmutex_id, &mutex);
		}

		if (mutex.try_unlock(false))
		{
			if (vsh_trace)
			{
				log_vsh_view_module_lwmutex_state("unlock_signal_only", ppu, lwmutex_id, &mutex, static_cast<u32>(CELL_OK));
			}
			return;
		}

		std::lock_guard lock(mutex.mutex);

		if (const auto cpu = mutex.reown<ppu_thread>())
		{
			if (vsh_trace)
			{
				log_vsh_view_module_lwmutex_state("unlock_reown", *static_cast<ppu_thread*>(cpu), lwmutex_id, &mutex, static_cast<u32>(CELL_OK));
			}

			if (static_cast<ppu_thread*>(cpu)->state & cpu_flag::again)
			{
				ppu.state += cpu_flag::again;
				return;
			}

			mutex.awake(cpu);
			notify.cleanup(); // lv2_lwmutex::mutex is not really active 99% of the time, can be ignored
		}
	});

	if (!mutex)
	{
		if (vsh_trace)
		{
			log_vsh_view_module_lwmutex_state("unlock_invalid", ppu, lwmutex_id, nullptr, static_cast<u32>(CELL_ESRCH));
		}
		return CELL_ESRCH;
	}

	if (vsh_trace)
	{
		log_vsh_view_module_lwmutex_state("unlock_return", ppu, lwmutex_id, mutex, static_cast<u32>(CELL_OK));
	}

	return CELL_OK;
}

error_code _sys_lwmutex_unlock2(ppu_thread& ppu, u32 lwmutex_id)
{
	ppu.state += cpu_flag::wait;

	sys_lwmutex.warning("_sys_lwmutex_unlock2(lwmutex_id=0x%x)", lwmutex_id);

	const auto mutex = idm::check<lv2_obj, lv2_lwmutex>(lwmutex_id, [&, notify = lv2_obj::notify_all_t()](lv2_lwmutex& mutex)
	{
		if (mutex.try_unlock(true))
		{
			return;
		}

		std::lock_guard lock(mutex.mutex);

		if (const auto cpu = mutex.reown<ppu_thread>(true))
		{
			if (static_cast<ppu_thread*>(cpu)->state & cpu_flag::again)
			{
				ppu.state += cpu_flag::again;
				return;
			}

			static_cast<ppu_thread*>(cpu)->gpr[3] = CELL_EBUSY;
			mutex.awake(cpu);
			notify.cleanup(); // lv2_lwmutex::mutex is not really active 99% of the time, can be ignored
		}
	});

	if (!mutex)
	{
		return CELL_ESRCH;
	}

	return CELL_OK;
}
