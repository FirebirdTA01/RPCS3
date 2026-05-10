#include "stdafx.h"
#include "lv2_process.h"

#include <cstring>

void lv2_process::reset()
{
	// Tear down per-process containers and zero the per-process VM state.
	// Caller (Emulator::destroy_process) is responsible for releasing
	// vm.base_addr / vm.sudo_addr / vm.exec_addr before calling reset();
	// this function does not touch those pointers.

	m_local_fxo.reset();
	m_jit_pool.clear();

	// Zero the vm_handle's per-process arrays. Single-threaded at the
	// destroy_process call site (the process has no live threads), so
	// memset on atomic_t arrays is well-defined here.
	std::memset(vm.page_flags.data(), 0, vm.page_flags.size() * sizeof(vm.page_flags[0]));
	std::memset(vm.reservations, 0, sizeof(vm.reservations));
	std::memset(vm.shmem, 0, sizeof(vm.shmem));
	std::memset(vm.range_lock_set, 0, sizeof(vm.range_lock_set));
	std::memset(vm.range_lock_bits, 0, sizeof(vm.range_lock_bits));
	vm.locations.clear();

	rsx_state = rsx::rsx_context_state{};

	m_app_version.clear();
	m_hash.clear();
	m_cat.clear();
	m_localized_title.clear();
	m_title.clear();
	m_dir.clear();
	m_sfo_dir.clear();
	m_game_dir = "PS3_GAME";
	m_path.clear();
	m_path_old.clear();
	m_path_original.clear();
	m_path_real.clear();
	m_title_id.clear();
	m_force_boot = false;
	m_config_mode = cfg_mode::custom;
	m_config_path.clear();
	m_db_config.reset();
	m_boot_source_type = 0;
	argv.clear();
	envp.clear();
	data.clear();
	disc.clear();
	klic.clear();
	hdd1.clear();
	init_mem_containers = nullptr;
	after_kill_callback = nullptr;
	m_pause_msgs_refs.clear();
	m_postponed_init_code.clear();
	m_precompilation_option = {};
	m_savestate_extension_flags1 = {};
	m_state_inspection_savestate = false;
	m_tty_file_init_pos = umax;
	m_ar.reset();
	m_pause_start_time = 0;
	m_pause_amend_time = 0;
	m_state = system_state::stopped;
	m_pid = 1;
	m_is_vsh = false;
}
