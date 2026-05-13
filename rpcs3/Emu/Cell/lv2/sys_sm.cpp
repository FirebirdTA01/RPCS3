#include "stdafx.h"
#include "Emu/System.h"

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/lv2/sys_process.h"
#include "Emu/multiproc_debug.h"

#include "sys_sm.h"


LOG_CHANNEL(sys_sm);

static atomic_t<u64> g_ext_event2_a1 = 0;
static atomic_t<u64> g_ext_event2_a2 = 0;
static atomic_t<u64> g_ext_event2_a3 = 0;

static ppu_thread* get_vsh_ppu_for_sm_trace()
{
	ppu_thread* ppu = cpu_thread::get_current<ppu_thread>();
	return ppu && ppu->owner_pid == 1 ? ppu : nullptr;
}

void sys_sm_queue_ext_event2(u64 a1, u64 a2, u64 a3)
{
	MPDBG_LOG(sys_sm, "SM_QUEUE_EXT_EVENT2: target_pid=1 active_pid=%u a1=0x%llx a2=0x%llx a3=0x%llx pre_a1=0x%llx pre_a2=0x%llx pre_a3=0x%llx",
		Emu.current_process().pid(), a1, a2, a3, +g_ext_event2_a1, +g_ext_event2_a2, +g_ext_event2_a3);

	g_ext_event2_a2 = a2;
	g_ext_event2_a3 = a3;
	g_ext_event2_a1 = a1;

	MPDBG_LOG(sys_sm, "SM_QUEUE_EXT_EVENT2_DONE: target_pid=1 active_pid=%u post_a1=0x%llx post_a2=0x%llx post_a3=0x%llx",
		Emu.current_process().pid(), +g_ext_event2_a1, +g_ext_event2_a2, +g_ext_event2_a3);
}

error_code sys_sm_get_params(vm::ptr<u8> a, vm::ptr<u8> b, vm::ptr<u32> c, vm::ptr<u64> d)
{
	sys_sm.todo("sys_sm_get_params(a=*0x%x, b=*0x%x, c=*0x%x, d=*0x%x)", a, b, c, d);

	if (a) *a = 0; else
	{
		if (const auto ppu = get_vsh_ppu_for_sm_trace())
		{
			MPDBG_LOG(sys_sm, "SM_GET_PARAMS: owner_pid=%u id=0x%x name=%s a=0x%x b=0x%x c=0x%x d=0x%x ret=0x%x fault_arg=a",
				ppu->owner_pid, ppu->id, ppu->get_name(), a.addr(), b.addr(), c.addr(), d.addr(), static_cast<u32>(CELL_EFAULT));
		}

		return CELL_EFAULT;
	}

	if (b) *b = 0; else
	{
		if (const auto ppu = get_vsh_ppu_for_sm_trace())
		{
			MPDBG_LOG(sys_sm, "SM_GET_PARAMS: owner_pid=%u id=0x%x name=%s a=0x%x b=0x%x c=0x%x d=0x%x ret=0x%x fault_arg=b out_a=0x%x",
				ppu->owner_pid, ppu->id, ppu->get_name(), a.addr(), b.addr(), c.addr(), d.addr(), static_cast<u32>(CELL_EFAULT), *a);
		}

		return CELL_EFAULT;
	}

	if (c) *c = 0x200; else
	{
		if (const auto ppu = get_vsh_ppu_for_sm_trace())
		{
			MPDBG_LOG(sys_sm, "SM_GET_PARAMS: owner_pid=%u id=0x%x name=%s a=0x%x b=0x%x c=0x%x d=0x%x ret=0x%x fault_arg=c out_a=0x%x out_b=0x%x",
				ppu->owner_pid, ppu->id, ppu->get_name(), a.addr(), b.addr(), c.addr(), d.addr(), static_cast<u32>(CELL_EFAULT), *a, *b);
		}

		return CELL_EFAULT;
	}

	if (d) *d = 7; else
	{
		if (const auto ppu = get_vsh_ppu_for_sm_trace())
		{
			MPDBG_LOG(sys_sm, "SM_GET_PARAMS: owner_pid=%u id=0x%x name=%s a=0x%x b=0x%x c=0x%x d=0x%x ret=0x%x fault_arg=d out_a=0x%x out_b=0x%x out_c=0x%x",
				ppu->owner_pid, ppu->id, ppu->get_name(), a.addr(), b.addr(), c.addr(), d.addr(), static_cast<u32>(CELL_EFAULT), *a, *b, *c);
		}

		return CELL_EFAULT;
	}

	if (const auto ppu = get_vsh_ppu_for_sm_trace())
	{
		MPDBG_LOG(sys_sm, "SM_GET_PARAMS: owner_pid=%u id=0x%x name=%s a=0x%x b=0x%x c=0x%x d=0x%x ret=0x%x out_a=0x%x out_b=0x%x out_c=0x%x out_d=0x%llx",
			ppu->owner_pid, ppu->id, ppu->get_name(), a.addr(), b.addr(), c.addr(), d.addr(), static_cast<u32>(CELL_OK), *a, *b, *c, *d);
	}

	return CELL_OK;
}

