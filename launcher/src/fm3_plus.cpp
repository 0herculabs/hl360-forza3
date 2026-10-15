#include "fm3_plus.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>

#include "i18n.h"
#include "miniz.h"
#include "safe_path.h"

namespace launcher {
namespace fs = std::filesystem;

namespace {

constexpr char kModDir[] = ".fm3-plus";
constexpr char kOriginalsDir[] = ".fm3-originals";  // shared with SetShadowsHidden
constexpr char kDlcPrefix[] = "@dlc/";  // plan destinations under the content folder

const char* ModeFolder(PlusMode mode) {
  switch (mode) {
    case PlusMode::kHard: return "Hard Mode";
    case PlusMode::kSandbox: return "Sandbox Mode";
    case PlusMode::kVanilla: return "Vanilla Mode";
    default: return "Normal Mode";
  }
}

fs::path ModeDir(const fs::path& game_dir, PlusMode mode) {
  return game_dir / kModDir / "modes" / ModeFolder(mode);
}

fs::path DlcDir(const fs::path& user_dir) {
  return user_dir / "0000000000000000" / "4D53084D" / "00000002";
}

std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// A release archive, read with miniz through a FILE* so non-ASCII Windows
// paths open too (and files over 4 GB: miniz handles zip64).
class Zip {
 public:
  explicit Zip(const fs::path& path) {
#if defined(_WIN32)
    if (_wfopen_s(&file_, path.c_str(), L"rb") != 0) file_ = nullptr;
#else
    file_ = std::fopen(path.c_str(), "rb");
#endif
    ok_ = file_ && mz_zip_reader_init_cfile(&zip_, file_, 0, 0);
  }
  ~Zip() {
    if (ok_) mz_zip_reader_end(&zip_);
    if (file_) std::fclose(file_);
  }
  Zip(const Zip&) = delete;
  Zip& operator=(const Zip&) = delete;

  bool ok() const { return ok_; }
  mz_uint count() { return mz_zip_reader_get_num_files(&zip_); }
  bool Stat(mz_uint index, mz_zip_archive_file_stat& stat) { return mz_zip_reader_file_stat(&zip_, index, &stat); }

  // Streams one entry to `out`, counting into progress.done; stops on cancel.
  bool Extract(mz_uint index, const fs::path& out, TaskProgress& progress) {
    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    struct Sink {
      std::ofstream* file;
      TaskProgress* progress;
    } sink{&file, &progress};
    const auto write = [](void* opaque, mz_uint64, const void* data, size_t size) -> size_t {
      auto* s = static_cast<Sink*>(opaque);
      s->file->write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
      s->progress->done += size;
      return s->progress->cancel || !*s->file ? 0 : size;
    };
    return mz_zip_reader_extract_to_callback(&zip_, index, write, &sink, 0) && file.good();
  }

