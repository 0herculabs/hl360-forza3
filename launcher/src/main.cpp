// fm3-launcher - profiles, settings and game install for the recompiled
// Forza Motorsport 3. Sits next to fm3.exe; writes fm3.user.toml (read by the
// game after its own fm3.toml) and launches the game.
//
// Layout: the cover art at the top of a column on the left with the profile
// picker and the play button under it; settings live in tabs on the right,
// grouped into cards. The cover is the dashboard tile art of the player's own
// Disc 1 (nxeart), extracted at install time - it is not part of the program.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../../src/crash_handler.h"
#include "fm3_plus.h"
#include "i18n.h"
#include "installer.h"
#include "settings.h"

namespace fs = std::filesystem;
using namespace launcher;

namespace {

// Palette, taken from the Forza Motorsport 3 logo.
const ImVec4 kAccent(0.88f, 0.25f, 0.16f, 1.0f);
const ImVec4 kAccentHover(0.97f, 0.36f, 0.25f, 1.0f);
const ImVec4 kOk(0.36f, 0.80f, 0.45f, 1.0f);
const ImVec4 kWarn(0.98f, 0.74f, 0.25f, 1.0f);
const ImVec4 kBad(0.95f, 0.35f, 0.30f, 1.0f);
const ImVec4 kText(0.93f, 0.93f, 0.95f, 1.0f);
const ImVec4 kDim(0.60f, 0.61f, 0.66f, 1.0f);
const ImVec4 kBg(0.067f, 0.071f, 0.082f, 1.0f);
const ImVec4 kCard(0.105f, 0.110f, 0.125f, 1.0f);
const ImVec4 kControl(0.165f, 0.172f, 0.192f, 1.0f);

float g_scale = 1.0f;
ImFont* g_bold = nullptr;
float S(float v) { return v * g_scale; }
ImU32 Col(const ImVec4& c, float alpha = 1.0f) {
  return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
}

enum class Tab { kProfiles, kGraphics, kControls, kAudio, kPlus, kInstall };

// --- Key capture -----------------------------------------------------------

// SDL scancode -> name understood by the runtime's ParseVirtualKey
// (src/ui/keybinds.cpp in the SDK).
std::string KeyName(SDL_Scancode sc) {
  if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return std::string(1, char('A' + (sc - SDL_SCANCODE_A)));
  if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return std::string(1, char('1' + (sc - SDL_SCANCODE_1)));
  if (sc == SDL_SCANCODE_0) return "0";
  if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return "F" + std::to_string(1 + sc - SDL_SCANCODE_F1);
  if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return "Numpad" + std::to_string(1 + sc - SDL_SCANCODE_KP_1);
  switch (sc) {
    case SDL_SCANCODE_KP_0: return "Numpad0";
    case SDL_SCANCODE_KP_ENTER: return "NumpadEnter";
    case SDL_SCANCODE_KP_PLUS: return "NumpadPlus";
    case SDL_SCANCODE_KP_MINUS: return "NumpadMinus";
    case SDL_SCANCODE_KP_MULTIPLY: return "NumpadStar";
    case SDL_SCANCODE_KP_DIVIDE: return "NumpadSlash";
    case SDL_SCANCODE_RETURN: return "Return";
    case SDL_SCANCODE_SPACE: return "Space";
    case SDL_SCANCODE_TAB: return "Tab";
    case SDL_SCANCODE_BACKSPACE: return "Backspace";
    case SDL_SCANCODE_DELETE: return "Delete";
    case SDL_SCANCODE_INSERT: return "Insert";
    case SDL_SCANCODE_HOME: return "Home";
    case SDL_SCANCODE_END: return "End";
    case SDL_SCANCODE_PAGEUP: return "PageUp";
    case SDL_SCANCODE_PAGEDOWN: return "PageDown";
    case SDL_SCANCODE_LEFT: return "Left";
    case SDL_SCANCODE_RIGHT: return "Right";
    case SDL_SCANCODE_UP: return "Up";
    case SDL_SCANCODE_DOWN: return "Down";
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return "Shift";
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return "Control";
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return "Alt";
    case SDL_SCANCODE_GRAVE: return "Backtick";
    case SDL_SCANCODE_MINUS: return "Minus";
    case SDL_SCANCODE_EQUALS: return "Plus";
    case SDL_SCANCODE_COMMA: return "Comma";
    case SDL_SCANCODE_PERIOD: return "Period";
    case SDL_SCANCODE_SEMICOLON: return "Semicolon";
    case SDL_SCANCODE_SLASH: return "Slash";
    case SDL_SCANCODE_BACKSLASH: return "Backslash";
    case SDL_SCANCODE_LEFTBRACKET: return "LBracket";
    case SDL_SCANCODE_RIGHTBRACKET: return "RBracket";
    case SDL_SCANCODE_APOSTROPHE: return "Quote";
    case SDL_SCANCODE_CAPSLOCK: return "CapsLock";
    default: return {};
  }
}

// Friendlier names for a few runtime key names when shown in chips.
std::string KeyLabel(const std::string& key) {
  const std::string shift = L("Shift+", "Mayús+");
  const std::pair<const char*, std::string> names[] = {
      {"Space", L("Space", "Espacio")}, {"Return", "Enter"}, {"Backspace", L("Backspace", "Retroceso")},
      {"Semicolon", L("; / Ñ", "Ñ / ;")}, {"Quote", "' / ´"}, {"Up", "↑"}, {"Down", "↓"},
      {"Left", "←"}, {"Right", "→"}, {"Control", "Ctrl"},
  };
  std::string base = key, prefix;
  if (key.rfind("Shift+", 0) == 0) base = key.substr(6), prefix = shift;
  for (const auto& [from, to] : names) {
    if (base == from) return prefix + to;
  }
  return prefix + base;
}

// --- Cover art -------------------------------------------------------------

struct Cover {
  SDL_GPUTexture* texture = nullptr;
  SDL_GPUSampler* sampler = nullptr;
  SDL_GPUTextureSamplerBinding binding{};
  int width = 0, height = 0;
  // ImGui >= 1.92.2's SDL_GPU backend takes the SDL_GPUTexture* itself (and
  // samples it with its own linear sampler).
  ImTextureID id() const { return (ImTextureID)(intptr_t)texture; }
};

void ReleaseCover(SDL_GPUDevice* gpu, Cover& cover) {
  if (cover.texture) SDL_ReleaseGPUTexture(gpu, cover.texture);
  if (cover.sampler) SDL_ReleaseGPUSampler(gpu, cover.sampler);
  cover = Cover{};
}

bool LoadCover(SDL_GPUDevice* gpu, const fs::path& file, Cover& out) {
  std::ifstream in(file, std::ios::binary);
  const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  int w = 0, h = 0, channels = 0;
  stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(data.data()), int(data.size()),
                                          &w, &h, &channels, 4);
  if (!pixels) return false;

  SDL_GPUTextureCreateInfo info = {};
  info.type = SDL_GPU_TEXTURETYPE_2D;
  info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  info.width = Uint32(w);
  info.height = Uint32(h);
  info.layer_count_or_depth = 1;
  info.num_levels = 1;
  out.texture = SDL_CreateGPUTexture(gpu, &info);

  SDL_GPUTransferBufferCreateInfo tb_info = {};
  tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  tb_info.size = Uint32(w * h * 4);
  SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(gpu, &tb_info);
  std::memcpy(SDL_MapGPUTransferBuffer(gpu, tb, false), pixels, size_t(w) * h * 4);
  SDL_UnmapGPUTransferBuffer(gpu, tb);
  stbi_image_free(pixels);

  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
  SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureTransferInfo src = {};
  src.transfer_buffer = tb;
  SDL_GPUTextureRegion region = {};
  region.texture = out.texture;
  region.w = Uint32(w);
  region.h = Uint32(h);
  region.d = 1;
  SDL_UploadToGPUTexture(copy, &src, &region, false);
  SDL_EndGPUCopyPass(copy);
  SDL_SubmitGPUCommandBuffer(cmd);
  SDL_ReleaseGPUTransferBuffer(gpu, tb);

  SDL_GPUSamplerCreateInfo sampler = {};
  sampler.min_filter = SDL_GPU_FILTER_LINEAR;
  sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
  sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
  sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  out.sampler = SDL_CreateGPUSampler(gpu, &sampler);
  out.binding.texture = out.texture;
  out.binding.sampler = out.sampler;
  out.width = w;
  out.height = h;
  return out.texture && out.sampler;
}

// --- App state -------------------------------------------------------------

struct App {
  SDL_Window* window = nullptr;
  SDL_GPUDevice* gpu = nullptr;
  fs::path exe_dir;
  fs::path user_config;      // fm3.user.toml: what the game reads
  fs::path port_config;      // fm3.toml: the port's own settings
  fs::path launcher_config;  // launcher.toml: UI language and profiles
  fs::path manifest_path;
  fs::path game_exe;
  fs::path tool_exe;
  fs::path cover_path;
  Cover cover;
  bool reload_cover = false;

  Settings settings;
  LauncherState state;
  std::vector<ManifestEntry> manifest;
  Tab tab = Tab::kProfiles;
  std::string notice;

  // Install / verify state
  enum class GameState { kUnknown, kMissing, kIncomplete, kUnverified, kVerified };
  GameState game_state = GameState::kUnknown;
  std::string game_state_detail;
  size_t content_packages = 0;
  size_t plus_content_packages = 0;
  TaskProgress task;
  std::thread worker;
  std::string task_title;
  enum class TaskKind { kNone, kPlayDisc, kContentDisc, kVerify, kPlus };
  TaskKind task_kind = TaskKind::kNone;
  std::atomic<int> import_step{0};  // play disc: 1 = extracting, 2 = verifying

  // Async file dialogs report here (SDL may call back on another thread).
  std::mutex dialog_mutex;
  std::optional<std::string> picked_iso;
  std::optional<std::string> picked_game_folder;
  std::optional<std::string> picked_user_folder;
  std::optional<std::string> picked_plus_base;
  std::optional<std::string> picked_plus_update;

  // FM3 Plus (Project Forza Plus 3) and the related tweaks.
  PlusState plus;
  std::string plus_base_zip;
  std::string plus_update_zip;
  PlusMode plus_mode = PlusMode::kNormal;
  bool plus_harder_ai = false;
  std::vector<std::string> skip_intro_saves;
  int skip_intro_index = 0;
  bool fov_ok = false;
  PlusFov fov;
  PlusFov fov_original;

  // Profiles
  char new_profile[32] = {};
  int delete_candidate = -1;
  bool open_delete_popup = false;

  // Key capture: index into settings.binds, and whether it appends.
  int capture_index = -1;
  bool capture_append = false;

  // Running game
  SDL_Process* game = nullptr;
  fs::file_time_type launched_at;
  std::string last_exit;

  bool Installed() const {
    return game_state == GameState::kVerified || game_state == GameState::kUnverified;
  }

  bool ContentInstalled() const {
    return plus.installed ? (!plus.dlc_added.empty() && plus_content_packages == plus.dlc_added.size())
                          : content_packages >= kContentPackages;
  }

