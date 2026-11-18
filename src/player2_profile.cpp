#include <string_view>
#include "player2_profile.h"
#include "local_profile_store.h"
#include "player2_car_rules.h"
#include <algorithm>
#include <atomic>
#include <thread>
#include <bit>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(player2_local_profile, false, "FM3/Profile", "Player two local Career progress and eligible shared-garage car picker");
REXCVAR_DECLARE(bool, player2_real_profile);
REXCVAR_DEFINE_INT32(player2_starting_credits, 0, "FM3/Research",
    "Credits given once to a brand-new second profile (0 credits, empty garage); 0 = none");
REXCVAR_DEFINE_STRING(player2_profile_name, "Player 2", "FM3/Profile", "Name used when creating the local player two profile");
REXCVAR_DEFINE_BOOL(player2_native_car_menu, false, "FM3/Profile", "Hand the Career car-selection screen to Player 2 after Player 1 chooses");

extern "C" void __imp__sub_823D8F90(PPCContext&,uint8_t*);

extern "C" void __imp__sub_822F6388(PPCContext&,uint8_t*);
extern "C" void __imp__sub_822714D0(PPCContext&,uint8_t*);
extern "C" void __imp__sub_82482188(PPCContext&,uint8_t*);
extern "C" void __imp__sub_82231A28(PPCContext&,uint8_t*);
// True while the native reward commit runs for Player 2. The commit posts
// global UI events (new reward car 822C0B60, credits instead 822C0790, level
// change 822C07C8) that Player 1's results flow would act on as its own.
static bool player2_commit_active=false;
namespace fm3 {
namespace {
LocalProfileStore store;
std::mutex mutex;
bool ready=false;
std::string failure;
struct Pending { uint32_t component=0,player=0,credits=0,xp=0,position=0; uint64_t id=0; bool committed=false; } pending;
uint32_t career_player=0;
uint64_t sequence=0;
uint32_t R(uint8_t* b,uint32_t p) { uint32_t v; std::memcpy(&v,b+p,4); return std::byteswap(v); }
void W(uint8_t* b,uint32_t p,uint32_t v) { v=std::byteswap(v); std::memcpy(b+p,&v,4); }
bool Ptr(uint32_t p) { return p>=0x10000 && p<0xFFFF0000; }
uint32_t Player(uint8_t* b,unsigned index) {
  if(R(b,0x82C231F4)!=0x82001CDC || R(b,0x82C23220)!=2) return 0;
  const auto sentinel=R(b,0x82C2321C);
  auto node=R(b,sentinel);
  for(unsigned i=0;i<index && node!=sentinel;++i) node=R(b,node);
  return node==sentinel?0:R(b,node+8);
}
// Whose garage Player 2 chooses from: their own once a real save volume is
// mounted on their profile holder ([CPlayer+0xAC] -> +0), otherwise Player 1's.
bool Player2OwnsGarage(uint8_t* b) {
  const auto second=Player(b,1);
  if(!REXCVAR_GET(player2_real_profile)||!second) return false;
  const auto holder=R(b,second+0xAC);
  return Ptr(holder)&&Ptr(R(b,holder));
}
uint32_t GarageOwner(uint8_t* b) {return Player2OwnsGarage(b)?Player(b,1):Player(b,0);}
std::string Text(uint8_t* b,uint32_t p,size_t limit=4096) {
  if(!Ptr(p)) throw std::runtime_error("Invalid game text pointer.");
  size_t n=0; while(n<limit && b[p+n]) ++n;
  if(n==limit) throw std::runtime_error("Game text exceeded its limit.");
  return {reinterpret_cast<char*>(b+p),n};
}
std::wstring Wide(const std::string& s) {
  const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);
  std::wstring w(n,L' ');
  if(n) MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),w.data(),n);
  return w;
}
// All native calls use a private context and a separate frame below scratch.
// They run on the existing guest thread, never from a host UI callback.
struct Guest {
  PPCContext ctx; uint8_t* b; uint32_t scratch;
  Guest(const PPCContext& c,uint8_t* base):ctx(c),b(base),scratch(c.r1.u32-0x3000) {
    ctx.r1.u64=scratch-0x100; std::memset(b+scratch,0,0x3000);
  }
  uint32_t Call(uint32_t fn,uint32_t a=0,uint32_t d=0,uint32_t e=0,uint32_t f=0,uint32_t g=0) {
    ctx.r3.u64=a;ctx.r4.u64=d;ctx.r5.u64=e;ctx.r6.u64=f;ctx.r7.u64=g;
    rex::runtime::ResolveIndirectFunction(fn)(ctx,b); return ctx.r3.u32;
  }
  uint32_t V(uint32_t obj,uint32_t slot,uint32_t a=0) {
    if(!Ptr(obj)||!Ptr(R(b,obj))) throw std::runtime_error("Invalid game object.");
    return Call(R(b,R(b,obj)+slot),obj,a);
  }
  std::string S(uint32_t p) { return Text(b,R(b,p+0x18)<16?p+4:R(b,p+4)); }
  void Put(uint32_t p,const std::string& s) {
    if(s.size()>0x1FFF) throw std::runtime_error("Car selection query exceeded its limit.");
    std::memcpy(b+p,s.c_str(),s.size()+1);
  }
  uint32_t Query(const std::string& sql) {
    Put(scratch+0x800,sql); W(b,scratch+0x100,0);
    auto db=R(b,R(b,0x82C2111C)+0xC);
    Call(R(b,R(b,db)+0x34),scratch+0x100,db,scratch+0x800);
    auto result=R(b,scratch+0x100);
    if(!Ptr(result)) {
      REXLOG_ERROR("PLAYER2_PROFILE query rejected by the game database: length={} sql={}",sql.size(),sql);
      throw std::runtime_error("Game garage query failed.");
    }
    return result;
  }
  // Index access avoids SQLite's qualified-name settings (Garage.Id vs Id).
  uint32_t Int(uint32_t row,uint32_t column) { return V(row,0x78,column); }
  // RecordSet::GetString returns const std::string&, not const char*.
  // 8277A678 returns variant+0x10, with inline/heap storage at +4/+0x18.
  std::string String(uint32_t row,uint32_t column) { return S(V(row,0x68,column)); }
  std::wstring LocalizedColumn(uint32_t row,uint32_t column) {
    // Database IDs encode a string-table selector and a 16-bit entry. Use
    // RecordSet's own localized getter, as the native car-selection UI does.
    // A LiveStrings hash lookup is a different interface and cannot resolve it.
    W(b,scratch+0x200,0);
    Call(R(b,R(b,row)+0x70),scratch+0x200,row,column);
    auto obj=R(b,scratch+0x200); auto p=V(obj,0x14);
    std::wstring text;
    if(Ptr(p)) for(unsigned i=0;i<256;++i) {
      wchar_t ch=static_cast<wchar_t>((b[p+i*2]<<8)|b[p+i*2+1]);
      if(!ch) break; text.push_back(ch);
    }
    V(obj,8);
    if(text.empty()) throw std::runtime_error("The game could not resolve this car's display name.");
    return text;
  }
};
using Eligibility=Player2CarRules;
Eligibility Eligible(Guest& g,uint32_t event,uint32_t user) {
  g.Call(0x822667E0,g.scratch);g.Call(0x82284648,event,g.scratch);
  g.Call(0x822D5C30,event);g.Call(0x822DC850,g.scratch);
  g.Call(0x822DCBD8,g.scratch+0x40,g.scratch,1,0);
  Eligibility e;e.predicate=g.S(g.scratch+0x40);
  g.Call(0x821F1560,g.scratch+0x40,1,0);g.Call(0x82751188,g.scratch);
  e.view=Text(g.b,g.Call(0x8227E198,user));
  if(e.view.empty()||e.view.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_")!=std::string::npos)
    throw std::runtime_error("Unexpected shared garage view name.");
  return e;
}
// This state belongs to the guest UI thread. A confirmed selection is keyed
// to both the event and the live P2 object and rechecked at race setup.
struct NativePicker {
  uint32_t screen=0,event=0,player=0,input=0,previous_input=0,garage=0;
  uint32_t host_controller=0xFFFFFFFF;
  std::string host_query,player2_query;
  uint32_t host_query_mode=0,reapplied=0;
  bool settled=false,use_host=false;
  std::chrono::steady_clock::time_point handed{};
  std::chrono::steady_clock::time_point checked{};
  bool active=false,reloading=false,confirmed=false;
  std::chrono::steady_clock::time_point opened{};
} native;
struct DirectPickerRequest {uint32_t event=0,player=0,screen=0;} direct_picker;
// Whether the last Career screen activated was the race-info screen (as
// opposed to the season calendar): Change Car lives there.
bool career_parent_is_race_info=false;
void NativeTitle(Guest& g,uint32_t screen,const std::wstring& text) {
  if(text.size()>200) return;
  W(g.b,g.scratch+0x400,0);
  for(size_t i=0;i<=text.size();++i) {
    const auto ch=i<text.size()?text[i]:0;
    g.b[g.scratch+0x500+i*2]=static_cast<uint8_t>(ch>>8);
    g.b[g.scratch+0x501+i*2]=static_cast<uint8_t>(ch);
  }
  g.Call(0x8229F7F0,g.scratch+0x400,g.scratch+0x500,0xFFFFFFFF);
  const auto header=g.V(screen,0x68);
  // SetTitle consumes the string reference, as in 823D9648.
  g.Call(0x8242EE70,header,g.scratch+0x400);
}
// The credit readout in the menu footer lives on CBackgroundMain (8231A3F0)
// and is refreshed by 8233DB08(background, player).
void ShowCreditsOf(Guest& g,uint32_t player) {
  try {
    const auto background=g.Call(0x8231A3F0,R(g.b,0x82C37720));
    if(Ptr(background)&&Ptr(player)) g.Call(0x8233DB08,background,player);
  } catch(const std::exception&) {}
}
void RestoreHostMenu(Guest& g) {
  ShowCreditsOf(g,Player(g.b,0));
  if(native.previous_input && native.host_controller<4) {
    const auto ui=g.Call(0x827017E8);
    g.V(ui,0x48,native.previous_input);
    // The UI owner alone does not choose the Career profile. 8226A510
    // resolves it from InputSystem::lastActiveController (827F4B38).
    // Mirror the native split-screen exit at 823E1068 before opening Career.
    const auto input_system=R(g.b,0x82C21118);
    g.b[input_system+0x6E78]=static_cast<uint8_t>(native.host_controller);
  }
}
void RestoreNativeInput(Guest& g) {
  if(native.active) RestoreHostMenu(g);
  native.active=false;
}
struct Car { uint32_t id; std::wstring label; uint32_t car_id=0; bool stock=false; };
struct Picker { std::vector<Car> cars; std::wstring summary; uint32_t preferred=0,selected=0; HWND list=nullptr; };
LRESULT CALLBACK PickerProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
  auto* p=reinterpret_cast<Picker*>(GetWindowLongPtrW(window,GWLP_USERDATA));
  if(message==WM_NCCREATE) {
    p=static_cast<Picker*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(p));
  }
  if(message==WM_CREATE) {
    const auto font=GetStockObject(DEFAULT_GUI_FONT);
    auto control=[&](const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id) {
      auto c=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
      SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE); return c;
    };
    control(L"STATIC",p->summary.c_str(),0,20,16,650,65,0);
    control(L"STATIC",L"Choose a car for Player 2. These shared-garage cars meet this event's rules.",0,20,87,650,32,0);
    p->list=control(L"LISTBOX",L"",WS_BORDER|WS_VSCROLL|WS_TABSTOP|LBS_NOTIFY,20,122,650,225,101);
    int selected=0;
    for(size_t i=0;i<p->cars.size();++i) {
      SendMessageW(p->list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(p->cars[i].label.c_str()));
      if(p->cars[i].id==p->preferred) selected=static_cast<int>(i);
    }
    SendMessageW(p->list,LB_SETCURSEL,selected,0);
    control(L"BUTTON",L"Use selected car",WS_TABSTOP|BS_DEFPUSHBUTTON,470,364,200,36,IDOK);
    control(L"STATIC",L"Mouse / arrow keys + Enter. Closing uses Player 1's eligible car.",0,20,407,650,30,0);
    SetFocus(p->list); return 0;
  }
  if(message==WM_COMMAND && p && (LOWORD(wp)==IDOK || (LOWORD(wp)==101 && HIWORD(wp)==LBN_DBLCLK))) {
    const auto i=SendMessageW(p->list,LB_GETCURSEL,0,0);
    if(i>=0 && static_cast<size_t>(i)<p->cars.size()) { p->selected=p->cars[i].id; DestroyWindow(window); }
    return 0;
  }
  if(message==WM_CLOSE) { DestroyWindow(window); return 0; }
  return DefWindowProcW(window,message,wp,lp);
}
uint32_t Choose(Picker& p) {
  static std::once_flag registered;
  std::call_once(registered,[]{WNDCLASSW wc{};wc.lpfnWndProc=PickerProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"FM3LocalPlayerPicker";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassW(&wc);});
  const auto x=std::max(0,(GetSystemMetrics(SM_CXSCREEN)-710)/2),y=std::max(0,(GetSystemMetrics(SM_CYSCREEN)-490)/2);
  auto window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_CONTROLPARENT,L"FM3LocalPlayerPicker",L"Player 2 - Career profile and car",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_VISIBLE,x,y,710,490,nullptr,nullptr,GetModuleHandleW(nullptr),&p);
  if(!window) throw std::runtime_error("Could not open player two car selection.");
  SetForegroundWindow(window);
  MSG msg{};
  while(IsWindow(window) && GetMessageW(&msg,nullptr,0,0)>0) {
    if(msg.message==WM_KEYDOWN && msg.wParam==VK_RETURN) { SendMessageW(window,WM_COMMAND,IDOK,0); continue; }
    if(!IsDialogMessageW(window,&msg)) {TranslateMessage(&msg);DispatchMessageW(&msg);}
  }
  return p.selected;
}
void Error(const std::string& text) {
  REXLOG_ERROR("PLAYER2_PROFILE {}",text);
  // Never block the game thread: the box can open behind the full-screen
  // views, which looked like a locked-up game (fm3_114).
  std::thread([wide=Wide(text)]{MessageBoxW(nullptr,wide.c_str(),L"Player 2 local profile",MB_OK|MB_ICONWARNING|MB_TOPMOST|MB_SETFOREGROUND);}).detach();
}
}