 private:
  mz_zip_archive zip_{};
  FILE* file_ = nullptr;
  bool ok_ = false;
};

struct Copy {
  Zip* zip;
  mz_uint index;
  std::string dest;  // '/'-separated, relative to the game folder (or kDlcPrefix + package path)
  uint64_t size;
};

// The runtime also uses legacy Windows paths. Extracting with a long-path
// prefix alone would install content that the game still cannot open.
bool CheckPaths(const fs::path& game_dir, const fs::path& user_dir,
                const std::map<std::string, Copy>& plan, bool updating, TaskProgress& progress) {
#if defined(_WIN32)
  for (const auto& [key, copy] : plan) {
    const bool dlc = copy.dest.rfind(kDlcPrefix, 0) == 0;
    fs::path target = dlc ? DlcDir(user_dir) / fs::u8path(copy.dest.substr(sizeof(kDlcPrefix) - 1))
                          : game_dir / fs::u8path(copy.dest);
    if (updating) target += ".plus-new";
    // Ordinary game files also need a backup below .fm3-plus/backup.
    const fs::path backup = !dlc && copy.dest.rfind(kModDir, 0) != 0
        ? game_dir / kModDir / "backup" / fs::u8path(copy.dest) : target;
    for (const auto& path : {target, backup}) {
      std::error_code ec;
      const fs::path full = fs::absolute(path, ec).lexically_normal();
      if (ec) { progress.SetStatus(L("Could not resolve the installation path.", "No se pudo resolver la ruta de instalacion.")); return false; }
      if (full.native().size() > 259 || full.parent_path().native().size() > 247) {
        progress.SetStatus(std::string(L(
            "FM3 Plus path too long. Choose shorter game and user-data folders in Install, e.g. C:\\FM3\\game and C:\\FM3\\user. No installation files were changed. Path: ",
            "Ruta de FM3 Plus demasiado larga. Elige carpetas mas cortas para el juego y los datos de usuario en Instalacion, p. ej. C:\\FM3\\game y C:\\FM3\\user. No se han cambiado archivos de instalacion. Ruta: ")) + full.u8string());
        return false;
      }
    }
  }
#endif
  return true;
}

// Adds every file under `<root><from>` to the plan as `<to><rest>`. Keyed
// case-insensitively, so the update archive overrides the base one.
bool Plan(Zip& zip, const std::string& root, const std::string& from, const std::string& to,
          std::map<std::string, Copy>& plan) {
  const std::string prefix = root + from;
  mz_zip_archive_file_stat stat;
  for (mz_uint i = 0; i < zip.count(); ++i) {
    if (!zip.Stat(i, stat) || stat.m_is_directory) continue;
    const std::string name = stat.m_filename;
    if (Lower(name).rfind(Lower(prefix), 0) != 0) continue;
    if (!IsSafeRelativePath(name.substr(prefix.size()))) return false;
    const std::string dest = to + name.substr(prefix.size());
    plan[Lower(dest)] = {&zip, i, dest, stat.m_uncomp_size};
  }
  return true;
}

// The folder that holds `folder` inside the archive ("Project Forza Plus 3/"
// for "Main Game Changes/"); nullopt when the archive has no such folder.
std::optional<std::string> FindRoot(Zip& zip, const std::string& folder) {
  mz_zip_archive_file_stat stat;
  for (mz_uint i = 0; i < zip.count(); ++i) {
    if (!zip.Stat(i, stat)) continue;
    const std::string name = stat.m_filename;
    const auto pos = name.find(folder);
    if (pos != std::string::npos && (pos == 0 || name[pos - 1] == '/')) return name.substr(0, pos);
  }
  return std::nullopt;
}

bool WriteState(const fs::path& game_dir, const PlusState& state) {
  std::ofstream out(game_dir / kModDir / "state.txt", std::ios::trunc);
  out << "mode " << int(state.mode) << "\nharder_ai " << int(state.harder_ai) << "\nupdated " << int(state.updated)
      << "\n";
  for (const auto& p : state.replaced) out << "replaced " << p << "\n";
  for (const auto& p : state.added) out << "added " << p << "\n";
  for (const auto& p : state.dlc_added) out << "dlc_added " << p << "\n";
  for (const auto& p : state.dlc_moved) out << "dlc_moved " << p << "\n";
  return bool(out);
}

// Removes `dir` and its parents while they are empty, up to `stop`.
void RemoveEmptyDirs(fs::path dir, const fs::path& stop) {
  std::error_code ec;
  while (dir != stop && dir.has_parent_path() && fs::is_empty(dir, ec) && !ec) {
    fs::remove(dir, ec);
    dir = dir.parent_path();
  }
}

// Puts the original files and packages back and deletes the mod's.
bool Restore(const fs::path& game_dir, const fs::path& user_dir, const PlusState& state, TaskProgress& progress) {
  std::error_code ec;
  bool ok = true;
  for (const auto* paths : {&state.replaced, &state.added, &state.dlc_added, &state.dlc_moved}) {
    for (const auto& p : *paths) {
      if (!IsSafeRelativePath(p)) {
        progress.SetStatus(L("Unsafe mod state path; backups preserved.", "Ruta insegura en el estado del mod; copias conservadas."));
        return false;
      }
    }
  }
  for (const auto& p : state.replaced) {
    fs::copy_file(game_dir / kModDir / "backup" / fs::u8path(p), game_dir / fs::u8path(p),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
      progress.SetStatus(std::string(L("Could not restore ", "No se pudo restaurar ")) + p);
      ok = false;
    }
  }
  for (const auto& p : state.added) {
    const fs::path file = game_dir / fs::u8path(p);
    fs::remove(file, ec);
    if (ec) ok = false;
    RemoveEmptyDirs(file.parent_path(), game_dir / "Media");
  }
  const fs::path dlc = DlcDir(user_dir);
  for (const auto& p : state.dlc_added) {
    fs::remove_all(dlc / fs::u8path(p), ec);
    if (ec) ok = false;
  }
  for (const auto& p : state.dlc_moved) {
    fs::rename(user_dir / kModDir / "dlc" / fs::u8path(p), dlc / fs::u8path(p), ec);
    if (ec) {
      progress.SetStatus(std::string(L("Could not restore the content package ",
                                       "No se pudo restaurar el paquete de contenido ")) + p);
      ok = false;
    }
  }
  if (ok) {
    fs::remove_all(game_dir / kModDir, ec);
    fs::remove_all(user_dir / kModDir, ec);
  }
  return ok;
}

// Copies every file under `from` over `to`, keeping relative paths.
bool CopyTree(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const fs::path out = to / fs::relative(it->path(), from, ec);
    fs::create_directories(out.parent_path(), ec);
    fs::copy_file(it->path(), out, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
  }
  return !ec;
}

std::string Missing() {
  return L("FM3 Plus is not installed.", "FM3 Plus no está instalado.");
}

}  // namespace

PlusState LoadPlus(const fs::path& game_dir) {
  PlusState state;
  std::ifstream in(game_dir / kModDir / "state.txt");
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto space = line.find(' ');
    if (space == std::string::npos) continue;
    const std::string key = line.substr(0, space), value = line.substr(space + 1);
    state.installed = true;
    if (key == "mode") state.mode = PlusMode(std::clamp(std::atoi(value.c_str()), 0, 3));
    if (key == "harder_ai") state.harder_ai = value == "1";
    if (key == "updated") state.updated = value == "1";
    if (key == "replaced") state.replaced.push_back(value);
    if (key == "added") state.added.push_back(value);
    if (key == "dlc_added") state.dlc_added.push_back(value);
    if (key == "dlc_moved") state.dlc_moved.push_back(value);
  }
  return state;
}

