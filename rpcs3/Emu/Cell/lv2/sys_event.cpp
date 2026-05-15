#include "stdafx.h"
#include "sys_event.h"

#include "Emu/IdManager.h"
#include "Emu/IPC.h"
#include "Emu/Memory/vm.h"
#include "Emu/System.h"

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/SPUThread.h"
#include "Emu/multiproc_debug.h"
#include "sys_process.h"
#include "sys_mmapper.h"
#include "sys_prx.h"

#include "util/asm.hpp"

#include <chrono>
#include <thread>

LOG_CHANNEL(sys_event);

static ppu_thread* get_vsh_ppu_for_event_trace(cpu_thread* cpu = cpu_thread::get_current())
{
	ppu_thread* ppu = cpu ? cpu->try_get<ppu_thread>() : nullptr;
	return ppu && ppu->owner_pid == 1 ? ppu : nullptr;
}

static ppu_thread* get_vsh_waiter_for_event_trace(lv2_event_queue& queue)
{
	for (ppu_thread* waiter = +queue.pq; waiter; waiter = waiter->next_cpu)
	{
		if (waiter->owner_pid == 1)
		{
			return waiter;
		}
	}

	return nullptr;
}

struct vsh_mcore_cmd_object_dump
{
	u32 ptr = 0;
	bool ok20 = false;
	bool ok30 = false;
	u32 w00 = 0;
	u32 w04 = 0;
	u32 w08 = 0;
	u32 w0c = 0;
	u32 w10 = 0;
	u32 w14 = 0;
	u32 w18 = 0;
	u32 w1c = 0;
	bool inner_ok = false;
	u32 i00 = 0;
	u32 i04 = 0;
	u32 i08 = 0;
	u32 i0c = 0;
	u32 i10 = 0;
	u32 i14 = 0;
	u32 i18 = 0;
	u32 i1c = 0;
};

atomic_t<u32> g_vsh_delayed_snapshot_request = 0;
static atomic_t<u32> g_vsh_mcore_keep_active_after_async = 0;

void request_vsh_pid1_delayed_snapshot(const char* reason);
void service_vsh_pid1_delayed_snapshot(ppu_thread& ppu, const char* reason);

static u32 read_vsh_u32_for_paf_trace(u32 addr)
{
	return vm::check_addr<4>(addr) ? static_cast<u32>(vm::read32(addr)) : 0xffff'ffff;
}

static void log_vsh_paf_globals(const char* reason)
{
	MPDBG_LOG(sys_event, "VSH_PAF_GLOBALS: reason=%s active_pid=%u input_pid=%u present_pid=%u f7a8=0x%x f7d4=0x%x f7d8=0x%x f7dc=0x%x f7e0=0x%x f7e8=0x%x f838=0x%x f840=0x%x f854=0x%x f89c=0x%x f8a0=0x%x",
		reason, Emu.current_process().pid(), Emu.GetInputForegroundPid(), Emu.GetForegroundPresentPid(),
		read_vsh_u32_for_paf_trace(0x72f7a8), read_vsh_u32_for_paf_trace(0x72f7d4),
		read_vsh_u32_for_paf_trace(0x72f7d8), read_vsh_u32_for_paf_trace(0x72f7dc),
		read_vsh_u32_for_paf_trace(0x72f7e0), read_vsh_u32_for_paf_trace(0x72f7e8),
		read_vsh_u32_for_paf_trace(0x72f838), read_vsh_u32_for_paf_trace(0x72f840),
		read_vsh_u32_for_paf_trace(0x72f854), read_vsh_u32_for_paf_trace(0x72f89c),
		read_vsh_u32_for_paf_trace(0x72f8a0));
}

struct vsh_pid1_wait_context
{
	u32 queue_id = 0;
	u64 queue_key = 0;
	u64 queue_name = 0;
	u32 queued = 0;
};

static vsh_pid1_wait_context find_vsh_pid1_event_wait_context(const ppu_thread& ppu)
{
	vsh_pid1_wait_context result{};

	idm::select<lv2_obj, lv2_event_queue>([&](u32, lv2_event_queue& queue)
	{
		std::lock_guard lock(queue.mutex);

		for (ppu_thread* waiter = +queue.pq; waiter; waiter = waiter->next_cpu)
		{
			if (waiter == &ppu)
			{
				result.queue_id = queue.id;
				result.queue_key = queue.key;
				result.queue_name = queue.name;
				result.queued = static_cast<u32>(queue.events.size());
				break;
			}
		}
	});

	return result;
}

static void dump_vsh_pid1_threads_after_mcore(const char* reason)
{
	u32 total = 0;
	u32 waiting_on_queue = 0;

	MPDBG_LOG(sys_event, "VSH_PID1_THREAD_SNAPSHOT: phase=begin reason=%s active_pid=%u input_pid=%u present_pid=%u",
		reason, Emu.current_process().pid(), Emu.GetInputForegroundPid(), Emu.GetForegroundPresentPid());

	idm::select<named_thread<ppu_thread>>([&](u32 id, named_thread<ppu_thread>& ppu)
	{
		if (ppu.owner_pid != 1)
		{
			return;
		}

		total++;

		const auto [status, wait_id] = lv2_obj::ppu_state(&ppu, false, false);
		const auto wait = find_vsh_pid1_event_wait_context(ppu);
		if (wait.queue_id)
		{
			waiting_on_queue++;
		}

		const auto state_flags = +ppu.state.load();
		const auto prio = ppu.prio.load();

		MPDBG_LOG(sys_event, "VSH_PID1_THREAD_SNAPSHOT: tid=0x%x name=%s status=%u wait_id=0x%x state=0x%llx cia=0x%x lr=0x%llx entry=0x%x rtoc=0x%x gpr3=0x%llx gpr4=0x%llx prio=%lld stack=0x%x stack_size=0x%x current=%s last=%s wait_queue=0x%x wait_key=0x%llx wait_qname=0x%llx wait_queued=%u",
			id, ppu.get_name(), static_cast<u32>(status), wait_id, state_flags, ppu.cia, ppu.lr,
			static_cast<u32>(ppu.entry_func.addr), static_cast<u32>(ppu.entry_func.rtoc), ppu.gpr[3], ppu.gpr[4],
			static_cast<s64>(prio.prio), ppu.stack_addr, ppu.stack_size,
			ppu.current_function ? ppu.current_function : "", ppu.last_function ? ppu.last_function : "",
			wait.queue_id, wait.queue_key, wait.queue_name, wait.queued);
	});

	MPDBG_LOG(sys_event, "VSH_PID1_THREAD_SNAPSHOT: phase=end reason=%s total=%u waiting_on_queue=%u",
		reason, total, waiting_on_queue);
}

void request_vsh_pid1_delayed_snapshot(const char* reason)
{
	const u32 old = g_vsh_delayed_snapshot_request.exchange(1);
	MPDBG_LOG(sys_event, "VSH_PID1_THREAD_SNAPSHOT: phase=request_delayed reason=%s old=%u active_pid=%u input_pid=%u present_pid=%u",
		reason, old, Emu.current_process().pid(), Emu.GetInputForegroundPid(), Emu.GetForegroundPresentPid());
}

void service_vsh_pid1_delayed_snapshot(ppu_thread& ppu, const char* reason)
{
	if (ppu.owner_pid != 1)
	{
		return;
	}

	if (g_vsh_delayed_snapshot_request.exchange(0))
	{
		dump_vsh_pid1_threads_after_mcore(reason);
	}
}


static vsh_mcore_cmd_object_dump dump_vsh_mcore_cmd_object(u32 ptr)
{
	vsh_mcore_cmd_object_dump dump{};
	dump.ptr = ptr;
	dump.ok20 = ptr && vm::check_addr<0x20>(ptr);
	dump.ok30 = ptr && vm::check_addr<0x30>(ptr);

	if (dump.ok20)
	{
		dump.w00 = static_cast<u32>(vm::read32(ptr + 0x00));
		dump.w04 = static_cast<u32>(vm::read32(ptr + 0x04));
		dump.w08 = static_cast<u32>(vm::read32(ptr + 0x08));
		dump.w0c = static_cast<u32>(vm::read32(ptr + 0x0c));
		dump.w10 = static_cast<u32>(vm::read32(ptr + 0x10));
		dump.w14 = static_cast<u32>(vm::read32(ptr + 0x14));
		dump.w18 = static_cast<u32>(vm::read32(ptr + 0x18));
		dump.w1c = static_cast<u32>(vm::read32(ptr + 0x1c));
		dump.inner_ok = dump.w10 && vm::check_addr<0x20>(dump.w10);
	}

	if (dump.inner_ok)
	{
		dump.i00 = static_cast<u32>(vm::read32(dump.w10 + 0x00));
		dump.i04 = static_cast<u32>(vm::read32(dump.w10 + 0x04));
		dump.i08 = static_cast<u32>(vm::read32(dump.w10 + 0x08));
		dump.i0c = static_cast<u32>(vm::read32(dump.w10 + 0x0c));
		dump.i10 = static_cast<u32>(vm::read32(dump.w10 + 0x10));
		dump.i14 = static_cast<u32>(vm::read32(dump.w10 + 0x14));
		dump.i18 = static_cast<u32>(vm::read32(dump.w10 + 0x18));
		dump.i1c = static_cast<u32>(vm::read32(dump.w10 + 0x1c));
	}

	return dump;
}

