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

// Adds every file under `<root><from>` to the plan as `<to><rest>`. Keyed
// case-insensitively, so the update archive overrides the base one.
void Plan(Zip& zip, const std::string& root, const std::string& from, const std::string& to,
          std::map<std::string, Copy>& plan) {
  const std::string prefix = root + from;
  mz_zip_archive_file_stat stat;
  for (mz_uint i = 0; i < zip.count(); ++i) {
    if (!zip.Stat(i, stat) || stat.m_is_directory) continue;
    const std::string name = stat.m_filename;
    if (Lower(name).rfind(Lower(prefix), 0) != 0) continue;
    const std::string dest = to + name.substr(prefix.size());
    plan[Lower(dest)] = {&zip, i, dest, stat.m_uncomp_size};
  }
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
    RemoveEmptyDirs(file.parent_path(), game_dir / "Media");
  }
  const fs::path dlc = DlcDir(user_dir);
  for (const auto& p : state.dlc_added) fs::remove_all(dlc / fs::u8path(p), ec);
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
  if (LoadPlus(game_dir).installed && !UninstallPlus(game_dir, user_dir, progress)) return false;
  // The mod brings its own track settings (shadows hidden among them).
  if (AreShadowsHidden(game_dir)) SetShadowsHidden(game_dir, false);

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
  Plan(base, *root, "Main Game Changes/Media/", "Media/", plan);
  Plan(base, *root, "Game Mode DBs/", std::string(kModDir) + "/modes/", plan);
  Plan(base, *root, "Disc 2 and DLCs/00000002/", kDlcPrefix, plan);
  if (update) {
    const auto update_root = FindRoot(*update, "Update Files/");
    if (!update_root) {
      progress.SetStatus(L("This is not the Project Forza Plus 3 update archive (no \"Update Files\" folder).",
                           "No es la actualización de Project Forza Plus 3 (falta la carpeta \"Update Files\")."));
      return false;
    }
    Plan(*update, *update_root, "Update Files/Game Mode DBs/", std::string(kModDir) + "/modes/", plan);
    Plan(*update, *update_root, "Update Files/Physics/", "Media/Physics/", plan);
  }

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

  fs::remove_all(game_dir / kModDir, ec);
  fs::create_directories(game_dir / kModDir, ec);
  PlusState state;
  state.installed = true;
  state.updated = update != nullptr;
  state.mode = mode;
  state.harder_ai = harder_ai;
  const auto fail = [&](const std::string& message) {
    Restore(game_dir, user_dir, state, progress);
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
  Plan(update, *root, "Update Files/Game Mode DBs/", std::string(kModDir) + "/modes/", plan);
  Plan(update, *root, "Update Files/Physics/", "Media/Physics/", plan);
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

std::vector<std::string> ListSkipIntroSaves(const fs::path& game_dir, PlusMode mode) {
  std::vector<std::string> out;
  std::error_code ec;
  // "Skip Intro Saves" (one per starter car), or "Skip Intro Save" in Sandbox.
  for (const char* folder : {"Skip Intro Saves", "Skip Intro Save"}) {
    const fs::path dir = ModeDir(game_dir, mode) / folder;
    if (fs::exists(dir / "ForzaProfile", ec)) out.push_back(folder);
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      if (!fs::exists(entry.path() / "ForzaProfile", ec)) continue;
      std::string name = entry.path().filename().u8string();
      if (name.rfind("Skip Intro ", 0) == 0) name = name.substr(11);
      out.push_back(name);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string ApplySkipIntroSave(const fs::path& game_dir, const fs::path& user_dir, PlusMode mode,
                               const std::string& save, const std::string& xuid) {
  if (!LoadPlus(game_dir).installed) return Missing();
  std::error_code ec;
  fs::path source;
  for (const char* folder : {"Skip Intro Saves", "Skip Intro Save"}) {
    const fs::path dir = ModeDir(game_dir, mode) / folder;
    if (save == folder && fs::exists(dir / "ForzaProfile", ec)) source = dir / "ForzaProfile";
    if (fs::exists(dir / fs::u8path("Skip Intro " + save) / "ForzaProfile", ec)) {
      source = dir / fs::u8path("Skip Intro " + save) / "ForzaProfile";
    }
  }
  if (source.empty()) return L("That save is not in the mod.", "Esa partida no está en el mod.");
  const fs::path saves = user_dir / fs::u8path(xuid) / "4D53084D" / "00000001";
  const fs::path target = saves / "ForzaProfile";
  if (fs::exists(target, ec)) {
    const fs::path aside = saves / fs::u8path("ForzaProfile.before-skip-intro-" + std::to_string(std::time(nullptr)));
    fs::rename(target, aside, ec);
    if (ec) return L("Could not set the current career aside.", "No se pudo apartar la trayectoria actual.");
  }
  fs::create_directories(target, ec);
  if (!CopyTree(source, target)) return L("Could not copy the save.", "No se pudo copiar la partida.");
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
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!read || !(out << text)) return L("Could not write CameraSettings.ini.", "No se pudo escribir CameraSettings.ini.");
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