bool InstallPlus(const fs::path& game_dir, const fs::path& user_dir, const fs::path& base_zip,
                 const fs::path& update_zip, PlusMode mode, bool harder_ai, TaskProgress& progress) {
  std::error_code ec;
  if (!fs::exists(game_dir / "default.xex", ec)) {
    progress.SetStatus(L("Install the game first (Install tab).", "Instala antes el juego (pestaña Instalación)."));
    return false;
  }
  Zip base(base_zip);
  std::unique_ptr<Zip> update;
  if (!update_zip.empty()) update = std::make_unique<Zip>(update_zip);
  if (!base.ok() || (update && !update->ok())) {
    progress.SetStatus(L("Could not open the archive.", "No se pudo abrir el archivo comprimido."));
    return false;
  }
  const auto root = FindRoot(base, "Main Game Changes/");
  if (!root) {
    progress.SetStatus(L("This is not the Project Forza Plus 3 archive (no \"Main Game Changes\" folder).",
                         "No es el archivo de Project Forza Plus 3 (falta la carpeta \"Main Game Changes\")."));
    return false;
  }
  std::map<std::string, Copy> plan;
  // Its default.xex (the 2009 release, edited) is not used: see fm3_plus.h.
  if (!Plan(base, *root, "Main Game Changes/Media/", "Media/", plan)) { progress.SetStatus(L("Unsafe archive path.", "Ruta insegura en el archivo.")); return false; }
  if (!Plan(base, *root, "Game Mode DBs/", std::string(kModDir) + "/modes/", plan)) { progress.SetStatus(L("Unsafe archive path.", "Ruta insegura en el archivo.")); return false; }
  if (!Plan(base, *root, "Disc 2 and DLCs/00000002/", kDlcPrefix, plan)) { progress.SetStatus(L("Unsafe archive path.", "Ruta insegura en el archivo.")); return false; }
  if (update) {
    const auto update_root = FindRoot(*update, "Update Files/");
    if (!update_root) {
      progress.SetStatus(L("This is not the Project Forza Plus 3 update archive (no \"Update Files\" folder).",
                           "No es la actualización de Project Forza Plus 3 (falta la carpeta \"Update Files\")."));
      return false;
    }
    if (!Plan(*update, *update_root, "Update Files/Game Mode DBs/", std::string(kModDir) + "/modes/", plan)) { progress.SetStatus(L("Unsafe archive path.", "Ruta insegura en el archivo.")); return false; }
    if (!Plan(*update, *update_root, "Update Files/Physics/", "Media/Physics/", plan)) { progress.SetStatus(L("Unsafe archive path.", "Ruta insegura en el archivo.")); return false; }
  }

  if (!CheckPaths(game_dir, user_dir, plan, false, progress)) return false;
  uint64_t total = 0;
  std::set<std::string> packages;
  for (const auto& [key, copy] : plan) {
    total += copy.size;
    if (copy.dest.rfind(kDlcPrefix, 0) == 0) {
      const std::string rest = copy.dest.substr(sizeof(kDlcPrefix) - 1);
      packages.insert(rest.substr(0, rest.find('/')));
    }
  }
  const auto space = fs::space(game_dir, ec);
  if (!ec && space.available < total + (1ull << 30)) {
    char text[160];
    std::snprintf(text, sizeof(text),
                  L("Not enough space: %.1f GB needed, %.1f GB free.", "No hay espacio suficiente: %.1f GB necesarios, %.1f GB libres."),
                  double(total) / 1e9, double(space.available) / 1e9);
    progress.SetStatus(text);
    return false;
  }
  progress.total = total;
  progress.done = 0;

  // Validate the replacement archive before touching an existing installation.
  if (LoadPlus(game_dir).installed && !UninstallPlus(game_dir, user_dir, progress)) return false;
  if (AreShadowsHidden(game_dir)) {
    const auto error = SetShadowsHidden(game_dir, false);
    if (!error.empty()) { progress.SetStatus(error); return false; }
  }
  fs::remove_all(game_dir / kModDir, ec);
  fs::create_directories(game_dir / kModDir, ec);
  PlusState state;
  state.installed = true;
  state.updated = update != nullptr;
  state.mode = mode;
  state.harder_ai = harder_ai;
  const auto fail = [&](const std::string& message) {
    if (!Restore(game_dir, user_dir, state, progress)) {
      WriteState(game_dir, state);
      progress.SetStatus(message + L(" Recovery incomplete; backups preserved.",
                                     " Recuperacion incompleta; copias conservadas."));
      return false;
    }
    progress.SetStatus(message);
    return false;
  };
  const auto cancelled = [] {
    return std::string(L("Installation cancelled; the game was left as it was.",
                         "Instalación cancelada; el juego se ha dejado como estaba."));
  };

  // The Ultimate Collection's content packages make way for the mod's (it
  // ships the same cars split into the original packs, with its own data).
  const fs::path dlc = DlcDir(user_dir);
  fs::create_directories(user_dir / kModDir / "dlc", ec);
  for (const auto& entry : fs::directory_iterator(dlc, ec)) {
    if (!entry.is_directory()) continue;
    const std::string name = entry.path().filename().u8string();
    fs::rename(entry.path(), user_dir / kModDir / "dlc" / fs::u8path(name), ec);
    if (ec) return fail(std::string(L("Could not set aside ", "No se pudo apartar ")) + name);
    state.dlc_moved.push_back(name);
  }
  state.dlc_added.assign(packages.begin(), packages.end());
  if (!WriteState(game_dir, state)) return fail(L("Could not save the mod state.", "No se pudo guardar el estado del mod."));

  for (const auto& [key, copy] : plan) {
    if (progress.cancel) return fail(cancelled());
    fs::path out;
    if (copy.dest.rfind(kDlcPrefix, 0) == 0) {
      out = dlc / fs::u8path(copy.dest.substr(sizeof(kDlcPrefix) - 1));
    } else {
      out = game_dir / fs::u8path(copy.dest);
      if (copy.dest.rfind(kModDir, 0) != 0) {
        if (fs::exists(out, ec)) {
          const fs::path backup = game_dir / kModDir / "backup" / fs::u8path(copy.dest);
          fs::create_directories(backup.parent_path(), ec);
          fs::copy_file(out, backup, fs::copy_options::overwrite_existing, ec);
          if (ec) return fail(std::string(L("Could not back up ", "No se pudo guardar una copia de ")) + copy.dest);
          state.replaced.push_back(copy.dest);
        } else {
          state.added.push_back(copy.dest);
        }
      }
    }
    if (!WriteState(game_dir, state)) return fail(L("Could not save the mod state.", "No se pudo guardar el estado del mod."));
    fs::create_directories(out.parent_path(), ec);
    progress.SetStatus(std::string(L("Installing ", "Instalando ")) + copy.dest);
    if (!copy.zip->Extract(copy.index, out, progress)) {
      return fail(progress.cancel ? cancelled() : std::string(L("Could not write ", "No se pudo escribir ")) + copy.dest);
    }
  }
  if (!WriteState(game_dir, state)) return fail(L("Could not save the mod state.", "No se pudo guardar el estado del mod."));
  const std::string error = SetPlusMode(game_dir, user_dir, mode, harder_ai);
  if (!error.empty()) return fail(error);
  progress.SetStatus(std::string(L("FM3 Plus installed: ", "FM3 Plus instalado: ")) +
                     std::to_string(state.added.size() + state.replaced.size()) +
                     L(" game files and ", " archivos del juego y ") + std::to_string(packages.size()) +
                     L(" content packages (originals kept).", " paquetes de contenido (originales guardados)."));
  return true;
}