static u32 find_vsh_mcore_shm_base()
{
	u32 base_addr = 0;

	idm::select<lv2_obj, lv2_memory>([&](u32, lv2_memory& mem)
	{
		if (base_addr || mem.key != 0x8005911000000020ull)
		{
			return;
		}

		if (const auto shm = mem.shm.load())
		{
			base_addr = vm::get_shm_addr(*shm);
		}
	});

	return base_addr;
}

static CellError send_vsh_mcore_host_response(lv2_event_queue& child_queue, u64 command, u64 command_data2, u64 command_data3)
{
	constexpr u64 base_key = 0x8005911000000020ull;
	constexpr u64 parent_queue_key = base_key + 2;
	constexpr u64 child_source = base_key + 5;
	constexpr u32 object_offset = 0x1ff00;
	constexpr u32 generic_object_offset = 0x1ffe0;
	constexpr u32 transfer_complete_object_offset = 0x1ffc0;
	constexpr u32 transfer_out0_object_offset = 0x1ffa0;
	constexpr u32 transfer_out1_object_offset = 0x1ff80;
	constexpr u32 transfer_out0_payload_offset = 0x1ff60;
	constexpr u32 object_size = 8;
	constexpr u32 transfer_object_size = 0x18;
	static atomic_t<u32> s_mcore_transfer_stage = 0;
	static atomic_t<u32> s_mcore_transfer_packets = 0;

	auto parent_queue = lv2_event_queue::find(parent_queue_key);
	if (!parent_queue)
	{
		MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: result=0x%x reason=missing_parent_queue child_queue=0x%x child_key=0x%llx command=0x%llx data2=0x%llx data3=0x%llx",
			static_cast<u32>(CELL_ESRCH), child_queue.id, child_queue.key, command, command_data2, command_data3);
		return CELL_ESRCH;
	}

	if (command == 3 && command_data2 == 0 && command_data3 == 0)
	{
		const u32 shm_base = find_vsh_mcore_shm_base();
		const u32 object_addr = shm_base + object_offset;

		if (!shm_base || !vm::check_addr<8>(object_addr))
		{
			MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: result=0x%x reason=shared_memory_lookup command=0x%llx shm_base=0x%x object_addr=0x%x child_queue=0x%x parent_queue=0x%x",
				static_cast<u32>(CELL_ESRCH), command, shm_base, object_addr, child_queue.id, parent_queue->id);
			return CELL_ESRCH;
		}

		vm::write32(object_addr + 0, object_size);
		vm::write32(object_addr + 4, 0);

		bool notified_thread = false;
		const CellError result = parent_queue->send(child_source, 0, object_size, object_offset, &notified_thread);
		MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: phase=reply_object child_queue=0x%x parent_queue=0x%x source=0x%llx command=0x%llx shm_base=0x%x object_addr=0x%x data1=0x0 data2=0x%x data3=0x%x result=0x%x notified=%d",
			child_queue.id, parent_queue->id, child_source, command, shm_base, object_addr, object_size, object_offset, static_cast<u32>(result), notified_thread);
		return result;
	}

	if (command == 1)
	{
		const u32 transfer_stage = s_mcore_transfer_stage.load();
		if (transfer_stage)
		{
			if (command_data2 != 0 || command_data3 != 0)
			{
				const u32 raw_ptr = static_cast<u32>(command_data3);
				const auto raw = dump_vsh_mcore_cmd_object(raw_ptr);
				const u32 shm_base = find_vsh_mcore_shm_base();
				const u32 translated_ptr = shm_base + raw_ptr;
				const auto translated = dump_vsh_mcore_cmd_object(translated_ptr);

				MPDBG_LOG(sys_event, "VSH_MCORE_CMD1_TRANSFER_OBJECT: mode=raw ptr=0x%x data2=0x%llx ok20=%d ok30=%d w00=0x%x w04=0x%x w08=0x%x w0c=0x%x w10=0x%x w14=0x%x w18=0x%x w1c=0x%x inner_ok=%d i00=0x%x i04=0x%x i08=0x%x i0c=0x%x i10=0x%x i14=0x%x i18=0x%x i1c=0x%x",
					raw.ptr, command_data2, raw.ok20, raw.ok30, raw.w00, raw.w04, raw.w08, raw.w0c, raw.w10, raw.w14, raw.w18, raw.w1c, raw.inner_ok, raw.i00, raw.i04, raw.i08, raw.i0c, raw.i10, raw.i14, raw.i18, raw.i1c);
				MPDBG_LOG(sys_event, "VSH_MCORE_CMD1_TRANSFER_OBJECT: mode=translated shm_base=0x%x data3=0x%x ptr=0x%x data2=0x%llx ok20=%d ok30=%d w00=0x%x w04=0x%x w08=0x%x w0c=0x%x w10=0x%x w14=0x%x w18=0x%x w1c=0x%x inner_ok=%d i00=0x%x i04=0x%x i08=0x%x i0c=0x%x i10=0x%x i14=0x%x i18=0x%x i1c=0x%x",
					shm_base, raw_ptr, translated.ptr, command_data2, translated.ok20, translated.ok30, translated.w00, translated.w04, translated.w08, translated.w0c, translated.w10, translated.w14, translated.w18, translated.w1c, translated.inner_ok, translated.i00, translated.i04, translated.i08, translated.i0c, translated.i10, translated.i14, translated.i18, translated.i1c);
				const u64 payload_offset = (static_cast<u64>(translated.w10) << 32) | translated.w14;
				const u32 payload_addr = shm_base + static_cast<u32>(payload_offset);
				const bool payload_ok = shm_base && payload_offset && vm::check_addr<0x20>(payload_addr);
				MPDBG_LOG(sys_event, "VSH_MCORE_CMD1_TRANSFER_PAYLOAD: shm_base=0x%x payload_offset=0x%llx payload_addr=0x%x payload_ok=%d p00=0x%x p04=0x%x p08=0x%x p0c=0x%x p10=0x%x p14=0x%x p18=0x%x p1c=0x%x",
					shm_base, payload_offset, payload_addr, payload_ok,
					payload_ok ? static_cast<u32>(vm::read32(payload_addr + 0x00)) : 0,
					payload_ok ? static_cast<u32>(vm::read32(payload_addr + 0x04)) : 0,
					payload_ok ? static_cast<u32>(vm::read32(payload_addr + 0x08)) : 0,
					payload_ok ? static_cast<u32>(vm::read32(payload_addr + 0x0c)) : 0,
					payload_ok ? static_cast<u32>(vm::read32(payload_addr + 0x10)) : 0,
					payload_ok ? static_cast<u32>(vm::read32(payload_addr + 0x14)) : 0,
					payload_ok ? static_cast<u32>(vm::read32(payload_addr + 0x18)) : 0,
					payload_ok ? static_cast<u32>(vm::read32(payload_addr + 0x1c)) : 0);

				bool notified_thread = false;
				const CellError result = parent_queue->send(child_source, 1, 0, 0, &notified_thread);
				const u32 transfer_packets = s_mcore_transfer_packets.fetch_add(1) + 1;
				MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: phase=reply_transfer_ack child_queue=0x%x parent_queue=0x%x source=0x%llx command=0x%llx command_data2=0x%llx command_data3=0x%llx stage=%u packets=%u data1=0x1 data2=0x0 data3=0x0 result=0x%x notified=%d",
					child_queue.id, parent_queue->id, child_source, command, command_data2, command_data3, transfer_stage, transfer_packets, static_cast<u32>(result), notified_thread);
				return result;
			}

			if (transfer_stage == 2 || transfer_stage == 3)
			{
				const u32 shm_base = find_vsh_mcore_shm_base();
				const u32 transfer_object_offset = transfer_stage == 2 ? transfer_out0_object_offset : transfer_out1_object_offset;
				const u32 object_addr = shm_base + transfer_object_offset;

				if (!shm_base || !vm::check_addr<transfer_object_size>(object_addr))
				{
					MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: result=0x%x reason=shared_memory_lookup_transfer_out command=0x%llx shm_base=0x%x object_addr=0x%x child_queue=0x%x parent_queue=0x%x stage=%u packets=%u",
						static_cast<u32>(CELL_ESRCH), command, shm_base, object_addr, child_queue.id, parent_queue->id, transfer_stage, s_mcore_transfer_packets.load());
					return CELL_ESRCH;
				}

				// 0x411 returns no first output and carries its small type-6 result
				// through the second output descriptor.
				if (transfer_stage == 2)
				{
					vm::write32(object_addr + 0x00, 0);
					vm::write32(object_addr + 0x04, 1);
					vm::write32(object_addr + 0x08, 0);
					vm::write32(object_addr + 0x0c, 0);
					vm::write64(object_addr + 0x10, 0);
				}
				else
				{
					const u32 payload_addr = shm_base + transfer_out0_payload_offset;
					if (!vm::check_addr<object_size>(payload_addr))
					{
						MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: result=0x%x reason=shared_memory_lookup_transfer_out_payload command=0x%llx shm_base=0x%x payload_addr=0x%x child_queue=0x%x parent_queue=0x%x stage=%u packets=%u",
							static_cast<u32>(CELL_ESRCH), command, shm_base, payload_addr, child_queue.id, parent_queue->id, transfer_stage, s_mcore_transfer_packets.load());
						return CELL_ESRCH;
					}

					vm::write32(payload_addr + 0x00, 6);
					vm::write32(payload_addr + 0x04, 0x44);
					vm::write32(object_addr + 0x00, object_size);
					vm::write32(object_addr + 0x04, 0);
					vm::write32(object_addr + 0x08, object_size);
					vm::write32(object_addr + 0x0c, 0);
					vm::write64(object_addr + 0x10, transfer_out0_payload_offset);
				}

				bool notified_thread = false;
				const CellError result = parent_queue->send(child_source, 1, transfer_object_size, transfer_object_offset, &notified_thread);
				MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: phase=reply_transfer_out child_queue=0x%x parent_queue=0x%x source=0x%llx command=0x%llx command_data2=0x%llx command_data3=0x%llx shm_base=0x%x object_addr=0x%x stage=%u packets=%u payload_offset=0x%x data1=0x1 data2=0x%x data3=0x%x result=0x%x notified=%d",
					child_queue.id, parent_queue->id, child_source, command, command_data2, command_data3, shm_base, object_addr, transfer_stage, s_mcore_transfer_packets.load(), transfer_stage == 3 ? transfer_out0_payload_offset : 0, transfer_object_size, transfer_object_offset, static_cast<u32>(result), notified_thread);

				if (!result)
				{
					s_mcore_transfer_stage = transfer_stage == 2 ? 3 : 4;
				}

				return result;
			}

			if (transfer_stage == 4)
			{
				s_mcore_transfer_stage = 0;
				g_vsh_mcore_keep_active_after_async = 1;
				MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: phase=consume_transfer_out_ack child_queue=0x%x parent_queue=0x%x source=0x%llx command=0x%llx command_data2=0x%llx command_data3=0x%llx stage=%u result=0x%x active=%u",
					child_queue.id, parent_queue->id, child_source, command, command_data2, command_data3, transfer_stage, static_cast<u32>(CELL_OK), Emu.current_process().pid());
				log_vsh_paf_globals("mcore_transfer_out_ack");
				dump_vsh_pid1_threads_after_mcore("mcore_transfer_out_ack");
				return {};
			}

			const u32 shm_base = find_vsh_mcore_shm_base();
			const u32 object_addr = shm_base + transfer_complete_object_offset;

			if (!shm_base || !vm::check_addr<8>(object_addr))
			{
				MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: result=0x%x reason=shared_memory_lookup_transfer_complete command=0x%llx shm_base=0x%x object_addr=0x%x child_queue=0x%x parent_queue=0x%x stage=%u packets=%u",
					static_cast<u32>(CELL_ESRCH), command, shm_base, object_addr, child_queue.id, parent_queue->id, transfer_stage, s_mcore_transfer_packets.load());
				return CELL_ESRCH;
			}

			vm::write32(object_addr + 0, object_size);
			vm::write32(object_addr + 4, 0);

			bool notified_thread = false;
			const CellError result = parent_queue->send(child_source, 0, object_size, transfer_complete_object_offset, &notified_thread);
			MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: phase=reply_transfer_complete child_queue=0x%x parent_queue=0x%x source=0x%llx command=0x%llx command_data2=0x%llx command_data3=0x%llx shm_base=0x%x object_addr=0x%x stage=%u packets=%u data1=0x0 data2=0x%x data3=0x%x result=0x%x notified=%d",
				child_queue.id, parent_queue->id, child_source, command, command_data2, command_data3, shm_base, object_addr, transfer_stage, s_mcore_transfer_packets.load(), object_size, transfer_complete_object_offset, static_cast<u32>(result), notified_thread);
			s_mcore_transfer_stage = result ? 0 : 2;
			s_mcore_transfer_packets = 0;
			return result;
		}

		const u32 shm_base = find_vsh_mcore_shm_base();
		const u32 object_addr = shm_base + generic_object_offset;
		const u32 status = command_data2 == 0 && command_data3 == 0 ? 0 : 7;

		if (!shm_base || !vm::check_addr<8>(object_addr))
		{
			MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: result=0x%x reason=shared_memory_lookup command=0x%llx shm_base=0x%x object_addr=0x%x child_queue=0x%x parent_queue=0x%x",
				static_cast<u32>(CELL_ESRCH), command, shm_base, object_addr, child_queue.id, parent_queue->id);
			return CELL_ESRCH;
		}

		vm::write32(object_addr + 0, object_size);
		vm::write32(object_addr + 4, status);

		bool notified_thread = false;
		const CellError result = parent_queue->send(child_source, 0, object_size, generic_object_offset, &notified_thread);
		MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: phase=reply_generic_object child_queue=0x%x parent_queue=0x%x source=0x%llx command=0x%llx command_data2=0x%llx command_data3=0x%llx shm_base=0x%x object_addr=0x%x status=0x%x data1=0x0 data2=0x%x data3=0x%x result=0x%x notified=%d",
			child_queue.id, parent_queue->id, child_source, command, command_data2, command_data3, shm_base, object_addr, status, object_size, generic_object_offset, static_cast<u32>(result), notified_thread);
		return result;
	}

	if (command == 5)
	{
		bool notified_thread = false;
		const CellError result = parent_queue->send(child_source, 0, 0, 0, &notified_thread);
		if (!result)
		{
			s_mcore_transfer_stage = 1;
			s_mcore_transfer_packets = 0;
		}
		MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: phase=reply_ack child_queue=0x%x parent_queue=0x%x source=0x%llx command=0x%llx data2=0x%llx data3=0x%llx result=0x%x notified=%d",
			child_queue.id, parent_queue->id, child_source, command, command_data2, command_data3, static_cast<u32>(result), notified_thread);
		return result;
	}

	MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: phase=unhandled child_queue=0x%x parent_queue=0x%x source=0x%llx command=0x%llx data2=0x%llx data3=0x%llx",
		child_queue.id, parent_queue->id, child_source, command, command_data2, command_data3);
	return CELL_EINVAL;
}

