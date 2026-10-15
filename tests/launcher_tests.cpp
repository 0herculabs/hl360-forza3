#include "fm3_plus.h"
#include "safe_path.h"
#include "sha256.h"
#include "miniz.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace launcher;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  const auto root = fs::temp_directory_path() / ("fm3-tests-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root / "game");
  try {
    for (const char* path : {"../outside", "Media/../../outside", "C:/outside", "/outside",
                             "Media\\..\\outside", "file:stream", "Media/../x", "Media/x. ", "Media//x"})
      Check(!IsSafeRelativePath(path), "unsafe path accepted");
    Check(IsSafeRelativePath("Media/Physics/PhysicsSettings.ini"), "valid path rejected");
    Check(IsSafeRelativePath("Normal Mode/Harder AI/gamedb.slt"), "mod path rejected");
    Sha256 sha;
    sha.Update("abc", 3);
    Check(sha.HexDigest() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 known vector failed");

    // A recognised magic header followed by a missing root must fail safely.
    {
      std::ofstream iso(root / "truncated.iso", std::ios::binary);
      iso.seekp(32 * 2048);
      iso.write("MICROSOFT*XBOX*MEDIA", 20);
    }
    Check(IdentifyIso(root / "truncated.iso") == DiscKind::kUnknown, "truncated ISO accepted");

    std::ofstream(root / "game/default.xex") << "test";
    const auto archive = (root / "unsafe.zip").string();
    mz_zip_archive zip{};
    Check(mz_zip_writer_init_file(&zip, archive.c_str(), 0), "cannot create fixture");
    const char* entry = "Main Game Changes/Media/../../outside.txt";
    Check(mz_zip_writer_add_mem(&zip, entry, "bad", 3, 0), "cannot add fixture");
    Check(mz_zip_writer_finalize_archive(&zip), "cannot finish fixture");
    mz_zip_writer_end(&zip);
    TaskProgress progress;
    Check(!InstallPlus(root / "game", root / "user", root / "unsafe.zip", {},
                       PlusMode::kNormal, false, progress), "unsafe mod accepted");
    Check(!fs::exists(root / "outside.txt"), "archive escaped destination");

    const auto valid_archive = (root / "valid.zip").string();
    zip = {};
    Check(mz_zip_writer_init_file(&zip, valid_archive.c_str(), 0), "cannot create valid fixture");
    Check(mz_zip_writer_add_mem(&zip, "Main Game Changes/Media/DB/gamedb.slt", "mod", 3, 0), "cannot add database");
    Check(mz_zip_writer_add_mem(&zip, "Game Mode DBs/Normal Mode/gamedb.slt", "mod", 3, 0), "cannot add mode");
    Check(mz_zip_writer_finalize_archive(&zip), "cannot finish valid fixture");
    mz_zip_writer_end(&zip);
    fs::create_directories(root / "game/Media/DB");
    fs::create_directories(root / "user/0000000000000000/4D53084D/00000002");
    std::ofstream(root / "game/Media/DB/gamedb.slt") << "original";
    Check(InstallPlus(root / "game", root / "user", root / "valid.zip", {},
                      PlusMode::kNormal, false, progress), "valid mod install failed");
#if defined(_WIN32)
    // Ultimate Collection permits a 96-character user root; Plus has longer
    // package paths. Reject them before uninstalling a working mod.
    const auto long_archive = (root / "long.zip").string();
    zip = {};
    Check(mz_zip_writer_init_file(&zip, long_archive.c_str(), 0), "cannot create long-path fixture");
    Check(mz_zip_writer_add_mem(&zip, "Main Game Changes/Media/DB/gamedb.slt", "new", 3, 0), "cannot add fixture root");
    const std::string package_path = std::string(40, 'A') + "/Media/DLCZips/000A_pri_65/media/cars/" +
                                     std::string(35, 'x') + "/" + std::string(40, 'y') + "_build_report.html";
    const std::string entry_path = "Disc 2 and DLCs/00000002/" + package_path;
    Check(mz_zip_writer_add_mem(&zip, entry_path.c_str(), "html", 4, 0), "cannot add long-path fixture");
    Check(mz_zip_writer_finalize_archive(&zip), "cannot finish long-path fixture");
    mz_zip_writer_end(&zip);
    std::wstring long_user = (root / "user-").wstring();
    long_user.append(96 - long_user.size(), L'x');
    Check(UserDirFitsContent(fs::path(long_user)), "fixture exceeds Disc 2 root limit");
    Check(!InstallPlus(root / "game", fs::path(long_user), root / "long.zip", {},
                       PlusMode::kNormal, false, progress), "long mod path accepted");
    Check(progress.Status().find("path too long") != std::string::npos, "missing long-path diagnostic");
    Check(LoadPlus(root / "game").installed, "long path removed installed mod");
    Check(!fs::exists(fs::path(long_user)), "long-path failure modified user folder");
#endif
    Check(!InstallPlus(root / "game", root / "user", root / "unsafe.zip", {},
                       PlusMode::kNormal, false, progress), "unsafe replacement accepted");
    Check(LoadPlus(root / "game").installed, "invalid replacement removed installed mod");
    Check(UninstallPlus(root / "game", root / "user", progress), "mod uninstall failed");
    std::string restored;
    std::ifstream(root / "game/Media/DB/gamedb.slt") >> restored;
    Check(restored == "original", "original database not restored");

    fs::create_directories(root / "game/.fm3-plus");
    std::ofstream(root / "outside.txt") << "keep";
    std::ofstream(root / "game/.fm3-plus/state.txt") << "added ../outside.txt\n";
    Check(!UninstallPlus(root / "game", root / "user", progress), "unsafe state accepted");
    Check(fs::exists(root / "outside.txt"), "uninstall escaped destination");
    Check(fs::exists(root / "game/.fm3-plus/state.txt"), "recovery state lost");

    fs::remove_all(root);
    std::cout << "Launcher regression checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << " (fixtures: " << root << ")\n";
    return 1;
  }
}