bool UpdatePlus(const fs::path& game_dir, const fs::path& user_dir, const fs::path& update_zip,
                TaskProgress& progress) {
  PlusState state = LoadPlus(game_dir);
  if (!state.installed) {
    progress.SetStatus(L("Install FM3 Plus first.", "Instala antes FM3 Plus."));
    return false;
  }
  Zip update(update_zip);
  if (!update.ok()) {
    progress.SetStatus(L("Could not open the archive.", "No se pudo abrir el archivo comprimido."));
    return false;
  }
  const auto root = FindRoot(update, "Update Files/");
  if (!root) {
    progress.SetStatus(L("This is not the Project Forza Plus 3 update archive (no \"Update Files\" folder).",
                         "No es la actualización de Project Forza Plus 3 (falta la carpeta \"Update Files\")."));
    return false;
  }
  std::map<std::string, Copy> plan;
  if (!Plan(update, *root, "Update Files/Game Mode DBs/", std::string(kModDir) + "/modes/", plan)) { progress.SetStatus(L("Unsafe archive path.", "Ruta insegura en el archivo.")); return false; }
  if (!Plan(update, *root, "Update Files/Physics/", "Media/Physics/", plan)) { progress.SetStatus(L("Unsafe archive path.", "Ruta insegura en el archivo.")); return false; }
  if (!CheckPaths(game_dir, user_dir, plan, true, progress)) return false;
  uint64_t total = 0;
  for (const auto& [key, copy] : plan) total += copy.size;
  progress.total = total;
  progress.done = 0;

  // Everything is extracted next to its target first and only moved into
  // place once the whole archive came out, so a failure leaves no half update.
  std::error_code ec;
  std::vector<std::pair<fs::path, fs::path>> staged;  // temporary, final
  const auto discard = [&](const std::string& message) {
    for (const auto& [tmp, out] : staged) fs::remove(tmp, ec);
    progress.SetStatus(message);
    return false;
  };
  for (const auto& [key, copy] : plan) {
    if (progress.cancel) return discard(L("Update cancelled; nothing was changed.", "Actualización cancelada; no se ha cambiado nada."));
    const fs::path out = game_dir / fs::u8path(copy.dest);
    const fs::path tmp = fs::path(out).concat(".plus-new");
    fs::create_directories(out.parent_path(), ec);
    progress.SetStatus(std::string(L("Updating ", "Actualizando ")) + copy.dest);
    staged.emplace_back(tmp, out);
    if (!copy.zip->Extract(copy.index, tmp, progress)) {
      return discard(progress.cancel ? L("Update cancelled; nothing was changed.", "Actualización cancelada; no se ha cambiado nada.")
                                     : std::string(L("Could not write ", "No se pudo escribir ")) + copy.dest);
    }
  }
  for (const auto& [key, copy] : plan) {
    if (copy.dest.rfind(kModDir, 0) == 0) continue;
    const bool known = std::find(state.replaced.begin(), state.replaced.end(), copy.dest) != state.replaced.end() ||
                       std::find(state.added.begin(), state.added.end(), copy.dest) != state.added.end();
    if (known) continue;
    const fs::path out = game_dir / fs::u8path(copy.dest);
    if (fs::exists(out, ec)) {
      const fs::path backup = game_dir / kModDir / "backup" / fs::u8path(copy.dest);
      fs::create_directories(backup.parent_path(), ec);
      fs::copy_file(out, backup, fs::copy_options::overwrite_existing, ec);
      if (ec) return discard(std::string(L("Could not back up ", "No se pudo guardar una copia de ")) + copy.dest);
      state.replaced.push_back(copy.dest);
    } else {
      state.added.push_back(copy.dest);
    }
  }
  // Preserve the uninstall inventory before committing any replacement.
  if (!WriteState(game_dir, state)) return discard(L("Could not save the mod state.", "No se pudo guardar el estado del mod."));
  for (const auto& [tmp, out] : staged) {
    fs::rename(tmp, out, ec);
    if (ec) {
      progress.SetStatus(std::string(L("Could not replace ", "No se pudo sustituir ")) + out.u8string());
      return false;
    }
  }
  state.updated = true;
  if (!WriteState(game_dir, state)) {
    progress.SetStatus(L("Could not save the mod state.", "No se pudo guardar el estado del mod."));
    return false;
  }
  const std::string error = SetPlusMode(game_dir, user_dir, state.mode, state.harder_ai);
  if (!error.empty()) {
    progress.SetStatus(error);
    return false;
  }
  progress.SetStatus(L("FM3 Plus update applied.", "Actualización de FM3 Plus aplicada."));
  return true;
}