static void schedule_vsh_mcore_host_response(u64 command, u64 command_data2, u64 command_data3)
{
	constexpr u64 child_queue_key = 0x8005911000000024ull;

	MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: phase=schedule_async command=0x%llx data2=0x%llx data3=0x%llx active=%u",
		command, command_data2, command_data3, Emu.current_process().pid());

	std::thread([command, command_data2, command_data3]()
	{
		std::this_thread::sleep_for(std::chrono::microseconds{1000});

		const u32 restore_active_pid = Emu.current_process().pid();
		const bool switched_active = restore_active_pid != 1;
		if (switched_active)
		{
			MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: phase=async_active_switch from_pid=%u to_pid=1 command=0x%llx data2=0x%llx data3=0x%llx",
				restore_active_pid, command, command_data2, command_data3);
			Emu.set_active_process(1, false, true, false);
		}

		CellError result = CELL_ESRCH;
		u32 child_queue_id = 0;

		auto child_queue = lv2_event_queue::find(child_queue_key);
		if (child_queue)
		{
			child_queue_id = child_queue->id;
			result = send_vsh_mcore_host_response(*child_queue, command, command_data2, command_data3);
		}
		else
		{
			MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: phase=async_missing_child_queue command=0x%llx data2=0x%llx data3=0x%llx result=0x%x",
				command, command_data2, command_data3, static_cast<u32>(CELL_ESRCH));
		}

		const bool keep_active = g_vsh_mcore_keep_active_after_async.exchange(0) != 0;

		if (switched_active && !keep_active && Emu.current_process().pid() != restore_active_pid)
		{
			Emu.set_active_process(restore_active_pid, false, true, false);
			MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: phase=async_active_restore to_pid=%u command=0x%llx data2=0x%llx data3=0x%llx",
				restore_active_pid, command, command_data2, command_data3);
		}
		else if (keep_active)
		{
			MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: phase=async_active_keep active_pid=%u skipped_restore_pid=%u command=0x%llx data2=0x%llx data3=0x%llx",
				Emu.current_process().pid(), restore_active_pid, command, command_data2, command_data3);
		}

		MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: phase=async_response_done child_queue=0x%x command=0x%llx data2=0x%llx data3=0x%llx result=0x%x",
			child_queue_id, command, command_data2, command_data3, static_cast<u32>(result));
	}).detach();
}

lv2_event_queue::lv2_event_queue(u32 protocol, s32 type, s32 size, u64 name, u64 ipc_key) noexcept
	: id(idm::last_id())
	, protocol{static_cast<u8>(protocol)}
	, type(static_cast<u8>(type))
	, size(static_cast<u8>(size))
	, name(name)
	, key(ipc_key)
{
}