  fs::path GameDir() const {
    return settings.game_data_root.empty() ? exe_dir / "game" : fs::u8path(settings.game_data_root);
  }
  fs::path UserDir() const {
    return settings.user_data_root.empty() ? exe_dir / "user" : fs::u8path(settings.user_data_root);
  }

  const Profile* ActiveProfile() const {
    for (const auto& p : state.profiles) {
      if (p.xuid == state.active_xuid) return &p;
    }
    return nullptr;
  }

  void Save() {
    settings.game_data_root = GameDir().u8string();
    settings.user_data_root = UserDir().u8string();
    if (const Profile* p = ActiveProfile()) {
      settings.profile_name = p->name;
      settings.profile_xuid = p->xuid;
    }
    if (!settings.Save(user_config)) {
      notice = std::string(L("Could not save ", "No se pudo guardar ")) + user_config.u8string();
    }
    state.Save(launcher_config);
  }

  void Load() {
    if (!state.Load(launcher_config)) {
      // First run: follow the system language (launcher UI and game).
      int count = 0;
      SDL_Locale** locales = SDL_GetPreferredLocales(&count);
      state.spanish = count > 0 && locales[0]->language && std::strcmp(locales[0]->language, "es") == 0;
      SDL_free(locales);
    }
    g_spanish = state.spanish;
    // First run of this launcher: start from the older files' values.
    if (!settings.Load(user_config)) {
      if (state.spanish) settings.user_language = 5;
      settings.Load(port_config);
      if (!state.legacy_game_dir.empty()) settings.game_data_root = state.legacy_game_dir;
      if (!state.legacy_user_dir.empty()) settings.user_data_root = state.legacy_user_dir;
    }
    if (state.profiles.empty()) {
      state.profiles.push_back({IsValidGamertag(settings.profile_name) ? settings.profile_name : "Player",
                                settings.profile_xuid.size() == 16 ? settings.profile_xuid : GenerateXuid()});
    }
    if (!ActiveProfile()) state.active_xuid = state.profiles.front().xuid;
    Save();
  }

  // The file list verification uses: files the mod replaced are skipped and
  // the ones the launcher edited (shadows, FOV) are checked at their originals.
  std::vector<ManifestEntry> VerifyManifest(const fs::path& dir) const {
    return WithOriginals(dir, WithoutPlusFiles(manifest, LoadPlus(dir)));
  }

  void CheckGame() {
    const fs::path dir = GameDir();
    std::error_code ec;
    content_packages = CountContentPackages(UserDir());
    plus = LoadPlus(dir);
    plus_content_packages = 0;
    if (plus.installed) {
      const fs::path dlc = UserDir() / "0000000000000000" / "4D53084D" / "00000002";
      for (const auto& package : plus.dlc_added) {
        if (fs::is_directory(dlc / fs::u8path(package), ec)) ++plus_content_packages;
      }
    }
    if (plus.installed) plus_mode = plus.mode, plus_harder_ai = plus.harder_ai;
    skip_intro_saves = plus.installed ? ListSkipIntroSaves(dir, plus.mode) : std::vector<std::string>{};
    skip_intro_index = std::clamp(skip_intro_index, 0, std::max(0, int(skip_intro_saves.size()) - 1));
    fov_ok = ReadPlusFov(dir, false, fov) && ReadPlusFov(dir, true, fov_original);
    if (!fs::exists(dir / "default.xex", ec)) {
      game_state = GameState::kMissing;
      game_state_detail = L("Install Disc 1 in the Install tab.", "Instala el Disco 1 en la pestaña Instalación.");
      return;
    }
    TaskProgress quick;
    const VerifyReport report = VerifyGame(dir, VerifyManifest(dir), false, quick);
    if (!report.ok()) {
      game_state = GameState::kIncomplete;
      game_state_detail = std::to_string(report.missing.size()) + L(" files missing, ", " archivos faltan, ") +
                          std::to_string(report.wrong_size.size()) +
                          L(" with a different size. Install Disc 1 again.",
                            " con otro tamaño. Vuelve a instalar el Disco 1.");
    } else if (fs::exists(VerifiedMarker(dir), ec)) {
      game_state = GameState::kVerified;
      game_state_detail = dir.u8string();
    } else {
      game_state = GameState::kUnverified;
      game_state_detail = L("Files present, contents not verified yet.",
                            "Archivos presentes, sin verificar el contenido.");
    }
    if (!fs::exists(cover_path, ec) && ExtractCover(dir, tool_exe, cover_path)) reload_cover = true;
  }

  bool TaskRunning() const { return task.state == TaskProgress::State::kRunning; }

  void StartTask(std::string title, TaskKind kind, std::function<bool()> body) {
    if (worker.joinable()) worker.join();
    task_title = std::move(title);
    task_kind = kind;
    task.cancel = false;
    task.done = 0;
    task.total = 0;
    task.SetStatus("");
    task.state = TaskProgress::State::kRunning;
    worker = std::thread([this, body = std::move(body)] {
      const bool ok = body();
      task.state = ok ? TaskProgress::State::kDone
                      : (task.cancel ? TaskProgress::State::kCancelled : TaskProgress::State::kFailed);
    });
  }

  // Full hash check; writes the verified marker on success.
  bool RunVerify(const fs::path& dir) {
    const VerifyReport report = VerifyGame(dir, VerifyManifest(dir), true, task);
    std::error_code ec;
    if (report.ok() && !task.cancel) {
      std::ofstream(VerifiedMarker(dir)) << "verified against game_files.sha256\n";
      task.SetStatus(std::string(L("All good: ", "Todo correcto: ")) + std::to_string(manifest.size()) +
                     L(" files match the version the port was built from.",
                       " archivos coinciden con la versión del port."));
      return true;
    }
    fs::remove(VerifiedMarker(dir), ec);
    std::string text = std::string(L("Verification failed: ", "La verificación falló: ")) +
                       std::to_string(report.missing.size()) + L(" missing, ", " faltan, ") +
                       std::to_string(report.wrong_size.size()) + L(" wrong size, ", " con otro tamaño, ") +
                       std::to_string(report.wrong_hash.size()) + L(" wrong contents.", " con contenido distinto.");
    const auto& first = !report.missing.empty()      ? report.missing
                        : !report.wrong_size.empty() ? report.wrong_size
                                                     : report.wrong_hash;
    if (!first.empty()) text += std::string(L("\nFirst: ", "\nPrimero: ")) + first.front();
    if (std::find(report.wrong_hash.begin(), report.wrong_hash.end(), "default.xex") != report.wrong_hash.end()) {
      text += L("\ndefault.xex does not match: the port was built from the American Ultimate Collection disc.",
                "\ndefault.xex no coincide: el port se hizo con el disco americano de la Ultimate Collection.");
    }
    task.SetStatus(text);
    return false;
  }

  // Starts the right install task for the disc image the player picked.
  void StartImport(const fs::path& iso) {
    Save();
    const DiscKind kind = IdentifyIso(iso);
    if (kind == DiscKind::kPlay) {
      const fs::path game_dir = GameDir();
      StartTask(L("Disc 1 (Play Disc)", "Disco 1 (Play Disc)"), TaskKind::kPlayDisc, [this, iso, game_dir] {
        import_step = 1;
        std::error_code ec;
        fs::remove(VerifiedMarker(game_dir), ec);
        if (!ExtractPlayDisc(iso, game_dir, task) || task.cancel) return false;
        import_step = 2;
        return RunVerify(game_dir);
      });
    } else if (kind == DiscKind::kContent) {
      const fs::path game_dir = GameDir(), user_dir = UserDir();
      StartTask(L("Disc 2 (Content Install Disc)", "Disco 2 (Content Install Disc)"), TaskKind::kContentDisc,
                [this, iso, game_dir, user_dir] {
                  return InstallContentDisc(iso, game_dir, user_dir, tool_exe, task);
                });
    } else {
      notice = std::string(L("That image is not a Forza Motorsport 3 Ultimate Collection disc: ",
                             "Esa imagen no es un disco de Forza Motorsport 3 Ultimate Collection: ")) +
               iso.filename().u8string();
    }
  }

  void LaunchGame() {
    Save();
    if (!fs::exists(game_exe)) {
      notice = std::string(L("Not found: ", "No se encuentra ")) + game_exe.u8string();
      return;
    }
    const std::string exe = game_exe.u8string();
    const std::string cwd = exe_dir.u8string();
    const char* args[] = {exe.c_str(), nullptr};
    // A WIN32-subsystem launcher has no console handles, so the default
    // (inherit stdio) fails in DuplicateHandle; the game logs to files anyway.
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, const_cast<char**>(args));
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, cwd.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    game = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!game) {
      notice = std::string(L("Could not start the game: ", "No se pudo lanzar el juego: ")) + SDL_GetError();
    } else {
      last_exit.clear();
      launched_at = fs::file_time_type::clock::now();
      SDL_MinimizeWindow(window);
    }
  }

  // Logs exist to explain failures: after a clean exit, remove the ones this
  // session wrote (fm3_NNN.log and its rotations). Crash reports stay.
  void DeleteSessionLogs() {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(exe_dir / "logs", ec)) {
      if (e.is_regular_file() && e.path().extension() == ".log" &&
          e.path().filename().u8string().rfind("fm3_", 0) == 0 &&
          e.last_write_time(ec) >= launched_at - std::chrono::seconds(2)) {
        fs::remove(e.path(), ec);
      }
    }
  }

  void PollGame() {
    if (!game) return;
    int exit_code = 0;
    if (SDL_WaitProcess(game, false, &exit_code)) {
      SDL_DestroyProcess(game);
      game = nullptr;
      SDL_RestoreWindow(window);
      if (exit_code != 0) {
        char hex[16];
        std::snprintf(hex, sizeof(hex), "0x%08X", unsigned(exit_code));
        last_exit = std::string(L("The game closed with an error (", "El juego se cerró con un error (")) + hex +
                    L("). The log and crash report are in the logs folder.",
                      "). El registro y el informe de fallo están en la carpeta logs.");
      } else {
        DeleteSessionLogs();
      }
    }
  }
};

// --- Widgets ---------------------------------------------------------------

void TextDim(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, kDim);
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

void TextColored(const ImVec4& color, const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

void Title(const char* text, float size = 1.0f) {
  ImGui::PushFont(g_bold, ImGui::GetStyle().FontSizeBase * size);
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
}

// A rounded panel with a title; rows go inside a two-column table (label on
// the left, control on the right).
bool BeginCard(const char* id, const char* title, const char* subtitle = nullptr) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(12));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(16)));
  ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  Title(title, 1.15f);
  if (subtitle) TextDim(subtitle);
  ImGui::Dummy(ImVec2(0, S(4)));
  return true;
}

void EndCard() {
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  ImGui::Dummy(ImVec2(0, S(10)));
}

bool BeginRows(const char* id) {
  if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_None)) return false;
  // Fixed label column, so hints wrap under their label instead of squeezing it.
  ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, S(250));
  ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch);
  return true;
}

// Starts a settings row: label (+ optional hint) left, control column right.
void Row(const char* label, const char* hint = nullptr) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (hint) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - S(16));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.88f);
    ImGui::TextUnformatted(hint);
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
  }
  ImGui::TableSetColumnIndex(1);
  ImGui::SetNextItemWidth(-FLT_MIN);
}

