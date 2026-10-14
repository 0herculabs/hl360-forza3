// Protected file signatures.
//
// FM3 checks 27 of its data files (gamedb.slt, PhysicsSettings.ini,
// GameTunableSettings.ini, CameraSettings.ini, profile schemas, string
// tables...) against a table that the XEX maps at 0x82CF0000, and stops with a
// disc read error when one differs. Mods such as Project Forza Plus 3 change
// those files and ship an edited executable - for the original 2009 release,
// not the Ultimate Collection this port is built from. Instead, the table is
// recomputed here from the files on disk before the game starts, so modified
// files are accepted. With the original files the result is identical.
//
// Layout (big-endian):
//   u32 block_size (0x8000), u32 count
//   count x { u32 offset, u32 file_size }   offsets from the end of this index
//   per file: name ("db\gamedb.slt", relative to Media, NUL-terminated),
//             then a SHA-1 of every block_size block of the file.

#include "file_signatures.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(resign_protected_files, true, "FM3",
                    "Recompute the protected-file signature table from the files on disk "
                    "(lets modified data files such as Project Forza Plus 3 load)");

namespace fm3 {
namespace {

constexpr uint32_t kTableAddress = 0x82CF0000;
constexpr uint32_t kBlockSize = 0x8000;
constexpr uint32_t kMaxEntries = 64;
constexpr size_t kHashSize = 20;

uint32_t LoadBe32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void StoreBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

bool Sha1(const uint8_t* data, size_t size, uint8_t out[kHashSize]) {
  return BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, const_cast<uint8_t*>(data),
                                   ULONG(size), out, kHashSize));
}

struct Entry {
  std::string name;
  uint32_t size;
};

}  // namespace

void ResignProtectedFiles(rex::Runtime* runtime, const std::filesystem::path& game_root) {
  if (!REXCVAR_GET(resign_protected_files)) {
    return;
  }
  uint8_t* table = runtime->memory()->TranslateVirtual(kTableAddress);
  if (!table || LoadBe32(table) != kBlockSize) {
    REXLOG_WARN("File signatures: no table at {:08X}; leaving it alone", kTableAddress);
    return;
  }
  const uint32_t count = LoadBe32(table + 4);
  if (!count || count > kMaxEntries) {
    REXLOG_WARN("File signatures: unexpected entry count {}", count);
    return;
  }
  const uint32_t data_start = 8 + count * 8;

  // Read the original index and the space it occupies.
  std::vector<Entry> entries;
  uint32_t table_end = data_start;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t offset = LoadBe32(table + 8 + i * 8);
    const uint32_t size = LoadBe32(table + 12 + i * 8);
    const char* name = reinterpret_cast<const char*>(table + data_start + offset);
    const size_t name_len = strnlen(name, 128);
    if (name_len == 0 || name_len == 128) {
      REXLOG_WARN("File signatures: malformed entry {}", i);
      return;
    }
    const uint32_t blocks = (size + kBlockSize - 1) / kBlockSize;
    table_end = std::max<uint32_t>(table_end,
                                   data_start + offset + uint32_t(name_len) + 1 + blocks * kHashSize);
    entries.push_back({std::string(name, name_len), size});
  }

  // Rebuild it from the files on disk.
  std::vector<uint8_t> rebuilt(data_start, 0);
  StoreBe32(rebuilt.data(), kBlockSize);
  StoreBe32(rebuilt.data() + 4, count);
  int changed = 0;
  for (uint32_t i = 0; i < count; ++i) {
    std::string rel = entries[i].name;
    for (char& c : rel) c = c == '\\' ? '/' : c;
    const std::filesystem::path path = game_root / "Media" / std::filesystem::u8path(rel);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      REXLOG_WARN("File signatures: {} not found; table left as shipped", path.string());
      return;
    }
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const uint32_t offset = uint32_t(rebuilt.size() - data_start);
    StoreBe32(rebuilt.data() + 8 + i * 8, offset);
    StoreBe32(rebuilt.data() + 12 + i * 8, uint32_t(data.size()));
    rebuilt.insert(rebuilt.end(), entries[i].name.begin(), entries[i].name.end());
    rebuilt.push_back(0);
    for (size_t pos = 0; pos < data.size() || (data.empty() && pos == 0); pos += kBlockSize) {
      uint8_t hash[kHashSize];
      const size_t len = std::min<size_t>(kBlockSize, data.size() - pos);
      if (!Sha1(reinterpret_cast<const uint8_t*>(data.data()) + pos, len, hash)) {
        REXLOG_WARN("File signatures: SHA-1 failed");
        return;
      }
      rebuilt.insert(rebuilt.end(), hash, hash + kHashSize);
      if (data.empty()) break;
    }
    if (data.size() != entries[i].size) ++changed;
  }

  if (rebuilt.size() > table_end) {
    REXLOG_ERROR(
        "File signatures: modified files need {} bytes of signatures, the table has {}; the game "
        "will reject them",
        rebuilt.size(), table_end);
    return;
  }
  if (std::memcmp(rebuilt.data(), table, rebuilt.size()) == 0) {
    REXLOG_INFO("File signatures: all {} protected files match the shipped table", count);
    return;
  }
  // The table lives in a read-only resource section: open it for writing.
  auto* heap = runtime->memory()->LookupHeap(kTableAddress);
  uint32_t old_protect = 0;
  if (!heap || !heap->Protect(kTableAddress, table_end,
                              rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                              &old_protect)) {
    REXLOG_ERROR("File signatures: could not make the table writable");
    return;
  }
  std::memcpy(table, rebuilt.data(), rebuilt.size());
  std::memset(table + rebuilt.size(), 0, table_end - rebuilt.size());
  heap->Protect(kTableAddress, table_end, old_protect);
  REXLOG_INFO("File signatures: table rebuilt from the files on disk ({} of {} changed size)",
              changed, count);
}

}  // namespace fm3