void InitPlayer2Profile(const std::filesystem::path& root,uint64_t host_xuid) {
  if(!REXCVAR_GET(player2_local_profile)) return;
  std::lock_guard lock(mutex);
  try {
    std::ostringstream folder; folder<<std::hex<<std::uppercase<<std::setw(16)<<std::setfill('0')<<host_xuid;
    store.Open(root/"local_profiles"/folder.str()/"player2.profile",REXCVAR_GET(player2_profile_name));
    FILETIME time{};GetSystemTimeAsFileTime(&time);sequence=(uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime;
    ready=true;
    REXLOG_WARN("PLAYER2_PROFILE loaded name={} credits={} xp={} races={} path={}",store.data.name,store.data.credits,store.data.xp,store.data.races,(root/"local_profiles"/folder.str()).string());
  } catch(const std::exception& e) {failure=e.what();REXLOG_ERROR("PLAYER2_PROFILE disabled: {}",failure);}
}
std::string Player2Name() {std::lock_guard lock(mutex);return ready?store.data.name:"Player 2";}
void SavePlayer2Points(uint32_t event,uint32_t points) {
  std::lock_guard lock(mutex);
  if(!ready||!career_player) return;
  try {auto next=store.data;next.series_points[event]=points;store.Save(next);}
  catch(const std::exception& e) {failure=e.what();REXLOG_ERROR("PLAYER2_PROFILE points save failed: {}",failure);}
}

void NativeCarMenuOpened(PPCContext& ctx,uint8_t* base,uint32_t screen) {
  if(!REXCVAR_GET(player2_native_car_menu)||!Player(base,1)||R(base,screen+0x264)==0xFFFFFFFF) return;
  Guest g(ctx,base);RestoreNativeInput(g);native={};
  NativeTitle(g,screen,L"PLAYER 1 - CHOOSE YOUR CAR");
  REXLOG_WARN("PLAYER2_NATIVE host car menu screen={:08X} event={}",screen,R(base,screen+0x264));
  // The derived init runs inside the base car-screen init. Defer the list
  // reload until the first input tick, after all list bindings exist.
  if(direct_picker.player==Player(base,1)&&direct_picker.event==R(base,screen+0x264))
    direct_picker.screen=screen;
  else direct_picker={};
  // Change Car pressed by Player 2's own controller on the race-info screen:
  // go straight to Player 2's list instead of Player 1's first.
  if(!direct_picker.screen&&career_parent_is_race_info) {
    try {
      const auto second=Player(base,1);
      const auto controller=g.Call(0x82B28530,g.V(second,0x44));
      const auto last=base[R(base,0x82C21118)+0x6E78];
      REXLOG_WARN("PLAYER2_NATIVE car menu opened from race info: last controller={} player 2 controller={}",last,controller);
      if(controller<4&&last==controller) direct_picker={R(base,screen+0x264),second,screen};
    } catch(const std::exception& e) {REXLOG_WARN("PLAYER2_NATIVE change-car owner check failed: {}",e.what());}
  }
}
bool BeginNativePlayer2(PPCContext& ctx,uint8_t* base) {
  const auto screen=ctx.r3.u32,player=Player(base,1);
  if(!REXCVAR_GET(player2_native_car_menu)||!REXCVAR_GET(player2_local_profile)||!player||R(base,screen+0x264)==0xFFFFFFFF) return false;
  if(native.active && native.screen==screen) return true;
  Guest g(ctx,base);
  try {
    const auto event=R(base,screen+0x264),user=g.V(GarageOwner(base),0x50);
    const auto e=Eligible(g,event,user);
    // Keep the game's thumbnails, car details and list navigation, with only
    // cars that already meet this event's rules in the shared garage.
    const auto sql=e.NativeQuery();
    const auto check=g.Query(e.Select("Garage.Id")+" LIMIT 1");
    const bool empty=g.V(check,0x98)!=0;g.V(check,8);
    if(empty) {REXLOG_WARN("PLAYER2_NATIVE no eligible Player 2 car for event={} (view {} rule {}); no handoff",event,e.view,e.predicate);return false;} // Existing loan-car/fallback picker handles this.
    const auto profile=g.V(player,0x44),controller=g.Call(0x82B28530,profile);
    if(controller>=4) throw std::runtime_error("Player 2's controller assignment is invalid.");
    const auto input=R(base,0x82C21118)+0x10+controller*0x1610;
    const auto ui=g.Call(0x827017E8),previous=g.V(ui,0x44);
    const auto host_controller=g.Call(0x82B28530,g.V(Player(base,0),0x44));
    if(host_controller>=4 || previous!=R(base,0x82C21118)+0x10+host_controller*0x1610)
      throw std::runtime_error("Career menu is not owned by Player 1's controller.");
    const auto source=R(base,screen+0x258);
    const auto host_query=g.S(source+0x9C);
    if(host_query.empty()||host_query.size()>0x1FFF)
      throw std::runtime_error("Could not preserve Player 1's car list for Back.");
    native={};native.screen=screen;native.event=event;native.player=player;
    native.host_controller=host_controller;
    native.host_query=host_query;native.host_query_mode=R(base,source+0xB8);
    native.input=input;native.previous_input=previous;native.active=true;native.reloading=true;
    native.player2_query=sql;
    g.Put(g.scratch+0x800,sql);
    g.Call(0x82446C50,R(base,screen+0x258),g.scratch+0x800,0);
    // Title first as well: the header seems to pick the text up on a rebuild
    // (fm3_084: Player 2's list still showed the Player 1 title).
    NativeTitle(g,screen,L"PLAYER 2 - CHOOSE YOUR CAR  (Y: USE PLAYER 1'S CAR)");
    // Rebuild the current list using its existing bindings.
    g.Call(0x823516C8,screen);
    ShowCreditsOf(g,player);
    try {
      const auto rows=g.Query(e.Select("Garage.Id, Garage.CarId, "+e.UseStock())+" ORDER BY Garage.Id LIMIT 12");
      std::string list;
      for(unsigned n=0;n<12&&!g.V(rows,0x98);++n) {
        list+=fmt::format(" [garage {} car {} stock {}]",g.Int(rows,0),g.Int(rows,1),g.Int(rows,2));
        g.V(rows,0x9C,0);
      }
      g.V(rows,8);
      REXLOG_WARN("PLAYER2_NATIVE Player 2 list from {} (own garage {}):{}",e.view,Player2OwnsGarage(base),list);
    } catch(const std::exception& x) {REXLOG_WARN("PLAYER2_NATIVE list probe failed: {}",x.what());}
    NativeTitle(g,screen,L"PLAYER 2 - CHOOSE YOUR CAR  (Y: USE PLAYER 1'S CAR)");
    g.V(ui,0x48,input);
    native.reloading=false;native.opened=std::chrono::steady_clock::now();native.handed=native.opened;
    REXLOG_WARN("PLAYER2_NATIVE handoff screen={:08X} event={} controller={} input={:08X} previous={:08X}",screen,event,controller,input,previous);
    return true;
  } catch(const std::exception& e) {
    RestoreNativeInput(g);native={};
    REXLOG_ERROR("PLAYER2_NATIVE handoff failed: {}; using race-start picker",e.what());
    return false;
  }
}
bool StartRaceNeedsPlayer2(PPCContext& ctx,uint8_t* base) {
  if(ctx.r4.u32!=0||!REXCVAR_GET(player2_native_car_menu)||!REXCVAR_GET(player2_local_profile)) return false;
  const auto player=Player(base,1),screen=ctx.r3.u32;
  if(!player) {direct_picker={};return false;}
  try {
    Guest g(ctx,base);
    const auto event=R(base,g.Call(0x82362B90,screen)+0x2A0);
    if(event==0xFFFFFFFF||(native.confirmed&&native.event==event&&native.player==player)) return false;
    uint32_t remembered=0;
    {std::lock_guard lock(mutex);if(ready) remembered=store.data.CarForSeries(event);}
    if(remembered) {
      // A sold car or changed upgrades may make yesterday's choice invalid.
      const auto e=Eligible(g,event,g.V(GarageOwner(base),0x50));
      const auto row=g.Query(e.Select("Garage.Id"," AND Garage.Id="+std::to_string(remembered)));
      const bool valid=!g.V(row,0x98);g.V(row,8);
      if(valid) {
        direct_picker={};
        REXLOG_WARN("PLAYER2_PROFILE keeping series car event={} garage={}",event,remembered);
        return false;
      }
      REXLOG_WARN("PLAYER2_PROFILE remembered series car no longer eligible event={} garage={}; opening picker",event,remembered);
    }
    // fm3_113: with no eligible car in Player 2's garage the car screen can
    // never hand over, so Start Race looped back to this menu forever. Let
    // the race start; race setup gives Player 2 a copy of Player 1's car.
    {
      const auto e=Eligible(g,event,g.V(GarageOwner(base),0x50));
      const auto check=g.Query(e.Select("Garage.Id")+" LIMIT 1");
      const bool empty=g.V(check,0x98)!=0;g.V(check,8);
      if(empty) {
        direct_picker={};
        REXLOG_WARN("PLAYER2_NATIVE Player 2 has no eligible car for event={} (view {} rule {}); starting with a copy of Player 1's car",event,e.view,e.predicate);
        return false;
      }
    }
    // CRaceInfo's own Change Car action (823D7F58) opens screen 0x92.
    // Use the same transition rather than opening a UI during race setup.
    const auto ui=g.Call(0x827017E8);
    direct_picker={event,player,0};
    g.Call(R(base,R(base,ui)+0x40),ui,R(base,screen+0x10C),0x92,0,0xFFFFFFFF);
    REXLOG_WARN("PLAYER2_NATIVE Start Race redirected to car UI event={} player={:08X}",event,player);
    return true;
  } catch(const std::exception& e) {
    direct_picker={};
    REXLOG_ERROR("PLAYER2_NATIVE Start Race redirect failed: {}",e.what());
    return false;
  }
}
bool OpenDirectPlayer2(PPCContext& ctx,uint8_t* base) {
  if(!direct_picker.screen||direct_picker.screen!=ctx.r3.u32) return false;
  if(direct_picker.player!=Player(base,1)||!REXCVAR_GET(player2_native_car_menu)) {direct_picker={};return false;}
  Guest g(ctx,base);
  if(g.Call(0x82362AA0,ctx.r3.u32)) return true;
  direct_picker={};
  // BeginNativePlayer2 only changes the list and its controller owner; it
  // never selects a car for P1. Back keeps the existing safe P1-list restore.
  PPCContext handoff=ctx;
  return BeginNativePlayer2(handoff,base);
}
// fm3_139: on a car screen opened from the calendar the list stayed Player 1's
// after the handoff (picks came back as Player 1's garage ids and were all
// refused; the header read MY CARS). Whatever reloads it, put Player 2's list
// and header back. `force` is used after a refused pick.
bool ReapplyNativeList(Guest& g,uint8_t* base,uint32_t screen,bool force,const wchar_t* title) {
  if(!native.active||native.screen!=screen||native.reloading||native.player2_query.empty()) return false;
  const auto source=R(base,screen+0x258);
  std::string current;
  try {current=g.S(source+0x9C);} catch(const std::exception&) {current="<unreadable>";}
  if(!force&&current==native.player2_query) return false;
  if(native.reapplied>=8) {
    if(native.reapplied==8) {++native.reapplied;REXLOG_ERROR("PLAYER2_NATIVE list keeps being replaced on screen={:08X}; giving up re-applying",screen);}
    return false;
  }
  ++native.reapplied;
  REXLOG_WARN("PLAYER2_NATIVE list on screen={:08X} was not Player 2's (forced={} same_query={} current={:.120}); re-applying #{}",screen,force,current==native.player2_query,current,native.reapplied);
  native.reloading=true;
  try {
    g.Put(g.scratch+0x800,native.player2_query);
    g.Call(0x82446C50,source,g.scratch+0x800,0);
    g.Call(0x823516C8,screen);
    NativeTitle(g,screen,title);
  } catch(const std::exception& e) {REXLOG_ERROR("PLAYER2_NATIVE re-apply failed: {}",e.what());}
  native.reloading=false;
  native.opened=std::chrono::steady_clock::now();
  return true;
}
bool SelectNativePlayer2(PPCContext& ctx,uint8_t* base) {
  const auto screen=ctx.r3.u32,row=ctx.r4.u32;
  if(!native.active||native.screen!=screen) return false;
  if(!row||native.reloading||std::chrono::steady_clock::now()-native.opened<std::chrono::milliseconds(250)) return true;
  Guest g(ctx,base);
  try {
    // The native row adapter uses GetInt(name), not the RecordSet interface.
    const auto id=g.V(row,0x14,0x820046D4);
    const auto e=Eligible(g,native.event,g.V(GarageOwner(base),0x50));
    const auto eligible=g.Query(e.Select("Garage.CarId, "+e.UseStock()," AND Garage.Id="+std::to_string(id)));
    const bool valid=!g.V(eligible,0x98);
    const auto model=valid?g.Int(eligible,0):0,stock=valid?g.Int(eligible,1):0;
    g.V(eligible,8);
    if(!valid||!id||id>=0x7FFFFFFF) {
      REXLOG_ERROR("PLAYER2_NATIVE rejected pick event={} garage id={} valid={} view={} rule={}; pick ignored",native.event,id,valid,e.view,e.predicate);
      // Never fail silently: say so in the header and rebuild Player 2's list,
      // in case the cars on screen are not that list.
      if(!ReapplyNativeList(g,base,screen,true,L"PLAYER 2 - THAT CAR IS NOT ELIGIBLE, PICK ANOTHER"))
        NativeTitle(g,screen,L"PLAYER 2 - THAT CAR IS NOT ELIGIBLE, PICK ANOTHER");
      return true;
    }
    native.garage=id;native.confirmed=true;native.use_host=false;
    RestoreNativeInput(g);
    NativeTitle(g,screen,L"CHOOSE YOUR CAR");
    REXLOG_WARN("PLAYER2_NATIVE confirmed event={} garage={} player={:08X} model={} stock={}; host selection preserved",native.event,id,native.player,model,stock);
    // Continue the original P1 event transition without changing P1's car.
    __imp__sub_823D8F90(ctx,base);
  } catch(const std::exception& e) {Error(e.what());}
  return true;
}
bool BackFromNativePlayer2(PPCContext& ctx,uint8_t* base) {
  const auto screen=ctx.r3.u32,input=ctx.r4.u32;
  if(!native.active||native.screen!=screen||!input) return false;
  Guest g(ctx,base);
  {
    const auto now=std::chrono::steady_clock::now();
    // fm3_140: the list built during the handoff (inside Player 1's own
    // button press) left Player 1's highlighted car as the current row, so
    // Player 2's first press was refused; the rebuild after that refusal
    // worked. Do that second rebuild unasked, once, shortly after the handoff.
    if(!native.settled&&now-native.handed>std::chrono::milliseconds(350)) {
      native.settled=true;
      ReapplyNativeList(g,base,screen,true,L"PLAYER 2 - CHOOSE YOUR CAR  (Y: USE PLAYER 1'S CAR)");
    } else if(now-native.checked>std::chrono::milliseconds(400)) {
      native.checked=now;
      ReapplyNativeList(g,base,screen,false,L"PLAYER 2 - CHOOSE YOUR CAR  (Y: USE PLAYER 1'S CAR)");
    }
  }
  // Y on Player 2's list: race in a copy of Player 1's car instead of one of
  // Player 2's own (Y otherwise only switches the stats panel, 82427E20).
  if(!g.Call(0x82362AA0,screen)&&g.Call(0x82210D08,input,8,124,1)) {
    native.garage=0;native.use_host=true;native.confirmed=true;
    RestoreNativeInput(g);
    NativeTitle(g,screen,L"CHOOSE YOUR CAR");
    REXLOG_WARN("PLAYER2_NATIVE Player 2 chose to use a copy of Player 1's car event={} player={:08X}",native.event,native.player);
    PPCContext next=ctx;next.r3.u64=screen;
    __imp__sub_823D8F90(next,base);
    return true;
  }
  // Match the native Back action, consuming it before the base car screen
  // can close this screen and reactivate Career's parent menu.
  if(g.Call(0x82362AA0,screen)||!g.Call(0x82210D08,input,8,0x43,1)) return false;
  try {
    native.reloading=true;
    g.Put(g.scratch+0x800,native.host_query);
    g.Call(0x82446C50,R(base,screen+0x258),g.scratch+0x800,native.host_query_mode);
    g.Call(0x823516C8,screen);
    NativeTitle(g,screen,L"PLAYER 1 - CHOOSE YOUR CAR");
    RestoreNativeInput(g);
    REXLOG_WARN("PLAYER2_NATIVE back to host car selection screen={:08X} event={}",screen,native.event);
    native={};
  } catch(const std::exception& e) {
    native.reloading=false;
    Error(e.what());
  }
  return true;
}
void NativeCarMenuClosed(PPCContext& ctx,uint8_t* base,uint32_t screen) {
  if(direct_picker.screen==screen) direct_picker={};
  if(native.screen!=screen) return;
  Guest g(ctx,base);RestoreNativeInput(g);native.screen=0;
  if(!native.confirmed) native={};
}
void NativeCareerCalendarOpening(PPCContext& ctx,uint8_t* base,const char* destination) {
  career_parent_is_race_info=std::string_view(destination)=="race-info";
  if(!REXCVAR_GET(player2_native_car_menu)||!native.confirmed||native.player!=Player(base,1)) return;
  // Screen activation is deferred. A held P2 confirm button can update the
  // last-active byte again between confirmation and the next screen's init.
  Guest g(ctx,base);RestoreHostMenu(g);
  const auto expected=g.V(Player(base,0),0x44);
  const auto actual=g.Call(0x82362E10);
  REXLOG_WARN("PLAYER2_NATIVE calendar host controller={} profile={:08X} expected={:08X} destination={}",native.host_controller,actual,expected,destination);
}

// Assists (fm3_142). Before a race 822714D0 reads the assist settings
// (SuggestedLine, Autobrake, ABS, StabilityControl, TractionControl,
// Shifting) from ONE profile, the event's main player (82482188), and writes
// them to that player's config (player vfunc 0x60, bytes +604..+609); it also
// writes the race-wide AI difficulty and damage into the event (+548, via
// 8248CED8). Player 2's config never got Player 2's settings, so Player 2
// drove and was paid (822F59E0 reads those bytes) by other values. Run the
// same routine a second time for Player 2, then put the race-wide block back
// to Player 1's so AI difficulty and damage stay one shared setting.
uint32_t assist_override_player=0;
struct {bool valid=false;uint32_t player=0;uint8_t bytes[6]{};} p2_assists;
std::string AssistBytes(uint8_t* base,uint32_t config) {
  return Ptr(config)?fmt::format("{} {} {} {} {} {}",base[config+604],base[config+605],base[config+606],base[config+607],base[config+608],base[config+609]):std::string("-");
}
void ReapplyPlayer2Assists(uint8_t* base,uint32_t player,uint32_t car) {
  if(!p2_assists.valid||p2_assists.player!=player||!Ptr(car)) return;
  const auto before=AssistBytes(base,car);
  std::memcpy(base+car+604,p2_assists.bytes,6);
  REXLOG_WARN("PLAYER2_ASSISTS kept Player 2's own assists after loading the car: {} -> {}",before,AssistBytes(base,car));
}

void PreparePlayer2Career(PPCContext& ctx,uint8_t* base,uint32_t player) {
  if(!REXCVAR_GET(player2_local_profile)) return;
  LocalProfile profile;
  {std::lock_guard lock(mutex);career_player=player;pending={};if(ready) profile=store.data;}
  if(!ready) Error(failure+"\nPlayer two progress will not be saved.");
  Guest g(ctx,base);
  // With their own garage, Player 2's garage ids are unrelated to Player
  // 1's, so "no choice" is 0 rather than Player 1's current garage id.
  const bool own=Player2OwnsGarage(base);
  const auto host=Player(base,0),user=g.V(own?player:host,0x50),car=g.V(player,0x60);
  const auto hostcar=R(base,host+0xB8),fallback=R(base,hostcar+0x258);
  uint32_t chosen=own?0:fallback;
  uint32_t stock_model=0;
  try {
    const auto event=R(base,0x82C374E4);
    const auto eligibility=Eligible(g,event,user);
    const auto& predicate=eligibility.predicate;
    Picker picker;picker.preferred=profile.garage_id;
    // Two short queries instead of one combined statement: the combined
    // upgraded-or-stock query was rejected at race setup (event 602, log054).
    // The first is the form validated in play before stock support existed.
    const auto collect=[&](uint32_t row,bool stock) {
      try {
        for(;!g.V(row,0x98);g.V(row,0x9C,0)) {
          if(picker.cars.size()>=4096) throw std::runtime_error("Shared garage exceeded its size limit.");
          const auto id=g.Int(row,0),year=g.Int(row,3),cls=g.Int(row,4),carId=g.Int(row,6);
          // Upgraded configurations take priority over the same car's stock one.
          if(stock && std::any_of(picker.cars.begin(),picker.cars.end(),[&](const Car& c){return c.id==id;})) continue;
          auto makeKey=g.String(row,2),modelKey=g.String(row,1);
          if(!id || id>=0x7FFFFFFF || !carId || carId>=0x7FFFFFFF || year<1880 || year>2100 || cls>10 || makeKey.empty() || modelKey.empty())
            throw std::runtime_error("The garage returned incomplete car details. No invalid car will be selected.");
          REXLOG_WARN("PLAYER2_PROFILE eligible row garage={} car={} year={} class={} stock={} make={} model={}",id,carId,year,cls,stock,makeKey,modelKey);
          auto make=g.LocalizedColumn(row,2),model=g.LocalizedColumn(row,1);
          static constexpr const wchar_t* classes[]={L"F",L"E",L"D",L"C",L"B",L"A",L"S",L"R3",L"R2",L"R1",L"U"};
          picker.cars.push_back({id,std::to_wstring(year)+L" "+make+L" "+model+L"   ["+(cls<std::size(classes)?classes[cls]:L"?")+L"]"+(stock?L" - stock configuration":L""),carId,stock});
        }
      } catch(...) {g.V(row,8);throw;}
      g.V(row,8);
    };
    collect(g.Query("SELECT Garage.Id AS Id, Garage.DisplayName AS DisplayName, Garage.MakeName AS MakeName, Garage.Year AS Year, Garage.ClassID AS ClassID, Garage.PerformanceIndex AS PerformanceIndex, Garage.CarId AS CarId FROM "+eligibility.view+" AS Garage WHERE ("+predicate+") AND IsInstalled=1 AND IsPurchased=1 ORDER BY ClassID, PerformanceIndex, Garage.Id"),false);
    try {
      collect(g.Query("SELECT Garage.Id, Garage.DisplayName, Garage.MakeName, Garage.Year, Stock.ClassID, Stock.PerformanceIndex, Garage.CarId"+eligibility.From()+" WHERE Garage.IsInstalled=1 AND Garage.IsPurchased=1 AND Stock.IsInstalled=1 AND Stock.IsPurchased=1 AND ("+eligibility.StockPredicate()+") ORDER BY Stock.ClassID, Stock.PerformanceIndex, Garage.Id"),true);
    } catch(const std::exception& e) {
      // Stock configurations are an extra; never lose the upgraded choices.
      REXLOG_ERROR("PLAYER2_PROFILE stock-configuration list unavailable ({}); continuing with upgraded cars only",e.what());
    }
    REXLOG_WARN("PLAYER2_PROFILE eligible event={} count={} host_garage={} own_garage={} predicate={}",event,picker.cars.size(),fallback,own,predicate);
    // No blocking message box here: it can open behind the game and look like a hang.
    if(picker.cars.empty()) REXLOG_WARN("PLAYER2_PROFILE no eligible car for event={}; Player 2 uses a copy of Player 1's car",event);
    else {
    picker.summary=Wide(profile.name)+L"  |  Level "+std::to_wstring(profile.level)+L"  |  "+std::to_wstring(profile.credits)+L" CR  |  "+std::to_wstring(profile.xp)+L" XP\n"+std::to_wstring(profile.races)+L" races completed, "+std::to_wstring(profile.wins)+L" wins. Garage and tunes shared with Player 1.";
    const bool native_valid=REXCVAR_GET(player2_native_car_menu)&&native.confirmed&&native.event==event&&native.player==player&&std::any_of(picker.cars.begin(),picker.cars.end(),[](const Car& c){return c.id==native.garage;});
    const bool host_copy=REXCVAR_GET(player2_native_car_menu)&&native.confirmed&&native.use_host&&native.event==event&&native.player==player;
    if(host_copy) {
      // chosen keeps its "no choice" value: the copy branch below is used.
      REXLOG_WARN("PLAYER2_NATIVE applying pre-race choice event={}: copy of Player 1's car",event);
    } else if(native_valid) {
      chosen=native.garage;
      REXLOG_WARN("PLAYER2_NATIVE applying pre-race choice event={} garage={}",event,chosen);
    } else if(const auto remembered=profile.CarForSeries(event); remembered&&std::any_of(picker.cars.begin(),picker.cars.end(),[&](const Car& c){return c.id==remembered;})) {
      chosen=remembered;
      REXLOG_WARN("PLAYER2_PROFILE applying remembered series car event={} garage={}",event,chosen);
    } else if(auto selected=Choose(picker)) chosen=selected;
    if(const auto selection=std::find_if(picker.cars.begin(),picker.cars.end(),[&](const Car& c){return c.id==chosen;});selection!=picker.cars.end()&&selection->stock)
      stock_model=selection->car_id;
    }
    native.confirmed=false;native.use_host=false;
  } catch(const std::exception& e) {
    Error(std::string(e.what())+"\nPlayer two will use Player one's eligible car for this race.");
  }
  // Load through the game's owned-car loader: deep copy upgrades/tuning and
  // livery, never alias the two players' mutable car structures.
  if(stock_model) {
    // Native factory-car loader used by Career at 822E390C. It rebuilds
    // stock parts/tune on P2's own car object without writing to the garage.
    g.Call(0x8229F428,car,stock_model);
    base[car+0x274]=1;
    REXLOG_WARN("PLAYER2_PROFILE loaded eligible stock configuration garage={} model={} class={} pi={}",chosen,stock_model,R(base,car),std::bit_cast<float>(R(base,car+4)));
  } else if((!own && chosen==fallback) || !chosen || chosen==0xFFFFFFFF) {
    // Includes stock/loaned event cars that do not have a garage row. The
    // original Career setup has already qualified and configured this car.
    g.Call(0x8223BE30,car+8,hostcar+8);
    W(base,car,R(base,hostcar));W(base,car+4,R(base,hostcar+4));
    W(base,car+0x258,fallback);base[car+0x274]=1;
  } else g.Call(0x8229F5A8,car,user,chosen,1);
  ReapplyPlayer2Assists(base,player,car);
  {std::lock_guard lock(mutex);if(ready) try {
    auto next=store.data;next.garage_id=chosen;
    const auto event=R(base,0x82C374E4);
    next.car_series_event=event!=0xFFFFFFFF&&chosen&&chosen<0x7FFFFFFF?event:0;
    store.Save(next);
  }catch(const std::exception& e){failure=e.what();REXLOG_ERROR("PLAYER2_PROFILE selection save failed: {}",failure);}}
  REXLOG_WARN("PLAYER2_PROFILE selected garage={} car={} event={}",chosen,R(base,car+8),R(base,0x82C374E4));
}

// 822F6060 only queues level reward cars on the user (vector at user+68:
// first +80, last +84, 8-byte {car id, source} entries); the gift screen
// 823469A8 delivers them for the menu's own user. Deliver Player 2's the same
// way: 82288668(out, user, car, colour 0, &garage id, source, &ref,
// schedule=1) then 822889F8.
// Adding a car to the garage takes 3-4 s on the game thread (fm3_109: one frame
// of 3895 ms right at the finish). After a race the cars Player 2 won are
// therefore only noted here and in a small file next to the local profile, and
// handed over when the game leaves the results screen (or at the next join if
// the game was closed first).
struct PendingCar {uint32_t car,source;};
std::vector<PendingCar> pending_cars;
std::filesystem::path PendingCarsFile() {return store.Path().parent_path()/"pending_reward_cars";}
void WritePendingCars() {
  std::error_code ec;
  if(pending_cars.empty()) {std::filesystem::remove(PendingCarsFile(),ec);return;}
  std::ofstream out(PendingCarsFile(),std::ios::trunc);
  for(const auto& c:pending_cars) out<<c.car<<' '<<c.source<<'\n';
}
// Caller holds `mutex`.
void DeferQueuedCars(Guest& g,uint8_t* base,uint32_t player) {
  try {
    const auto user=g.V(player,0x50);
    const auto first=R(base,user+80),last=R(base,user+84);
    if(!Ptr(first)||last<first||last-first>8*64) return;
    for(auto entry=first;entry<last;entry+=8) {
      pending_cars.push_back({R(base,entry),R(base,entry+4)});
      REXLOG_WARN("PLAYER2_PROFILE level reward car {} noted for delivery after the results screen",R(base,entry));
    }
    W(base,user+84,first);
    WritePendingCars();
  } catch(const std::exception& e) {REXLOG_ERROR("PLAYER2_PROFILE reward car note failed: {}",e.what());}
}
unsigned DeliverCar(Guest& g,uint8_t* base,uint32_t user,uint32_t car_id,uint32_t source) {
  const auto out=g.scratch+0x200,garage=g.scratch+0x210,ref=g.scratch+0x220;
  W(base,out,0);W(base,garage,0);W(base,ref,0);
  base[g.ctx.r1.u32+87]=0;
  g.ctx.r8.u64=source;g.ctx.r9.u64=ref;g.ctx.r10.u64=1;
  g.Call(0x82288668,out,user,car_id,0,garage);
  const auto operation=R(base,out),garage_id=R(base,garage);
  if(garage_id) g.Call(0x822889F8,user,garage_id);
  if(Ptr(operation)) g.V(operation,8);
  REXLOG_WARN("PLAYER2_PROFILE level reward car {} delivered to Player 2 garage id {}",car_id,garage_id);
  return garage_id?1u:0u;
}
// Hand over everything noted (and anything left in the file by an earlier
// session). Runs on the game thread; does nothing without a mounted Player 2 save.
void DeliverPendingCars(PPCContext& ctx,uint8_t* base) {
  std::vector<PendingCar> cars;
  {
    std::lock_guard lock(mutex);
    if(!ready) return;
    if(pending_cars.empty()) {
      std::ifstream in(PendingCarsFile());
      PendingCar c{};
      while(in>>c.car>>c.source) if(pending_cars.size()<16) pending_cars.push_back(c);
    }
    if(pending_cars.empty()) return;
    cars=pending_cars;
  }
  try {
    const auto player=Player(base,1);
    const auto holder=player?R(base,player+0xAC):0;
    if(!player||!Ptr(holder)||!Ptr(R(base,holder))) return; // keep them for the next chance
    Guest g(ctx,base);
    const auto user=g.V(player,0x50);
    unsigned delivered=0;
    for(const auto& c:cars) delivered+=DeliverCar(g,base,user,c.car,c.source);
    g.Call(0x82269720,holder);
    {std::lock_guard lock(mutex);pending_cars.clear();WritePendingCars();}
    REXLOG_WARN("PLAYER2_PROFILE {} of {} noted reward cars delivered and saved",delivered,cars.size());
  } catch(const std::exception& e) {REXLOG_ERROR("PLAYER2_PROFILE deferred reward car delivery failed: {}",e.what());}
}
unsigned DeliverQueuedCars(Guest& g,uint8_t* base,uint32_t player) {
  unsigned delivered=0;
  try {
    const auto user=g.V(player,0x50);
    const auto first=R(base,user+80),last=R(base,user+84);
    if(Ptr(first)&&last>=first&&last-first<=8*64) {
      for(auto entry=first;entry<last;entry+=8) {
        const auto car_id=R(base,entry),source=R(base,entry+4);
        const auto out=g.scratch+0x200,garage=g.scratch+0x210,ref=g.scratch+0x220;
        W(base,out,0);W(base,garage,0);W(base,ref,0);
        base[g.ctx.r1.u32+87]=0;
        g.ctx.r8.u64=source;g.ctx.r9.u64=ref;g.ctx.r10.u64=1;
        g.Call(0x82288668,out,user,car_id,0,garage);
        const auto operation=R(base,out),garage_id=R(base,garage);
        if(garage_id) {g.Call(0x822889F8,user,garage_id);++delivered;}
        if(Ptr(operation)) g.V(operation,8);
        REXLOG_WARN("PLAYER2_PROFILE level reward car {} delivered to Player 2 garage id {}",car_id,garage_id);
      }
      W(base,user+84,first);
    }
  } catch(const std::exception& e) {REXLOG_ERROR("PLAYER2_PROFILE reward car delivery failed: {}",e.what());}
  return delivered;
}
// Diagnostic (fm3_113): what each player's payout is made of. Logs the reward
// calculation's output block for every player, and the component's inputs and
// the player's credits around each native commit.
unsigned PlayerIndex(uint8_t* base,uint32_t player) {return player&&player==Player(base,0)?1:player&&player==Player(base,1)?2:0;}
uint32_t CreditsOf(const PPCContext& ctx,uint8_t* base,uint32_t player) {
  if(!Ptr(player)) return 0;
  const auto holder=R(base,player+0xAC);
  if(!Ptr(holder)||!Ptr(R(base,holder))) return 0;
  Guest g(ctx,base);return g.Call(0x82261010,holder);
}
void TraceCalc(uint8_t* base,uint32_t player,uint32_t position,uint32_t output) {
  if(!Ptr(output)) return;
  std::string words;
  for(unsigned i=0;i<12;++i) words+=fmt::format("{}{}",i?" ":"",R(base,output+i*4));
  REXLOG_WARN("REWARD_TRACE calculated for player {} ({:08X}) place={} block=[{}]",PlayerIndex(base,player),player,position,words);
}
void TraceCommit(const PPCContext& ctx,uint8_t* base,uint32_t component,uint32_t player,bool after,uint32_t before_credits) {
  if(!Ptr(component)) return;
  const auto now=CreditsOf(ctx,base,player);
  if(!after) REXLOG_WARN("REWARD_TRACE commit start player {} ({:08X}) credits_in={} xp_in={} place={} wallet={}",PlayerIndex(base,player),player,R(base,component+60),R(base,component+80),R(base,component+232),now);
  else REXLOG_WARN("REWARD_TRACE commit end   player {} ({:08X}) wallet {} -> {} (change {})",PlayerIndex(base,player),player,before_credits,now,int64_t(now)-int64_t(before_credits));
}
// Player 2's result for the companion's results banner: place, credits, xp,
// level (0 until committed), wallet total, reward cars won.
std::atomic<int32_t> p2_result[7]; // [6] = base prize for the place
void Player2Result(int32_t out[7]) {for(int i=0;i<7;++i) out[i]=p2_result[i].load();}
void ResetRewards(uint32_t component) {for(auto& v:p2_result) v=0;std::lock_guard lock(mutex);pending={};pending.component=component;pending.id=++sequence;}
void CaptureRewards(uint8_t* base,uint32_t component,uint32_t player,uint32_t position,uint32_t output) {
  std::lock_guard lock(mutex);
  if(!ready||player!=career_player||player!=Player(base,1)||!position||position>8) return;
  if(pending.component!=component) {pending={};pending.component=component;pending.id=++sequence;}
  pending.player=player;pending.position=position;pending.credits=R(base,output+4);pending.xp=R(base,output+0x18);
  REXLOG_WARN("PLAYER2_PROFILE reward calculated place={} credits={} xp={}",position,pending.credits,pending.xp);
  p2_result[1]=int32_t(pending.credits);p2_result[2]=int32_t(pending.xp);p2_result[3]=0;p2_result[4]=0;p2_result[5]=0;p2_result[6]=int32_t(R(base,output));p2_result[0]=int32_t(position);
}
void CommitRewards(PPCContext& ctx,uint8_t* base,uint32_t component) {
  std::lock_guard lock(mutex);
  if(!ready||pending.committed||!pending.player||pending.component!=component||base[component+4]||!R(base,component+0xE8)) return;
  try {
    Guest g(ctx,base);
    const auto table=component+0x10,count=R(base,table);
    if(count<2||count>1000) throw std::runtime_error("Unexpected Career level table.");
    auto cap=g.Call(0x8228BC10,table,count-1);
    auto xp=static_cast<uint32_t>(std::min<uint64_t>(cap,store.data.xp+pending.xp));
    auto level=g.Call(0x8228BC38,table,xp);
    if(store.Award(pending.id,pending.credits,xp,level+1,pending.position)) {
      REXLOG_WARN("PLAYER2_PROFILE saved award={} place={} earned_cr={} earned_xp={} total_cr={} total_xp={} level={} races={}",pending.id,pending.position,pending.credits,pending.xp,store.data.credits,store.data.xp,store.data.level,store.data.races);
      // With a real second profile, run the game's own per-player commit
      // (822F6388(component, player)) for Player 2: it adds credits, updates
      // career stats, adds XP, sets the level and, on a level-up, awards the
      // driver-level reward cars through 822F6060. It reads its inputs from
      // the component, which holds Player 1's race: substitute Player 2's
      // credits (+60), XP (+80) and position (+232) for the call and restore.
      // Distance, time and top speed stay as Player 1's (same race).
      if(REXCVAR_GET(player2_real_profile)) {
        const auto holder=R(base,pending.player+0xAC);
        if(Ptr(holder)&&Ptr(R(base,holder))) {
          // Player 2's real profile gained no XP before this change; the
          // first native commit carries the whole local total across.
          const auto marker=store.Path().parent_path()/"real_xp_synced";
          std::error_code ec;
          const bool synced=std::filesystem::exists(marker,ec);
          const uint32_t xp_add=synced?pending.xp:xp;
          const auto credits0=R(base,component+60),xp0=R(base,component+80),place0=R(base,component+232);
          const auto before=g.Call(0x82261010,holder);
          W(base,component+60,pending.credits);W(base,component+80,xp_add);W(base,component+232,pending.position);
          // One-time catch-up: reward cars for levels Player 2 reached before
          // deliveries worked (log081: the car was queued, its popup went to
          // Player 1's results flow, and nothing reached Player 2's garage).
          // Queue the cars for (level at 0 XP, level before this race]; the
          // commit below queues any for this race's own level-up.
          const auto cars_marker=store.Path().parent_path()/"reward_cars_synced";
          player2_commit_active=true;
          if(!std::filesystem::exists(cars_marker,ec)) {
            const auto level0=g.Call(0x8228BC38,table,0);
            const auto level_before=g.Call(0x8228BC38,table,xp>pending.xp?xp-pending.xp:0);
            if(synced&&level_before>level0) {
              g.Call(0x822F6060,component,pending.player,level0,level_before);
              REXLOG_WARN("PLAYER2_PROFILE reward car catch-up for levels {}..{}",level0,level_before);
            }
            std::ofstream(cars_marker)<<"1";
          }
          g.ctx.r3.u64=component;g.ctx.r4.u64=pending.player;
          __imp__sub_822F6388(g.ctx,base);
          player2_commit_active=false;
          W(base,component+60,credits0);W(base,component+80,xp0);W(base,component+232,place0);
          // 822F6060 only queues level reward cars on the user (vector at
          // user+68: first +80, last +84, 8-byte {car id, source} entries);
          // the gift screen 823469A8 delivers them for the menu's own user.
          // Deliver Player 2's here the same way: 82288668(out, user, car,
          // colour 0, &garage id, source, &ref, schedule=1) then 822889F8.
          DeferQueuedCars(g,base,pending.player);
          g.Call(0x82269720,holder);
          p2_result[3]=int32_t(store.data.level);p2_result[4]=int32_t(g.Call(0x82261010,holder));p2_result[5]=int32_t(pending_cars.size());
          if(!synced) {std::ofstream(marker)<<"1";}
          REXLOG_WARN("PLAYER2_PROFILE native commit for Player 2: credits {} -> {}, xp added {}{}",before,g.Call(0x82261010,holder),xp_add,synced?"":" (first sync of local total)");
        } else REXLOG_WARN("PLAYER2_PROFILE real save not mounted; winnings kept in the local file only");
      }
    }
    pending.committed=true;
  } catch(const std::exception& e) {failure=e.what();REXLOG_ERROR("PLAYER2_PROFILE reward save failed: {}",failure);}
}
// 8230D978 runs when Restart is chosen in the pause menu (and from three
// other callers), before the confirmation prompt. It takes the current
// player's user record X (8230CC78), copies the 32-byte block at X+448 and
// 82762B90 then dereferences the pointer at +28 of that copy. In Career co-op
// that pointer is null for Player 1 (it is set in solo), so the game reads
// null+72 (crash_20261112_110543/114321/115340). The routine only refreshes a
// race-description record; the restart itself happens later, on confirm, via
// 823E1C40. Resolve X exactly as the game does and skip this one refresh only
// when it would dereference null. Every other case runs the original.
void CareerRaceSettingsRefresh(PPCContext& ctx,uint8_t* base,void(*original)(PPCContext&,uint8_t*)) {
  bool skip=false;
  try {
    Guest g(ctx,base);
    const auto out=g.scratch+0x40;
    W(base,out,0);
    g.Call(0x8230CC78,out);
    const auto record=R(base,out);
    if(Ptr(record)) {
      const auto pointer=R(base,record+448+28);
      skip=!pointer;
      REXLOG_WARN("PLAYER2_CAREER race description refresh caller={:08X} players={} record={:08X} block=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}] action={}",
          static_cast<uint32_t>(ctx.lr),R(base,0x82C231F4+0x2C),record,R(base,record+448),R(base,record+452),R(base,record+456),R(base,record+460),
          R(base,record+464),R(base,record+468),R(base,record+472),pointer,skip?"skipped (null pointer would crash)":"original");
      g.V(record,8); // release the reference 8230CC78 took, as 8230D718 does
    }
  } catch(const std::exception& e) {REXLOG_ERROR("PLAYER2_CAREER race description check failed: {}",e.what());}
  if(!skip) original(ctx,base);
  // First game-thread call after a finished race comes from leaving the
  // results screen (caller 8233E708), while the menu is loading.
  DeliverPendingCars(ctx,base);
}

uint32_t AssistOverridePlayer() {return assist_override_player;}

// Tune screen, Aero page (82402570). A downforce slider is unlocked only when
// the part's upgrade level in the car's level table (tune+356 + 4*part; front
// bumper = part 33 at +488 via 822C7D10, rear wing = part 8 at +388 via
// 822C7D30) is 3. In gamedb only level-3 bumpers/wings have a range
// (Downforce0 != Downforce1), so the two always agree in the original game.
// fm3_143: the Integra's race front bumper range was loaded (physics +2540 /
// +2548 = 50-100 lb, shown 75) but the slider stayed locked, so the level
// entry was below 3. Follow the installed part's range, as the game intends.
bool AeroSliderUnlocked(uint8_t* base,uint32_t tune,bool front) {
  if(!Ptr(tune)) return false;
  const auto physics=R(base,tune+4);
  if(!Ptr(physics)) return false;
  const float d0=std::bit_cast<float>(R(base,physics+(front?2540:2620)));
  const float d1=std::bit_cast<float>(R(base,physics+(front?2548:2628)));
  if(!(d1>d0)) return false;
  static std::atomic<unsigned> notes{0};
  if(notes.fetch_add(1)<12) {
    std::string levels;
    for(unsigned i=0;i<42;++i) levels+=fmt::format("{}{}",i?" ":"",int32_t(R(base,tune+356+i*4)));
    REXLOG_WARN("TUNE_AERO unlocked {} downforce: level entry {} is below 3 but the installed part has a {:.1f}-{:.1f} kg range | levels[0..41]={}",front?"front":"rear",int32_t(R(base,tune+(front?488:388))),d0,d1,levels);
  }
  return true;
}
void ApplyPlayer2Assists(PPCContext& ctx,uint8_t* base,uint64_t arg3,uint64_t arg4) {
  if(assist_override_player||!REXCVAR_GET(player2_local_profile)||!REXCVAR_GET(player2_real_profile)||!Player2OwnsGarage(base)) return;
  const auto second=Player(base,1),first=Player(base,0);
  if(!second||!first) return;
  try {
    Guest g(ctx,base);
    const auto event=R(base,0x82C2110C);
    if(!Ptr(event)) return;
    const auto race=g.Call(0x82482218,event);
    if(!Ptr(race)) return;
    std::vector<uint8_t> shared(base+race,base+race+572);
    const auto config2=g.V(second,0x60),config1=g.V(first,0x60);
    const auto before=AssistBytes(base,config2);
    assist_override_player=second;
    PPCContext call=g.ctx;call.r3.u64=arg3;call.r4.u64=arg4;
    __imp__sub_822714D0(call,base);
    assist_override_player=0;
    // Player 1's AI difficulty and damage stay the race's.
    std::memcpy(base+g.scratch+0x1000,shared.data(),shared.size());
    g.Call(0x8248CED8,event,g.scratch+0x1000);
    if(Ptr(config2)) {
      p2_assists.valid=true;p2_assists.player=second;std::memcpy(p2_assists.bytes,base+config2+604,6);
    }
    REXLOG_WARN("PLAYER2_ASSISTS applied Player 2's own profile settings: player 2 {} -> {} | player 1 {} | race AI/damage kept as Player 1's",before,AssistBytes(base,config2),AssistBytes(base,config1));
  } catch(const std::exception& e) {
    assist_override_player=0;
    REXLOG_ERROR("PLAYER2_ASSISTS could not apply Player 2's settings: {}",e.what());
  }
}

// Series payout (fm3_142). When a series ends, 82231A28(career, record) pays
// the career's player by series position: record holds 16-bit points, [0]
// event, [1] the player, [2..8] the other seven; Player 2 is slot 7 = [8]
// (career_trace.cpp). Amount = 822E4460(event, position) / 2. Nothing paid
// Player 2. Pay Player 2 the same way from Player 2's own position.
void PayPlayer2Series(PPCContext& ctx,uint8_t* base,uint32_t record) {
  if(!REXCVAR_GET(player2_local_profile)||!Ptr(record)) return;
  const auto second=Player(base,1);
  if(!second) return;
  try {
    const auto hw=[&](unsigned i){uint16_t v;std::memcpy(&v,base+record+i*2,2);return uint32_t(std::byteswap(v));};
    const auto event=hw(0),points=hw(8);
    uint32_t position=1;
    for(unsigned i=1;i<8;++i) if(points<hw(i)) ++position;
    Guest g(ctx,base);
    const auto total=int32_t(g.Call(0x822E4460,event,position));
    const auto amount=total/2;
    if(amount<=0||amount>10000000) {REXLOG_WARN("PLAYER2_PROFILE series payout event={} points={} position={}: nothing to pay ({})",event,points,position,total);return;}
    uint32_t before=0,after=0;
    const auto holder=R(base,second+0xAC);
    if(REXCVAR_GET(player2_real_profile)&&Ptr(holder)&&Ptr(R(base,holder))) {
      before=g.Call(0x82261010,holder);
      g.Call(0x82263568,holder,before+uint32_t(amount));
      after=g.Call(0x82261010,holder);
    }
    {
      std::lock_guard lock(mutex);
      if(ready) {auto next=store.data;next.credits+=uint64_t(amount);store.Save(next);}
    }
    REXLOG_WARN("PLAYER2_PROFILE series payout event={} points={} position={} amount={} wallet {} -> {}",event,points,position,amount,before,after);
  } catch(const std::exception& e) {REXLOG_ERROR("PLAYER2_PROFILE series payout failed: {}",e.what());}
}
}