void EndRows() { ImGui::EndTable(); }

// iOS-style switch.
bool Toggle(const char* id, bool* value) {
  const float h = ImGui::GetFrameHeight() * 0.9f;
  const float w = h * 1.85f;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
  ImVec2 p = ImGui::GetCursorScreenPos();
  p.y += (ImGui::GetFrameHeight() - h) * 0.5f;
  ImGui::InvisibleButton(id, ImVec2(w, ImGui::GetFrameHeight()));
  const bool clicked = ImGui::IsItemClicked();
  if (clicked) *value = !*value;
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec4 track = *value ? (hovered ? kAccentHover : kAccent)
                              : (hovered ? ImVec4(0.26f, 0.27f, 0.30f, 1) : ImVec4(0.21f, 0.22f, 0.25f, 1));
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Col(track), h * 0.5f);
  const float r = h * 0.5f - S(3);
  const float cx = *value ? p.x + w - h * 0.5f : p.x + h * 0.5f;
  dl->AddCircleFilled(ImVec2(cx, p.y + h * 0.5f), r, IM_COL32(255, 255, 255, 255));
  return clicked;
}

// Row of buttons, one selected. Fills the available width.
template <typename T>
bool Segmented(const char* id, T& value, const std::vector<std::pair<T, const char*>>& items) {
  ImGui::PushID(id);
  bool changed = false;
  const float full = ImGui::GetContentRegionAvail().x;
  const float gap = S(4);
  const float each = (full - gap * float(items.size() - 1)) / float(items.size());
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(7));
  for (size_t i = 0; i < items.size(); ++i) {
    const bool active = items[i].first == value;
    ImGui::PushStyleColor(ImGuiCol_Button, active ? kAccent : kControl);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? kAccentHover : ImVec4(0.22f, 0.23f, 0.26f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent);
    if (i) ImGui::SameLine();
    ImGui::PushID(int(i));
    if (ImGui::Button(items[i].second, ImVec2(each, 0)) && !active) {
      value = items[i].first;
      changed = true;
    }
    ImGui::PopID();
    ImGui::PopStyleColor(3);
  }
  ImGui::PopStyleVar(2);
  ImGui::PopID();
  return changed;
}

template <typename T>
bool Combo(const char* id, T& value, const std::vector<std::pair<T, const char*>>& items) {
  const char* preview = "";
  for (const auto& [v, name] : items) {
    if (v == value) preview = name;
  }
  bool changed = false;
  if (ImGui::BeginCombo(id, preview)) {
    for (const auto& [v, name] : items) {
      if (ImGui::Selectable(name, v == value)) {
        value = v;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

bool PrimaryButton(const char* label, const ImVec2& size) {
  ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
  const bool pressed = ImGui::Button(label, size);
  ImGui::PopStyleColor(4);
  return pressed;
}

// Small rounded label, e.g. a key or "Active".
void Chip(const char* text, const ImVec4& bg, const ImVec4& fg = kText) {
  const ImVec2 pad(S(8), S(3));
  const ImVec2 size = ImGui::CalcTextSize(text);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float y = p.y + (ImGui::GetFrameHeight() - size.y - pad.y * 2) * 0.5f;
  ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + size.x + pad.x * 2, y + size.y + pad.y * 2),
                                            Col(bg), S(6));
  ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + pad.x, y + pad.y), Col(fg), text);
  ImGui::Dummy(ImVec2(size.x + pad.x * 2, ImGui::GetFrameHeight()));
}

void StatusDot(const ImVec4& color) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float r = S(5);
  ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f + S(1)), r,
                                              Col(color));
  ImGui::Dummy(ImVec2(r * 2 + S(6), ImGui::GetTextLineHeight()));
  ImGui::SameLine(0, 0);
}

// --- Left column: cover, profile, play ------------------------------------

float CoverWidth() { return S(400); }
float CoverHeight(const App& app) {
  const float aspect = app.cover.texture ? float(app.cover.height) / float(app.cover.width) : 0.76f;
  return CoverWidth() * aspect;
}
float SettingsWidth() { return S(800); }
float WindowHeight() { return S(840); }

