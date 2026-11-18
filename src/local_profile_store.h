#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fm3 {
struct LocalProfile {
  std::string name = "Player 2";
  uint64_t credits = 0, xp = 0, races = 0, wins = 0, last_award = 0;
  uint32_t level = 1, garage_id = 0, last_position = 0;
  uint32_t car_series_event = 0;
  std::map<uint32_t, uint32_t> series_points;
  uint32_t CarForSeries(uint32_t event) const {
    return event && event==car_series_event && garage_id && garage_id<0x7FFFFFFF ? garage_id : 0;
  }
};

// Independent of the emulated Xbox account and its save containers.
// The checksum catches truncated/partial files; it is not an authentication key.
class LocalProfileStore {
 public:
  ~LocalProfileStore() { if (lock_ != INVALID_HANDLE_VALUE) CloseHandle(lock_); }
  LocalProfileStore() = default;
  LocalProfileStore(const LocalProfileStore&) = delete;
  LocalProfileStore& operator=(const LocalProfileStore&) = delete;
  LocalProfile data;
  const std::filesystem::path& Path() const { return path_; }
  void Open(const std::filesystem::path& path, const std::string& name) {
    path_ = path;
    std::filesystem::create_directories(path.parent_path());
    lock_ = CreateFileW((path.wstring()+L".lock").c_str(), GENERIC_READ|GENERIC_WRITE,
                        0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Player 2 profile is already open or its folder is not writable.");
    if (std::filesystem::exists(path)) {
      if (std::filesystem::file_size(path) > 128*1024) throw std::runtime_error("Player 2 profile is too large.");
      std::ifstream file(path, std::ios::binary);
      std::string text((std::istreambuf_iterator<char>(file)), {});
      data = Decode(text);
    } else {
      data.name = name;
      Save(data);
    }
  }
  static uint64_t Hash(const std::string& text) {
    uint64_t h=14695981039346656037ull;
    for (unsigned char c:text) h=(h^c)*1099511628211ull;
    return h;
  }
  static std::string Encode(const LocalProfile& p) {
    Validate(p);
    std::ostringstream out;
    out << "FM3_LOCAL_PROFILE 2\n" << std::quoted(p.name) << '\n'
        << p.credits << ' ' << p.xp << ' ' << p.level << ' ' << p.races << ' ' << p.wins << '\n'
        << p.garage_id << ' ' << p.last_position << ' ' << p.last_award << '\n'
        << p.car_series_event << '\n'
        << p.series_points.size() << '\n';
    for (auto [event,points]:p.series_points) out << event << ' ' << points << '\n';
    const auto body=out.str();
    out << "CHECKSUM " << std::hex << Hash(body) << '\n';
    return out.str();
  }
  static LocalProfile Decode(const std::string& text) {
    const auto at=text.rfind("CHECKSUM ");
    if(at==std::string::npos) throw std::runtime_error("Player 2 profile is incomplete; existing file preserved.");
    uint64_t checksum=0;
    std::istringstream tail(text.substr(at+9));
    if(!(tail>>std::hex>>checksum) || (tail>>std::ws,!tail.eof()) || checksum!=Hash(text.substr(0,at)))
      throw std::runtime_error("Player 2 profile checksum failed; existing file preserved.");
    LocalProfile p;
    std::istringstream in(text.substr(0,at));
    std::string magic; unsigned version=0; size_t count=0;
    if(!(in>>magic>>version) || magic!="FM3_LOCAL_PROFILE" || (version!=1&&version!=2) ||
       !(in>>std::quoted(p.name)>>p.credits>>p.xp>>p.level>>p.races>>p.wins
            >>p.garage_id>>p.last_position>>p.last_award))
      throw std::runtime_error("Player 2 profile format is invalid; existing file preserved.");
    // Version 1 retains all earnings and points; its unscoped last car must
    // be chosen once before it can be remembered for a specific series.
    if((version==2&&!(in>>p.car_series_event)) || !(in>>count) || count>4096)
      throw std::runtime_error("Player 2 profile series data is invalid; existing file preserved.");
    for(size_t i=0;i<count;++i) {
      uint32_t event,points;
      if(!(in>>event>>points) || !p.series_points.emplace(event,points).second)
        throw std::runtime_error("Invalid Player 2 series record.");
    }
    if(in>>std::ws,!in.eof()) throw std::runtime_error("Unexpected Player 2 profile data.");
    Validate(p);
    return p;
  }
  void Save(const LocalProfile& next) {
    const auto text=Encode(next);
    const auto temp=path_.wstring()+L".tmp";
    HANDLE h=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot write Player 2 profile.");
    DWORD written=0;
    bool ok=WriteFile(h,text.data(),static_cast<DWORD>(text.size()),&written,nullptr) && written==text.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if(!ok) throw std::runtime_error("Could not flush Player 2 profile; previous file preserved.");
    if(std::filesystem::exists(path_)) {
      const auto backup=path_.wstring()+L".bak";
      ok=ReplaceFileW(path_.c_str(),temp.c_str(),backup.c_str(),0,nullptr,nullptr)!=0;
    } else ok=MoveFileExW(temp.c_str(),path_.c_str(),MOVEFILE_WRITE_THROUGH)!=0;
    if(!ok) throw std::runtime_error("Could not replace Player 2 profile; previous file preserved.");
    data=next;
  }
  bool Award(uint64_t id,uint32_t credits,uint32_t xp,uint32_t level,uint32_t position) {
    if(id==data.last_award) return false;
    if(!id || !position || position>8 || credits>10000000 || xp>10000000)
      throw std::runtime_error("Invalid Player 2 race award.");
    auto next=data;
    next.last_award=id; next.credits+=credits; next.xp=xp; next.level=level;
    ++next.races; next.wins+=position==1; next.last_position=position;
    Save(next); return true;
  }
 private:
  static void Validate(const LocalProfile& p) {
    if(p.name.empty() || p.name.size()>15 || p.name.find_first_of("\r\n\t")!=std::string::npos ||
       p.credits>1000000000000ull || p.xp>10000000 || !p.level || p.level>1000 ||
       p.races>1000000000 || p.wins>p.races || p.last_position>8 || p.series_points.size()>4096 ||
       (p.car_series_event && (p.car_series_event==0xFFFFFFFF || !p.garage_id || p.garage_id>=0x7FFFFFFF)))
      throw std::runtime_error("Invalid Player 2 profile values.");
    for(auto [event,points]:p.series_points) if(!event || points>65535)
      throw std::runtime_error("Invalid Player 2 series points.");
  }
  std::filesystem::path path_;
  HANDLE lock_=INVALID_HANDLE_VALUE;
};
}
