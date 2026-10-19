// fm3 - Forza Motorsport 3 recompiled for PC.

#include "fm3_app.h"

#include "crash_handler.h"
#include "file_signatures.h"
#include "frame_stats.h"

#include <charconv>

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/filesystem.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/ui/overlay/achievement_notification.h>

REXCVAR_DEFINE_STRING(profile_name, "", "FM3/Profile",
                      "Gamertag of the signed-in profile (empty = SDK default)");
REXCVAR_DEFINE_STRING(profile_xuid, "", "FM3/Profile",
                      "Offline XUID of the signed-in profile, 16 hex digits");

#if defined(__APPLE__)
#define FM3_DEFAULT_GPU_BACKEND "vulkan"
#else
#define FM3_DEFAULT_GPU_BACKEND "d3d12"
#endif

REXCVAR_DEFINE_STRING(gpu_backend, FM3_DEFAULT_GPU_BACKEND, "FM3/Graphics",
                      "Graphics backend for the xenos GPU plugin: any, d3d12, vulkan, metal")
    .allowed({"any", "d3d12", "vulkan", "metal"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(crash_test, false, "FM3/Debug",
                    "Crash on purpose after startup to test the crash report")
    .debug_only();

// Two config files next to the exe:
//   fm3.toml       port settings (what the title needs to run), shipped
//   fm3.user.toml  the player's choices, written by the launcher
// The user file loads second so it wins. It also becomes the SDK's config
// path, because the SDK reloads that file after this hook (and the F4 settings
// overlay saves there).
//
// The SDK reads game_data_root/user_data_root before it loads any config, so
// both files are loaded here first: then they can carry the folders.
void Fm3App::OnConfigurePaths(rex::PathConfig& paths) {
  const auto exe_dir = rex::filesystem::GetExecutableFolder();
  const auto user_config = exe_dir / "fm3.user.toml";
  if (std::filesystem::exists(paths.config_path)) {
    rex::cvar::LoadConfig(paths.config_path);
  }
  if (std::filesystem::exists(user_config)) {
    rex::cvar::LoadConfig(user_config);
    paths.config_path = user_config;
  }
  if (paths.game_data_root.empty()) {
    const std::string from_config = REXCVAR_GET(game_data_root);
    paths.game_data_root = from_config.empty() ? exe_dir / "game" : std::filesystem::path(from_config);
  }
  const std::string user_from_config = REXCVAR_GET(user_data_root);
  if (!user_from_config.empty()) {
    paths.user_data_root = user_from_config;
  }
}

void Fm3App::OnPostInitLogging() {
  fm3::InstallCrashHandler(rex::filesystem::GetExecutableFolder() / "logs");
}

void Fm3App::OnPreSetup(rex::RuntimeConfig& config) {
  if (config.graphics || !config.gpu_plugin.empty()) {
    return;  // --gpu_plugin given explicitly; let ReXApp load it.
  }
  config.graphics = rex::system::LoadGpuPlugin("xenos", REXCVAR_GET(gpu_backend));
  if (!config.graphics) {
    // Fall back to ReXApp's loader, which reports the failure to the user.
    REXLOG_ERROR("Could not create the '{}' backend, retrying with any",
                 REXCVAR_GET(gpu_backend));
    config.gpu_plugin = "xenos";
  }
}

// The game writes temporary data (e.g. cache:\replay_stream during races) to
// the console's cache partition and never mounts it itself, so give it a
// writable host folder.
static void MountCachePartition(rex::Runtime* runtime, const std::filesystem::path& user_root) {
  constexpr char kMountPath[] = "\\Device\\Fm3Cache";
  auto host_path = user_root / "cache_partition";
  std::error_code ec;
  std::filesystem::create_directories(host_path, ec);
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(kMountPath, host_path, false);
  auto* fs = runtime->file_system();
  if (!device->Initialize() || !fs->RegisterDevice(std::move(device)) ||
      !fs->RegisterSymbolicLink("cache:", kMountPath)) {
    REXLOG_ERROR("Could not mount cache: at {}", host_path.string());
    return;
  }
  REXLOG_INFO("Mounted cache: at {}", host_path.string());
}

void Fm3App::OnPostLoadXexImage() {
  fm3::ResignProtectedFiles(runtime(), game_data_root());
}

void Fm3App::OnPostSetup() {
  SetGuestFrameStats([] { return GetFm3FrameStats(); });
  MountCachePartition(runtime(), user_data_root());

  auto* profile = runtime()->kernel_state()->user_profile();

  const std::string& name = REXCVAR_GET(profile_name);
  if (!name.empty()) {
    // Xbox gamertags are at most 15 characters.
    profile->set_name(name.substr(0, 15));
  }

  const std::string& xuid_str = REXCVAR_GET(profile_xuid);
  uint64_t xuid = 0;
  auto [end, ec] = std::from_chars(xuid_str.data(), xuid_str.data() + xuid_str.size(), xuid, 16);
  if (!xuid_str.empty() && ec == std::errc() && end == xuid_str.data() + xuid_str.size()) {
    // Bits 54-55 set make the XUID look like a restricted account to the game.
    profile->set_xuid(xuid & ~0x00C0000000000000ull);
  } else if (!xuid_str.empty()) {
    REXLOG_WARN("Ignoring invalid profile_xuid '{}'", xuid_str);
  }

  REXLOG_INFO("Signed in as '{}' (XUID {:016X})", profile->name(), profile->xuid());

  if (REXCVAR_GET(crash_test)) {
    *static_cast<volatile int*>(nullptr) = 0;
  }
}

std::unique_ptr<rex::ui::AchievementNotificationDialog> Fm3App::CreateAchievementNotificationDialog() {
  return nullptr;
}