void CoverPanel(App& app, const ImVec2& size) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + size.x, p0.y + CoverHeight(app));

  // Column background, a darker shade of the card colour.
  dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), Col(ImVec4(0.085f, 0.09f, 0.10f, 1)));
  if (app.cover.texture) {
    dl->AddImage(app.cover.id(), p0, p1);
  } else {
    // No cover yet (Disc 1 not installed): a panel in the logo's colours.
    dl->AddRectFilledMultiColor(p0, p1, IM_COL32(48, 50, 56, 255), IM_COL32(30, 31, 36, 255),
                                IM_COL32(14, 14, 16, 255), IM_COL32(24, 25, 29, 255));
    ImGui::PushFont(g_bold, ImGui::GetStyle().FontSizeBase * 2.6f);
    dl->AddText(ImVec2(p0.x + S(28), p0.y + S(70)), Col(kAccent), "FORZA");
    ImGui::PopFont();
    ImGui::PushFont(g_bold, ImGui::GetStyle().FontSizeBase * 1.4f);
    dl->AddText(ImVec2(p0.x + S(30), p0.y + S(130)), IM_COL32(235, 235, 240, 235), "MOTORSPORT 3");
    ImGui::PopFont();
  }

  // Controls under the cover.
  const float pad = S(22);
  const float inner_w = size.x - pad * 2;
  ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p1.y + S(18)));
  ImGui::BeginGroup();
  ImGui::PushStyleColor(ImGuiCol_Text, kDim);
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
  ImGui::TextUnformatted(L("PROFILE", "PERFIL"));
  ImGui::PopFont();
  ImGui::PopStyleColor();
  ImGui::SetNextItemWidth(inner_w);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, kControl);
  const Profile* active = app.ActiveProfile();
  if (ImGui::BeginCombo("##profile", active ? active->name.c_str() : "")) {
    for (const auto& p : app.state.profiles) {
      ImGui::PushID(p.xuid.c_str());
      if (ImGui::Selectable(p.name.c_str(), &p == active)) {
        app.state.active_xuid = p.xuid;
        app.Save();
      }
      ImGui::PopID();
    }
    ImGui::Separator();
    if (ImGui::Selectable(L("Manage profiles...", "Gestionar perfiles..."))) app.tab = Tab::kProfiles;
    ImGui::EndCombo();
  }
  ImGui::PopStyleColor();
  ImGui::Dummy(ImVec2(0, S(6)));

  switch (app.game_state) {
    case App::GameState::kVerified: StatusDot(kOk); ImGui::TextUnformatted(L("Game installed and verified", "Juego instalado y verificado")); break;
    case App::GameState::kUnverified: StatusDot(kWarn); ImGui::TextUnformatted(L("Game installed (not verified)", "Juego instalado (sin verificar)")); break;
    case App::GameState::kIncomplete: StatusDot(kBad); ImGui::TextUnformatted(L("Incomplete install", "Instalación incompleta")); break;
    case App::GameState::kMissing: StatusDot(kBad); ImGui::TextUnformatted(L("Game not installed", "Juego no instalado")); break;
    default: break;
  }
  if (app.Installed()) {
    const bool all = app.ContentInstalled();
    StatusDot(all ? kOk : kWarn);
    if (app.plus.installed) {
      ImGui::Text(L("FM3 Plus content: %zu of %zu packs", "Contenido de FM3 Plus: %zu de %zu packs"),
                  app.plus_content_packages, app.plus.dlc_added.size());
    } else {
      ImGui::Text(L("Disc 2 content: %zu of %zu packs", "Contenido del Disco 2: %zu de %zu packs"),
                  app.content_packages, kContentPackages);
    }
  }
  ImGui::Dummy(ImVec2(0, S(6)));

  if (app.TaskRunning()) {
    // Any running task takes the play button's place, so its progress is
    // always on screen.
    const float fraction = app.task.total ? float(double(app.task.done) / double(app.task.total)) : 0.0f;
    const char* heading = app.task_kind == App::TaskKind::kPlayDisc      ? L("INSTALLING DISC 1", "INSTALANDO DISCO 1")
                          : app.task_kind == App::TaskKind::kContentDisc ? L("INSTALLING DISC 2", "INSTALANDO DISCO 2")
                          : app.task_kind == App::TaskKind::kPlus        ? "FM3 PLUS"
                                                                         : L("VERIFYING FILES", "VERIFICANDO ARCHIVOS");
    ImGui::PushFont(g_bold, ImGui::GetStyle().FontSizeBase * 1.05f);
    ImGui::Text("%s  %d%%", heading, int(fraction * 100));
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(6));
    ImGui::ProgressBar(fraction, ImVec2(inner_w, S(14)), "");
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (app.task_kind == App::TaskKind::kPlayDisc) {
      ImGui::TextUnformatted(app.import_step == 2 ? L("Step 2/2: verifying", "Paso 2/2: verificando")
                                                  : L("Step 1/2: extracting", "Paso 1/2: extrayendo"));
      ImGui::SameLine();
    }
    if (app.task.total) {
      ImGui::Text("%.2f / %.2f GB", double(app.task.done) / 1e9, double(app.task.total) / 1e9);
    }
    if (ImGui::SmallButton(L("Cancel", "Cancelar"))) app.task.cancel = true;
    ImGui::PushTextWrapPos(p0.x + size.x - pad);
    TextDim(app.task.Status().c_str());
    ImGui::PopTextWrapPos();
  } else {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(10));
    ImGui::PushFont(g_bold, ImGui::GetStyle().FontSizeBase * 1.45f);
    if (app.Installed()) {
      ImGui::BeginDisabled(app.game != nullptr);
      if (PrimaryButton(app.game ? L("PLAYING", "EN JUEGO") : L("PLAY", "JUGAR"), ImVec2(inner_w, S(62)))) {
        app.LaunchGame();
      }
      ImGui::EndDisabled();
    } else if (PrimaryButton(L("INSTALL", "INSTALAR"), ImVec2(inner_w, S(62)))) {
      app.tab = Tab::kInstall;
    }
    ImGui::PopFont();
    ImGui::PopStyleVar();
  }
  if (app.task.state == TaskProgress::State::kFailed) {
    ImGui::PushTextWrapPos(p0.x + size.x - pad);
    TextColored(kBad, app.task.Status().c_str());
    ImGui::PopTextWrapPos();
  }
  if (!app.last_exit.empty()) {
    ImGui::PushTextWrapPos(p0.x + size.x - pad);
    TextColored(kBad, app.last_exit.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::EndGroup();
  ImGui::SetCursorScreenPos(p0);
  ImGui::Dummy(size);
}

// --- Tabs ------------------------------------------------------------------

void TabProfiles(App& app) {
  BeginCard("##profiles", L("Profiles", "Perfiles"),
            L("Each profile is a gamertag with its own saves, cars and options.",
              "Cada perfil es un gamertag con sus propias partidas, coches y opciones."));
  const float tile_w = S(180), tile_h = S(150);
  const float avail = ImGui::GetContentRegionAvail().x;
  const int per_row = std::max(1, int((avail + S(12)) / (tile_w + S(12))));
  int index = 0;
  for (int i = 0; i < int(app.state.profiles.size()); ++i) {
    const Profile& profile = app.state.profiles[i];
    const bool active = profile.xuid == app.state.active_xuid;
    if (index % per_row) ImGui::SameLine(0, S(12));
    ImGui::PushID(profile.xuid.c_str());
    ImGui::BeginGroup();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("tile", ImVec2(tile_w, tile_h));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked() && !active) {
      app.state.active_xuid = profile.xuid;
      app.Save();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + tile_w, p.y + tile_h),
                      Col(hovered ? ImVec4(0.17f, 0.18f, 0.20f, 1) : kControl), S(12));
    if (active) dl->AddRect(p, ImVec2(p.x + tile_w, p.y + tile_h), Col(kAccent), S(12), 0, S(2));
    // Avatar: initial in a circle.
    const ImVec2 c(p.x + tile_w * 0.5f, p.y + S(52));
    dl->AddCircleFilled(c, S(30), Col(active ? kAccent : ImVec4(0.30f, 0.31f, 0.35f, 1)));
    const char initial[2] = {char(std::toupper(static_cast<unsigned char>(profile.name[0]))), 0};
    ImGui::PushFont(g_bold, ImGui::GetStyle().FontSizeBase * 1.6f);
    const ImVec2 isz = ImGui::CalcTextSize(initial);
    dl->AddText(ImVec2(c.x - isz.x * 0.5f, c.y - isz.y * 0.5f), IM_COL32_WHITE, initial);
    ImGui::PopFont();
    const ImVec2 nsz = ImGui::CalcTextSize(profile.name.c_str());
    dl->AddText(ImVec2(p.x + (tile_w - nsz.x) * 0.5f, p.y + S(94)), Col(kText), profile.name.c_str());
    const char* tag = active ? L("Active", "Activo") : L("Use", "Usar");
    const ImVec2 tsz = ImGui::CalcTextSize(tag);
    dl->AddText(ImVec2(p.x + (tile_w - tsz.x) * 0.5f, p.y + S(120)), Col(active ? kAccent : kDim), tag);
    // Below the tile, outside it, so the click never reaches the tile.
    if (!active) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(kBad.x, kBad.y, kBad.z, 0.25f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(kBad.x, kBad.y, kBad.z, 0.45f));
      ImGui::PushStyleColor(ImGuiCol_Text, kBad);
      if (ImGui::Button(L("Delete profile", "Eliminar perfil"), ImVec2(tile_w, 0))) {
        app.delete_candidate = i;
        app.open_delete_popup = true;
      }
      ImGui::PopStyleColor(4);
    } else {
      ImGui::Dummy(ImVec2(tile_w, ImGui::GetFrameHeight()));
    }
    ImGui::EndGroup();
    ImGui::PopID();
    ++index;
  }
  EndCard();

  BeginCard("##newprofile", L("New profile", "Nuevo perfil"),
            L("1 to 15 letters, digits or spaces.", "De 1 a 15 letras, números o espacios."));
  const float button_w = S(120);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button_w - S(10));
  const bool enter = ImGui::InputTextWithHint("##new", L("Profile name", "Nombre del perfil"), app.new_profile,
                                              sizeof(app.new_profile), ImGuiInputTextFlags_EnterReturnsTrue);
  const std::string name = app.new_profile;
  const bool valid = IsValidGamertag(name);
  const bool exists = std::any_of(app.state.profiles.begin(), app.state.profiles.end(), [&](const Profile& p) {
    return SDL_strcasecmp(p.name.c_str(), name.c_str()) == 0;
  });
  ImGui::SameLine(0, S(10));
  ImGui::BeginDisabled(!valid || exists);
  if (PrimaryButton(L("Create", "Crear"), ImVec2(button_w, 0)) || (enter && valid && !exists)) {
    app.state.profiles.push_back({name, GenerateXuid()});
    app.state.active_xuid = app.state.profiles.back().xuid;
    app.new_profile[0] = 0;
    app.Save();
  }
  ImGui::EndDisabled();
  if (!name.empty() && !valid) TextColored(kWarn, L("Only letters, digits and single spaces.", "Solo letras, números y espacios simples."));
  else if (exists) TextColored(kWarn, L("A profile with that name already exists.", "Ya existe un perfil con ese nombre."));
  EndCard();

  if (app.open_delete_popup) {
    ImGui::OpenPopup("###delete_profile");
    app.open_delete_popup = false;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  const std::string popup_title = std::string(L("Delete profile", "Eliminar perfil")) + "###delete_profile";
  if (ImGui::BeginPopupModal(popup_title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (app.delete_candidate < 0 || app.delete_candidate >= int(app.state.profiles.size())) {
      ImGui::CloseCurrentPopup();
    } else {
      const Profile victim = app.state.profiles[app.delete_candidate];
      ImGui::Text(L("Delete profile \"%s\" and its saves?", "¿Eliminar el perfil \"%s\" y sus partidas?"),
                  victim.name.c_str());
      TextDim(L("Its saves are moved to the deleted-profiles folder in the user data, in case you change your mind.",
                "Sus partidas se mueven a la carpeta deleted-profiles de los datos de usuario, por si te arrepientes."));
      ImGui::Dummy(ImVec2(0, S(6)));
      ImGui::PushStyleColor(ImGuiCol_Button, kBad);
      if (ImGui::Button(L("Delete", "Eliminar"), ImVec2(S(130), 0))) {
        const fs::path trash = app.UserDir() / "deleted-profiles";
        const fs::path from = app.UserDir() / victim.xuid;
        std::error_code ec;
        if (fs::exists(from, ec)) {
          fs::create_directories(trash, ec);
          fs::rename(from, trash / fs::u8path(victim.name + "-" + victim.xuid + "-" + std::to_string(std::time(nullptr))), ec);
        }
        if (ec) {
          app.notice = std::string(L("Could not delete the profile: ", "No se pudo eliminar el perfil: ")) + ec.message();
        } else {
          app.state.profiles.erase(app.state.profiles.begin() + app.delete_candidate);
          app.Save();
        }
        app.delete_candidate = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::PopStyleColor();
      ImGui::SameLine();
      if (ImGui::Button(L("Cancel", "Cancelar"), ImVec2(S(130), 0))) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

// Quality presets map onto the individual settings; anything else is "custom".
struct Preset {
  int id;
  int scale, aniso;
  const char* aa;
  const char* present;
};
const Preset kPresets[] = {
    {0, 1, 3, "none", "bilinear"},
    {1, 2, 5, "none", "bilinear"},
    {2, 3, 5, "fxaa", "cas"},
};

int CurrentPreset(const Settings& s) {
  for (const auto& p : kPresets) {
    if (s.resolution_scale == p.scale && s.anisotropic_override == p.aniso && s.swap_post_effect == p.aa &&
        s.present_effect == p.present) {
      return p.id;
    }
  }
  return 3;
}

void TabGraphics(App& app) {
  Settings& s = app.settings;
  bool changed = false;

  BeginCard("##display", L("Display", "Pantalla"));
  if (BeginRows("##d")) {
    Row(L("Mode", "Modo"));
    int mode = s.fullscreen ? 1 : 0;
    if (Segmented<int>("mode", mode, {{1, L("Fullscreen", "Pantalla completa")}, {0, L("Window", "Ventana")}})) {
      s.fullscreen = mode == 1;
      changed = true;
    }
    Row(L("Monitor", "Monitor"));
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    std::vector<std::string> names;
    for (int i = 0; i < count; ++i) {
      const char* n = SDL_GetDisplayName(displays[i]);
      names.push_back(std::to_string(i + 1) + " · " + (n ? n : "Monitor"));
    }
    SDL_free(displays);
    std::vector<std::pair<int, const char*>> monitors{{0, L("Primary", "Principal")}};
    for (int i = 0; i < count; ++i) monitors.emplace_back(i + 1, names[i].c_str());
    changed |= Combo<int>("##monitor", s.monitor, monitors);
    if (!s.fullscreen) {
      Row(L("Window size", "Tamaño de ventana"));
      int size = s.window_width * 10000 + s.window_height;
      if (Combo<int>("##winsize", size,
                     {{12800720, "1280 × 720"},
                      {16000900, "1600 × 900"},
                      {19201080, "1920 × 1080"},
                      {25601440, "2560 × 1440"},
                      {38402160, "3840 × 2160"}})) {
        s.window_width = size / 10000;
        s.window_height = size % 10000;
        changed = true;
      }
    }
    Row(L("Keep 16:9", "Mantener 16:9"));
    changed |= Toggle("##letterbox", &s.present_letterbox);
    EndRows();
  }
  EndCard();

  BeginCard("##quality", L("Image quality", "Calidad de imagen"));
  int preset = CurrentPreset(s);
  if (Segmented<int>("preset", preset,
                     {{0, "Original 720p"}, {1, L("High 1440p", "Alta 1440p")}, {2, "Ultra 4K"},
                      {3, L("Custom", "Personalizada")}}) &&
      preset < 3) {
    const Preset& p = kPresets[preset];
    s.resolution_scale = p.scale;
    s.anisotropic_override = p.aniso;
    s.swap_post_effect = p.aa;
    s.present_effect = p.present;
    changed = true;
  }
  ImGui::Dummy(ImVec2(0, S(8)));
  if (BeginRows("##q")) {
    Row(L("Internal resolution", "Resolución interna"), L("Xbox 360: 720p", "Xbox 360: 720p"));
    changed |= Segmented<int>("scale", s.resolution_scale,
                              {{1, "720p (1×)"}, {2, "1440p (2×)"}, {3, "4K (3×)"}, {4, "5K (4×)"}});
    Row(L("Anisotropic filtering", "Filtrado anisótropo"));
    changed |= Combo<int>("##aniso", s.anisotropic_override,
                          {{0, L("Off", "Desactivado")}, {1, "1×"}, {2, "2×"}, {3, "4×"}, {4, "8×"}, {5, "16×"}});
    Row(L("Anti-aliasing", "Antialiasing"));
    changed |= Segmented<std::string>(
        "aa", s.swap_post_effect, {{"none", L("None", "Ninguno")}, {"fxaa", "FXAA"}, {"fxaa_extreme", "FXAA+"}});
    Row(L("Output scaling", "Escalado a pantalla"));
    changed |= Segmented<std::string>(
        "present", s.present_effect, {{"bilinear", L("Bilinear", "Bilineal")}, {"cas", "AMD CAS"}, {"fsr", "AMD FSR"}});
    EndRows();
  }
  EndCard();

  if (ImGui::Button(L("Restore defaults", "Restaurar valores predeterminados"))) {
    s.ResetDisplay();
    changed = true;
  }
  if (changed) app.Save();
}

void TabControls(App& app) {
  Settings& s = app.settings;
  bool changed = false;

  BeginCard("##pad", L("Controller", "Mando"));
  if (BeginRows("##p")) {
    Row(L("Input system", "Sistema de entrada"));
    changed |= Segmented<std::string>("backend", s.input_backend, {{"sdl", "SDL"}, {"xinput", "XInput"}});
    EndRows();
  }
  EndCard();

  BeginCard("##kb", L("Keyboard and mouse", "Teclado y ratón"));
  if (BeginRows("##k")) {
    Row(L("Use the keyboard as a controller", "Usar el teclado como mando"));
    changed |= Toggle("##mnk", &s.mnk_mode);
    if (s.mnk_mode) {
      Row(L("Mouse as right stick", "Ratón como stick derecho"));
      changed |= Toggle("##mouse", &s.mnk_mouse);
      if (s.mnk_mouse) {
        Row(L("Mouse sensitivity", "Sensibilidad del ratón"));
        float sens = float(s.mnk_sensitivity);
        if (ImGui::SliderFloat("##sens", &sens, 0.1f, 5.0f, "%.1f")) {
          s.mnk_sensitivity = sens;
          changed = true;
        }
      }
    }
    EndRows();
  }
  EndCard();

  if (s.mnk_mode) {
    BeginCard("##binds", L("Key bindings", "Asignación de teclas"));
    if (app.capture_index >= 0) {
      TextColored(kAccent, L("Press a key to assign it  ·  Esc cancels", "Pulsa una tecla para asignarla  ·  Esc cancela"));
    }
    const char* group = "";
    for (int i = 0; i < int(s.binds.size()); ++i) {
      auto& b = s.binds[i];
      if (std::strcmp(group, b.group) != 0) {
        group = b.group;
        ImGui::Dummy(ImVec2(0, S(6)));
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::PushFont(g_bold, ImGui::GetStyle().FontSizeBase * 0.9f);
        ImGui::TextUnformatted(L(b.group, b.group_es));
        ImGui::PopFont();
        ImGui::PopStyleColor();
      }
      ImGui::PushID(i);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(L(b.label, b.label_es));
      ImGui::SameLine(S(230));
      if (app.capture_index == i) {
        Chip("...", kAccent);
      } else if (b.keys.empty()) {
        Chip(L("unassigned", "sin asignar"), kControl, kDim);
      } else {
        size_t start = 0;
        bool first = true;
        while (start <= b.keys.size()) {
          const size_t comma = b.keys.find(',', start);
          const std::string key = b.keys.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
          if (!first) ImGui::SameLine(0, S(6));
          Chip(KeyLabel(key).c_str(), kControl);
          first = false;
          if (comma == std::string::npos) break;
          start = comma + 1;
        }
      }
      const float buttons_w = S(250);
      ImGui::SameLine(ImGui::GetContentRegionMax().x - buttons_w);
      if (ImGui::Button(L("Change", "Cambiar"), ImVec2(S(80), 0))) app.capture_index = i, app.capture_append = false;
      ImGui::SameLine(0, S(4));
      if (ImGui::Button("+", ImVec2(S(32), 0))) app.capture_index = i, app.capture_append = true;
      ImGui::SameLine(0, S(4));
      if (ImGui::Button(L("Clear", "Quitar"), ImVec2(S(64), 0))) b.keys.clear(), changed = true;
      ImGui::SameLine(0, S(4));
      if (ImGui::Button("↺", ImVec2(S(32), 0))) b.keys = b.default_keys, changed = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(L("Default key: %s", "Tecla por defecto: %s"), b.default_keys);
      ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, S(8)));
    if (ImGui::Button(L("Restore all keys", "Restaurar todas las teclas"))) {
      s.ResetBinds();
      changed = true;
    }
    EndCard();
  }
  if (changed) app.Save();
}

void TabAudio(App& app) {
  Settings& s = app.settings;
  bool changed = false;
  BeginCard("##lang", L("Language", "Idioma"));
  if (BeginRows("##l")) {
    Row(L("Game language", "Idioma del juego"));
    changed |= Combo<int>("##language", s.user_language,
                          {{5, "Español"},
                           {1, "English"},
                           {4, "Français"},
                           {3, "Deutsch"},
                           {6, "Italiano"},
                           {11, "Polski"},
                           {12, L("Russian", "Ruso")},
                           {2, L("Japanese", "Japonés")},
                           {7, L("Korean", "Coreano")},
                           {8, L("Chinese", "Chino")}});
    EndRows();
  }
  EndCard();
  BeginCard("##sound", L("Sound", "Sonido"));
  if (BeginRows("##s")) {
    Row(L("Mute the game", "Silenciar el juego"));
    changed |= Toggle("##mute", &s.audio_mute);
    EndRows();
  }
  EndCard();
  if (changed) app.Save();
}

void CardTask(App& app);

// Opens a .zip picker; the result lands in app.*slot (guarded by dialog_mutex).
void PickZip(App& app, std::optional<std::string> App::*slot) {
  static const SDL_DialogFileFilter filters[] = {{"Zip", "zip"}};
  struct Request {
    App* app;
    std::optional<std::string> App::*slot;
  };
  SDL_ShowOpenFileDialog(
      [](void* user, const char* const* files, int) {
        std::unique_ptr<Request> request(static_cast<Request*>(user));
        if (files && files[0]) {
          std::lock_guard lock(request->app->dialog_mutex);
          request->app->*(request->slot) = files[0];
        }
      },
      new Request{&app, slot}, app.window, filters, 1, nullptr, false);
}

std::vector<std::pair<PlusMode, const char*>> PlusModes() {
  return {{PlusMode::kNormal, "Normal"},
          {PlusMode::kHard, L("Hard", "Difícil")},
          {PlusMode::kSandbox, "Sandbox"},
          {PlusMode::kVanilla, "Vanilla"}};
}

void CardPlusMod(App& app) {
  BeginCard("##plus", "Project Forza Plus 3",
            L("Community mod: real turbos and superchargers, FM4 tyre physics, a new PI system, fuel and tyre "
              "wear, engine and drivetrain swaps, every car and wheel purchasable, four career modes and the "
              "DLC packs. Installed from its .zip files; the original files are kept, so it can be removed.",
              "Mod de la comunidad: turbos y compresores reales, física de neumáticos de FM4, nuevo sistema de "
              "PI, consumo y desgaste, cambios de motor y tracción, todos los coches y llantas a la venta, cuatro "
              "modos de trayectoria y los packs de DLC. Se instala desde sus .zip y guarda los originales, así "
              "que se puede quitar."));
  const auto file_button = [](const std::string& path, const char* id) {
    const std::string label =
        (path.empty() ? std::string(L("Choose .zip", "Elegir .zip")) : fs::u8path(path).filename().u8string()) + id;
    return ImGui::Button(label.c_str());
  };
  if (!app.Installed()) {
    StatusDot(kWarn);
    ImGui::TextUnformatted(L("Install the game first (Install tab).", "Instala antes el juego (pestaña Instalación)."));
  } else if (!app.plus.installed) {
    ImGui::BeginDisabled(app.TaskRunning() || app.game);
    if (BeginRows("##plus_new")) {
      Row(L("Mod archive", "Archivo del mod"), "Project Forza Plus 3 v1.3.zip");
      if (file_button(app.plus_base_zip, "##plus_base")) PickZip(app, &App::picked_plus_base);
      Row(L("Update (optional)", "Actualización (opcional)"), "Project Forza Plus 3 Update v1.3.zip");
      if (file_button(app.plus_update_zip, "##plus_update")) PickZip(app, &App::picked_plus_update);
      Row(L("Career mode", "Modo de trayectoria"));
      Segmented<PlusMode>("plus_mode", app.plus_mode, PlusModes());
      Row(L("Harder AI", "IA más difícil"));
      Toggle("##plus_ai", &app.plus_harder_ai);
      EndRows();
    }
    TextDim(L("About 10 GB are extracted. The Ultimate Collection's content packs are set aside while the mod "
              "is installed (it brings its own versions).",
              "Se extraen unos 10 GB. Los packs de contenido de la Ultimate Collection se apartan mientras el mod "
              "esté instalado (trae sus propias versiones)."));
    ImGui::BeginDisabled(app.plus_base_zip.empty());
    if (PrimaryButton(L("Install FM3 Plus", "Instalar FM3 Plus"), ImVec2(S(260), S(40)))) {
      const fs::path game_dir = app.GameDir(), user_dir = app.UserDir();
      const fs::path base = fs::u8path(app.plus_base_zip);
      const fs::path update = app.plus_update_zip.empty() ? fs::path() : fs::u8path(app.plus_update_zip);
      const PlusMode mode = app.plus_mode;
      const bool harder_ai = app.plus_harder_ai;
      app.StartTask("FM3 Plus", App::TaskKind::kPlus, [&app, game_dir, user_dir, base, update, mode, harder_ai] {
        return InstallPlus(game_dir, user_dir, base, update, mode, harder_ai, app.task);
      });
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
  } else {
    StatusDot(kOk);
    ImGui::TextUnformatted(app.plus.updated ? L("Installed, with the update", "Instalado, con la actualización")
                                            : L("Installed", "Instalado"));
    ImGui::BeginDisabled(app.TaskRunning() || app.game);
    PlusMode mode = app.plus.mode;
    bool harder_ai = app.plus.harder_ai;
    bool changed = false;
    if (BeginRows("##plus_set")) {
      Row(L("Career mode", "Modo de trayectoria"),
          L("The mod suggests a separate profile for each mode.", "El mod recomienda un perfil distinto para cada modo."));
      changed |= Segmented<PlusMode>("plus_mode", mode, PlusModes());
      Row(L("Harder AI", "IA más difícil"));
      changed |= Toggle("##plus_ai", &harder_ai);
      if (!app.plus.updated) {
        Row(L("Update", "Actualización"), "Project Forza Plus 3 Update v1.3.zip");
        if (file_button(app.plus_update_zip, "##plus_update")) PickZip(app, &App::picked_plus_update);
        if (!app.plus_update_zip.empty()) {
          ImGui::SameLine();
          if (ImGui::SmallButton(L("Apply##plus_update", "Aplicar##plus_update"))) {
            const fs::path game_dir = app.GameDir(), user_dir = app.UserDir();
            const fs::path update = fs::u8path(app.plus_update_zip);
            app.StartTask("FM3 Plus", App::TaskKind::kPlus,
                          [&app, game_dir, user_dir, update] { return UpdatePlus(game_dir, user_dir, update, app.task); });
          }
        }
      }
      EndRows();
    }
    if (changed) {
      const std::string error = SetPlusMode(app.GameDir(), app.UserDir(), mode, harder_ai);
      if (!error.empty()) app.notice = error;
      app.CheckGame();
    }
    if (ImGui::Button(L("Uninstall", "Desinstalar"), ImVec2(S(170), 0))) {
      const fs::path game_dir = app.GameDir(), user_dir = app.UserDir();
      app.StartTask("FM3 Plus", App::TaskKind::kPlus,
                    [&app, game_dir, user_dir] { return UninstallPlus(game_dir, user_dir, app.task); });
    }
    ImGui::EndDisabled();
  }
  EndCard();
}

void CardSkipIntro(App& app) {
  if (!app.plus.installed) return;
  BeginCard("##skipintro", L("Quick start", "Inicio rápido"),
            L("The mod's \"Skip Intro\" saves: start a career with the chosen car and some credits, skipping the "
              "first race. Replaces the active profile's career (the current one is moved aside, not deleted).",
              "Las partidas \"Skip Intro\" del mod: empiezas la trayectoria con el coche elegido y algo de dinero, "
              "sin la primera carrera. Sustituye la trayectoria del perfil activo (la actual se aparta, no se borra)."));
  if (app.skip_intro_saves.empty()) {
    TextDim(L("This career mode has no Skip Intro saves.", "Este modo de trayectoria no tiene partidas Skip Intro."));
    EndCard();
    return;
  }
  const Profile* profile = app.ActiveProfile();
  ImGui::BeginDisabled(app.TaskRunning() || app.game || !profile);
  if (BeginRows("##si")) {
    Row(L("Starter car", "Coche inicial"));
    std::vector<std::pair<int, const char*>> items;
    for (int i = 0; i < int(app.skip_intro_saves.size()); ++i) items.emplace_back(i, app.skip_intro_saves[i].c_str());
    Combo<int>("##si_car", app.skip_intro_index, items);
    EndRows();
  }
  const std::string label = std::string(L("Use for profile ", "Usar en el perfil ")) + (profile ? profile->name : "");
  if (ImGui::Button(label.c_str()) && profile) {
    const std::string error = ApplySkipIntroSave(app.GameDir(), app.UserDir(), app.plus.mode,
                                                 app.skip_intro_saves[app.skip_intro_index], profile->xuid);
    app.notice = error.empty() ? std::string(L("Quick-start save copied to ", "Partida de inicio rápido copiada a ")) +
                                     profile->name
                               : error;
  }
  ImGui::EndDisabled();
  EndCard();
}

void CardFov(App& app) {
  BeginCard("##fov", L("Field of view", "Campo de visión"),
            L("Camera angle in degrees for each view: under 100 mph, 100-150 mph and over 150 mph. Takes effect "
              "the next time the game starts. The cockpit view cannot be changed.",
              "Ángulo de la cámara en grados para cada vista: a menos de 160 km/h, entre 160 y 240 km/h y a más "
              "de 240 km/h. Se aplica la próxima vez que arranques el juego. La vista interior no se puede cambiar."));
  if (!app.fov_ok) {
    TextDim(L("Media/Camera/CameraSettings.ini is missing: install the game first.",
              "Falta Media/Camera/CameraSettings.ini: instala antes el juego."));
    EndCard();
    return;
  }
  const char* names[kCameraCount] = {L("Chase, near", "Persecución cercana"), L("Chase, far", "Persecución lejana"),
                                     L("Hood", "Capó"), L("Bumper", "Parachoques")};
  bool commit = false;
  ImGui::BeginDisabled(app.TaskRunning() || app.game);
  if (BeginRows("##fov_rows")) {
    for (int c = 0; c < kCameraCount; ++c) {
      const int* o = app.fov_original.degrees[c];
      char hint[64];
      std::snprintf(hint, sizeof(hint), "%s %d / %d / %d", L("Original:", "Original:"), o[0], o[1], o[2]);
      Row(names[c], hint);
      ImGui::PushID(c);
      ImGui::SliderInt3("##fov", app.fov.degrees[c], 30, 110);
      commit |= ImGui::IsItemDeactivatedAfterEdit();
      ImGui::PopID();
    }
    EndRows();
  }
  if (ImGui::Button(L("Original values", "Valores originales"), ImVec2(S(200), 0))) {
    app.fov = app.fov_original;
    commit = true;
  }
  ImGui::EndDisabled();
  if (commit) {
    const std::string error = SetPlusFov(app.GameDir(), app.fov);
    if (!error.empty()) app.notice = error;
    app.CheckGame();
  }
  EndCard();
}

void TabPlus(App& app) {
  CardTask(app);
  CardPlusMod(app);
  CardSkipIntro(app);
  CardFov(app);
}

// Progress / result of the running or last task.
void CardTask(App& app) {
  const auto task_state = app.task.state.load();
  if (task_state == TaskProgress::State::kIdle) return;
  BeginCard("##task", app.task_title.c_str());
  if (task_state == TaskProgress::State::kRunning) {
    const uint64_t total = app.task.total, done = app.task.done;
    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "%.1f / %.1f GB", double(done) / 1e9, double(total) / 1e9);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
    ImGui::ProgressBar(total ? float(double(done) / double(total)) : 0.0f, ImVec2(-1, S(10)), "");
    ImGui::PopStyleColor();
    ImGui::Text("%s", total ? overlay : L("Preparing...", "Preparando..."));
    TextDim(app.task.Status().c_str());
    if (ImGui::Button(L("Cancel", "Cancelar"))) app.task.cancel = true;
  } else {
    TextColored(task_state == TaskProgress::State::kDone        ? kOk
                : task_state == TaskProgress::State::kCancelled ? kWarn
                                                                : kBad,
                app.task.Status().c_str());
  }
  EndCard();
}

// Opens the .iso picker; the install task starts from the main loop.
void PickIso(App& app) {
  // SDL reads the filters until the callback runs, so they must outlive this frame.
  static const SDL_DialogFileFilter filters_en[] = {{"Xbox 360 disc image", "iso"}, {"All files", "*"}};
  static const SDL_DialogFileFilter filters_es[] = {{"Imagen de disco de Xbox 360", "iso"},
                                                    {"Todos los archivos", "*"}};
  const SDL_DialogFileFilter* filters = g_spanish ? filters_es : filters_en;
  SDL_ShowOpenFileDialog(
      [](void* user, const char* const* files, int) {
        auto* a = static_cast<App*>(user);
        if (files && files[0]) {
          std::lock_guard lock(a->dialog_mutex);
          a->picked_iso = files[0];
        }
      },
      &app, app.window, filters, 2, nullptr, false);
}

void PickFolder(App& app, std::optional<std::string> App::*slot) {
  struct Request {
    App* app;
    std::optional<std::string> App::*slot;
  };
  SDL_ShowOpenFolderDialog(
      [](void* user, const char* const* files, int) {
        std::unique_ptr<Request> request(static_cast<Request*>(user));
        if (files && files[0]) {
          std::lock_guard lock(request->app->dialog_mutex);
          request->app->*(request->slot) = files[0];
        }
      },
      new Request{&app, slot}, app.window, nullptr, false);
}

// One step of the install wizard: number, title, state and its button.
void WizardStep(App& app, int number, const char* title, const char* subtitle, bool done, bool enabled,
                const char* state_text, const char* button) {
  const std::string id = "##step" + std::to_string(number);
  BeginCard(id.c_str(), title, subtitle);
  StatusDot(done ? kOk : (enabled ? kWarn : kDim));
  ImGui::TextUnformatted(state_text);
  ImGui::Dummy(ImVec2(0, S(2)));
  ImGui::BeginDisabled(!enabled || app.TaskRunning() || app.game);
  ImGui::PushID(number);
  if (done ? ImGui::Button(button, ImVec2(S(280), S(36))) : PrimaryButton(button, ImVec2(S(280), S(40)))) {
    PickIso(app);
  }
  ImGui::PopID();
  ImGui::EndDisabled();
  EndCard();
}

void TabInstall(App& app) {
  CardTask(app);

  // Step 1: Disc 1 (Play Disc) -> game folder.
  const char* disc1_state = "";
  switch (app.game_state) {
    case App::GameState::kVerified: disc1_state = L("Installed and verified", "Instalado y verificado"); break;
    case App::GameState::kUnverified: disc1_state = L("Installed, not verified", "Instalado, sin verificar"); break;
    case App::GameState::kIncomplete: disc1_state = L("Incomplete install", "Instalación incompleta"); break;
    default: disc1_state = L("Not installed", "No instalado"); break;
  }
  WizardStep(app, 1, L("Step 1 · Disc 1 (Play Disc)", "Paso 1 · Disco 1 (Play Disc)"),
             L("Pick the disc image (.iso) of Disc 1 of your Forza Motorsport 3 Ultimate Collection. About "
               "7 GB are extracted and every file is checked.",
               "Elige la imagen de disco (.iso) del Disco 1 de tu Forza Motorsport 3 Ultimate Collection. Se "
               "extraen unos 7 GB y se comprueba cada archivo."),
             app.Installed(), true, disc1_state,
             app.Installed() ? L("Reinstall Disc 1...", "Reinstalar Disco 1...")
                             : L("Choose Disc 1 ISO and install", "Elegir ISO del Disco 1 e instalar"));

  // Step 2 normally means the 8 Ultimate Collection packages. FM3 Plus
  // deliberately replaces them with its own expanded content set, so do not
  // report the official pack count or offer to overwrite the mod.
  const bool content_done = app.ContentInstalled();
  char content_state[128];
  if (app.plus.installed) {
    std::snprintf(content_state, sizeof(content_state),
                  L("FM3 Plus content: %zu of %zu packs installed",
                    "Contenido de FM3 Plus: %zu de %zu packs instalados"),
                  app.plus_content_packages, app.plus.dlc_added.size());
  } else {
    std::snprintf(content_state, sizeof(content_state),
                  L("%zu of %zu content packs installed", "%zu de %zu packs de contenido instalados"),
                  app.content_packages, kContentPackages);
  }
  WizardStep(app, 2, L("Step 2 · Disc 2 (Content Install Disc)", "Paso 2 · Disco 2 (Content Install Disc)"),
             app.plus.installed
                 ? L("FM3 Plus replaces the eight Ultimate Collection packs with its own modified content set. "
                     "The original packs are kept and return when the mod is removed.",
                     "FM3 Plus sustituye los ocho packs de Ultimate Collection por su propio conjunto modificado. "
                     "Los originales se conservan y vuelven al quitar el mod.")
                 : L("The cars and tracks of the Ultimate Collection (8 packs, about 3.4 GB). Without them the game "
                     "warns that Disc 2 content is not installed.",
                     "Los coches y circuitos de la Ultimate Collection (8 packs, unos 3,4 GB). Sin ellos el juego "
                     "avisa de que falta el contenido del Disco 2."),
             content_done, app.Installed() && !app.plus.installed,
             app.Installed() ? content_state : L("Install Disc 1 first", "Instala antes el Disco 1"),
             app.plus.installed ? L("Managed by FM3 Plus", "Gestionado por FM3 Plus")
             : content_done ? L("Reinstall Disc 2...", "Reinstalar Disco 2...")
                          : L("Choose Disc 2 ISO and install", "Elegir ISO del Disco 2 e instalar"));

  if (!UserDirFitsContent(app.UserDir())) {
    TextColored(kWarn, L("The user data folder path is too long for the Disc 2 files: choose a shorter one below.",
                         "La ruta de la carpeta de datos de usuario es demasiado larga para los archivos del "
                         "Disco 2: elige una más corta abajo."));
    ImGui::Dummy(ImVec2(0, S(6)));
  }
  BeginCard("##folders", L("Folders", "Carpetas"),
            L("Game files (Disc 1) and user data (saves, Disc 2 content). By default, \"game\" and \"user\" "
              "next to the launcher.",
              "Archivos del juego (Disco 1) y datos de usuario (partidas, contenido del Disco 2). Por defecto, "
              "\"game\" y \"user\" junto al launcher."));
  ImGui::BeginDisabled(app.TaskRunning() || app.game);
  if (BeginRows("##f")) {
    Row(L("Game", "Juego"));
    std::string dir = app.GameDir().u8string();
    ImGui::InputText("##gamedir", dir.data(), dir.size() + 1, ImGuiInputTextFlags_ReadOnly);
    Row(L("User data", "Datos de usuario"));
    std::string udir = app.UserDir().u8string();
    ImGui::InputText("##userdir", udir.data(), udir.size() + 1, ImGuiInputTextFlags_ReadOnly);
    EndRows();
  }
  if (ImGui::Button(L("Change game folder", "Cambiar carpeta del juego"), ImVec2(S(220), 0))) {
    PickFolder(app, &App::picked_game_folder);
  }
  ImGui::SameLine();
  if (ImGui::Button(L("Change user folder", "Cambiar carpeta de usuario"), ImVec2(S(220), 0))) {
    PickFolder(app, &App::picked_user_folder);
  }
  if (app.Installed()) {
    ImGui::SameLine();
    if (ImGui::Button(L("Verify files", "Verificar archivos"), ImVec2(S(170), 0))) {
      const fs::path game_dir = app.GameDir();
      app.StartTask(L("Verification", "Verificación"), App::TaskKind::kVerify,
                    [&app, game_dir] { return app.RunVerify(game_dir); });
    }
  }
  ImGui::EndDisabled();
  EndCard();
}

// Underlined text tabs across the top of the settings column.
void TabBar(App& app) {
  const std::pair<Tab, const char*> tabs[] = {{Tab::kProfiles, L("Profiles", "Perfiles")},
                                              {Tab::kGraphics, L("Graphics", "Gráficos")},
                                              {Tab::kControls, L("Controls", "Controles")},
                                              {Tab::kAudio, L("Language & sound", "Idioma y sonido")},
                                              {Tab::kPlus, "FM3 Plus"},
                                              {Tab::kInstall, L("Install", "Instalación")}};
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::PushFont(g_bold, ImGui::GetStyle().FontSizeBase * 1.05f);
  for (const auto& [tab, label] : tabs) {
    const bool active = app.tab == tab;
    const ImVec2 sz = ImGui::CalcTextSize(label);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    if (ImGui::InvisibleButton("tab", ImVec2(sz.x + S(8), sz.y + S(16)))) {
      app.tab = tab;
      app.capture_index = -1;
    }
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    dl->AddText(ImVec2(p.x + S(4), p.y + S(4)), Col(active || hovered ? kText : kDim), label);
    if (active) {
      dl->AddRectFilled(ImVec2(p.x + S(4), p.y + sz.y + S(10)), ImVec2(p.x + sz.x + S(4), p.y + sz.y + S(13)),
                        Col(kAccent), S(2));
    }
    ImGui::SameLine(0, S(22));
  }
  ImGui::PopFont();

  // Launcher language, right-aligned: EN | ES.
  const float pill_w = S(42);
  ImGui::SameLine(ImGui::GetContentRegionMax().x - pill_w * 2 - S(4));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(7));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(4), 0));
  for (const auto& [spanish, code] : {std::pair{false, "EN"}, std::pair{true, "ES"}}) {
    const bool active = g_spanish == spanish;
    ImGui::PushStyleColor(ImGuiCol_Button, active ? kAccent : kControl);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? kAccentHover : ImVec4(0.22f, 0.23f, 0.26f, 1));
    if (ImGui::Button(code, ImVec2(pill_w, 0)) && !active) {
      g_spanish = spanish;
      app.state.spanish = spanish;
      app.Save();
      app.CheckGame();  // status texts are localized
    }
    ImGui::PopStyleColor(2);
    if (!spanish) ImGui::SameLine();
  }
  ImGui::PopStyleVar(2);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  dl->AddLine(ImVec2(p.x, p.y - S(2)), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y - S(2)),
              Col(ImVec4(1, 1, 1, 0.07f)));
  ImGui::Dummy(ImVec2(0, S(10)));
}