bool UninstallPlus(const fs::path& game_dir, const fs::path& user_dir, TaskProgress& progress) {
  const PlusState state = LoadPlus(game_dir);
  if (!state.installed) return true;
  progress.SetStatus(L("Removing FM3 Plus...", "Quitando FM3 Plus..."));
  if (!Restore(game_dir, user_dir, state, progress)) return false;
  progress.SetStatus(L("FM3 Plus removed; the original files and content are back.",
                       "FM3 Plus quitado; se han restaurado los archivos y el contenido originales."));
  return true;
}

std::string SetPlusMode(const fs::path& game_dir, const fs::path& user_dir, PlusMode mode, bool harder_ai) {
  PlusState state = LoadPlus(game_dir);
  if (!state.installed) return Missing();
  const fs::path dir = ModeDir(game_dir, mode);
  const fs::path db = harder_ai ? dir / "Harder AI" / "gamedb.slt" : dir / "gamedb.slt";
  std::error_code ec;
  fs::copy_file(db, game_dir / "Media" / "DB" / "gamedb.slt", fs::copy_options::overwrite_existing, ec);
  if (ec) return L("That mode's database is missing from the mod.", "Falta la base de datos de ese modo en el mod.");
  // Each mode also has its own databases inside the content packages.
  if (fs::exists(dir / "DLC DBs", ec) && !CopyTree(dir / "DLC DBs", DlcDir(user_dir))) {
    return L("Could not copy the mode's content databases.", "No se pudieron copiar las bases de datos del contenido.");
  }
  state.mode = mode;
  state.harder_ai = harder_ai;
  if (!WriteState(game_dir, state)) return L("Could not save the mod state.", "No se pudo guardar el estado del mod.");
  return {};
}

