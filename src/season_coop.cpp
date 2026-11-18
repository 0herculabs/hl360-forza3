// Local Season Play entry. Reuse the game's guest join/profile lifecycle.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(season_coop_menu, false, "FM3/Profile",
    "Solo/co-op choice for Season Play; Career Multiplayer starts local co-op Season Play");
REXCVAR_DEFINE_BOOL(season_coop_prompt, false, "FM3/Profile",
    "Ask Solo or Co-op when Season Play is selected; off: Season Play is solo, Multiplayer is co-op");
REXCVAR_DECLARE(bool, career_guest_retain);
REXCVAR_DECLARE(bool, player2_real_profile);
// Player 2's controller index for the race HUD (career_trace.cpp); 0xFF = none.
namespace fm3 { void DescribePlayerGarages(PPCContext& ctx, uint8_t* base); }
extern "C" uint32_t fm3_coop_second_controller;
extern "C" void fm3_set_coop_second_input(uint8_t* base);
extern "C" void fm3_clear_coop_second_input(uint8_t* base);

namespace {
constexpr uint32_t manager=0x82C231F4;
uint32_t R(uint8_t* b,uint32_t p) {uint32_t v;std::memcpy(&v,b+p,4);return std::byteswap(v);}
bool Ptr(uint32_t p) {return p>=0x10000 && p<0xFFFF0000;}
unsigned Count(uint8_t* b) {return R(b,manager)==0x82001CDC?R(b,manager+0x2C):0;}
uint32_t Player(uint8_t* b,unsigned index) {
  if(Count(b)<1 || Count(b)>2 || index>=Count(b)) return 0;
  const auto sentinel=R(b,manager+0x28);auto node=R(b,sentinel);
  for(unsigned i=0;i<index && node!=sentinel;++i) node=R(b,node);
  return node==sentinel?0:R(b,node+8);
}
struct Guest {
  PPCContext ctx;uint8_t* b;uint32_t scratch;
  Guest(const PPCContext& c,uint8_t* base):ctx(c),b(base),scratch(c.r1.u32-0x1000) {
    ctx.r1.u64=scratch-0x100;std::memset(b+scratch,0,0x1000);
  }
  uint32_t Call(uint32_t fn,uint32_t a=0,uint32_t d=0,uint32_t e=0) {
    ctx.r3.u64=a;ctx.r4.u64=d;ctx.r5.u64=e;
    rex::runtime::ResolveIndirectFunction(fn)(ctx,b);return ctx.r3.u32;
  }
  uint32_t V(uint32_t obj,uint32_t slot,uint32_t arg=0) {
    if(!Ptr(obj)||!Ptr(R(b,obj))) throw std::runtime_error("Invalid local player object.");
    return Call(R(b,R(b,obj)+slot),obj,arg);
  }
  bool Input(unsigned index,uint16_t* buttons=nullptr) {
    if(Call(0x82B668EC,index,0,scratch)!=0) return false;
    if(buttons) *buttons=static_cast<uint16_t>((b[scratch+4]<<8)|b[scratch+5]);
    return true;
  }
  unsigned Controller(uint32_t player) {
    const auto profile=V(player,0x44);
    if(!Ptr(profile)) throw std::runtime_error("The local player profile is not ready.");
    const auto index=Call(0x82B28530,profile);
    if(index>=4) throw std::runtime_error("Unexpected controller assignment.");
    return index;
  }
};
enum class Mode {Cancel=0,Solo=101,Coop=102};
struct Choice {Guest* guest;unsigned controller;Mode result=Mode::Cancel;uint16_t last=0;bool armed=false;};
LRESULT CALLBACK ChoiceProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
  auto* p=reinterpret_cast<Choice*>(GetWindowLongPtrW(window,GWLP_USERDATA));
  if(message==WM_NCCREATE) {
    p=static_cast<Choice*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(p));
  }
  if(message==WM_CREATE) {
    const auto font=GetStockObject(DEFAULT_GUI_FONT);
    auto control=[&](const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id) {
      auto c=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
      SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return c;
    };
    control(L"STATIC",L"How would you like to play this season?",0,24,20,480,28,0);
    control(L"STATIC",L"Co-op uses two controllers. Each player keeps their own points and progress, sharing Player 1's garage and event rules.",0,24,55,480,50,0);
    auto solo=control(L"BUTTON",L"Solo",WS_TABSTOP|BS_DEFPUSHBUTTON,24,120,148,42,101);
    control(L"BUTTON",L"Co-op",WS_TABSTOP,188,120,148,42,102);
    control(L"BUTTON",L"Cancel",WS_TABSTOP,352,120,148,42,IDCANCEL);
    control(L"STATIC",L"D-pad: choose    A: select    B: cancel\nMouse or Tab / Enter also work.",0,24,181,480,40,0);
    SetFocus(solo);SetTimer(window,1,30,nullptr);return 0;
  }
  if(message==WM_COMMAND && p) {
    const int id=LOWORD(wp);
    if(id==101||id==102||id==IDCANCEL) {
      p->result=id==101?Mode::Solo:id==102?Mode::Coop:Mode::Cancel;
      DestroyWindow(window);return 0;
    }
  }
  if(message==WM_TIMER && p) {
    uint16_t buttons=0;
    if(!p->guest->Input(p->controller,&buttons)) return 0;
    // Ignore the A press that opened Season Play until it is released.
    if(!p->armed) {p->armed=(buttons&0x300F)==0;p->last=buttons;return 0;}
    const auto pressed=static_cast<uint16_t>(buttons&~p->last);p->last=buttons;
    if(pressed&0x2000) {SendMessageW(window,WM_COMMAND,IDCANCEL,0);return 0;}
    int id=GetDlgCtrlID(GetFocus());
    if(pressed&0x000F) {
      const int ids[]={101,102,IDCANCEL};int at=id==102?1:id==IDCANCEL?2:0;
      at=(at+((pressed&0x0005)?2:1))%3;SetFocus(GetDlgItem(window,ids[at]));
    } else if(pressed&0x1000) SendMessageW(window,WM_COMMAND,id==102?102:id==IDCANCEL?IDCANCEL:101,0);
    return 0;
  }
  if(message==WM_CLOSE) {DestroyWindow(window);return 0;}
  if(message==WM_DESTROY) {KillTimer(window,1);return 0;}
  return DefWindowProcW(window,message,wp,lp);
}
Mode Choose(Guest& g,unsigned controller) {
  static std::once_flag once;
  std::call_once(once,[]{WNDCLASSW wc{};wc.lpfnWndProc=ChoiceProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"FM3SeasonCoopChoice";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassW(&wc);});
  Choice choice{&g,controller};
  auto window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_CONTROLPARENT,L"FM3SeasonCoopChoice",L"Season Play - Solo or Co-op",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_VISIBLE,std::max(0,(GetSystemMetrics(SM_CXSCREEN)-540)/2),std::max(0,(GetSystemMetrics(SM_CYSCREEN)-270)/2),540,270,nullptr,nullptr,GetModuleHandleW(nullptr),&choice);
  if(!window) throw std::runtime_error("Could not open the Season Play choice.");
  SetForegroundWindow(window);MSG msg{};
  while(IsWindow(window)&&GetMessageW(&msg,nullptr,0,0)>0) {
    if(msg.message==WM_KEYDOWN && (msg.wParam==VK_RETURN || msg.wParam==VK_ESCAPE)) {
      const int id=msg.wParam==VK_ESCAPE?IDCANCEL:GetDlgCtrlID(GetFocus());
      SendMessageW(window,WM_COMMAND,id==102?102:id==IDCANCEL?IDCANCEL:101,0);continue;
    }
    if(!IsDialogMessageW(window,&msg)) {TranslateMessage(&msg);DispatchMessageW(&msg);}
  }
  return choice.result;
}
void SetMode(Guest& g,Mode mode) {
  auto* b=g.b;const auto host=Player(b,0);
  if(!host) throw std::runtime_error("Enter Career with Player 1 first.");
  const auto hostController=g.Controller(host);
  if(mode==Mode::Solo) {
    if(Count(b)==2) {
      // Original split-screen teardown removes P2 and unloads the guest profile.
      // Our retain hook applies only to the stock split-screen exit callers.
      g.ctx.lr=0;g.Call(0x822C23D0);
    }
    if(Count(b)!=1 || Player(b,0)!=host) throw std::runtime_error("Could not switch to Solo. Return to the main menu before trying again.");
    fm3_clear_coop_second_input(b);
    fm3_coop_second_controller=0xFF;
    REXLOG_WARN("SEASON_COOP solo ready controller={} local_count=1",hostController);return;
  }
  if(!REXCVAR_GET(career_guest_retain)) throw std::runtime_error("Career co-op is not enabled in this build's configuration.");
  if(Count(b)==2) {
    if(!g.Input(g.Controller(Player(b,1)))) throw std::runtime_error("Connect Player 2's controller, then select Co-op again.");
    fm3_coop_second_controller=g.Controller(Player(b,1));
    REXLOG_WARN("SEASON_COOP using existing second player");return;
  }
  unsigned second=4;
  for(unsigned i=0;i<4;++i) if(i!=hostController && g.Input(i)) {second=i;break;}
  if(second==4) throw std::runtime_error("A second controller was not detected. Connect it, then select Co-op again.");
  const auto services=R(b,0x82C2111C);
  if(!Ptr(services)) throw std::runtime_error("Career services are not ready.");
  const auto coordinator=R(b,services+0xA8);
  if(!Ptr(coordinator) || R(b,coordinator+0x18)!=manager) throw std::runtime_error("The local player manager is not ready.");
  // Exact native Guest selection sequence from 823DEA48: create a guest
  // sign-in object for this controller, then join/load through the coordinator.
  const auto signins=g.Call(0x82754428);
  // With the second-profile experiment the runtime reports controller 1 as a
  // signed-in profile; creating a guest sign-in would replace it.
  if(REXCVAR_GET(player2_real_profile)&&second==1) REXLOG_WARN("SEASON_COOP joining controller 1 as a signed-in profile (no guest sign-in)");
  else g.V(signins,0x10,second);
  const auto joined=g.Call(0x8226A5E0,coordinator,second);
  const auto player=Player(b,1);
  if(!joined || Count(b)!=2 || !player || !R(b,player+0xAC) || !R(b,player+0xB0) || !R(b,player+0xB8)) {
    if(Count(b)==2) {g.ctx.lr=0;g.Call(0x822C23D0);}
    throw std::runtime_error("Player 2's guest profile could not be loaded. Return to the main menu and try again.");
  }
  fm3_coop_second_controller=second;
  REXLOG_WARN("SEASON_COOP joined controller={} player={:08X} profile={:08X} car={:08X}",second,player,R(b,player+0xAC),R(b,player+0xB8));
}
}