void ApplyStyle() {
  ImGuiStyle& style = ImGui::GetStyle();
  ImGui::StyleColorsDark();
  style.WindowRounding = 0;
  style.ChildRounding = S(12);
  style.FrameRounding = S(8);
  style.PopupRounding = S(10);
  style.GrabRounding = S(8);
  style.ScrollbarRounding = S(8);
  style.TabRounding = S(8);
  style.FramePadding = ImVec2(S(12), S(8));
  style.ItemSpacing = ImVec2(S(10), S(10));
  style.WindowPadding = ImVec2(S(0), S(0));
  style.ScrollbarSize = S(10);
  style.WindowBorderSize = 0;
  style.ChildBorderSize = 0;
  style.PopupBorderSize = 0;
  style.FrameBorderSize = 0;
  ImVec4* c = style.Colors;
  c[ImGuiCol_Text] = kText;
  c[ImGuiCol_TextDisabled] = kDim;
  c[ImGuiCol_WindowBg] = kBg;
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.125f, 0.14f, 0.98f);
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
  c[ImGuiCol_FrameBg] = kControl;
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.22f, 0.25f, 1);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.25f, 0.28f, 1);
  c[ImGuiCol_Button] = kControl;
  c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.23f, 0.26f, 1);
  c[ImGuiCol_ButtonActive] = kAccent;
  c[ImGuiCol_Header] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.30f);
  c[ImGuiCol_HeaderHovered] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.45f);
  c[ImGuiCol_HeaderActive] = kAccent;
  c[ImGuiCol_CheckMark] = kAccent;
  c[ImGuiCol_SliderGrab] = kAccent;
  c[ImGuiCol_SliderGrabActive] = kAccentHover;
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.12f);
  c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.07f);
  c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TableRowBgAlt] = ImVec4(0, 0, 0, 0);
}

