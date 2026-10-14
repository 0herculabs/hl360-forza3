#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "installer.h"

namespace launcher {

// Project Forza Plus 3 ("FM3 Plus"): a data mod installed from its release
// archives ("Project Forza Plus 3 v1.3.zip" and the optional "Project Forza
// Plus 3 Update v1.3.zip").
//
// The mod targets the original 2009 release and ships that build's edited
// default.xex; this port is the Ultimate Collection, so the executable is left
// alone and the game re-signs the protected files it changes at startup
// (src/file_signatures.cpp). Installed:
//   Main Game Changes/Media      -> <game>/Media (originals kept)
//   Disc 2 and DLCs/00000002     -> <user>/0000000000000000/4D53084D/00000002
//                                   (the Ultimate Collection's own packs are
//                                   set aside: the mod ships them split up)
//   Game Mode DBs (+ update)     -> <game>/.fm3-plus/modes, one per career mode
//   Update Files/Physics         -> <game>/Media/Physics
//
// Everything needed to undo it lives in <game>/.fm3-plus (state.txt, backup/,
// modes/) and <user>/.fm3-plus (the set-aside packs).
enum class PlusMode { kNormal, kHard, kSandbox, kVanilla };

struct PlusState {
  bool installed = false;
  bool updated = false;
  PlusMode mode = PlusMode::kNormal;
  bool harder_ai = false;
  std::vector<std::string> replaced;  // game files it overwrote (originals in backup/)
  std::vector<std::string> added;     // game files it created
  std::vector<std::string> dlc_added;  // its content packages
  std::vector<std::string> dlc_moved;  // the Ultimate Collection's, set aside
};

PlusState LoadPlus(const std::filesystem::path& game_dir);

// Installs (or reinstalls) the mod. `update_zip` may be empty. Undoes its own
// changes if it fails or is cancelled.
bool InstallPlus(const std::filesystem::path& game_dir, const std::filesystem::path& user_dir,
                 const std::filesystem::path& base_zip, const std::filesystem::path& update_zip,
                 PlusMode mode, bool harder_ai, TaskProgress& progress);

// Applies the update archive over an installed mod.
bool UpdatePlus(const std::filesystem::path& game_dir, const std::filesystem::path& user_dir,
                const std::filesystem::path& update_zip, TaskProgress& progress);

// Restores the original files and content packages.
bool UninstallPlus(const std::filesystem::path& game_dir, const std::filesystem::path& user_dir,
                   TaskProgress& progress);

// Puts the chosen career mode in place: its gamedb.slt (or the harder-AI one)
// and its content-package databases. Returns an error message, or empty.
std::string SetPlusMode(const std::filesystem::path& game_dir, const std::filesystem::path& user_dir,
                        PlusMode mode, bool harder_ai);

// "Skip Intro" saves of a mode: a starter car and some credits, skipping the
// first race. Names as in the mod ("Golf GTI", "Civic Type R"...).
std::vector<std::string> ListSkipIntroSaves(const std::filesystem::path& game_dir, PlusMode mode);
// Copies one into the profile's save folder (<user>/<xuid>/4D53084D/00000001),
// moving the current career aside. Returns an error message, or empty.
std::string ApplySkipIntroSave(const std::filesystem::path& game_dir, const std::filesystem::path& user_dir,
                               PlusMode mode, const std::string& save, const std::string& xuid);

// Field of view of the four driving cameras in Media/Camera/CameraSettings.ini,
// in degrees per speed band: [0] under 100 mph, [1] 100-150 mph, [2] above.
enum PlusCamera { kChaseNear, kChaseFar, kHood, kBumper, kCameraCount };
struct PlusFov {
  int degrees[kCameraCount][3] = {};
};

// Reads the current values, or the game's own (`original`, from the backup
// once the file was changed). False if the file is missing.
bool ReadPlusFov(const std::filesystem::path& game_dir, bool original, PlusFov& out);
// Writes the values; the first change backs the original up. The game accepts
// the edited file thanks to the signature rebuild. Returns an error, or empty.
std::string SetPlusFov(const std::filesystem::path& game_dir, const PlusFov& fov);

// The manifest minus the files the mod replaced, for verification.
std::vector<ManifestEntry> WithoutPlusFiles(const std::vector<ManifestEntry>& manifest, const PlusState& state);

}  // namespace launcher
