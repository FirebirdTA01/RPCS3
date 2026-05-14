#pragma once

#include "util/types.hpp"
#include "util/atomic.hpp"
#include "util/shared_ptr.hpp"
#include "Utilities/bit_set.h"
#include "config_mode.h"
#include "games_config.h"
#include "Emu/Cell/lv2/lv2_process.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <array>
#include <shared_mutex>
#include <set>

void init_fxo_for_exec(utils::serial*, bool);

enum class localized_string_id;
enum class video_renderer;

class spu_thread;

template <typename T>
class named_thread;

namespace cfg
{
	class _base;
}

enum class game_boot_result : u32
{
	no_errors,
	generic_error,
	nothing_to_boot,
	wrong_disc_location,
	invalid_file_or_folder,
	invalid_bdvd_folder,
	install_failed,
	decryption_error,
	file_creation_error,
	firmware_missing,
	firmware_version,
	unsupported_disc_type,
	savestate_corrupted,
	savestate_version_unsupported,
	still_running,
	already_added,
	currently_restricted,
	database_config_missing,
};

constexpr bool is_error(game_boot_result res)
{
	return res != game_boot_result::no_errors;
}

struct EmuCallbacks
{
	std::function<void(std::function<void()>, atomic_t<u32>*)> call_from_main_thread;
	std::function<void(bool)> on_run; // (start_playtime) continuing or going ingame, so start the clock
	std::function<void()> on_pause;
	std::function<void()> on_resume;
	std::function<void()> on_stop;
	std::function<void()> on_ready;
	std::function<void()> on_missing_fw;
	std::function<void(std::shared_ptr<atomic_t<bool>>, int)> on_emulation_stop_no_response;
	std::function<void(std::shared_ptr<atomic_t<bool>>, stx::shared_ptr<utils::serial>, stx::atomic_ptr<std::string>*, std::shared_ptr<void>)> on_save_state_progress;
	std::function<void(bool enabled)> enable_disc_eject;
	std::function<void(bool enabled)> enable_disc_insert;
	std::function<bool(bool, std::function<void()>)> try_to_quit; // (force_quit, on_exit) Try to close RPCS3
	std::function<void(s32, s32)> handle_taskbar_progress; // (type, value) type: 0 for reset, 1 for increment, 2 for set_limit, 3 for set_value
	std::function<void()> init_kb_handler;
	std::function<void()> init_mouse_handler;
	std::function<void(std::string_view title_id)> init_pad_handler;
	std::function<void()> update_emu_settings;
	std::function<void()> save_emu_settings;
	std::function<void()> close_gs_frame;
	std::function<std::unique_ptr<class GSFrameBase>()> get_gs_frame;
	std::function<std::shared_ptr<class camera_handler_base>()> get_camera_handler;
	std::function<std::shared_ptr<class music_handler_base>()> get_music_handler;
	std::function<void(utils::serial*)> init_gs_render;
	std::function<std::shared_ptr<class AudioBackend>()> get_audio;
	std::function<std::shared_ptr<class audio_device_enumerator>(u64)> get_audio_enumerator; // (audio_renderer)
	std::function<std::shared_ptr<class MsgDialogBase>()> get_msg_dialog;
	std::function<std::shared_ptr<class OskDialogBase>()> get_osk_dialog;
	std::function<std::unique_ptr<class SaveDialogBase>()> get_save_dialog;
	std::function<std::shared_ptr<class SendMessageDialogBase>()> get_sendmessage_dialog;
	std::function<std::shared_ptr<class RecvMessageDialogBase>()> get_recvmessage_dialog;
	std::function<std::unique_ptr<class TrophyNotificationBase>()> get_trophy_notification_dialog;
	std::function<std::string(localized_string_id, const char*)> get_localized_string;
	std::function<std::u32string(localized_string_id, const char*)> get_localized_u32string;
	std::function<std::string(const cfg::_base*, u32)> get_localized_setting;
	std::function<std::string(std::string_view)> get_photo_path;
	std::function<void(const std::string&, std::optional<f32>)> play_sound;
	std::function<bool(const std::string&, std::string&, s32&, s32&, s32&)> get_image_info; // (filename, sub_type, width, height, CellSearchOrientation)
	std::function<bool(const std::string&, s32, s32, s32&, s32&, u8*, bool)> get_scaled_image; // (filename, target_width, target_height, width, height, dst, force_fit)
	std::string(*resolve_path)(std::string_view) = [](std::string_view arg){ return std::string{arg}; }; // Resolve path using Qt
	std::function<std::vector<std::string>()> get_font_dirs;
	std::function<bool(const std::vector<std::string>&)> on_install_pkgs;
	std::function<void(u32)> add_breakpoint;
	std::function<bool()> display_sleep_control_supported;
	std::function<void(bool)> enable_display_sleep;
	std::function<void()> check_microphone_permissions;
	std::function<std::unique_ptr<class video_source>()> make_video_source;
	std::function<void(bool)> enable_gamemode;
	std::function<std::string(const std::string&)> get_database_config;
};