lv2_event_queue::lv2_event_queue(utils::serial& ar) noexcept
	: id(idm::last_id())
	, protocol(ar)
	, type(ar)
	, size(ar)
	, name(ar)
	, key(ar)
{
	ar(events);
}

std::function<void(void*)> lv2_event_queue::load(utils::serial& ar)
{
	auto queue = make_shared<lv2_event_queue>(stx::exact_t<utils::serial&>(ar));
	return [ptr = lv2_obj::load(queue->key, queue)](void* storage) { *static_cast<atomic_ptr<lv2_obj>*>(storage) = ptr; };
}

void lv2_event_queue::save(utils::serial& ar)
{
	ar(protocol, type, size, name, key, events);
}

void lv2_event_queue::save_ptr(utils::serial& ar, lv2_event_queue* q)
{
	if (!lv2_obj::check(q))
	{
		ar(u32{0});
		return;
	}

	ar(q->id);
}

shared_ptr<lv2_event_queue> lv2_event_queue::load_ptr(utils::serial& ar, shared_ptr<lv2_event_queue>& queue, std::string_view msg)
{
	const u32 id = ar.pop<u32>();

	if (!id)
	{
		return {};
	}

	if (auto q = idm::get_unlocked<lv2_obj, lv2_event_queue>(id))
	{
		// Already initialized
		return q;
	}

	if (id >> 24 != id_base >> 24)
	{
		fmt::throw_exception("Failed in event queue pointer deserialization (invalid ID): location: %s, id=0x%x", msg, id);
	}

	Emu.PostponeInitCode([id, &queue, msg_str = std::string{msg}]()
	{
		// Defer resolving
		queue = idm::get_unlocked<lv2_obj, lv2_event_queue>(id);

		if (!queue)
		{
			fmt::throw_exception("Failed in event queue pointer deserialization (not found): location: %s, id=0x%x", msg_str, id);
		}
	});

	// Null until resolved
	return {};
}

lv2_event_port::lv2_event_port(utils::serial& ar)
	: type(ar)
	, name(ar)
	, queue(lv2_event_queue::load_ptr(ar, queue, "eventport"))
{
}

void lv2_event_port::save(utils::serial& ar)
{
	ar(type, name);

	lv2_event_queue::save_ptr(ar, queue.get());
}

shared_ptr<lv2_event_queue> lv2_event_queue::find(u64 ipc_key)
{
	ppu_thread* trace_ppu = get_vsh_ppu_for_event_trace();

	if (ipc_key == SYS_EVENT_QUEUE_LOCAL)
	{
		if (trace_ppu)
		{
			MPDBG_LOG(sys_event, "VSH_LV2_EVENT_QUEUE_LOOKUP: queue_id=0x%x key=0x%llx qname=0x%llx owner_pid=%u cia=0x%x lr=0x%llx found=0 local=1",
				0, ipc_key, 0, trace_ppu->owner_pid, trace_ppu->cia, trace_ppu->lr);
		}

		// Invalid IPC key
		return {};
	}

	auto queue = fxo::get<ipc_manager<lv2_event_queue, u64>>().get(ipc_key);

	if (trace_ppu)
	{
		MPDBG_LOG(sys_event, "VSH_LV2_EVENT_QUEUE_LOOKUP: queue_id=0x%x key=0x%llx qname=0x%llx owner_pid=%u cia=0x%x lr=0x%llx found=%d queued=%zu",
			queue ? queue->id : 0, ipc_key, queue ? queue->name : 0, trace_ppu->owner_pid, trace_ppu->cia, trace_ppu->lr, !!queue, queue ? queue->events.size() : 0);
	}

	return queue;
}

extern void resume_spu_thread_group_from_waiting(spu_thread& spu, std::array<shared_ptr<named_thread<spu_thread>>, 8>& notify_spus);

CellError lv2_event_queue::send(lv2_event event, bool* notified_thread, lv2_event_port* port)
{
	if (notified_thread)
	{
		*notified_thread = false;
	}

	struct notify_spus_t 
	{
		std::array<shared_ptr<named_thread<spu_thread>>, 8> spus;

		~notify_spus_t() noexcept
		{
			for (auto& spu : spus)
			{
				if (spu && spu->state & cpu_flag::wait)
				{
					spu->state.notify_one();
				}
			}
		}
	} notify_spus{};

	std::lock_guard lock(mutex);

	if (!exists)
	{
		return CELL_ENOTCONN;
	}

	if (!pq && !sq)
	{
		if (events.size() < this->size + 0u)
		{
			// Save event
			events.emplace_back(event);
			return {};
		}

		return CELL_EBUSY;
	}

	if (type == SYS_PPU_QUEUE)
	{
		// Store event in registers
		auto& ppu = static_cast<ppu_thread&>(*schedule<ppu_thread>(pq, protocol));

		if (ppu.state & cpu_flag::again)
		{
			if (auto cpu = get_current_cpu_thread())
			{
				cpu->state += cpu_flag::again + cpu_flag::exit;
			}

			sys_event.warning("Ignored event!");

			// Fake error for abort
			return CELL_EAGAIN;
		}

		std::tie(ppu.gpr[4], ppu.gpr[5], ppu.gpr[6], ppu.gpr[7]) = event;

		awake(&ppu);

		if (port)
		{
			if (ppu_thread* const current_ppu = cpu_thread::get_current<ppu_thread>();
				current_ppu && ppu.prio.load().prio < current_ppu->prio.load().prio)
			{
				// Block event port disconnection for the time being of sending events.
				// PPU -> lower prio PPU is the only case that can cause thread blocking.
				port->is_busy++;
				ensure(notified_thread);
				*notified_thread = true;
			}
		}
	}
	else
	{
		// Store event in In_MBox
		auto& spu = static_cast<spu_thread&>(*schedule<spu_thread>(sq, protocol));

		if (spu.state & cpu_flag::again)
		{
			if (auto cpu = get_current_cpu_thread())
			{
				cpu->state += cpu_flag::exit + cpu_flag::again;
			}

			sys_event.warning("Ignored event!");

			// Fake error for abort
			return CELL_EAGAIN;
		}

		const u32 data1 = static_cast<u32>(std::get<1>(event));
		const u32 data2 = static_cast<u32>(std::get<2>(event));
		const u32 data3 = static_cast<u32>(std::get<3>(event));
		spu.ch_in_mbox.set_values(4, CELL_OK, data1, data2, data3);
		resume_spu_thread_group_from_waiting(spu, notify_spus.spus);
	}

	return {};
}

error_code sys_event_queue_create(cpu_thread& cpu, vm::ptr<u32> equeue_id, vm::ptr<sys_event_queue_attribute_t> attr, u64 ipc_key, s32 size)
{
	cpu.state += cpu_flag::wait;
	ppu_thread* trace_ppu = get_vsh_ppu_for_event_trace(&cpu);

	sys_event.warning("sys_event_queue_create(equeue_id=*0x%x, attr=*0x%x, ipc_key=0x%llx, size=%d)", equeue_id, attr, ipc_key, size);

	if (size <= 0 || size > 127)
	{
		if (trace_ppu)
		{
			MPDBG_LOG(sys_event, "EVENT_QUEUE_CREATE: owner_pid=%u id=0x%x name=%s equeue_ptr=0x%x attr=0x%x ipc_key=0x%llx size=%d ret=0x%x",
				trace_ppu->owner_pid, trace_ppu->id, trace_ppu->get_name(), equeue_id.addr(), attr.addr(), ipc_key, size, static_cast<u32>(CELL_EINVAL));
		}

		return CELL_EINVAL;
	}

	const u32 protocol = attr->protocol;
	const u32 type = attr->type;
	const u64 name = attr->name_u64;

	if (protocol != SYS_SYNC_FIFO && protocol != SYS_SYNC_PRIORITY)
	{
		sys_event.error("sys_event_queue_create(): unknown protocol (0x%x)", protocol);
		if (trace_ppu)
		{
			MPDBG_LOG(sys_event, "EVENT_QUEUE_CREATE: owner_pid=%u id=0x%x name=%s equeue_ptr=0x%x attr=0x%x ipc_key=0x%llx size=%d protocol=0x%x type=0x%x qname=0x%llx ret=0x%x",
				trace_ppu->owner_pid, trace_ppu->id, trace_ppu->get_name(), equeue_id.addr(), attr.addr(), ipc_key, size, protocol, type, name, static_cast<u32>(CELL_EINVAL));
		}

		return CELL_EINVAL;
	}

	if (type != SYS_PPU_QUEUE && type != SYS_SPU_QUEUE)
	{
		sys_event.error("sys_event_queue_create(): unknown type (0x%x)", type);
		if (trace_ppu)
		{
			MPDBG_LOG(sys_event, "EVENT_QUEUE_CREATE: owner_pid=%u id=0x%x name=%s equeue_ptr=0x%x attr=0x%x ipc_key=0x%llx size=%d protocol=0x%x type=0x%x qname=0x%llx ret=0x%x",
				trace_ppu->owner_pid, trace_ppu->id, trace_ppu->get_name(), equeue_id.addr(), attr.addr(), ipc_key, size, protocol, type, name, static_cast<u32>(CELL_EINVAL));
		}

		return CELL_EINVAL;
	}

	const u32 pshared = ipc_key == SYS_EVENT_QUEUE_LOCAL ? SYS_SYNC_NOT_PROCESS_SHARED : SYS_SYNC_PROCESS_SHARED;
	constexpr u32 flags = SYS_SYNC_NEWLY_CREATED;

	if (const auto error = lv2_obj::create<lv2_event_queue>(pshared, ipc_key, flags, [&]()
	{
		return make_shared<lv2_event_queue>(protocol, type, size, name, ipc_key);
	}))
	{
		if (trace_ppu)
		{
			MPDBG_LOG(sys_event, "EVENT_QUEUE_CREATE: owner_pid=%u id=0x%x name=%s equeue_ptr=0x%x attr=0x%x ipc_key=0x%llx size=%d protocol=0x%x type=0x%x qname=0x%llx ret=0x%x",
				trace_ppu->owner_pid, trace_ppu->id, trace_ppu->get_name(), equeue_id.addr(), attr.addr(), ipc_key, size, protocol, type, name, static_cast<u32>(error));
		}

		return error;
	}

	cpu.check_state();
	*equeue_id = idm::last_id();
	if (trace_ppu)
	{
		MPDBG_LOG(sys_event, "EVENT_QUEUE_CREATE: owner_pid=%u id=0x%x name=%s equeue_ptr=0x%x attr=0x%x equeue_id=0x%x ipc_key=0x%llx size=%d protocol=0x%x type=0x%x qname=0x%llx ret=0x%x",
			trace_ppu->owner_pid, trace_ppu->id, trace_ppu->get_name(), equeue_id.addr(), attr.addr(), *equeue_id, ipc_key, size, protocol, type, name, static_cast<u32>(CELL_OK));
		MPDBG_LOG(sys_event, "VSH_LV2_EVENT_QUEUE_CREATE: queue_id=0x%x key=0x%llx qname=0x%llx owner_pid=%u cia=0x%x lr=0x%llx",
			*equeue_id, ipc_key, name, trace_ppu->owner_pid, trace_ppu->cia, trace_ppu->lr);
	}

	return CELL_OK;
}