error_code sys_sm_get_ext_event2(vm::ptr<u64> a1, vm::ptr<u64> a2, vm::ptr<u64> a3, u64 a4)
{
	sys_sm.trace("sys_sm_get_ext_event2(a1=*0x%x, a2=*0x%x, a3=*0x%x, a4=*0x%x)", a1, a2, a3, a4);

	const u64 pre_event1 = +g_ext_event2_a1;
	const u64 pre_event2 = +g_ext_event2_a2;
	const u64 pre_event3 = +g_ext_event2_a3;

	if (a4 != 0 && a4 != 1)
	{
		if (const auto ppu = get_vsh_ppu_for_sm_trace())
		{
			MPDBG_LOG(sys_sm, "SM_GET_EXT_EVENT2: owner_pid=%u id=0x%x name=%s a1=0x%x a2=0x%x a3=0x%x a4=0x%llx ret=0x%x delivered=0 pre=0x%llx,0x%llx,0x%llx post=0x%llx,0x%llx,0x%llx",
				ppu->owner_pid, ppu->id, ppu->get_name(), a1.addr(), a2.addr(), a3.addr(), a4, static_cast<u32>(CELL_EINVAL),
				pre_event1, pre_event2, pre_event3, +g_ext_event2_a1, +g_ext_event2_a2, +g_ext_event2_a3);
		}

		return CELL_EINVAL;
	}

	// a1 == 7 - "yesHOT" external hot-key event
	// a2 looks to be used if a1 is either 5 or 3?
	// a3 looks to be ignored in vsh

	if (!a1 || !a2 || !a3)
	{
		if (const auto ppu = get_vsh_ppu_for_sm_trace())
		{
			MPDBG_LOG(sys_sm, "SM_GET_EXT_EVENT2: owner_pid=%u id=0x%x name=%s a1=0x%x a2=0x%x a3=0x%x a4=0x%llx ret=0x%x delivered=0 pre=0x%llx,0x%llx,0x%llx post=0x%llx,0x%llx,0x%llx fault_mask=0x%x",
				ppu->owner_pid, ppu->id, ppu->get_name(), a1.addr(), a2.addr(), a3.addr(), a4, static_cast<u32>(CELL_EFAULT),
				pre_event1, pre_event2, pre_event3, +g_ext_event2_a1, +g_ext_event2_a2, +g_ext_event2_a3,
				(!a1 ? 1 : 0) | (!a2 ? 2 : 0) | (!a3 ? 4 : 0));
		}

		return CELL_EFAULT;
	}

	const u64 event1 = g_ext_event2_a1.exchange(0);
	if (event1)
	{
		*a1 = event1;
		*a2 = g_ext_event2_a2.exchange(0);
		*a3 = g_ext_event2_a3.exchange(0);

		if (const auto ppu = get_vsh_ppu_for_sm_trace())
		{
			MPDBG_LOG(sys_sm, "SM_GET_EXT_EVENT2: owner_pid=%u id=0x%x name=%s a1=0x%x a2=0x%x a3=0x%x a4=0x%llx ret=0x%x delivered=1 pre=0x%llx,0x%llx,0x%llx event=0x%llx,0x%llx,0x%llx post=0x%llx,0x%llx,0x%llx",
				ppu->owner_pid, ppu->id, ppu->get_name(), a1.addr(), a2.addr(), a3.addr(), a4, static_cast<u32>(CELL_OK),
				pre_event1, pre_event2, pre_event3, *a1, *a2, *a3, +g_ext_event2_a1, +g_ext_event2_a2, +g_ext_event2_a3);
		}

		return CELL_OK;
	}

	*a1 = 0;
	*a2 = 0;
	*a3 = 0;

	if (const auto ppu = get_vsh_ppu_for_sm_trace())
	{
		MPDBG_LOG(sys_sm, "SM_GET_EXT_EVENT2: owner_pid=%u id=0x%x name=%s a1=0x%x a2=0x%x a3=0x%x a4=0x%llx ret=0x%x delivered=0 pre=0x%llx,0x%llx,0x%llx event=0x0,0x0,0x0 post=0x%llx,0x%llx,0x%llx",
			ppu->owner_pid, ppu->id, ppu->get_name(), a1.addr(), a2.addr(), a3.addr(), a4, static_cast<u32>(CELL_EAGAIN),
			pre_event1, pre_event2, pre_event3, +g_ext_event2_a1, +g_ext_event2_a2, +g_ext_event2_a3);
	}

	// eagain for no event
	return not_an_error(CELL_EAGAIN);
}

error_code sys_sm_shutdown(ppu_thread& ppu, u16 op, vm::ptr<void> param, u64 size)
{
	ppu.state += cpu_flag::wait;

	sys_sm.success("sys_sm_shutdown(op=0x%x, param=*0x%x, size=0x%x)", op, param, size);

	if (!g_ps3_process_info.has_root_perm())
	{
		return CELL_ENOSYS;
	}

	switch (op)
	{
	case 0x100:
	case 0x1100:
	{
		sys_sm.success("Received shutdown request from application");
		_sys_process_exit(ppu, 0, 0, 0);
		break;
	}
	case 0x200:
	case 0x1200:
	{
		sys_sm.success("Received reboot request from application");
		lv2_exitspawn(ppu, Emu.current_process().RefArgv(), Emu.current_process().RefEnvp(), Emu.current_process().RefData());
		break;
	}
	case 0x8201:
	case 0x8202:
	case 0x8204:
	{
		sys_sm.warning("Unsupported LPAR operation: 0x%x", op);
		return CELL_ENOTSUP;
	}
	default: return CELL_EINVAL;
	}

	return CELL_OK;
}

error_code sys_sm_set_shop_mode(s32 mode)
{
	sys_sm.todo("sys_sm_set_shop_mode(mode=0x%x)", mode);

	return CELL_OK;
}

error_code sys_sm_control_led(u8 led, u8 action)
{
	sys_sm.todo("sys_sm_control_led(led=0x%x, action=0x%x)", led, action);

	return CELL_OK;
}

error_code sys_sm_ring_buzzer(u64 packet, u64 a1, u64 a2)
{
	sys_sm.todo("sys_sm_ring_buzzer(packet=0x%x, a1=0x%x, a2=0x%x)", packet, a1, a2);

	return CELL_OK;
}