// Career's pause menu (CPauseMenu) talks to the menu background scene:
// 8231A3F0 is dynamic_cast<ForzaUI::CBackgroundMain*>(ui->scene(0)). In solo
// Career that scene stays alive under the race. In Career co-op the cast
// returns null (crash_20261112_120702: 8233DB08 read null+0xBC0), and several
// callers post messages to it without the null check that 82362C30 has.
// Treat a missing background scene as "nothing to notify", as 82362C30 does.
namespace fm3 {
void MissingBackgroundScene(PPCContext& ctx,uint8_t* base,const char* what,uint32_t detail) {
  static std::atomic<unsigned> logged{0};
  if(logged.fetch_add(1)<12)
    REXLOG_WARN("PLAYER2_CAREER background scene missing: {} skipped caller={:08X} detail={:08X} players={}",what,static_cast<uint32_t>(ctx.lr),detail,R(base,0x82C231F4+0x2C));
}
void DescribeSceneZero(PPCContext& ctx,uint8_t* base) {
  static std::atomic<unsigned> logged{0};
  if(logged.fetch_add(1)>=4) return;
  try {
    Guest g(ctx,base);
    const auto ui=g.Call(0x827017E8);
    const auto scene=Ptr(ui)?g.V(ui,132,0):0;
    std::string name="none";
    if(Ptr(scene)&&Ptr(R(base,scene))) {
      const auto locator=R(base,R(base,scene)-4);
      const auto type=Ptr(locator)?R(base,locator+12):0;
      name=Ptr(type)?Text(base,type+8,96):"unknown";
    }
    REXLOG_WARN("PLAYER2_CAREER CBackgroundMain cast failed caller={:08X} scene0={:08X} class={} players={}",static_cast<uint32_t>(ctx.lr),scene,name,R(base,0x82C231F4+0x2C));
  } catch(const std::exception& e) {REXLOG_ERROR("PLAYER2_CAREER scene probe failed: {}",e.what());}
}
}
namespace fm3 { void RestartCall(PPCContext& ctx,uint8_t* base,const char* what,uint32_t a0,uint32_t a1); }
extern "C" void __imp__sub_8231A3F0(PPCContext&,uint8_t*);
extern "C" void sub_8231A3F0(PPCContext& ctx,uint8_t* base) {
  __imp__sub_8231A3F0(ctx,base);
  fm3::RestartCall(ctx,base,"8231A3F0 scene lookup result",ctx.r3.u32,0);
  if(!ctx.r3.u32) {fm3::DescribeSceneZero(ctx,base);ctx.r3.u64=0;}
}
extern "C" void __imp__sub_826FFBD8(PPCContext&,uint8_t*);
extern "C" void sub_826FFBD8(PPCContext& ctx,uint8_t* base) {
  fm3::RestartCall(ctx,base,"826FFBD8 scene message",ctx.r3.u32,ctx.r4.u32);
  if(!ctx.r3.u32) {fm3::MissingBackgroundScene(ctx,base,"scene message",ctx.r4.u32);return;}
  __imp__sub_826FFBD8(ctx,base);
}
extern "C" void __imp__sub_8233DB08(PPCContext&,uint8_t*);
extern "C" void sub_8233DB08(PPCContext& ctx,uint8_t* base) {
  fm3::RestartCall(ctx,base,"8233DB08 current-player update",ctx.r3.u32,ctx.r4.u32);
  if(!ctx.r3.u32) {fm3::MissingBackgroundScene(ctx,base,"current-player update",ctx.r4.u32);return;}
  __imp__sub_8233DB08(ctx,base);
}
// The pause menu's Restart dispatcher (823E2918..823E29E4 inside 823E1D90)
// picks its route from the game mode at state+768: 2 selects the two-player
// prompt 823DF098, anything else falls to the single-player route, which
// needs the background scene. Career co-op keeps mode 1 with two local
// players and no background scene, so present mode 2 for that one dispatch
// and restore it when the handler returns. Stock split-screen already has
// mode 2 and is unaffected; solo is never touched.
namespace fm3 {
namespace {
bool restart_tracing=false;
bool restart_mode_flipped=false;
uint32_t restart_mode_saved=0;
uint32_t RestartModeAddress(uint8_t* base) {const auto g=R(base,0x82C37720);return Ptr(g)?g+16+768:0;}
bool TwoLocalPlayers(uint8_t* base) {return R(base,0x82C231F4)==0x82001CDC&&R(base,0x82C23220)==2;}
}
void RestartDispatchBegins(PPCContext& ctx,uint8_t* base) {
  // Withdrawn: this dispatcher also serves Career menu screens, so changing
  // mode768 here was wrong. Kept as a no-op so the hooks stay inert.
  (void)ctx;(void)base;
}
void RestartDispatchEnds(uint8_t* base) {
  if(restart_tracing) {restart_tracing=false;REXLOG_WARN("PLAYER2_CAREER restart trace ends");}
  if(!restart_mode_flipped) return;
  restart_mode_flipped=false;
  const auto address=RestartModeAddress(base);
  if(address&&R(base,address)==2) W(base,address,restart_mode_saved);
  REXLOG_WARN("PLAYER2_CAREER restart dispatch finished; mode768 restored to {}",restart_mode_saved);
}
std::string GuestClassName(uint8_t* base,uint32_t object);
void RestartCall(PPCContext& ctx,uint8_t* base,const char* what,uint32_t a0,uint32_t a1) {
  if(restart_tracing) REXLOG_WARN("PLAYER2_CAREER restart call: {} caller={:08X} a={:08X} b={:08X} class={}",what,static_cast<uint32_t>(ctx.lr),a0,a1,GuestClassName(base,a0));
}
void RestartTrace(PPCContext& ctx,uint8_t* base,const char* step) {
  if(TwoLocalPlayers(base)||restart_tracing) REXLOG_WARN("PLAYER2_CAREER restart step: {} caller={:08X}",step,static_cast<uint32_t>(ctx.lr));
}
}
// Diagnostic: CUserInterface4 keeps an array of 148-byte layers at
// [ui+76]+12..+16; 827170F0 returns the top scene of a layer (or null when the
// layer's count at +24 is zero). Print each layer's count and top scene class
// whenever the picture changes while the pause menu is handling input.
namespace fm3 {
std::string GuestClassName(uint8_t* base,uint32_t object) {
  if(!Ptr(object)||!Ptr(R(base,object))) return "none";
  const auto locator=R(base,R(base,object)-4);
  const auto type=Ptr(locator)?R(base,locator+12):0;
  try {return Ptr(type)?Text(base,type+8,96):std::string("unknown");} catch(...) {return "unreadable";}
}
// Diagnostic: each local player's garage view name (8227E198 on the user from
// CPlayer vtable +0x50) and how many cars it holds.
void DescribePlayerGarages(PPCContext& ctx,uint8_t* base) {
  for(unsigned i=0;i<2;++i) try {
    const auto player=Player(base,i);
    if(!player) continue;
    Guest g(ctx,base);
    const auto user=g.V(player,0x50);
    const auto view=Text(base,g.Call(0x8227E198,user));
    uint32_t cars=0xFFFFFFFF;
    if(!view.empty()) {const auto row=g.Query("SELECT COUNT(1) FROM "+view);cars=g.V(row,0x98)?0:g.Int(row,0);g.V(row,8);}
    const auto holder=R(base,player+0xAC);
    auto credits=Ptr(holder)?g.Call(0x82261010,holder):0;
    REXLOG_WARN("PLAYER2_PROFILE player{} user={:08X} garage view='{}' cars={} credits={}",i,user,view,cars,credits);
    const auto grant=REXCVAR_GET(player2_starting_credits);
    if(i==1&&REXCVAR_GET(player2_real_profile)&&grant>0&&credits==0&&cars==0&&Ptr(holder)&&Ptr(R(base,holder))) {
      g.Call(0x82263568,holder,static_cast<uint32_t>(grant));
      g.Call(0x82269720,holder);
      REXLOG_WARN("PLAYER2_PROFILE new second profile given starting credits {}",g.Call(0x82261010,holder));
    }
    // One-time catch-up at join: reward cars for driver levels Player 2 already
    // reached before deliveries worked. Done here, not after a race, because
    // the next series can need the reward car. 822F6060 ignores its first
    // argument; it takes the player and the (old, new] native level range.
    if(i==1&&REXCVAR_GET(player2_real_profile)&&Ptr(holder)&&Ptr(R(base,holder))) {
      uint32_t level=0;std::filesystem::path marker;
      {std::lock_guard lock(mutex);if(ready) {level=store.data.level;marker=store.Path().parent_path()/"reward_cars_synced";}}
      std::error_code ec;
      if(!marker.empty()&&!std::filesystem::exists(marker,ec)&&level>1&&level<200) {
        player2_commit_active=true;
        g.Call(0x822F6060,0,player,0,level-1);
        player2_commit_active=false;
        const auto delivered=DeliverQueuedCars(g,base,player);
        g.Call(0x82269720,holder);
        std::ofstream(marker)<<"1";
        REXLOG_WARN("PLAYER2_PROFILE reward car catch-up at join for native levels 1..{}: {} delivered",level-1,delivered);
      }
    }
  } catch(const std::exception& e) {REXLOG_ERROR("PLAYER2_PROFILE garage probe player{} failed: {}",i,e.what());}
  // Cars noted by an earlier session that closed before the hand-over.
  DeliverPendingCars(ctx,base);
}
// Pure memory reads (no guest calls), so it is safe from the frame-stats hook.
// Each layer holds a std::list of entries whose scene pointer is at node+12.
std::string UiLayersSummary(uint8_t* base) {
  const auto ui=R(base,0x82C76C1C);
  const auto stack=Ptr(ui)?R(base,ui+76):0;
  if(!Ptr(stack)) return "ui not ready";
  const auto begin=R(base,stack+12),end=R(base,stack+16);
  if(!Ptr(begin)||end<begin||end-begin>148*32) return "layers unreadable";
  std::string text=fmt::format("players={}",R(base,0x82C231F4)==0x82001CDC?R(base,0x82C231F4+0x2C):0);
  for(uint32_t layer=begin,index=0;layer<end;layer+=148,++index) {
    const auto count=R(base,layer+24),head=R(base,layer+20);
    text+=fmt::format(" | L{} n={}:",index,count);
    if(!count||!Ptr(head)) continue;
    auto node=R(base,head);
    for(unsigned i=0;i<8&&Ptr(node)&&node!=head;++i,node=R(base,node)) {
      auto name=GuestClassName(base,R(base,node+12));
      if(name.rfind(".?AV",0)==0) name=name.substr(4);
      if(const auto at=name.find("@");at!=std::string::npos) name.resize(at);
      text+=" "+name;
    }
  }
  return text;
}
}
extern "C" void __imp__sub_82362F00(PPCContext&,uint8_t*);
extern "C" void sub_82362F00(PPCContext& ctx,uint8_t* base) {
  const auto caller=static_cast<uint32_t>(ctx.lr);
  __imp__sub_82362F00(ctx,base);
  if(caller==0x823E2920) {const auto result=ctx.r3.u64;fm3::RestartDispatchBegins(ctx,base);ctx.r3.u64=result;}
}
extern "C" void __imp__sub_823E1D90(PPCContext&,uint8_t*);
extern "C" void sub_823E1D90(PPCContext& ctx,uint8_t* base) {
  __imp__sub_823E1D90(ctx,base);fm3::RestartDispatchEnds(base);
}
extern "C" void __imp__sub_8231A9B8(PPCContext&,uint8_t*);
extern "C" void sub_8231A9B8(PPCContext& ctx,uint8_t* base) {
  fm3::RestartCall(ctx,base,"8231A9B8 environment switch",ctx.r3.u32,ctx.r4.u32);__imp__sub_8231A9B8(ctx,base);
}
#define FM3_RESTART_STEP(address,label) extern "C" void __imp__sub_##address(PPCContext&,uint8_t*); extern "C" void sub_##address(PPCContext& ctx,uint8_t* base) {   fm3::RestartTrace(ctx,base,label);__imp__sub_##address(ctx,base); }
FM3_RESTART_STEP(823DF098,"two-player restart prompt 823DF098")
FM3_RESTART_STEP(823DFF78,"single-player restart prompt 823DFF78")
FM3_RESTART_STEP(823E2E50,"prompt answer handler 823E2E50")
FM3_RESTART_STEP(823DF648,"two-player confirm 823DF648")
FM3_RESTART_STEP(823E1C40,"restart 823E1C40")
FM3_RESTART_STEP(823E0290,"route 823E0290")
extern "C" void __imp__sub_8230D978(PPCContext&,uint8_t*);
extern "C" void sub_8230D978(PPCContext& ctx,uint8_t* base) {
  fm3::CareerRaceSettingsRefresh(ctx,base,__imp__sub_8230D978);
}
#define FM3_P2_COMMIT_SILENT(address,label) extern "C" void __imp__sub_##address(PPCContext&,uint8_t*); extern "C" void sub_##address(PPCContext& ctx,uint8_t* base) {   if(player2_commit_active) {REXLOG_WARN("PLAYER2_PROFILE suppressed UI event during Player 2 commit: " label " value={}",ctx.r4.u32);return;}   __imp__sub_##address(ctx,base); }
FM3_P2_COMMIT_SILENT(822C0B60,"new reward car")
FM3_P2_COMMIT_SILENT(822C0790,"credits in place of car")
FM3_P2_COMMIT_SILENT(822C07C8,"level change")
extern "C" void __imp__sub_822F6CA0(PPCContext&,uint8_t*);
extern "C" void sub_822F6CA0(PPCContext& ctx,uint8_t* base) {
  const auto component=ctx.r3.u32;__imp__sub_822F6CA0(ctx,base);fm3::ResetRewards(component);
}
extern "C" void __imp__sub_822F5E30(PPCContext&,uint8_t*);
extern "C" void sub_822F5E30(PPCContext& ctx,uint8_t* base) {
  const auto component=ctx.r3.u32,player=ctx.r4.u32,position=ctx.r5.u32,output=ctx.r7.u32;
  __imp__sub_822F5E30(ctx,base);fm3::TraceCalc(base,player,position,output);fm3::CaptureRewards(base,component,player,position,output);
}
extern "C" void __imp__sub_822F6388(PPCContext&,uint8_t*);
extern "C" void sub_822F6388(PPCContext& ctx,uint8_t* base) {
  const auto component=ctx.r3.u32,player=ctx.r4.u32;
  const auto wallet=fm3::CreditsOf(ctx,base,player);
  fm3::TraceCommit(ctx,base,component,player,false,wallet);
  __imp__sub_822F6388(ctx,base);
  fm3::TraceCommit(ctx,base,component,player,true,wallet);
  fm3::CommitRewards(ctx,base,component);
}
// Every credit change through the wallet's add function, with its caller.
extern "C" void __imp__sub_82263568(PPCContext&,uint8_t*);
extern "C" void sub_82263568(PPCContext& ctx,uint8_t* base) {
  static std::atomic<unsigned> notes{0};
  if(notes.fetch_add(1)<400) REXLOG_WARN("REWARD_TRACE wallet {:08X} set to {} caller={:08X}",ctx.r3.u32,int32_t(ctx.r4.u32),uint32_t(ctx.lr));
  __imp__sub_82263568(ctx,base);
}