error_code sys_event_queue_destroy(ppu_thread& ppu, u32 equeue_id, s32 mode)
{
	ppu.state += cpu_flag::wait;

	sys_event.warning("sys_event_queue_destroy(equeue_id=0x%x, mode=%d)", equeue_id, mode);

	if (mode && mode != SYS_EVENT_QUEUE_DESTROY_FORCE)
	{
		return CELL_EINVAL;
	}

	struct notify_spus_t 
	{
		std::array<shared_ptr<named_thread<spu_thread>>, 8> spus;

		~notify_spus_t() noexcept
		{
			for (auto& spu : spus)
			{
				if (spu && spu->state & cpu_flag::wait)
				{
					spu->state.notify_one();
				}
			}
		}
	} notify_spus{};

	std::vector<lv2_event> events;

	std::unique_lock<shared_mutex> qlock;

	cpu_thread* head{};

	const auto queue = idm::withdraw<lv2_obj, lv2_event_queue>(equeue_id, [&](lv2_event_queue& queue) -> CellError
	{
		qlock = std::unique_lock{queue.mutex};

		head = queue.type == SYS_PPU_QUEUE ? static_cast<cpu_thread*>(+queue.pq) : +queue.sq;

		if (!mode && head)
		{
			return CELL_EBUSY;
		}

		for (auto cpu = head; cpu; cpu = cpu->get_next_cpu())
		{
			if (cpu->state & cpu_flag::again)
			{
				ppu.state += cpu_flag::again;
				return CELL_EAGAIN;
			}
		}

		if (!queue.events.empty())
		{
			// Copy events for logging, does not empty
			events.insert(events.begin(), queue.events.begin(), queue.events.end());
		}

		lv2_obj::on_id_destroy(queue, queue.key);

		if (!head)
		{
			qlock.unlock();
		}

		return {};
	});

	if (!queue)
	{
		return CELL_ESRCH;
	}

	if (ppu.state & cpu_flag::again)
	{
		return {};
	}

	if (queue.ret)
	{
		return queue.ret;
	}

	std::string lost_data;

	if (qlock.owns_lock())
	{
		if (sys_event.warning)
		{
			u32 size = 0;

			for (auto cpu = head; cpu; cpu = cpu->get_next_cpu())
			{
				size++;
			}

			fmt::append(lost_data, "Forcefully awaken waiters (%u):\n", size);

			for (auto cpu = head; cpu; cpu = cpu->get_next_cpu())
			{
				lost_data += cpu->get_name();
				lost_data += '\n';
			}
		}

		if (queue->type == SYS_PPU_QUEUE)
		{
			for (auto cpu = +queue->pq; cpu; cpu = cpu->next_cpu)
			{
				cpu->gpr[3] = CELL_ECANCELED;
				queue->append(cpu);
			}

			atomic_storage<ppu_thread*>::release(queue->pq, nullptr);
			lv2_obj::awake_all();
		}
		else
		{
			for (auto cpu = +queue->sq; cpu; cpu = cpu->next_cpu)
			{
				cpu->ch_in_mbox.set_values(1, CELL_ECANCELED);
				resume_spu_thread_group_from_waiting(*cpu, notify_spus.spus);
			}

			atomic_storage<spu_thread*>::release(queue->sq, nullptr);
		}

		qlock.unlock();
	}

	if (sys_event.warning)
	{
		if (!events.empty())
		{
			fmt::append(lost_data, "Unread queue events (%u):\n", events.size());
		}

		for (const lv2_event& evt : events)
		{
			fmt::append(lost_data, "data0=0x%x, data1=0x%x, data2=0x%x, data3=0x%x\n"
				, std::get<0>(evt), std::get<1>(evt), std::get<2>(evt), std::get<3>(evt));
		}

		if (!lost_data.empty())
		{
			sys_event.warning("sys_event_queue_destroy(): %s", lost_data);
		}
	}

	return CELL_OK;
}

error_code sys_event_queue_tryreceive(ppu_thread& ppu, u32 equeue_id, vm::ptr<sys_event_t> event_array, s32 size, vm::ptr<u32> number)
{
	ppu.state += cpu_flag::wait;

	sys_event.trace("sys_event_queue_tryreceive(equeue_id=0x%x, event_array=*0x%x, size=%d, number=*0x%x)", equeue_id, event_array, size, number);

	const auto queue = idm::get_unlocked<lv2_obj, lv2_event_queue>(equeue_id);

	if (!queue)
	{
		return CELL_ESRCH;
	}

	if (queue->type != SYS_PPU_QUEUE)
	{
		return CELL_EINVAL;
	}

	std::array<sys_event_t, 127> events;

	std::unique_lock lock(queue->mutex);

	if (!queue->exists)
	{
		return CELL_ESRCH;
	}

	s32 count = 0;

	while (count < size && !queue->events.empty())
	{
		auto& dest = events[count++];
		std::tie(dest.source, dest.data1, dest.data2, dest.data3) = queue->events.front();
		queue->events.pop_front();
	}

	lock.unlock();
	ppu.check_state();

	std::copy_n(events.begin(), count, event_array.get_ptr());
	*number = count;

	return CELL_OK;
}