extern "C" void __imp__sub_82394FE0(PPCContext&,uint8_t*);
extern "C" void sub_82394FE0(PPCContext& ctx,uint8_t* base) {
  const auto action=ctx.r4.u32;
  // With a real second profile, the Career screens (My Cars, Buy Cars, ...)
  // show the player manager's current player: 824821E8 returns the player
  // for the controller index at manager+132. Log077: entries opened with
  // Player 2's controller still showed Player 1's garage and credits. Make
  // the current player the one whose controller selected this main-menu
  // entry; the two race entries (9, 11) always belong to Player 1.
  if(REXCVAR_GET(player2_real_profile)&&Count(base)==2) try {
    Guest g(ctx,base);
    const auto host=g.Controller(Player(base,0)),second=g.Controller(Player(base,1));
    const auto input=R(base,0x82C21118);
    const uint8_t last=Ptr(input)?base[input+0x6E78]:0xFF;
    const uint8_t target=static_cast<uint8_t>(action!=9&&action!=11&&last==second?second:host);
    if(host<4&&second<4&&base[manager+132]!=target) {
      base[manager+132]=target;
      // The credit readout lives on the menu background (CBackgroundMain,
      // 8231A3F0) and is refreshed by 8233DB08(background, player), which
      // reads the player's balance with 82261010(profile holder).
      const auto player=Player(base,target==second?1:0);
      const auto background=g.Call(0x8231A3F0,R(base,0x82C37720));
      const auto credits0=g.Call(0x82261010,R(base,Player(base,0)+0xAC));
      const auto credits1=g.Call(0x82261010,R(base,Player(base,1)+0xAC));
      if(Ptr(background)&&player) g.Call(0x8233DB08,background,player);
      // The background also keeps "the car being worked on" (number, paint,
      // fitted parts). Left on the other player's car, the upgrade shop
      // previews this player's part on that car and the part lookup returns
      // nothing (crash_20261114_100724: springs and dampers as Player 2).
      // 8230D978 is the game's own "show the current player's car".
      if(Ptr(background)&&player) g.Call(0x8230D978);
      REXLOG_WARN("SEASON_COOP main menu action={} by controller {}: current player is now controller {} (credits: player 1 {}, player 2 {})",action,last,target,credits0,credits1);
    }
  } catch(const std::exception& e) {REXLOG_ERROR("SEASON_COOP current-player switch failed: {}",e.what());}
  // Career menu action9: Season Play; action11: Multiplayer. The original
  // Season handler still owns calendar selection and the screen transition.
  if(!REXCVAR_GET(season_coop_menu) || (action!=9 && action!=11)) {
    __imp__sub_82394FE0(ctx,base);return;
  }
  REXLOG_WARN("SEASON_COOP menu action={} local_count={}",action,Count(base));
  try {
    Guest g(ctx,base);
    // Without the prompt the two menu entries are the choice: Season Play
    // is solo and Multiplayer is co-op.
    const auto mode=action==11?Mode::Coop:
        REXCVAR_GET(season_coop_prompt)?Choose(g,g.Controller(Player(base,0))):Mode::Solo;
    if(mode==Mode::Cancel) {ctx.r3.u64=0;return;}
    const auto before=Count(base);
    const auto screen=ctx.r3.u32,screen_vtable=R(base,screen);
    SetMode(g,mode);
    if(REXCVAR_GET(player2_real_profile)&&mode==Mode::Coop&&before==1) {
      // Mounting and loading a real save for Player 2 is not the in-memory
      // default the stock join makes; continuing into Season Play with this
      // handler's screen pointer crashed (crash_20261112_133809, garbage read
      // in the menu's action map). Stop here; selecting Multiplayer again
      // takes the "existing second player" path.
      REXLOG_WARN("SEASON_COOP Player 2 profile joined (screen {:08X} vtable {:08X} -> {:08X}); select Multiplayer again to continue",screen,screen_vtable,R(base,screen));
      fm3::DescribePlayerGarages(ctx,base);
      fm3_set_coop_second_input(base);
      ctx.r3.u64=0;return;
    }
    ctx.r4.u64=9;__imp__sub_82394FE0(ctx,base);
  } catch(const std::exception& e) {
    REXLOG_ERROR("SEASON_COOP {}",e.what());
    MessageBoxA(nullptr,e.what(),"Season Play",MB_OK|MB_ICONINFORMATION|MB_TOPMOST);
    ctx.r3.u64=0;
  }
}