namespace utils
{
	struct serial;
};

class Emulator final
{
	EmuCallbacks m_cb;

	atomic_t<u64> m_stop_ctr{1}; // Increments when emulation is stopped
	atomic_t<bool> m_emu_state_close_pending = false;
	atomic_t<u64> m_restrict_emu_state_change{0};


	std::array<lv2_process, 2> m_processes;

	// TODO: promote to atomic_t<u32>. Written by set_active_process under
	// m_vm_swap_mutex (unique_lock) and read by current_process() from
	// many threads without any lock — formally a data race per the C++
	// memory model. Benign on x86 (a u32 word load is atomic in practice)
	// but UB and not portable to weakly-ordered architectures.
	u32 m_active_process_index = 0;

	// Input-foreground process pid. Tracks set_active_process today; later
	// pieces add independent updates for PS-button-driven VSH XMB routing.
	atomic_t<u32> m_input_foreground_pid{1};

	// Present-foreground process pid. Currently mirrored by set_active_process;
	// kept separate from input foreground so a future compositor can layer
	// presentation independently from controller ownership.
	atomic_t<u32> m_foreground_present_pid{1};

	// Latched from VSH before a co-resident game applies its own config.
	// Game-specific configs may reset g_cfg.misc.use_vsh_native_overlay, but
	// the PS-button routing choice belongs to the VSH-rooted session.
	atomic_t<bool> m_use_vsh_native_overlay{false};
	atomic_t<bool> m_vsh_native_overlay_present_pending{false};

	// VM swap mutex — held by set_active_process during pointer switch.
	// TODO: this is currently dead code. The original intent was that
	// system threads reading guest memory would take a shared_lock and
	// thus serialize against the swap, but no such callers exist. Cross-
	// process safety is presently provided entirely by suspend_process
	// (PPU/SPU thread park barrier) plus rsx::thread::pause(). Either
	// audit and add shared_lock callers to non-RSX system threads, or
	// delete this mutex when the multi-process model moves off the
	// global swap (cf. (III) target architecture).
	mutable std::shared_mutex m_vm_swap_mutex;

public:
	// Host system threads (network_thread, etc) take this shared_lock around
	// iterations of process-local IDM/fxo to serialize against set_active_process's
	// VM-globals swap. The swap mutates vm::g_*, m_active_process_index, and
	// m_input_foreground_pid — host threads reading these mid-iteration race-deref
	// stale guest pointers.
	std::shared_mutex& vm_swap_mutex() const { return m_vm_swap_mutex; }

private:

	bool m_co_resident_load = false;

	games_config m_games_config;

	video_renderer m_default_renderer;
	std::string m_default_graphics_adapter;

	std::string m_usr{"00000001"};
	u32 m_usrid{1};

	bool m_continuous_mode = false;
	bool m_has_gui = true;
	bool m_add_database_config = false;

	// True when the currently running process belongs to a boot chain rooted in VSH (the XMB).
	// Set by lv2_exitspawn after a successful BootGame whose source process was VSH (or already
	// VSH-rooted via a previous exitspawn). Reset by BootGame for non-continuous boots.
	// Consumed by _sys_process_exit to decide whether to relaunch VSH on game exit.
	bool m_launched_from_vsh = false;

	void ExecPostponedInitCode()
	{
		m_processes[m_active_process_index].ExecPostponedInitCode();
	}

public:
	static constexpr std::string_view game_id_boot_prefix = "%RPCS3_GAMEID%:";
	static constexpr std::string_view vfs_boot_prefix = "%RPCS3_VFS%:";

	Emulator() noexcept;
	~Emulator() noexcept;

	static bool IsAvailable() noexcept;