error_code sys_event_queue_receive(ppu_thread& ppu, u32 equeue_id, vm::ptr<sys_event_t> dummy_event, u64 timeout)
{
	ppu.state += cpu_flag::wait;

	sys_event.trace("sys_event_queue_receive(equeue_id=0x%x, *0x%x, timeout=0x%llx)", equeue_id, dummy_event, timeout);

	ppu.gpr[3] = CELL_OK;

	const auto queue = idm::get<lv2_obj, lv2_event_queue>(equeue_id, [&, notify = lv2_obj::notify_all_t()](lv2_event_queue& queue) -> CellError
	{
		if (queue.type != SYS_PPU_QUEUE)
		{
			return CELL_EINVAL;
		}

		lv2_obj::prepare_for_sleep(ppu);

		std::lock_guard lock(queue.mutex);

		// "/dev_flash/vsh/module/msmw2.sprx" seems to rely on some cryptic shared memory behaviour that we don't emulate correctly
		// This is a hack to avoid waiting for 1m40s every time we boot vsh
		if (queue.key == 0x8005911000000012 && Emu.IsVsh())
		{
			sys_event.todo("sys_event_queue_receive(equeue_id=0x%x, *0x%x, timeout=0x%llx) Bypassing timeout for msmw2.sprx", equeue_id, dummy_event, timeout);
			timeout = 1;
		}

		if (queue.events.empty())
		{
			if (ppu.owner_pid == 1)
			{
				MPDBG_LOG(sys_event, "EVENT_QUEUE_RECEIVE_WAIT: owner_pid=%u id=0x%x name=%s equeue_id=0x%x key=0x%llx qname=0x%llx timeout=0x%llx queued=0 waiters_ppu=%d waiters_spu=%d",
					ppu.owner_pid, ppu.id, ppu.get_name(), equeue_id, queue.key, queue.name, timeout, !!queue.pq, !!queue.sq);
			}

			// Diagnostic: when a non-pid-1 thread is about to block on an event
			// queue, log the queue key + owner + CIA + timeout. Helps identify
			// cross-process IPC stalls where the producer is in a different
			// (suspended) process. Drop once per-process IPC tracking is
			// implemented in sys_event / cellAudio / sys_rsxaudio so cross-
			// process queue lookups resolve through per-process owners.
			if (ppu.owner_pid != 1)
			{
				sys_event.notice("sys_event_queue_receive blocking: pid=%u name=%s cia=0x%x equeue_id=0x%x key=0x%llx type=%u timeout=0x%llx",
					ppu.owner_pid, ppu.get_name(), ppu.cia, equeue_id, queue.key, queue.type, timeout);
			}

			queue.sleep(ppu, timeout);
			lv2_obj::emplace(queue.pq, &ppu);
			return CELL_EBUSY;
		}

		std::tie(ppu.gpr[4], ppu.gpr[5], ppu.gpr[6], ppu.gpr[7]) = queue.events.front();
		queue.events.pop_front();
		if (ppu.owner_pid == 1)
		{
			MPDBG_LOG(sys_event, "EVENT_QUEUE_RECEIVE: owner_pid=%u id=0x%x name=%s equeue_id=0x%x key=0x%llx qname=0x%llx timeout=0x%llx ret=0x%x source=0x%llx data1=0x%llx data2=0x%llx data3=0x%llx queued=1",
				ppu.owner_pid, ppu.id, ppu.get_name(), equeue_id, queue.key, queue.name, timeout, static_cast<u32>(CELL_OK), ppu.gpr[4], ppu.gpr[5], ppu.gpr[6], ppu.gpr[7]);
		}

		return {};
	});

	if (!queue)
	{
		if (ppu.owner_pid == 1)
		{
			MPDBG_LOG(sys_event, "EVENT_QUEUE_RECEIVE: owner_pid=%u id=0x%x name=%s equeue_id=0x%x timeout=0x%llx ret=0x%x missing=1",
				ppu.owner_pid, ppu.id, ppu.get_name(), equeue_id, timeout, static_cast<u32>(CELL_ESRCH));
		}

		return CELL_ESRCH;
	}

	if (queue.ret)
	{
		if (queue.ret != CELL_EBUSY)
		{
			if (ppu.owner_pid == 1)
			{
				MPDBG_LOG(sys_event, "EVENT_QUEUE_RECEIVE: owner_pid=%u id=0x%x name=%s equeue_id=0x%x key=0x%llx qname=0x%llx timeout=0x%llx ret=0x%x early=1",
					ppu.owner_pid, ppu.id, ppu.get_name(), equeue_id, queue->key, queue->name, timeout, static_cast<u32>(queue.ret));
			}

			return queue.ret;
		}
	}
	else
	{
		if (ppu.owner_pid == 1)
		{
			MPDBG_LOG(sys_event, "EVENT_QUEUE_RECEIVE: owner_pid=%u id=0x%x name=%s equeue_id=0x%x key=0x%llx qname=0x%llx timeout=0x%llx ret=0x%x source=0x%llx data1=0x%llx data2=0x%llx data3=0x%llx queued=1",
				ppu.owner_pid, ppu.id, ppu.get_name(), equeue_id, queue->key, queue->name, timeout, static_cast<u32>(CELL_OK), ppu.gpr[4], ppu.gpr[5], ppu.gpr[6], ppu.gpr[7]);
		}

		return CELL_OK;
	}

	// If cancelled, gpr[3] will be non-zero. Other registers must contain event data.
	while (auto state = +ppu.state)
	{
		if (state & cpu_flag::signal && ppu.state.test_and_reset(cpu_flag::signal))
		{
			break;
		}

		if (is_stopped(state))
		{
			std::lock_guard lock_rsx(queue->mutex);

			for (auto cpu = +queue->pq; cpu; cpu = cpu->next_cpu)
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

				if (!atomic_storage<ppu_thread*>::load(queue->pq))
				{
					// Waiters queue is empty, so the thread must have been signaled
					queue->mutex.lock_unlock();
					break;
				}

				std::lock_guard lock(queue->mutex);

				if (!queue->unqueue(queue->pq, &ppu))
				{
					break;
				}

				ppu.gpr[3] = CELL_ETIMEDOUT;
				break;
			}
		}
		else
		{
			ppu.state.wait(state);
		}
	}

	if (ppu.owner_pid == 1)
	{
		MPDBG_LOG(sys_event, "EVENT_QUEUE_RECEIVE: owner_pid=%u id=0x%x name=%s equeue_id=0x%x key=0x%llx qname=0x%llx timeout=0x%llx ret=0x%llx source=0x%llx data1=0x%llx data2=0x%llx data3=0x%llx waited=1",
			ppu.owner_pid, ppu.id, ppu.get_name(), equeue_id, queue->key, queue->name, timeout, ppu.gpr[3], ppu.gpr[4], ppu.gpr[5], ppu.gpr[6], ppu.gpr[7]);
	}

	return not_an_error(ppu.gpr[3]);
}

error_code sys_event_queue_drain(ppu_thread& ppu, u32 equeue_id)
{
	ppu.state += cpu_flag::wait;

	sys_event.trace("sys_event_queue_drain(equeue_id=0x%x)", equeue_id);

	const auto queue = idm::check<lv2_obj, lv2_event_queue>(equeue_id, [&](lv2_event_queue& queue)
	{
		std::lock_guard lock(queue.mutex);

		queue.events.clear();
	});

	if (!queue)
	{
		return CELL_ESRCH;
	}

	return CELL_OK;
}

error_code sys_event_port_create(cpu_thread& cpu, vm::ptr<u32> eport_id, s32 port_type, u64 name)
{
	cpu.state += cpu_flag::wait;
	ppu_thread* trace_ppu = get_vsh_ppu_for_event_trace(&cpu);

	sys_event.warning("sys_event_port_create(eport_id=*0x%x, port_type=%d, name=0x%llx)", eport_id, port_type, name);

	if (port_type != SYS_EVENT_PORT_LOCAL && port_type != SYS_EVENT_PORT_IPC)
	{
		sys_event.error("sys_event_port_create(): unknown port type (%d)", port_type);
		if (trace_ppu)
		{
			MPDBG_LOG(sys_event, "EVENT_PORT_CREATE: owner_pid=%u id=0x%x name=%s eport_ptr=0x%x port_type=%d port_name=0x%llx ret=0x%x cia=0x%x lr=0x%llx",
				trace_ppu->owner_pid, trace_ppu->id, trace_ppu->get_name(), eport_id.addr(), port_type, name, static_cast<u32>(CELL_EINVAL), trace_ppu->cia, trace_ppu->lr);
		}

		return CELL_EINVAL;
	}

	if (const u32 id = idm::make<lv2_obj, lv2_event_port>(port_type, name))
	{
		cpu.check_state();
		*eport_id = id;
		if (trace_ppu)
		{
			MPDBG_LOG(sys_event, "EVENT_PORT_CREATE: owner_pid=%u id=0x%x name=%s eport_ptr=0x%x eport_id=0x%x port_type=%d port_name=0x%llx ret=0x%x cia=0x%x lr=0x%llx",
				trace_ppu->owner_pid, trace_ppu->id, trace_ppu->get_name(), eport_id.addr(), *eport_id, port_type, name, static_cast<u32>(CELL_OK), trace_ppu->cia, trace_ppu->lr);
			MPDBG_LOG(sys_event, "VSH_LV2_EVENT_PORT_CREATE: port_id=0x%x port_type=%d port_name=0x%llx owner_pid=%u cia=0x%x lr=0x%llx",
				*eport_id, port_type, name, trace_ppu->owner_pid, trace_ppu->cia, trace_ppu->lr);
		}

		return CELL_OK;
	}

	if (trace_ppu)
	{
		MPDBG_LOG(sys_event, "EVENT_PORT_CREATE: owner_pid=%u id=0x%x name=%s eport_ptr=0x%x port_type=%d port_name=0x%llx ret=0x%x cia=0x%x lr=0x%llx",
			trace_ppu->owner_pid, trace_ppu->id, trace_ppu->get_name(), eport_id.addr(), port_type, name, static_cast<u32>(CELL_EAGAIN), trace_ppu->cia, trace_ppu->lr);
	}

	return CELL_EAGAIN;
}

error_code sys_event_port_destroy(ppu_thread& ppu, u32 eport_id)
{
	ppu.state += cpu_flag::wait;

	sys_event.warning("sys_event_port_destroy(eport_id=0x%x)", eport_id);

	const auto port = idm::withdraw<lv2_obj, lv2_event_port>(eport_id, [](lv2_event_port& port) -> CellError
	{
		if (lv2_obj::check(port.queue))
		{
			return CELL_EISCONN;
		}

		return {};
	});

	if (!port)
	{
		return CELL_ESRCH;
	}

	if (port.ret)
	{
		return port.ret;
	}

	return CELL_OK;
}