extern "C" void __imp__sub_823D9648(PPCContext&,uint8_t*);
extern "C" void sub_823D9648(PPCContext& ctx,uint8_t* base) {
  const auto screen=ctx.r3.u32;__imp__sub_823D9648(ctx,base);
  fm3::NativeCarMenuOpened(ctx,base,screen);
}
extern "C" void sub_823D8F90(PPCContext& ctx,uint8_t* base) {
  if(!fm3::BeginNativePlayer2(ctx,base)) __imp__sub_823D8F90(ctx,base);
}
extern "C" void __imp__sub_823D9198(PPCContext&,uint8_t*);
extern "C" void sub_823D9198(PPCContext& ctx,uint8_t* base) {
  if(!fm3::SelectNativePlayer2(ctx,base)) __imp__sub_823D9198(ctx,base);
}
extern "C" void __imp__sub_823D8F40(PPCContext&,uint8_t*);
extern "C" void sub_823D8F40(PPCContext& ctx,uint8_t* base) {
  fm3::NativeCarMenuClosed(ctx,base,ctx.r3.u32);__imp__sub_823D8F40(ctx,base);
}
extern "C" void __imp__sub_823D8998(PPCContext&,uint8_t*);
extern "C" void sub_823D8998(PPCContext& ctx,uint8_t* base) {
  fm3::NativeCareerCalendarOpening(ctx,base,"race-info");__imp__sub_823D8998(ctx,base);
}
extern "C" void __imp__sub_82343B80(PPCContext&,uint8_t*);
extern "C" void sub_82343B80(PPCContext& ctx,uint8_t* base) {
  // The season calendar has a separate activation path from CRaceInfo.
  // Restore before 82343B90 resolves the profile and caches its calendar
  // at screen+19C; doing it in 823435F0 is too late to repair that cache.
  fm3::NativeCareerCalendarOpening(ctx,base,"season-calendar");__imp__sub_82343B80(ctx,base);
}
extern "C" void __imp__sub_823504E0(PPCContext&,uint8_t*);
extern "C" void sub_823504E0(PPCContext& ctx,uint8_t* base) {
  if(fm3::OpenDirectPlayer2(ctx,base)) return;
  if(!fm3::BackFromNativePlayer2(ctx,base)) __imp__sub_823504E0(ctx,base);
}
extern "C" void __imp__sub_823D75C0(PPCContext&,uint8_t*);
extern "C" void sub_823D75C0(PPCContext& ctx,uint8_t* base) {
  if(!fm3::StartRaceNeedsPlayer2(ctx,base)) __imp__sub_823D75C0(ctx,base);
}
// Starting grid. 822F2300 sorts the cars and writes each one's grid slot to
// the byte at car+0x2EA4 (0 = pole). In Career it then moves only the first
// human it finds to the event's grid position, so the second local player
// keeps a sorted slot near the front. With two local players, move the human
// who is further forward to the slot directly ahead of the other human and
// close the gap behind them.
REXCVAR_DEFINE_BOOL(coop_grid_players_together, true, "FM3/Research",
    "Co-op: start the second local player directly ahead of the first instead of near the front");