	void SetCallbacks(EmuCallbacks&& cb)
	{
		m_cb = std::move(cb);
	}

	void SetGameDir(const std::string& game_dir) { m_processes[m_active_process_index].RefGameDir() = game_dir; }

	const auto& GetCallbacks() const
	{
		return m_cb;
	}

	// Call from the GUI thread
	void CallFromMainThread(std::function<void()>&& func, atomic_t<u32>* wake_up = nullptr, bool track_emu_state = true, u64 stop_ctr = umax,
		std::source_location src_loc = std::source_location::current()) const;

	// Blocking call from the GUI thread
	void BlockingCallFromMainThread(std::function<void()>&& func, bool track_emu_state = true, std::source_location src_loc = std::source_location::current()) const;

	enum class stop_counter_t : u64{};

	// Returns a different value each time we start a new emulation.
	stop_counter_t GetEmulationIdentifier(bool subtract_one = false) const
	{
		if (subtract_one)
		{
			return stop_counter_t{m_stop_ctr - 1};
		}

		return stop_counter_t{+m_stop_ctr};
	}

	void CallFromMainThread(std::function<void()>&& func, stop_counter_t counter,
		std::source_location src_loc = std::source_location::current()) const
	{
		CallFromMainThread(std::move(func), nullptr, true, static_cast<u64>(counter), src_loc);
	}

	void PostponeInitCode(std::function<void()>&& func)
	{
		m_processes[m_active_process_index].RefPostponedInitCode().emplace_back(std::move(func));
	}

	/** Set emulator mode to running unconditionnaly.
	 * Required to execute various part (PPUInterpreter, memory manager...) outside of rpcs3.
	 */
	void SetTestMode()
	{
		m_processes[m_active_process_index].RefState() = system_state::running;
	}

	void SetPrecompileCacheOption(emu_precompilation_option_t option)
	{
		m_processes[m_active_process_index].SetPrecompilationOption(option);
	}

	lv2_process& current_process() { return m_processes[m_active_process_index]; }
	const lv2_process& current_process() const { return m_processes[m_active_process_index]; }

	// Look up a process by pid (1-indexed: pid 1 -> slot 0, pid 2 -> slot 1).
	// Used by fxo / idm routing to direct process-local lookups to the
	// calling thread's owner process under co-resident execution.
	lv2_process& process_by_pid(u32 pid)
	{
		const u32 idx = pid - 1;
		ensure(idx < m_processes.size());
		return m_processes[idx];
	}
	const lv2_process& process_by_pid(u32 pid) const
	{
		const u32 idx = pid - 1;
		ensure(idx < m_processes.size());
		return m_processes[idx];
	}

	// Get VM base for a specific process (system threads use this for their owner)
	u8* get_process_vm_base(u32 pid) const
	{
		const u32 idx = pid - 1;
		return idx < m_processes.size() ? m_processes[idx].vm_handle().base_addr : nullptr;
	}

	// Multi-process API (debug-only — not yet exposed via PS3 syscalls)
	u32 create_process();
	void set_active_process(u32 pid, bool suspend_outgoing = true, bool resume_incoming = true, bool pause_outgoing_rsx = true);
	u32 GetInputForegroundPid() const { return m_input_foreground_pid; }
	void SetInputForegroundPid(u32 pid) { m_input_foreground_pid = pid; }
	u32 GetForegroundPresentPid() const { return m_foreground_present_pid; }
	void SetForegroundPresentPid(u32 pid) { m_foreground_present_pid = pid; }
	bool UseVshNativeOverlay() const { return m_use_vsh_native_overlay; }
	void SetUseVshNativeOverlay(bool enabled) { m_use_vsh_native_overlay = enabled; }
	void RequestVshNativeOverlayPresent() { m_vsh_native_overlay_present_pending = true; }
	void TryConsumeVshNativeOverlayPresentRequest(u32 owner_pid)
	{
		if (owner_pid == 1 && m_vsh_native_overlay_present_pending.exchange(false))
		{
			m_foreground_present_pid = 1;
		}
	}
	void suspend_process(u32 pid);
	void resume_process(u32 pid);
	void destroy_process(u32 pid);

	// Co-resident load: when loading a second process while another is alive,
	// destructive operations in Init/Load are skipped.
	void EnterCoResidentLoad() noexcept { m_co_resident_load = true; }
	void ExitCoResidentLoad() noexcept { m_co_resident_load = false; }
	bool IsCoResidentLoad() const noexcept { return m_co_resident_load; }