error_code sys_event_port_connect_local(cpu_thread& cpu, u32 eport_id, u32 equeue_id)
{
	cpu.state += cpu_flag::wait;
	ppu_thread* trace_ppu = get_vsh_ppu_for_event_trace(&cpu);

	sys_event.warning("sys_event_port_connect_local(eport_id=0x%x, equeue_id=0x%x)", eport_id, equeue_id);

	std::lock_guard lock(id_manager::g_mutex);

	const auto port = idm::check_unlocked<lv2_obj, lv2_event_port>(eport_id);
	auto queue = idm::get_unlocked<lv2_obj, lv2_event_queue>(equeue_id);

	if (!port || !queue)
	{
		if (trace_ppu)
		{
			MPDBG_LOG(sys_event, "EVENT_PORT_CONNECT_LOCAL: owner_pid=%u id=0x%x name=%s eport_id=0x%x equeue_id=0x%x ret=0x%x missing_port=%d missing_queue=%d",
				trace_ppu->owner_pid, trace_ppu->id, trace_ppu->get_name(), eport_id, equeue_id, static_cast<u32>(CELL_ESRCH), !port, !queue);
		}

		return CELL_ESRCH;
	}

	if (port->type != SYS_EVENT_PORT_LOCAL)
	{
		if (trace_ppu)
		{
			MPDBG_LOG(sys_event, "EVENT_PORT_CONNECT_LOCAL: owner_pid=%u id=0x%x name=%s eport_id=0x%x equeue_id=0x%x qname=0x%llx key=0x%llx port_type=%d ret=0x%x",
				trace_ppu->owner_pid, trace_ppu->id, trace_ppu->get_name(), eport_id, equeue_id, queue->name, queue->key, port->type, static_cast<u32>(CELL_EINVAL));
		}

		return CELL_EINVAL;
	}

	if (lv2_obj::check(port->queue))
	{
		if (trace_ppu)
		{
			MPDBG_LOG(sys_event, "EVENT_PORT_CONNECT_LOCAL: owner_pid=%u id=0x%x name=%s eport_id=0x%x equeue_id=0x%x qname=0x%llx key=0x%llx ret=0x%x already=1",
				trace_ppu->owner_pid, trace_ppu->id, trace_ppu->get_name(), eport_id, equeue_id, queue->name, queue->key, static_cast<u32>(CELL_EISCONN));
		}

		return CELL_EISCONN;
	}

	const u64 qname = queue->name;
	const u64 qkey = queue->key;
	port->queue = std::move(queue);

	if (trace_ppu)
	{
		MPDBG_LOG(sys_event, "EVENT_PORT_CONNECT_LOCAL: owner_pid=%u id=0x%x name=%s eport_id=0x%x equeue_id=0x%x qname=0x%llx key=0x%llx ret=0x%x",
			trace_ppu->owner_pid, trace_ppu->id, trace_ppu->get_name(), eport_id, equeue_id, qname, qkey, static_cast<u32>(CELL_OK));
		MPDBG_LOG(sys_event, "VSH_LV2_EVENT_PORT_CONNECT_LOCAL: port_id=0x%x target_queue=0x%x key=0x%llx qname=0x%llx owner_pid=%u cia=0x%x lr=0x%llx",
			eport_id, equeue_id, qkey, qname, trace_ppu->owner_pid, trace_ppu->cia, trace_ppu->lr);
	}

	return CELL_OK;
}

error_code sys_event_port_connect_ipc(ppu_thread& ppu, u32 eport_id, u64 ipc_key)
{
	ppu.state += cpu_flag::wait;

	sys_event.warning("sys_event_port_connect_ipc(eport_id=0x%x, ipc_key=0x%x)", eport_id, ipc_key);

	if (ipc_key == 0)
	{
		if (ppu.owner_pid == 1)
		{
			MPDBG_LOG(sys_event, "EVENT_PORT_CONNECT_IPC: owner_pid=%u id=0x%x name=%s eport_id=0x%x ipc_key=0x%llx ret=0x%x cia=0x%x lr=0x%llx",
				ppu.owner_pid, ppu.id, ppu.get_name(), eport_id, ipc_key, static_cast<u32>(CELL_EINVAL), ppu.cia, ppu.lr);
			MPDBG_LOG(sys_event, "VSH_LV2_EVENT_PORT_CONNECT_IPC: port_id=0x%x target_queue=0x%x key=0x%llx qname=0x%llx owner_pid=%u cia=0x%x lr=0x%llx ret=0x%x missing_port=0 missing_queue=1",
				eport_id, 0, ipc_key, 0, ppu.owner_pid, ppu.cia, ppu.lr, static_cast<u32>(CELL_EINVAL));
		}

		return CELL_EINVAL;
	}

	auto queue = lv2_event_queue::find(ipc_key);

	std::lock_guard lock(id_manager::g_mutex);

	const auto port = idm::check_unlocked<lv2_obj, lv2_event_port>(eport_id);

	if (!port || !queue)
	{
		if (ppu.owner_pid == 1)
		{
			MPDBG_LOG(sys_event, "EVENT_PORT_CONNECT_IPC: owner_pid=%u id=0x%x name=%s eport_id=0x%x ipc_key=0x%llx ret=0x%x missing_port=%d missing_queue=%d cia=0x%x lr=0x%llx",
				ppu.owner_pid, ppu.id, ppu.get_name(), eport_id, ipc_key, static_cast<u32>(CELL_ESRCH), !port, !queue, ppu.cia, ppu.lr);
			MPDBG_LOG(sys_event, "VSH_LV2_EVENT_PORT_CONNECT_IPC: port_id=0x%x target_queue=0x%x key=0x%llx qname=0x%llx owner_pid=%u cia=0x%x lr=0x%llx ret=0x%x missing_port=%d missing_queue=%d",
				eport_id, queue ? queue->id : 0, ipc_key, queue ? queue->name : 0, ppu.owner_pid, ppu.cia, ppu.lr, static_cast<u32>(CELL_ESRCH), !port, !queue);
		}

		return CELL_ESRCH;
	}

	if (port->type != SYS_EVENT_PORT_IPC)
	{
		if (ppu.owner_pid == 1)
		{
			MPDBG_LOG(sys_event, "EVENT_PORT_CONNECT_IPC: owner_pid=%u id=0x%x name=%s eport_id=0x%x equeue_id=0x%x ipc_key=0x%llx qname=0x%llx port_type=%d ret=0x%x cia=0x%x lr=0x%llx",
				ppu.owner_pid, ppu.id, ppu.get_name(), eport_id, queue->id, ipc_key, queue->name, port->type, static_cast<u32>(CELL_EINVAL), ppu.cia, ppu.lr);
			MPDBG_LOG(sys_event, "VSH_LV2_EVENT_PORT_CONNECT_IPC: port_id=0x%x target_queue=0x%x key=0x%llx qname=0x%llx owner_pid=%u cia=0x%x lr=0x%llx ret=0x%x port_type=%d",
				eport_id, queue->id, ipc_key, queue->name, ppu.owner_pid, ppu.cia, ppu.lr, static_cast<u32>(CELL_EINVAL), port->type);
		}

		return CELL_EINVAL;
	}

	if (lv2_obj::check(port->queue))
	{
		if (ppu.owner_pid == 1)
		{
			MPDBG_LOG(sys_event, "EVENT_PORT_CONNECT_IPC: owner_pid=%u id=0x%x name=%s eport_id=0x%x equeue_id=0x%x ipc_key=0x%llx qname=0x%llx ret=0x%x already=1 cia=0x%x lr=0x%llx",
				ppu.owner_pid, ppu.id, ppu.get_name(), eport_id, queue->id, ipc_key, queue->name, static_cast<u32>(CELL_EISCONN), ppu.cia, ppu.lr);
			MPDBG_LOG(sys_event, "VSH_LV2_EVENT_PORT_CONNECT_IPC: port_id=0x%x target_queue=0x%x key=0x%llx qname=0x%llx owner_pid=%u cia=0x%x lr=0x%llx ret=0x%x already=1",
				eport_id, queue->id, ipc_key, queue->name, ppu.owner_pid, ppu.cia, ppu.lr, static_cast<u32>(CELL_EISCONN));
		}

		return CELL_EISCONN;
	}

	const u32 qid = queue->id;
	const u64 qname = queue->name;
	port->queue = std::move(queue);

	if (ppu.owner_pid == 1)
	{
		MPDBG_LOG(sys_event, "EVENT_PORT_CONNECT_IPC: owner_pid=%u id=0x%x name=%s eport_id=0x%x equeue_id=0x%x ipc_key=0x%llx qname=0x%llx ret=0x%x cia=0x%x lr=0x%llx",
			ppu.owner_pid, ppu.id, ppu.get_name(), eport_id, qid, ipc_key, qname, static_cast<u32>(CELL_OK), ppu.cia, ppu.lr);
		MPDBG_LOG(sys_event, "VSH_LV2_EVENT_PORT_CONNECT_IPC: port_id=0x%x target_queue=0x%x key=0x%llx qname=0x%llx owner_pid=%u cia=0x%x lr=0x%llx ret=0x%x",
			eport_id, qid, ipc_key, qname, ppu.owner_pid, ppu.cia, ppu.lr, static_cast<u32>(CELL_OK));
	}

	return CELL_OK;
}