extern "C" void __imp__sub_822F2300(PPCContext&,uint8_t*);
extern "C" void sub_822F2300(PPCContext& ctx,uint8_t* base) {
  __imp__sub_822F2300(ctx,base);
  if(!REXCVAR_GET(coop_grid_players_together)||!fm3::TwoLocalPlayers(base)) return;
  const auto result=ctx.r3.u64;
  try {
    fm3::Guest g(ctx,base);
    const auto list=g.Call(0x823023A8,fm3::R(base,g.Call(0x822110A0,fm3::R(base,0x82C2111C),0)));
    const auto begin=fm3::R(base,list+0xC),end=fm3::R(base,list+0x10);
    if(!fm3::Ptr(begin)||end<begin||end-begin>4*32) throw std::runtime_error("unexpected car list");
    struct Entry {uint32_t car;uint8_t slot;bool human;};
    std::vector<Entry> cars;
    for(auto at=begin;at<end;at+=4) {
      const auto car=fm3::R(base,at);
      if(!fm3::Ptr(car)||base[car+0x2EE2]) continue;
      bool human=false;
      if(const auto driver=g.Call(0x82214920,car)) human=(g.Call(0x8221F8C0,g.V(driver,0x58))&0xFF)!=0;
      cars.push_back({car,base[car+0x2EA4],human});
    }
    std::vector<Entry*> humans;
    for(auto& c:cars) if(c.human) humans.push_back(&c);
    std::string before;
    for(const auto& c:cars) before+=fmt::format(" {}{}",c.slot,c.human?"H":"");
    if(humans.size()==2) {
      auto* front=humans[0]->slot<humans[1]->slot?humans[0]:humans[1];
      auto* back=front==humans[0]?humans[1]:humans[0];
      const uint8_t from=front->slot,to=static_cast<uint8_t>(back->slot-1);
      if(to>from) {
        for(auto& c:cars) if(c.slot>from&&c.slot<=to) base[c.car+0x2EA4]=--c.slot;
        base[front->car+0x2EA4]=front->slot=to;
      }
      REXLOG_WARN("COOP_GRID slots before:{} | front human {} -> {} (other human at {})",before,from,front->slot,back->slot);
    } else REXLOG_WARN("COOP_GRID unchanged, {} human cars; slots:{}",humans.size(),before);
  } catch(const std::exception& e) {REXLOG_ERROR("COOP_GRID failed: {}",e.what());}
  ctx.r3.u64=result;
}