namespace {

constexpr char kCameraFile[] = "Media/Camera/CameraSettings.ini";
// Section of each PlusCamera in CameraSettings.ini.
constexpr const char* kCameraSections[kCameraCount] = {"FollowLowCam", "FollowHighCam", "HoodCam", "BumperHighCam"};

// "<Section>\FOV\FOV<n>" -> camera and band, or false.
bool FovKey(const std::string& key, int& camera, int& band) {
  for (int c = 0; c < kCameraCount; ++c) {
    const std::string prefix = std::string(kCameraSections[c]) + "\\FOV\\FOV";
    if (key.size() == prefix.size() + 1 && key.rfind(prefix, 0) == 0 && key.back() >= '0' && key.back() <= '2') {
      camera = c;
      band = key.back() - '0';
      return true;
    }
  }
  return false;
}

// The file is plain "<key> <value>" lines (CRLF); `edit` may rewrite a line.
template <typename Edit>
bool ForEachLine(const fs::path& file, Edit edit, std::string* rewritten) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    end = end == std::string::npos ? text.size() : end + 1;
    std::string line = text.substr(start, end - start);
    std::string eol;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
      eol.insert(eol.begin(), line.back());
      line.pop_back();
    }
    edit(line);
    if (rewritten) *rewritten += line + eol;
    start = end;
  }
  return true;
}

}  // namespace