	void Init();

	const u32& GetBootSourceType() const
	{
		return m_processes[m_active_process_index].GetBootSourceType();
	}

	const std::string& GetBoot() const
	{
		return m_processes[m_active_process_index].GetPath();
	}

	const std::string& GetLastBoot() const
	{
		return m_processes[m_active_process_index].GetPathOriginal();
	}

	const std::string& GetTitleID() const
	{
		return m_processes[m_active_process_index].GetTitleID();
	}

	const std::string& GetTitle() const
	{
		return m_processes[m_active_process_index].GetTitle();
	}

	const std::string& GetLocalizedTitle() const
	{
		return m_processes[m_active_process_index].GetLocalizedTitle();
	}

	const std::string GetTitleAndTitleID() const
	{
		return m_processes[m_active_process_index].GetTitle() + (m_processes[m_active_process_index].GetTitleID().empty() ? "" : " [" + m_processes[m_active_process_index].GetTitleID() + "]");
	}

	const std::string& GetAppVersion() const
	{
		return m_processes[m_active_process_index].GetAppVersion();
	}

	const std::string& GetExecutableHash() const
	{
		return m_processes[m_active_process_index].GetExecutableHash();
	}

	void SetExecutableHash(std::string hash) { m_processes[m_active_process_index].SetExecutableHash(std::move(hash)); }

	const std::string& GetCat() const
	{
		return m_processes[m_active_process_index].GetCat();
	}

	const std::string& GetFakeCat() const;

	const std::string& GetDir() const
	{
		return m_processes[m_active_process_index].GetDir();
	}

	const std::string GetSfoDir(bool prefer_disc_sfo) const;

	// String for GUI dialogs.
	const std::string& GetUsr() const
	{
		return m_usr;
	}

	const games_config& GetGamesConfig() const
	{
		return m_games_config;
	}

	// Get deserialization manager
	utils::serial* DeserialManager() const;

	// u32 for cell.
	u32 GetUsrId() const
	{
		return m_usrid;
	}

	void SetUsr(const std::string& user);

	u64 GetPauseTime() const
	{
		return m_processes[m_active_process_index].GetPauseAmendTime();
	}

	const std::string& GetUsedConfig() const
	{
		return m_processes[m_active_process_index].GetConfigPath();
	}

	const std::string& GetUsedDatabaseConfig() const
	{
		static std::string empty_db_config;
		return m_processes[m_active_process_index].GetDbConfig() ? *m_processes[m_active_process_index].GetDbConfig() : empty_db_config;
	}

	bool IsChildProcess() const
	{
		return m_processes[m_active_process_index].GetConfigMode() == cfg_mode::continuous;
	}

	bool ContinuousModeEnabled(bool reset)
	{
		if (reset)
		{
			return std::exchange(m_continuous_mode, false);
		}
		return m_continuous_mode;
	}

	bool IsLaunchedFromVsh() const
	{
		return m_launched_from_vsh;
	}

	void SetLaunchedFromVsh(bool v)
	{
		m_launched_from_vsh = v;
	}

	class emulation_state_guard_t
	{
		class Emulator* _this = nullptr;
		bool active = true;

	public:
		explicit emulation_state_guard_t(Emulator* this0) noexcept
			: _this(this0)
		{
			_this->m_restrict_emu_state_change++;
		}

		~emulation_state_guard_t() noexcept
		{
			if (active)
			{
				_this->m_restrict_emu_state_change.try_dec(0);
			}
		}

		emulation_state_guard_t(emulation_state_guard_t&& rhs) noexcept
		{
			_this = rhs._this;
			active = std::exchange(rhs.active, false);
		}

		emulation_state_guard_t& operator=(const emulation_state_guard_t&) = delete;
		emulation_state_guard_t(const emulation_state_guard_t&) = delete;
	};

	emulation_state_guard_t MakeEmulationStateGuard()
	{
		return emulation_state_guard_t{this};
	}

	game_boot_result BootGame(const std::string& path, const std::string& title_id = "", bool direct = false, cfg_mode config_mode = cfg_mode::custom, const std::string& config_path = "", const std::optional<std::string>& db_config = std::nullopt);
	bool BootRsxCapture(const std::string& path);

	void SetForceBoot(bool force_boot);
	void SetContinuousMode(bool continuous_mode);

