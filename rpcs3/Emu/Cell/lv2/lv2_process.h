#pragma once

#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <optional>
#include "util/types.hpp"
#include "util/atomic.hpp"
#include "Emu/config_mode.h"
#include "Utilities/bit_set.h"
#include "util/fixed_typemap.hpp"
#include "Emu/Memory/vm.h"
#include "Emu/RSX/RSXContext.h"

enum class system_state : u32
{
	stopped,
	loading,
	stopping,
	running,
	paused,
	frozen, // paused but cannot resume
	ready,
	starting,
};

enum class SaveStateExtentionFlags1 : u8
{
	SupportsMenuOpenResume,
	ShouldCloseMenu,

	__bitset_enum_max,
};

struct emu_precompilation_option_t
{
	bool is_fast = false;
};

class lv2_process
{
public:
	lv2_process() = default;

	// Reset all per-process state to fresh-default values, so that a future
	// create_process call can re-use the same slot index without leaking
	// state from the destroyed process. Must only be called from
	// Emulator::destroy_process AFTER the host VA mappings (vm.base_addr,
	// vm.sudo_addr, vm.exec_addr) have been released and nulled, and after
	// suspend_process has parked all threads belonging to this slot.
	// Currently dormant — destroyed slots are not re-created in the
	// debug-only multi-process API — but lands the teardown so the future
	// (III) milestone can re-use slots without surprise.
	void reset();

	// --- m_state ---
	system_state GetState() const { return m_state; }
	atomic_t<system_state>& RefState() { return m_state; }
	const atomic_t<system_state>& RefState() const { return m_state; }

	u32 pid() const { return m_pid; }
	void set_pid(u32 pid) { m_pid = pid; }

	stx::manual_typemap<lv2_process, 0x20'00000, 128>& local_fxo() { return m_local_fxo; }
	const stx::manual_typemap<lv2_process, 0x20'00000, 128>& local_fxo() const { return m_local_fxo; }

	vm::vm_handle& vm_handle() { return vm; }
	const vm::vm_handle& vm_handle() const { return vm; }

	rsx::rsx_context_state& rsx_ctx() { return rsx_state; }
	const rsx::rsx_context_state& rsx_ctx() const { return rsx_state; }

	// --- display strings ---
	const std::string& GetAppVersion() const { return m_app_version; }
	std::string& RefAppVersion() { return m_app_version; }
	void SetAppVersion(std::string v) { m_app_version = std::move(v); }
	void ClearAppVersion() { m_app_version.clear(); }

	const std::string& GetExecutableHash() const { return m_hash; }
	std::string& RefExecutableHash() { return m_hash; }
	void SetExecutableHash(std::string v) { m_hash = std::move(v); }
	void ClearExecutableHash() { m_hash.clear(); }

	const std::string& GetCat() const { return m_cat; }
	std::string& RefCat() { return m_cat; }
	void SetCat(std::string v) { m_cat = std::move(v); }
	void ClearCat() { m_cat.clear(); }

	const std::string& GetLocalizedTitle() const { return m_localized_title; }
	std::string& RefLocalizedTitle() { return m_localized_title; }
	void SetLocalizedTitle(std::string v) { m_localized_title = std::move(v); }
	void ClearLocalizedTitle() { m_localized_title.clear(); }

	const std::string& GetTitle() const { return m_title; }
	std::string& RefTitle() { return m_title; }
	void SetTitle(std::string v) { m_title = std::move(v); }
	void ClearTitle() { m_title.clear(); }

	// --- paths and dirs ---
	const std::string& GetDir() const { return m_dir; }
	std::string& RefDir() { return m_dir; }
	void SetDir(std::string v) { m_dir = std::move(v); }
	void ClearDir() { m_dir.clear(); }

	const std::string& GetSfoDir() const { return m_sfo_dir; }
	std::string& RefSfoDir() { return m_sfo_dir; }
	void SetSfoDir(std::string v) { m_sfo_dir = std::move(v); }
	void ClearSfoDir() { m_sfo_dir.clear(); }

	const std::string& GetGameDir() const { return m_game_dir; }
	std::string& RefGameDir() { return m_game_dir; }
	void SetGameDir(std::string v) { m_game_dir = std::move(v); }

	const std::string& GetPath() const { return m_path; }
	void SetPath(std::string v) { m_path = std::move(v); }
	void ClearPath() { m_path.clear(); }
	std::string& RefPath() { return m_path; }

	const std::string& GetPathOld() const { return m_path_old; }
	std::string& RefPathOld() { return m_path_old; }
	void SetPathOld(std::string v) { m_path_old = std::move(v); }
	void ClearPathOld() { m_path_old.clear(); }

	const std::string& GetPathOriginal() const { return m_path_original; }
	void SetPathOriginal(std::string v) { m_path_original = std::move(v); }
	void ClearPathOriginal() { m_path_original.clear(); }
	std::string& RefPathOriginal() { return m_path_original; }

	const std::string& GetPathReal() const { return m_path_real; }
	std::string& RefPathReal() { return m_path_real; }
	void SetPathReal(std::string v) { m_path_real = std::move(v); }
	void ClearPathReal() { m_path_real.clear(); }

	const std::string& GetTitleID() const { return m_title_id; }
	std::string& RefTitleID() { return m_title_id; }
	void SetTitleID(std::string v) { m_title_id = std::move(v); }
	void ClearTitleID() { m_title_id.clear(); }

	// --- flags, vectors, callbacks ---
	bool GetForceBoot() const { return m_force_boot; }
	void SetForceBoot(bool v) { m_force_boot = v; }
	bool& RefForceBoot() { return m_force_boot; }  // for std::exchange