// See ApplyPlayer2Assists.
extern "C" void sub_82482188(PPCContext& ctx,uint8_t* base) {
  if(const auto player=fm3::AssistOverridePlayer()) {ctx.r3.u64=player;return;}
  __imp__sub_82482188(ctx,base);
}
extern "C" void sub_822714D0(PPCContext& ctx,uint8_t* base) {
  const auto arg3=ctx.r3.u64,arg4=ctx.r4.u64;
  __imp__sub_822714D0(ctx,base);
  fm3::ApplyPlayer2Assists(ctx,base,arg3,arg4);
}
// See PayPlayer2Series.
extern "C" void sub_82231A28(PPCContext& ctx,uint8_t* base) {
  const auto record=ctx.r4.u32;
  __imp__sub_82231A28(ctx,base);
  fm3::PayPlayer2Series(ctx,base,record);
}

// See AeroSliderUnlocked.
extern "C" void __imp__sub_822C7D10(PPCContext&,uint8_t*);
extern "C" void sub_822C7D10(PPCContext& ctx,uint8_t* base) {
  const auto tune=ctx.r3.u32;
  __imp__sub_822C7D10(ctx,base);
  if(!(ctx.r3.u32&1)&&fm3::AeroSliderUnlocked(base,tune,true)) ctx.r3.u64=1;
}
extern "C" void __imp__sub_822C7D30(PPCContext&,uint8_t*);
extern "C" void sub_822C7D30(PPCContext& ctx,uint8_t* base) {
  const auto tune=ctx.r3.u32;
  __imp__sub_822C7D30(ctx,base);
  if(!(ctx.r3.u32&1)&&fm3::AeroSliderUnlocked(base,tune,false)) ctx.r3.u64=1;
}