error_code sys_event_port_disconnect(ppu_thread& ppu, u32 eport_id)
{
	ppu.state += cpu_flag::wait;

	sys_event.warning("sys_event_port_disconnect(eport_id=0x%x)", eport_id);

	std::lock_guard lock(id_manager::g_mutex);

	const auto port = idm::check_unlocked<lv2_obj, lv2_event_port>(eport_id);

	if (!port)
	{
		return CELL_ESRCH;
	}

	if (!lv2_obj::check(port->queue))
	{
		return CELL_ENOTCONN;
	}

	if (port->is_busy)
	{
		return CELL_EBUSY;
	}

	port->queue.reset();

	return CELL_OK;
}

error_code sys_event_port_send(u32 eport_id, u64 data1, u64 data2, u64 data3)
{
	const auto cpu = cpu_thread::get_current();
	const auto ppu = cpu ? cpu->try_get<ppu_thread>() : nullptr;
	const bool trace_vsh = ppu && ppu->owner_pid == 1;

	if (cpu)
	{
		cpu->state += cpu_flag::wait;
	}

	sys_event.trace("sys_event_port_send(eport_id=0x%x, data1=0x%llx, data2=0x%llx, data3=0x%llx)", eport_id, data1, data2, data3);

	bool notified_thread = false;

	const auto port = idm::check<lv2_obj, lv2_event_port>(eport_id, [&, notify = lv2_obj::notify_all_t()](lv2_event_port& port) -> CellError
	{
		if (ppu && ppu->loaded_from_savestate)
		{
			port.is_busy++;
			notified_thread = true;
			return {};
		}

		if (lv2_obj::check(port.queue))
		{
			const u64 source = port.name ? port.name : (u64{process_getpid() + 0u} << 32) | u64{eport_id};
			ppu_thread* target_vsh = get_vsh_waiter_for_event_trace(*port.queue);
			if (trace_vsh || target_vsh)
			{
				MPDBG_LOG(sys_event, "EVENT_PORT_SEND: owner_pid=%u id=0x%x name=%s eport_id=0x%x target_queue=0x%x qname=0x%llx qkey=0x%llx target_vsh_waiter=0x%x target_vsh_name=%s source=0x%llx data1=0x%llx data2=0x%llx data3=0x%llx queued_before=%zu wait_ppu=%d ret=pending",
					ppu ? ppu->owner_pid : 0, ppu ? ppu->id : 0, ppu ? ppu->get_name() : "<host>", eport_id,
					port.queue->id, port.queue->name, port.queue->key, target_vsh ? target_vsh->id : 0, target_vsh ? target_vsh->get_name() : "",
					source, data1, data2, data3, port.queue->events.size(), !!port.queue->pq);
				MPDBG_LOG(sys_event, "VSH_LV2_EVENT_PORT_SEND: port_id=0x%x target_queue=0x%x key=0x%llx qname=0x%llx data1=0x%llx data2=0x%llx data3=0x%llx source_pid=%u cia=0x%x lr=0x%llx",
					eport_id, port.queue->id, port.queue->key, port.queue->name, data1, data2, data3, ppu ? ppu->owner_pid : process_getpid(), ppu ? ppu->cia : 0, ppu ? ppu->lr : 0);

				if (port.queue->key == 0x8005911000000024ull && source == 0x8005911000000023ull && data1 == 5)
				{
					const u32 raw_ptr = static_cast<u32>(data3);
					const auto raw = dump_vsh_mcore_cmd_object(raw_ptr);
					const u32 shm_base = find_vsh_mcore_shm_base();
					const u32 translated_ptr = shm_base + raw_ptr;
					const auto translated = dump_vsh_mcore_cmd_object(translated_ptr);

					MPDBG_LOG(sys_event, "VSH_MCORE_CMD5_OBJECT: mode=raw ptr=0x%x ok20=%d ok30=%d w00=0x%x w04=0x%x w08=0x%x w0c=0x%x w10=0x%x w14=0x%x w18=0x%x w1c=0x%x inner_ok=%d i00=0x%x i04=0x%x i08=0x%x i0c=0x%x i10=0x%x i14=0x%x i18=0x%x i1c=0x%x",
						raw.ptr, raw.ok20, raw.ok30, raw.w00, raw.w04, raw.w08, raw.w0c, raw.w10, raw.w14, raw.w18, raw.w1c, raw.inner_ok, raw.i00, raw.i04, raw.i08, raw.i0c, raw.i10, raw.i14, raw.i18, raw.i1c);
					MPDBG_LOG(sys_event, "VSH_MCORE_CMD5_OBJECT: mode=translated shm_base=0x%x data3=0x%x ptr=0x%x ok20=%d ok30=%d w00=0x%x w04=0x%x w08=0x%x w0c=0x%x w10=0x%x w14=0x%x w18=0x%x w1c=0x%x inner_ok=%d i00=0x%x i04=0x%x i08=0x%x i0c=0x%x i10=0x%x i14=0x%x i18=0x%x i1c=0x%x",
						shm_base, raw_ptr, translated.ptr, translated.ok20, translated.ok30, translated.w00, translated.w04, translated.w08, translated.w0c, translated.w10, translated.w14, translated.w18, translated.w1c, translated.inner_ok, translated.i00, translated.i04, translated.i08, translated.i0c, translated.i10, translated.i14, translated.i18, translated.i1c);
				}

				if (port.queue->key == 0x8005911000000024ull && source == 0x8005911000000023ull)
				{
					schedule_vsh_mcore_host_response(data1, data2, data3);
					MPDBG_LOG(sys_event, "VSH_MCORE_HOST_EVENT: phase=consume_parent_command_async port_id=0x%x child_queue=0x%x source=0x%llx data1=0x%llx data2=0x%llx data3=0x%llx result=0x%x",
						eport_id, port.queue->id, source, data1, data2, data3, static_cast<u32>(CELL_OK));
					return {};
				}
			}

			return port.queue->send(source, data1, data2, data3, &notified_thread, ppu && port.queue->type == SYS_PPU_QUEUE ? &port : nullptr);
		}

		if (trace_vsh)
		{
			MPDBG_LOG(sys_event, "EVENT_PORT_SEND: owner_pid=%u id=0x%x name=%s eport_id=0x%x data1=0x%llx data2=0x%llx data3=0x%llx ret=0x%x disconnected=1",
				ppu->owner_pid, ppu->id, ppu->get_name(), eport_id, data1, data2, data3, static_cast<u32>(CELL_ENOTCONN));
		}

		return CELL_ENOTCONN;
	});

	if (!port)
	{
		if (trace_vsh)
		{
			MPDBG_LOG(sys_event, "EVENT_PORT_SEND: owner_pid=%u id=0x%x name=%s eport_id=0x%x data1=0x%llx data2=0x%llx data3=0x%llx ret=0x%x missing_port=1",
				ppu->owner_pid, ppu->id, ppu->get_name(), eport_id, data1, data2, data3, static_cast<u32>(CELL_ESRCH));
		}

		return CELL_ESRCH;
	}

	if (ppu && notified_thread)
	{
		// Wait to be requeued
		if (ppu->test_stopped())
		{
			// Wait again on savestate load
			ppu->state += cpu_flag::again;
		}

		port->is_busy--;
		if (trace_vsh)
		{
			MPDBG_LOG(sys_event, "EVENT_PORT_SEND: owner_pid=%u id=0x%x name=%s eport_id=0x%x data1=0x%llx data2=0x%llx data3=0x%llx ret=0x%x notified_thread=1",
				ppu->owner_pid, ppu->id, ppu->get_name(), eport_id, data1, data2, data3, static_cast<u32>(CELL_OK));
		}

		return CELL_OK;
	}

	if (port.ret)
	{
		if (trace_vsh)
		{
			MPDBG_LOG(sys_event, "EVENT_PORT_SEND: owner_pid=%u id=0x%x name=%s eport_id=0x%x data1=0x%llx data2=0x%llx data3=0x%llx ret=0x%x notified_thread=0",
				ppu->owner_pid, ppu->id, ppu->get_name(), eport_id, data1, data2, data3, static_cast<u32>(port.ret));
		}

		if (port.ret == CELL_EAGAIN)
		{
			// Not really an error code exposed to games (thread has raised cpu_flag::again)
			return not_an_error(CELL_EAGAIN);
		}

		if (port.ret == CELL_EBUSY)
		{
			return not_an_error(CELL_EBUSY);
		}

		return port.ret;
	}

	if (trace_vsh)
	{
		MPDBG_LOG(sys_event, "EVENT_PORT_SEND: owner_pid=%u id=0x%x name=%s eport_id=0x%x data1=0x%llx data2=0x%llx data3=0x%llx ret=0x%x notified_thread=0",
			ppu->owner_pid, ppu->id, ppu->get_name(), eport_id, data1, data2, data3, static_cast<u32>(CELL_OK));
	}

	return CELL_OK;
}