	cfg_mode GetConfigMode() const { return m_config_mode; }
	void SetConfigMode(cfg_mode v) { m_config_mode = v; }
	cfg_mode& RefConfigMode() { return m_config_mode; }  // for std::tie

	const std::string& GetConfigPath() const { return m_config_path; }
	void SetConfigPath(std::string v) { m_config_path = std::move(v); }
	void ClearConfigPath() { m_config_path.clear(); }
	std::string& RefConfigPath() { return m_config_path; }  // for std::tie

	const std::optional<std::string>& GetDbConfig() const { return m_db_config; }
	void SetDbConfig(std::optional<std::string> v) { m_db_config = std::move(v); }
	std::optional<std::string>& RefDbConfig() { return m_db_config; }  // for std::tie

	const u32& GetBootSourceType() const { return m_boot_source_type; }
	u32& RefBootSourceType() { return m_boot_source_type; }
	void SetBootSourceType(u32 v) { m_boot_source_type = v; }

	// argv, envp, data, disc, klic, hdd1 — accessors
	std::vector<std::string>& RefArgv() { return argv; }
	const std::vector<std::string>& GetArgv() const { return argv; }
	std::vector<std::string>& RefEnvp() { return envp; }
	const std::vector<std::string>& GetEnvp() const { return envp; }
	std::vector<u8>& RefData() { return data; }
	std::string& RefDisc() { return disc; }
	std::vector<u128>& RefKlic() { return klic; }
	std::string& RefHdd1() { return hdd1; }

	std::function<void(u32)>& RefInitMemContainers() { return init_mem_containers; }
	std::function<void()>& RefAfterKillCallback() { return after_kill_callback; }

	std::vector<std::shared_ptr<atomic_t<u32>>>& RefPauseMsgsRefs() { return m_pause_msgs_refs; }

	std::vector<std::function<void()>>& RefPostponedInitCode() { return m_postponed_init_code; }
	void ExecPostponedInitCode()
	{
		for (auto&& func : ::as_rvalue(std::move(m_postponed_init_code)))
		{
			func();
		}
	}

	emu_precompilation_option_t GetPrecompilationOption() const { return m_precompilation_option; }
	emu_precompilation_option_t& RefPrecompilationOption() { return m_precompilation_option; }
	void SetPrecompilationOption(emu_precompilation_option_t v) { m_precompilation_option = v; }

	// --- savestate machinery ---
	bs_t<SaveStateExtentionFlags1>& RefSavestateExtensionFlags1() { return m_savestate_extension_flags1; }
	const bs_t<SaveStateExtentionFlags1>& GetSavestateExtensionFlags1() const { return m_savestate_extension_flags1; }

	bool GetStateInspectionSavestate() const { return m_state_inspection_savestate; }
	bool& RefStateInspectionSavestate() { return m_state_inspection_savestate; }
	void SetStateInspectionSavestate(bool v) { m_state_inspection_savestate = v; }

	usz& RefTtyFileInitPos() { return m_tty_file_init_pos; }
	const usz& GetTtyFileInitPos() const { return m_tty_file_init_pos; }

	std::shared_ptr<utils::serial>& RefAr() { return m_ar; }
	const std::shared_ptr<utils::serial>& GetAr() const { return m_ar; }

	atomic_t<u64>& RefPauseStartTime() { return m_pause_start_time; }
	u64 GetPauseStartTime() const { return m_pause_start_time.load(); }
	void SetPauseStartTime(u64 v) { m_pause_start_time = v; }

	atomic_t<u64>& RefPauseAmendTime() { return m_pause_amend_time; }
	u64 GetPauseAmendTime() const { return m_pause_amend_time.load(); }
	void SetPauseAmendTime(u64 v) { m_pause_amend_time = v; }

private:
	std::string m_app_version;
	std::string m_hash;
	std::string m_cat;
	std::string m_localized_title;
	std::string m_title;
	std::string m_dir;
	std::string m_sfo_dir;
	std::string m_game_dir{"PS3_GAME"};
	std::string m_path;
	std::string m_path_old;
	std::string m_path_original;
	std::string m_path_real;
	std::string m_title_id;
	bool m_force_boot = false;
	cfg_mode m_config_mode = cfg_mode::custom;
	std::string m_config_path;
	std::optional<std::string> m_db_config;
	u32 m_boot_source_type = 0;
	std::vector<std::string> argv;
	std::vector<std::string> envp;
	std::vector<u8> data;
	std::string disc;
	std::vector<u128> klic;
	std::string hdd1;
	std::function<void(u32)> init_mem_containers;
	std::function<void()> after_kill_callback;
	std::vector<std::shared_ptr<atomic_t<u32>>> m_pause_msgs_refs;
	std::vector<std::function<void()>> m_postponed_init_code;
	emu_precompilation_option_t m_precompilation_option{};
	bs_t<SaveStateExtentionFlags1> m_savestate_extension_flags1{};
	bool m_state_inspection_savestate = false;
	usz m_tty_file_init_pos = umax;
	std::shared_ptr<utils::serial> m_ar;
	atomic_t<u64> m_pause_start_time{0};
	atomic_t<u64> m_pause_amend_time{0};
	atomic_t<system_state> m_state{system_state::stopped};
	u32 m_pid = 1;
	stx::manual_typemap<lv2_process, 0x20'00000, 128> m_local_fxo;
	vm::vm_handle vm;
	rsx::rsx_context_state rsx_state;
};