	game_boot_result Load(const std::string& title_id = "", bool is_disc_patch = false, usz recursion_count = 0);
	void Run(bool start_playtime);
	void RunPPU();
	void FixGuestTime();
	void FinalizeRunRequest();

	bool IsBootingRestricted() const
	{
		return m_restrict_emu_state_change != 0;
	}

private:
	struct savestate_stage
	{
		bool prepared = false;
		std::vector<std::pair<shared_ptr<named_thread<spu_thread>>, u32>> paused_spus;
	};
public:

	bool Pause(bool freeze_emulation = false, bool show_resume_message = true);
	void Resume();
	void GracefulShutdown(bool allow_autoexit = true, bool async_op = false, bool savestate = false, bool continuous_mode = false);
	void Kill(bool allow_autoexit = true, bool savestate = false, savestate_stage* stage = nullptr);
	game_boot_result Restart(bool graceful = true, bool reset_path = true);
	bool Quit(bool force_quit);
	static void CleanUp();

	bool IsRunning() const { return m_processes[m_active_process_index].GetState() == system_state::running; }
	bool IsPaused() const { system_state state = m_processes[m_active_process_index].GetState(); return state >= system_state::paused && state <= system_state::frozen; }
	bool IsPausedOrReady() const { return m_processes[m_active_process_index].GetState() >= system_state::paused; }
	bool IsProcessPausedOrReady(u32 pid) const
	{
		const u32 idx = pid - 1;
		if (idx >= m_processes.size())
		{
			return false;
		}
		return m_processes[idx].GetState() >= system_state::paused;
	}
	bool IsStopped(bool test_fully = false) const { return test_fully ? m_processes[m_active_process_index].GetState() == system_state::stopped : m_processes[m_active_process_index].GetState() <= system_state::stopping; }
	bool IsReady()   const { return m_processes[m_active_process_index].GetState() == system_state::ready; }
	bool IsStarting() const { return m_processes[m_active_process_index].GetState() == system_state::starting; }
	void WaitReady() const { m_processes[m_active_process_index].RefState().wait(system_state::ready); }
	auto GetStatus(bool fixup = true) const { system_state state = m_processes[m_active_process_index].GetState(); return fixup && state == system_state::frozen ? system_state::paused : fixup && state == system_state::stopping ? system_state::stopped : state; }

	bool HasGui() const { return m_has_gui; }
	void SetHasGui(bool has_gui) { m_has_gui = has_gui; }

	void SetDefaultRenderer(video_renderer renderer) { m_default_renderer = renderer; }
	void SetDefaultGraphicsAdapter(std::string adapter) { m_default_graphics_adapter = std::move(adapter); }

	std::string GetFormattedTitle(double fps) const;

	void ConfigurePPUCache() const;

	std::set<std::string> GetGameDirs() const;
	u32 AddGamesFromDir(const std::string& path);
	game_boot_result AddGame(const std::string& path);
	game_boot_result AddGameToYml(const std::string& path);
	u32 RemoveGamesFromDir(const std::string& games_dir, const std::vector<std::string>& serials_to_remove_from_yml = {}, bool save_on_disk = true);
	u32 RemoveGames(const std::vector<std::string>& title_id_list, bool save_on_disk = true);
	game_boot_result RemoveGameFromYml(const std::string& title_id);

	// Check if path is inside the specified directory
	bool IsPathInsideDir(std::string_view path, std::string_view dir) const;
	game_boot_result VerifyPathCasing(std::string_view path, std::string_view dir, bool from_dir) const;

	void EjectDisc();
	game_boot_result InsertDisc(const std::string& path);

	static game_boot_result GetElfPathFromDir(std::string& elf_path, const std::string& path);
	static void GetBdvdDir(std::string& bdvd_dir, std::string& sfb_dir, std::string& game_dir, const std::string& elf_dir);
	friend void init_fxo_for_exec(utils::serial*, bool);

	static bool IsVsh();
	bool IsActiveProcessVsh() const { return m_processes[m_active_process_index].is_vsh(); }

	bool IsVshCoResident() const
	{
		const lv2_process& slot0 = m_processes[0];
		if (!slot0.is_vsh()) return false;
		return slot0.GetState() > system_state::stopping;
	}

	static bool IsValidSfb(const std::string& path);

	static void SaveSettings(const std::string& settings, const std::string& title_id);
};

extern Emulator Emu;