// Headless mode for scripts and tests:
//   fm3-launcher --import-iso <iso> --game-dir <dir> --user-dir <dir> [--log <file>]
//     Disc 1: extract + full verification. Disc 2: content install (needs Disc 1
//     in --game-dir). The disc is detected from the image.
//   fm3-launcher --verify --game-dir <dir> [--log <file>]
//   fm3-launcher --install-plus <zip> [--plus-update <zip>] [--plus-mode 0-3] [--plus-harder-ai]
//                --game-dir <dir> --user-dir <dir> [--log <file>]
//   fm3-launcher --uninstall-plus --game-dir <dir> --user-dir <dir> [--log <file>]
// Same code paths as the UI; prints progress to the log, exit code 0 = ok.
int RunHeadless(int argc, char** argv) {
  std::string iso, dir, user_dir, log_path, plus_zip, plus_update;
  bool verify_only = false, plus_uninstall = false, plus_harder_ai = false;
  int plus_mode = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--import-iso" && i + 1 < argc) iso = argv[++i];
    else if (a == "--game-dir" && i + 1 < argc) dir = argv[++i];
    else if (a == "--user-dir" && i + 1 < argc) user_dir = argv[++i];
    else if (a == "--log" && i + 1 < argc) log_path = argv[++i];
    else if (a == "--verify") verify_only = true;
    else if (a == "--install-plus" && i + 1 < argc) plus_zip = argv[++i];
    else if (a == "--plus-update" && i + 1 < argc) plus_update = argv[++i];
    else if (a == "--plus-mode" && i + 1 < argc) plus_mode = std::clamp(std::atoi(argv[++i]), 0, 3);
    else if (a == "--plus-harder-ai") plus_harder_ai = true;
    else if (a == "--uninstall-plus") plus_uninstall = true;
  }
  std::ofstream log(log_path.empty() ? "launcher-headless.log" : log_path);
  const fs::path exe_dir = fs::u8path(SDL_GetBasePath());
  const auto manifest = LoadManifest(exe_dir / "game_files.sha256");
  if (manifest.empty() || dir.empty()) {
    log << "error: missing game_files.sha256 or --game-dir\n";
    return 2;
  }
  if ((!plus_zip.empty() || plus_uninstall) && user_dir.empty()) {
    log << "error: --user-dir is required for mod operations\n";
    return 2;
  }
  TaskProgress progress;
  std::atomic<bool> finished{false};
  std::thread reporter([&] {
    while (!finished) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      log << progress.done / 1000000 << "/" << progress.total / 1000000 << " MB  " << progress.Status()
          << std::endl;
    }
  });
  const auto start = std::chrono::steady_clock::now();
  bool ok = true;
  bool verify = verify_only;
  if (!plus_zip.empty() || plus_uninstall) {
    ok = plus_uninstall ? UninstallPlus(fs::u8path(dir), fs::u8path(user_dir), progress)
                        : InstallPlus(fs::u8path(dir), fs::u8path(user_dir), fs::u8path(plus_zip),
                                      fs::u8path(plus_update), PlusMode(plus_mode), plus_harder_ai, progress);
    log << (plus_uninstall ? "uninstall-plus: " : "install-plus: ") << (ok ? "ok" : "FAILED") << " - "
        << progress.Status() << std::endl;
  } else if (!verify_only) {
    const DiscKind kind = IdentifyIso(fs::u8path(iso));
    log << "disc: " << (kind == DiscKind::kPlay ? "play" : kind == DiscKind::kContent ? "content" : "unknown")
        << std::endl;
    if (kind == DiscKind::kPlay) {
      ok = ExtractPlayDisc(fs::u8path(iso), fs::u8path(dir), progress);
      verify = ok;
      log << "extract: " << (ok ? "ok" : "FAILED") << " - " << progress.Status() << std::endl;
    } else if (kind == DiscKind::kContent) {
      if (user_dir.empty()) {
        log << "error: --user-dir is required for Disc 2\n";
        ok = false;
      } else {
        ok = InstallContentDisc(fs::u8path(iso), fs::u8path(dir), fs::u8path(user_dir),
                                exe_dir / "fm3tool.exe", progress);
        log << "content: " << (ok ? "ok" : "FAILED") << " - " << progress.Status() << std::endl;
        log << "packages: " << CountContentPackages(fs::u8path(user_dir)) << "/" << kContentPackages << std::endl;
      }
    } else {
      ok = false;
    }
  }
  if (ok && verify) {
    const VerifyReport report = VerifyGame(
        fs::u8path(dir), WithOriginals(fs::u8path(dir), WithoutPlusFiles(manifest, LoadPlus(fs::u8path(dir)))), true,
        progress);
    ok = report.ok();
    log << "verify: " << (ok ? "ok" : "FAILED") << " missing=" << report.missing.size()
        << " size=" << report.wrong_size.size() << " hash=" << report.wrong_hash.size() << std::endl;
    for (const auto* list : {&report.missing, &report.wrong_size, &report.wrong_hash}) {
      for (const auto& p : *list) log << "  " << p << "\n";
    }
    if (ok) std::ofstream(VerifiedMarker(fs::u8path(dir))) << "verified against game_files.sha256\n";
  }
  finished = true;
  reporter.join();
  log << "seconds: "
      << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count()
      << std::endl;
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  fm3::InstallCrashHandler(fs::u8path(SDL_GetBasePath()) / "logs");
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--import-iso" || a == "--verify" || a == "--install-plus" || a == "--uninstall-plus") {
      return RunHeadless(argc, argv);
    }
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FM3 Launcher", SDL_GetError(), nullptr);
    return 1;
  }
  App app;
  app.exe_dir = fs::u8path(SDL_GetBasePath());
  app.user_config = app.exe_dir / "fm3.user.toml";
  app.port_config = app.exe_dir / "fm3.toml";
  app.launcher_config = app.exe_dir / "launcher.toml";
  app.manifest_path = app.exe_dir / "game_files.sha256";
  app.game_exe = app.exe_dir / "fm3.exe";
  app.tool_exe = app.exe_dir / "fm3tool.exe";
  app.cover_path = app.exe_dir / "cover.jpg";
  app.Load();
  app.manifest = LoadManifest(app.manifest_path);
  if (app.manifest.empty()) {
    app.notice = L("game_files.sha256 is missing next to the launcher: the game cannot be verified.",
                   "Falta game_files.sha256 junto al launcher: no se puede verificar el juego.");
  }
  app.CheckGame();
  if (!app.Installed()) app.tab = Tab::kInstall;

  g_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  if (g_scale <= 0) g_scale = 1.0f;
  app.window = SDL_CreateWindow("Forza Motorsport 3", int(CoverWidth() + SettingsWidth()), int(WindowHeight()),
                                SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  // The SDK's SDL3 is built without the 2D renderer; SDL_GPU (D3D12 on
  // Windows, Vulkan elsewhere) is what it has.
  SDL_GPUDevice* gpu = SDL_CreateGPUDevice(
      SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
  if (!gpu || !SDL_ClaimWindowForGPUDevice(gpu, app.window)) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FM3 Launcher",
                             (std::string(L("Could not start the GPU: ", "No se pudo iniciar la GPU: ")) + SDL_GetError()).c_str(),
                             app.window);
    return 1;
  }
  app.gpu = gpu;
  SDL_SetGPUSwapchainParameters(gpu, app.window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);
  const SDL_GPUTextureFormat swap_format = SDL_GetGPUSwapchainTextureFormat(gpu, app.window);
  if (fs::exists(app.cover_path)) LoadCover(gpu, app.cover_path, app.cover);
  app.reload_cover = false;
  SDL_SetWindowPosition(app.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  SDL_ShowWindow(app.window);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  ApplyStyle();
  const float font_size = 17.0f * g_scale;
  const char* regular = "C:\\Windows\\Fonts\\segoeui.ttf";
  const char* bold = "C:\\Windows\\Fonts\\seguisb.ttf";
  // Segoe UI lacks arrows and a few symbols (↺ ← ↑); merge Segoe UI Symbol in.
  const char* symbols = "C:\\Windows\\Fonts\\seguisym.ttf";
  ImFontConfig merge;
  merge.MergeMode = true;
  if (fs::exists(regular)) {
    io.Fonts->AddFontFromFileTTF(regular, font_size);
    if (fs::exists(symbols)) io.Fonts->AddFontFromFileTTF(symbols, font_size, &merge);
  }
  if (fs::exists(bold)) {
    g_bold = io.Fonts->AddFontFromFileTTF(bold, font_size);
    if (fs::exists(symbols)) io.Fonts->AddFontFromFileTTF(symbols, font_size, &merge);
  }
  ImGui::GetStyle().FontSizeBase = font_size;
  ImGui_ImplSDL3_InitForSDLGPU(app.window);
  ImGui_ImplSDLGPU3_InitInfo gpu_info = {};
  gpu_info.Device = gpu;
  gpu_info.ColorTargetFormat = swap_format;
  gpu_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
  ImGui_ImplSDLGPU3_Init(&gpu_info);

  // --screenshot <file.bmp> [--tab profiles|graphics|controls|audio|install]:
  // render a few frames, save the window and exit (docs and UI checks).
  std::string screenshot;
  int screenshot_frame = 4;
  for (int i = 1; i + 1 < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--screenshot") screenshot = argv[i + 1];
    if (a == "--screenshot-frame") screenshot_frame = std::max(4, std::atoi(argv[i + 1]));
    if (a == "--tab") {
      const std::string t = argv[i + 1];
      app.tab = t == "plus"       ? Tab::kPlus
                : t == "graphics" ? Tab::kGraphics
                : t == "controls" ? Tab::kControls
                : t == "audio"    ? Tab::kAudio
                : t == "install"  ? Tab::kInstall
                                  : Tab::kProfiles;
    }
    if (a == "--lang") {
      g_spanish = std::string(argv[i + 1]) == "es";
    }
  }
  int frames = 0;

  // --play: press PLAY right away (desktop shortcuts straight into the game).
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--play" && app.Installed()) app.LaunchGame();
    // --start-import <iso>: same as picking the ISO in the Install tab (tests).
    if (a == "--start-import" && i + 1 < argc) {
      std::lock_guard lock(app.dialog_mutex);
      app.picked_iso = argv[i + 1];
    }
  }

  bool running = true;
  while (running) {
    const bool capture_now = !screenshot.empty() && ++frames == screenshot_frame;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (app.capture_index >= 0 && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        auto& bind = app.settings.binds[app.capture_index];
        if (event.key.scancode != SDL_SCANCODE_ESCAPE) {
          std::string name = KeyName(event.key.scancode);
          if (!name.empty()) {
            if ((event.key.mod & SDL_KMOD_SHIFT) && name != "Shift") name = "Shift+" + name;
            bind.keys = app.capture_append && !bind.keys.empty() ? bind.keys + "," + name : name;
            app.Save();
          }
        }
        app.capture_index = -1;
        continue;  // don't let ImGui see the captured key
      }
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT) running = false;
    }
    if (SDL_GetWindowFlags(app.window) & SDL_WINDOW_MINIMIZED) {
      app.PollGame();
      SDL_Delay(100);
      continue;
    }

    // Results from file dialogs.
    {
      std::lock_guard lock(app.dialog_mutex);
      if (app.picked_game_folder) {
        app.settings.game_data_root = *app.picked_game_folder;
        app.picked_game_folder.reset();
        app.Save();
        app.CheckGame();
      }
      if (app.picked_user_folder) {
        app.settings.user_data_root = *app.picked_user_folder;
        app.picked_user_folder.reset();
        app.Save();
        app.CheckGame();
      }
      if (app.picked_plus_base) {
        app.plus_base_zip = *app.picked_plus_base;
        app.picked_plus_base.reset();
      }
      if (app.picked_plus_update) {
        app.plus_update_zip = *app.picked_plus_update;
        app.picked_plus_update.reset();
      }
      if (app.picked_iso) {
        const fs::path iso = fs::u8path(*app.picked_iso);
        app.picked_iso.reset();
        app.StartImport(iso);
      }
    }
    static TaskProgress::State last_state = TaskProgress::State::kIdle;
    if (last_state == TaskProgress::State::kRunning && !app.TaskRunning()) {
      app.CheckGame();
    }
    last_state = app.task.state;
    if (app.reload_cover) {
      app.reload_cover = false;
      ReleaseCover(gpu, app.cover);
      LoadCover(gpu, app.cover_path, app.cover);
    }
    app.PollGame();

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollWithMouse);

    // Left: the cover column (fixed width, the cover shown whole).
    const float cover_w = CoverWidth();
    ImGui::BeginChild("##cover", ImVec2(cover_w, vp->Size.y), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    CoverPanel(app, ImVec2(cover_w, vp->Size.y));
    ImGui::EndChild();
    ImGui::SameLine(0, 0);

    // Right: tabs + content.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(34), S(26)));
    ImGui::BeginChild("##settings", ImVec2(0, vp->Size.y), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    TabBar(app);
    if (!app.notice.empty()) {
      TextColored(kWarn, app.notice.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton(L("Dismiss", "Cerrar"))) app.notice.clear();
    }
    ImGui::BeginChild("##content", ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(16)));
    switch (app.tab) {
      case Tab::kProfiles: TabProfiles(app); break;
      case Tab::kGraphics: TabGraphics(app); break;
      case Tab::kControls: TabControls(app); break;
      case Tab::kAudio: TabAudio(app); break;
      case Tab::kPlus: TabPlus(app); break;
      case Tab::kInstall: TabInstall(app); break;
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::End();

    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUTexture* swapchain = nullptr;
    Uint32 width = 0, height = 0;
    SDL_WaitAndAcquireGPUSwapchainTexture(cmd, app.window, &swapchain, &width, &height);
    // Screenshot mode draws into an offscreen copy that can be downloaded.
    SDL_GPUTexture* target = swapchain;
    SDL_GPUTexture* offscreen = nullptr;
    if (capture_now && swapchain) {
      SDL_GPUTextureCreateInfo info = {};
      info.type = SDL_GPU_TEXTURETYPE_2D;
      info.format = swap_format;
      info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
      info.width = width;
      info.height = height;
      info.layer_count_or_depth = 1;
      info.num_levels = 1;
      offscreen = SDL_CreateGPUTexture(gpu, &info);
      target = offscreen;
    }
    if (target && draw_data->DisplaySize.x > 0) {
      ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd);
      SDL_GPUColorTargetInfo color = {};
      color.texture = target;
      color.clear_color = SDL_FColor{kBg.x, kBg.y, kBg.z, 1.0f};
      color.load_op = SDL_GPU_LOADOP_CLEAR;
      color.store_op = SDL_GPU_STOREOP_STORE;
      SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
      ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd, pass);
      SDL_EndGPURenderPass(pass);
    }
    if (offscreen) {
      SDL_GPUTransferBufferCreateInfo tb_info = {};
      tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
      tb_info.size = width * height * 4;
      SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(gpu, &tb_info);
      SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
      SDL_GPUTextureRegion region = {};
      region.texture = offscreen;
      region.w = width;
      region.h = height;
      region.d = 1;
      SDL_GPUTextureTransferInfo dst = {};
      dst.transfer_buffer = tb;
      SDL_DownloadFromGPUTexture(copy, &region, &dst);
      SDL_EndGPUCopyPass(copy);
      SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
      SDL_WaitForGPUFences(gpu, true, &fence, 1);
      SDL_ReleaseGPUFence(gpu, fence);
      void* pixels = SDL_MapGPUTransferBuffer(gpu, tb, false);
      SDL_Surface* shot = SDL_CreateSurfaceFrom(int(width), int(height),
                                                SDL_GetPixelFormatFromGPUTextureFormat(swap_format), pixels,
                                                int(width * 4));
      if (!shot || !SDL_SaveBMP(shot, screenshot.c_str())) {
        std::ofstream(screenshot + ".txt") << "screenshot failed: " << SDL_GetError() << "\n";
      }
      SDL_DestroySurface(shot);
      SDL_UnmapGPUTransferBuffer(gpu, tb);
      SDL_ReleaseGPUTransferBuffer(gpu, tb);
      SDL_ReleaseGPUTexture(gpu, offscreen);
      running = false;
    } else {
      SDL_SubmitGPUCommandBuffer(cmd);
    }
  }

  app.task.cancel = true;
  if (app.worker.joinable()) app.worker.join();
  SDL_WaitForGPUIdle(gpu);
  ImGui_ImplSDL3_Shutdown();
  ImGui_ImplSDLGPU3_Shutdown();
  ImGui::DestroyContext();
  ReleaseCover(gpu, app.cover);
  SDL_ReleaseWindowFromGPUDevice(gpu, app.window);
  SDL_DestroyGPUDevice(gpu);
  SDL_DestroyWindow(app.window);
  SDL_Quit();
  return 0;
}