bool ReadPlusFov(const fs::path& game_dir, bool original, PlusFov& out) {
  const fs::path backup = game_dir / kOriginalsDir / fs::u8path(kCameraFile);
  std::error_code ec;
  const fs::path file = original && fs::exists(backup, ec) ? backup : game_dir / fs::u8path(kCameraFile);
  return ForEachLine(
      file,
      [&](const std::string& line) {
        const auto space = line.find(' ');
        int camera, band;
        if (space != std::string::npos && FovKey(line.substr(0, space), camera, band)) {
          out.degrees[camera][band] = std::atoi(line.c_str() + space + 1);
        }
      },
      nullptr);
}

std::string SetPlusFov(const fs::path& game_dir, const PlusFov& fov) {
  const fs::path file = game_dir / fs::u8path(kCameraFile);
  const fs::path backup = game_dir / kOriginalsDir / fs::u8path(kCameraFile);
  std::error_code ec;
  if (!fs::exists(backup, ec)) {
    fs::create_directories(backup.parent_path(), ec);
    fs::copy_file(file, backup, ec);
    if (ec) return L("Could not back up CameraSettings.ini.", "No se pudo guardar una copia de CameraSettings.ini.");
  }
  std::string text;
  const bool read = ForEachLine(
      file,
      [&](std::string& line) {
        const auto space = line.find(' ');
        int camera, band;
        if (space != std::string::npos && FovKey(line.substr(0, space), camera, band)) {
          line = line.substr(0, space + 1) + std::to_string(fov.degrees[camera][band]);
        }
      },
      &text);
  if (!read) return L("Could not read CameraSettings.ini.", "No se pudo leer CameraSettings.ini.");
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!(out << text)) return L("Could not write CameraSettings.ini.", "No se pudo escribir CameraSettings.ini.");
  return {};
}

std::vector<ManifestEntry> WithoutPlusFiles(const std::vector<ManifestEntry>& manifest, const PlusState& state) {
  if (!state.installed) return manifest;
  std::set<std::string> replaced;
  for (const auto& p : state.replaced) replaced.insert(Lower(p));
  std::vector<ManifestEntry> out;
  for (const auto& entry : manifest) {
    if (!replaced.count(Lower(entry.path))) out.push_back(entry);
  }
  return out;
}

}  // namespace launcher
