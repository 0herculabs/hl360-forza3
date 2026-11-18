#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Display-only companion. No game memory, saves, input injection, audio,
// desktop capture, display-mode changes, or modifications to the game window.
namespace {
struct Monitor { RECT rect; std::wstring name; bool primary; };
struct View { HWND window{}; HTHUMBNAIL thumbnail{}; };
std::vector<Monitor> monitors;
std::array<View,2> views;
HWND source{},panel{},status{};
DWORD requested_pid=0;
std::wstring expected;
std::ofstream log_file;
int mode=0; // 0 hidden, 1 split race, 2 whole game (menus)
bool swapped=false,side_by_side=false;
// Stretch each player's strip over the whole monitor. Only looks right when the
// game draws squashed 16:9 views (split_fullscreen_views = true).
bool fill=false;
// The game publishes 1 in the shared value FM3RaceView_<pid> while the split-screen race
// HUD is the top screen, 0 otherwise (pause menu, loading, results, menus).
// With views shown, follow it: one player per monitor only while racing,
// the whole picture on both monitors for anything else.
HANDLE race_map{};const volatile LONG* race_flag{};
DWORD source_process=0;
int RaceState() {
  if(!race_flag&&source_process) {
    const auto name=L"Local\\FM3RaceView_"+std::to_wstring(source_process);
    race_map=OpenFileMappingW(FILE_MAP_READ,FALSE,name.c_str());
    if(race_map) race_flag=static_cast<const volatile LONG*>(MapViewOfFile(race_map,FILE_MAP_READ,0,0,sizeof(LONG)));
  }
  return race_flag?(*race_flag!=0):-1;
}
RECT last_client{};
bool dirty=true;

void Log(const std::string& s) {
  SYSTEMTIME t{};GetLocalTime(&t);char stamp[16];
  wsprintfA(stamp,"%02d:%02d:%02d ",t.wHour,t.wMinute,t.wSecond);
  log_file<<stamp<<s<<std::endl;
}
// Windows was seen to accept HWND_TOPMOST for the view on the game's monitor
// and leave it non-topmost behind the game window; clearing the state first
// (HWND_NOTOPMOST) makes the request take effect.
void Place(HWND w,const RECT& r) {
  if(!(GetWindowLongPtrW(w,GWL_EXSTYLE)&WS_EX_TOPMOST))
    SetWindowPos(w,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
  SetWindowPos(w,HWND_TOPMOST,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE|SWP_SHOWWINDOW);
}
std::filesystem::path Folder() {
  wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);
  return std::filesystem::path(path).parent_path();
}
BOOL CALLBACK MonitorEnum(HMONITOR h,HDC,LPRECT,LPARAM) {
  MONITORINFOEXW info{};info.cbSize=sizeof(info);
  if(GetMonitorInfoW(h,&info)) monitors.push_back({info.rcMonitor,info.szDevice,(info.dwFlags&MONITORINFOF_PRIMARY)!=0});
  return TRUE;
}
void ReadMonitors() {
  monitors.clear();EnumDisplayMonitors(nullptr,nullptr,MonitorEnum,0);
  std::stable_sort(monitors.begin(),monitors.end(),[](const Monitor& a,const Monitor& b){
    if(a.primary!=b.primary) return a.primary;
    if(a.rect.left!=b.rect.left) return a.rect.left<b.rect.left;
    return a.rect.top<b.rect.top;
  });
  Log("Monitors="+std::to_string(monitors.size()));
  for(const auto& m:monitors) Log("display rect="+std::to_string(m.rect.left)+","+std::to_string(m.rect.top)+","+std::to_string(m.rect.right)+","+std::to_string(m.rect.bottom)+" primary="+std::to_string(m.primary));
}
BOOL CALLBACK SourceEnum(HWND w,LPARAM) {
  if(!IsWindowVisible(w)||GetWindow(w,GW_OWNER)) return TRUE;
  DWORD pid=0;GetWindowThreadProcessId(w,&pid);
  if(requested_pid&&pid!=requested_pid) return TRUE;
  HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
  if(!p) return TRUE;
  wchar_t path[32768]{};DWORD length=32768;
  const bool match=QueryFullProcessImageNameW(p,0,path,&length)&&_wcsicmp(path,expected.c_str())==0;
  CloseHandle(p);
  if(match) {source=w;return FALSE;}return TRUE;
}
RECT Slice(RECT r,int player,bool side) {
  if(side) {LONG mid=r.left+(r.right-r.left)/2;if(player==0) r.right=mid;else r.left=mid;}
  else {LONG mid=r.top+(r.bottom-r.top)/2;if(player==0) r.bottom=mid;else r.top=mid;}
  return r;
}
RECT Fit(RECT src,LONG width,LONG height) {
  const LONG sw=src.right-src.left,sh=src.bottom-src.top;
  if(sw<=0||sh<=0||width<=0||height<=0) return {};
  const double scale=std::min(double(width)/sw,double(height)/sh);
  const LONG w=std::max(1L,LONG(sw*scale)),h=std::max(1L,LONG(sh*scale));
  const LONG x=(width-w)/2,y=(height-h)/2;return {x,y,x+w,y+h};
}
void TextStatus(const std::wstring& s) {if(status) SetWindowTextW(status,s.c_str());}
// Sharp full-screen views: with --fill the game stretches its picture over its
// whole window (present_letterbox = false), so a borderless window one monitor
// wide and two monitors tall gives each player's half a full monitor of real
// pixels. The lower half hangs off the bottom of the screen; the views copy
// it from there. The original frame, size and position come back on Hide.
bool tall=false;LONG_PTR saved_style=0;WINDOWPLACEMENT saved_place{sizeof(WINDOWPLACEMENT)};
void MakeTall() {
  if(tall||!fill||!IsWindow(source)||monitors.empty()) return;
  saved_place.length=sizeof(saved_place);
  if(!GetWindowPlacement(source,&saved_place)) return;
  saved_style=GetWindowLongPtrW(source,GWL_STYLE);
  if(IsZoomed(source)) ShowWindow(source,SW_RESTORE);
  SetWindowLongPtrW(source,GWL_STYLE,(saved_style&~(WS_CAPTION|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_SYSMENU|WS_MAXIMIZE))|WS_POPUP);
  const RECT m=monitors[0].rect;const LONG w=m.right-m.left,h=m.bottom-m.top;
  SetWindowPos(source,nullptr,m.left,m.top,w,h*2,SWP_FRAMECHANGED|SWP_NOZORDER|SWP_NOACTIVATE);
  RECT now{};GetClientRect(source,&now);
  Log("game window made tall: client="+std::to_string(now.right)+"x"+std::to_string(now.bottom));
  tall=true;
}
void RestoreWindow() {
  if(!tall) return;
  tall=false;
  if(!IsWindow(source)) return;
  SetWindowLongPtrW(source,GWL_STYLE,saved_style);
  SetWindowPos(source,nullptr,0,0,0,0,SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
  SetWindowPlacement(source,&saved_place);
  Log("game window restored");
}
void Hide() {
  RestoreWindow();
  for(auto& v:views) if(v.window) ShowWindow(v.window,SW_HIDE);
  mode=0;dirty=true;
  TextStatus(L"Displays hidden. The normal game window is unchanged.");
}
void ReleaseViews() {
  for(auto& v:views) {
    if(v.thumbnail) DwmUnregisterThumbnail(v.thumbnail);
    v.thumbnail=nullptr;
    if(v.window) DestroyWindow(v.window);
    v.window=nullptr;
  }
}
bool Refresh() {
  if(!mode) return true;
  if(!IsWindow(source)||IsIconic(source)||monitors.size()<2) {Hide();TextStatus(L"Keep the game open and restored, with two monitors connected.");return false;}
  RECT client{};if(!GetClientRect(source,&client)||client.right<=0||client.bottom<=0) return false;
  if(!dirty&&EqualRect(&client,&last_client)) {
    // Re-assert every tick: the view sharing the game's monitor was found
    // without its topmost flag, behind the game window.
    for(int i=0;i<2;++i) {
      // Only when the flag was lost: re-raising every tick would also cover
      // other on-top tools such as the Snipping Tool overlay.
      if(!(GetWindowLongPtrW(views[i].window,GWL_EXSTYLE)&WS_EX_TOPMOST)||!IsWindowVisible(views[i].window)) Place(views[i].window,monitors[i].rect);
    }
    return true;
  }
  last_client=client;
  // Letterboxed presentation uses the game's 16:9 output inside its client.
  // Remove only the outer presentation bars before splitting, never HUD pixels.
  RECT game=fill?client:Fit({0,0,16,9},client.right,client.bottom);
  for(int i=0;i<2;++i) {
    const int player=swapped?1-i:i;
    RECT crop=mode==1?Slice(game,player,side_by_side):game;
    const auto& monitor=monitors[i];const LONG w=monitor.rect.right-monitor.rect.left,h=monitor.rect.bottom-monitor.rect.top;
    DWM_THUMBNAIL_PROPERTIES props{};
    props.dwFlags=DWM_TNP_RECTDESTINATION|DWM_TNP_RECTSOURCE|DWM_TNP_OPACITY|DWM_TNP_VISIBLE|DWM_TNP_SOURCECLIENTAREAONLY;
    props.rcSource=crop;props.rcDestination=fill?RECT{0,0,w,h}:Fit(crop,w,h);props.opacity=255;props.fVisible=TRUE;props.fSourceClientAreaOnly=TRUE;
    const HRESULT hr=DwmUpdateThumbnailProperties(views[i].thumbnail,&props);
    if(FAILED(hr)) {Log("DwmUpdateThumbnailProperties failed="+std::to_string(hr));Hide();TextStatus(L"Windows could not display the live game. See FM3-dual-monitor.log.");return false;}
    SetWindowTextW(views[i].window,(L"FM3 - Player "+std::to_wstring(player+1)+L" monitor").c_str());
    Place(views[i].window,monitor.rect);
    Log("view="+std::to_string(i)+" player="+std::to_string(player+1)+" crop="+std::to_string(crop.left)+","+std::to_string(crop.top)+","+std::to_string(crop.right)+","+std::to_string(crop.bottom)+" dest="+std::to_string(props.rcDestination.left)+","+std::to_string(props.rcDestination.top)+","+std::to_string(props.rcDestination.right)+","+std::to_string(props.rcDestination.bottom));
  }
  dirty=false;return true;
}
void Show(int next) {
  if(monitors.size()<2) {TextStatus(L"Two active monitors are required.");return;}
  if(!IsWindow(source)) {TextStatus(L"Launch the Career game first, then reopen this companion.");return;}
  mode=next;dirty=true;MakeTall();
  if(Refresh()) {
    Log("mode="+std::to_string(mode)+" race flag "+std::to_string(RaceState()));
    TextStatus(mode==1?L"Race: one player per monitor. Ctrl+Alt+F8 hides the displays.":L"Menus: whole game on both displays. Ctrl+Alt+F8 switches to race views.");
    SetForegroundWindow(source);
  }
}
LRESULT CALLBACK ViewProc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
  if(msg==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  if(msg==WM_LBUTTONDOWN) {SetForegroundWindow(source);return 0;}
  if(msg==WM_CLOSE) {Hide();return 0;}
  if(msg==WM_PAINT) {PAINTSTRUCT ps{};HDC dc=BeginPaint(w,&ps);FillRect(dc,&ps.rcPaint,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));EndPaint(w,&ps);return 0;}
  return DefWindowProcW(w,msg,wp,lp);
}
// Player 2's race result. The game's results screen only shows Player 1; the
// game publishes Player 2's numbers in the shared block FM3Player2Result_<pid>
// {show, place, credits, xp, level, wallet, reward cars, base prize}.
// show = 2 (results screen on top) with the views up: Player 1's panels on
// Player 2's monitor are covered with Player 2's own (a full-monitor,
// click-through window; magenta is the see-through colour).
// Otherwise a small banner at the top right.
HWND banner{};HANDLE result_map{};const volatile LONG* result{};
LONG shown[8]{};bool banner_up=false,panels=false;
const COLORREF kKey=RGB(255,0,255);
std::wstring Thousands(LONG v) {
  std::wstring t=std::to_wstring(v<0?-v:v);
  for(int i=int(t.size())-3;i>0;i-=3) t.insert(i,L",");
  return v<0?L"-"+t:t;
}
std::wstring Signed(LONG v) {return (v<0?L"":L"+")+Thousands(v)+L" CR";}
std::wstring Ordinal(LONG n,bool upper) {
  const bool teen=n%100>=11&&n%100<=13;
  const wchar_t* lower=teen?L"th":n%10==1?L"st":n%10==2?L"nd":n%10==3?L"rd":L"th";
  const wchar_t* caps=teen?L"TH":n%10==1?L"ST":n%10==2?L"ND":n%10==3?L"RD":L"TH";
  return std::to_wstring(n)+(upper?caps:lower);
}
void Fill(HDC dc,const RECT& r,COLORREF c) {HBRUSH b=CreateSolidBrush(c);FillRect(dc,&r,b);DeleteObject(b);}
void Text(HDC dc,const std::wstring& s,RECT r,int height,int weight,COLORREF c,UINT align) {
  HFONT f=CreateFontW(-height,0,0,0,weight,0,0,0,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Segoe UI");
  HGDIOBJ old=SelectObject(dc,f);SetTextColor(dc,c);
  DrawTextW(dc,s.c_str(),-1,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|align);
  SelectObject(dc,old);DeleteObject(f);
}
void PaintBanner(HDC dc,const RECT& r) {
  Fill(dc,r,RGB(24,24,24));
  RECT stripe=r;stripe.right=stripe.left+std::max(6L,r.right/60);Fill(dc,stripe,RGB(232,52,22));
  const int h=r.bottom;const LONG pad=stripe.right+h/5;
  const std::wstring top=L"PLAYER 2    "+Ordinal(shown[1],true)+L" PLACE    +"+Thousands(shown[2])+L" CR    +"+Thousands(shown[3])+L" XP";
  std::wstring bottom;
  if(shown[4]>0) {
    bottom=L"Level "+std::to_wstring(shown[4])+L"     "+Thousands(shown[5])+L" CR total";
    if(shown[6]>0) bottom+=L"     New reward car - check the garage";
  } else bottom=L"Winnings are added when you continue";
  Text(dc,top,{pad,h*8/100,r.right-h/8,h*55/100},h*30/100,FW_BOLD,RGB(255,255,255),DT_LEFT);
  Text(dc,bottom,{pad,h*56/100,r.right-h/8,h*94/100},h*24/100,FW_NORMAL,RGB(200,200,200),DT_LEFT);
}
// Positions are fractions of the game's 1920x1080 results screen.
void PaintPanels(HDC dc,const RECT& r) {
  Fill(dc,r,kKey);
  const auto X=[&](int x){return LONG(r.right*x/1920);};
  const auto Y=[&](int y){return LONG(r.bottom*y/1080);};
  const COLORREF back=RGB(14,14,14),line=RGB(52,52,52),white=RGB(255,255,255),grey=RGB(205,205,205);
  // Winnings table and total.
  Fill(dc,{X(541),Y(250),X(1826),Y(740)},back);
  Fill(dc,{X(541),Y(250),X(1826),Y(300)},RGB(0,0,0));
  Text(dc,L"race winnings  -  PLAYER 2",{X(553),Y(250),X(1810),Y(300)},Y(24),FW_BOLD,white,DT_LEFT);
  const LONG base=shown[7],paid=shown[2],other=paid-base;
  struct Row {std::wstring name,value;bool strong;};
  const Row rows[3]={
    {Ordinal(shown[1],false),Signed(base),true},
    {L"difficulty and damage",Signed(other),false},
    {L"experience",L"+"+Thousands(shown[3])+L" XP",false}};
  for(int i=0;i<3;++i) {
    const int top=300+i*95;
    Fill(dc,{X(541),Y(top+94),X(1826),Y(top+95)},line);
    Text(dc,rows[i].name,{X(553),Y(top),X(1200),Y(top+95)},Y(rows[i].strong?44:36),FW_NORMAL,rows[i].strong?white:grey,DT_LEFT);
    Text(dc,rows[i].value,{X(1200),Y(top),X(1808),Y(top+95)},Y(rows[i].strong?42:34),FW_BOLD,rows[i].strong?white:grey,DT_RIGHT);
  }
  Text(dc,L"Total",{X(1200),Y(592),X(1808),Y(634)},Y(30),FW_NORMAL,grey,DT_RIGHT);
  Text(dc,Signed(paid),{X(900),Y(628),X(1808),Y(716)},Y(80),FW_BOLD,white,DT_RIGHT);
  std::wstring note;
  if(shown[4]>0) {
    note=L"Level "+std::to_wstring(shown[4])+L"    "+Thousands(shown[5])+L" CR in the bank";
    if(shown[6]>0) note+=L"    New reward car";
  }
  if(!note.empty()) Text(dc,note,{X(553),Y(640),X(1100),Y(716)},Y(26),FW_NORMAL,grey,DT_LEFT);
  // "You placed".
  Fill(dc,{X(128),Y(778),X(540),Y(972)},back);
  Text(dc,L"Player 2 placed",{X(155),Y(784),X(535),Y(826)},Y(30),FW_NORMAL,grey,DT_LEFT);
  Text(dc,Ordinal(shown[1],false),{X(148),Y(820),X(535),Y(968)},Y(140),FW_BOLD,white,DT_LEFT);
}
LRESULT CALLBACK BannerProc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
  if(msg==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  if(msg==WM_ERASEBKGND) return 1;
  if(msg==WM_PAINT) {
    PAINTSTRUCT ps{};HDC dc=BeginPaint(w,&ps);
    RECT r{};GetClientRect(w,&r);
    // Draw off screen first so the panels never flicker.
    HDC mem=CreateCompatibleDC(dc);HBITMAP bmp=CreateCompatibleBitmap(dc,r.right,r.bottom);
    HGDIOBJ old=SelectObject(mem,bmp);SetBkMode(mem,TRANSPARENT);
    if(panels) PaintPanels(mem,r);else PaintBanner(mem,r);
    BitBlt(dc,0,0,r.right,r.bottom,mem,0,0,SRCCOPY);
    SelectObject(mem,old);DeleteObject(bmp);DeleteDC(mem);
    EndPaint(w,&ps);return 0;
  }
  return DefWindowProcW(w,msg,wp,lp);
}
void UpdateBanner() {
  if(!banner) return;
  if(!result&&source_process) {
    const auto name=L"Local\\FM3Player2Result_"+std::to_wstring(source_process);
    result_map=OpenFileMappingW(FILE_MAP_READ,FALSE,name.c_str());
    if(result_map) result=static_cast<const volatile LONG*>(MapViewOfFile(result_map,FILE_MAP_READ,0,0,8*sizeof(LONG)));
  }
  LONG now[8]{};
  if(result) for(int i=0;i<8;++i) now[i]=result[i];
  const bool want=now[0]>0&&now[1]>0&&now[1]<=16&&IsWindow(source)&&!IsIconic(source);
  if(!want) {
    if(banner_up) {ShowWindow(banner,SW_HIDE);banner_up=false;Log("result display hidden");}
    return;
  }
  // Player 2's monitor while the views are up, otherwise the game window.
  const bool on_monitor=mode&&monitors.size()>=2;
  RECT area{};
  if(on_monitor) area=monitors[swapped?0:1].rect;
  else {RECT c{};GetClientRect(source,&c);POINT o{0,0};ClientToScreen(source,&o);area={o.x,o.y,o.x+c.right,o.y+c.bottom};}
  const bool want_panels=on_monitor&&now[0]==2;
  const LONG w=area.right-area.left,h=area.bottom-area.top;
  const RECT place=want_panels?area:RECT{area.left+w*50/100,area.top+h*3/100,area.left+w*98/100,area.top+h*14/100};
  bool changed=!banner_up||want_panels!=panels;
  for(int i=1;i<8;++i) if(now[i]!=shown[i]) changed=true;
  RECT have{};GetWindowRect(banner,&have);
  if(changed||!EqualRect(&have,&place)||!(GetWindowLongPtrW(banner,GWL_EXSTYLE)&WS_EX_TOPMOST)) {
    for(int i=0;i<8;++i) shown[i]=now[i];
    if(want_panels!=panels||!banner_up) {
      panels=want_panels;
      if(panels) SetLayeredWindowAttributes(banner,kKey,244,LWA_COLORKEY|LWA_ALPHA);
      else SetLayeredWindowAttributes(banner,0,238,LWA_ALPHA);
      Log(std::string("result display: ")+(panels?"panels":"banner")+" place="+std::to_string(now[1])+" credits="+std::to_string(now[2])+" base="+std::to_string(now[7])+" xp="+std::to_string(now[3]));
    }
    SetWindowPos(banner,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    SetWindowPos(banner,HWND_TOPMOST,place.left,place.top,place.right-place.left,place.bottom-place.top,SWP_NOACTIVATE|SWP_SHOWWINDOW);
    InvalidateRect(banner,nullptr,TRUE);
    banner_up=true;
  }
}
LRESULT CALLBACK PanelProc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
  if(msg==WM_COMMAND) {
    switch(LOWORD(wp)) {
      case 101:Show(1);break;
      case 102:Show(2);break;
      case 103:Hide();SetForegroundWindow(source);break;
      case 104:swapped=!swapped;dirty=true;Refresh();break;
      case 105:side_by_side=SendDlgItemMessageW(w,105,BM_GETCHECK,0,0)==BST_CHECKED;dirty=true;Refresh();break;
      case 106:DestroyWindow(w);break;
      case 107:fill=SendDlgItemMessageW(w,107,BM_GETCHECK,0,0)==BST_CHECKED;dirty=true;Refresh();break;
    }return 0;
  }
  if(msg==WM_HOTKEY) {
    if(wp==1) {if(mode) Hide();else Show(1);}
    if(wp==2) Show(2);
    if(wp==3) {swapped=!swapped;dirty=true;Refresh();}
    if(wp==4) Hide();
    return 0;
  }
  if(msg==WM_TIMER) {
    if(!IsWindow(source)) {DestroyWindow(w);return 0;}
    if(mode) {
      const int racing=RaceState();
      const int wanted=racing<0?mode:(racing?1:2);
      if(wanted!=mode) {Log("auto mode="+std::to_string(wanted)+" (race flag "+std::to_string(racing)+")");mode=wanted;dirty=true;}
    }
    Refresh();UpdateBanner();return 0;
  }
  if(msg==WM_DISPLAYCHANGE) {Hide();ReadMonitors();TextStatus(L"Display layout changed. Select Race views again when ready.");return 0;}
  if(msg==WM_CLOSE) {DestroyWindow(w);return 0;}
  if(msg==WM_DESTROY) {RestoreWindow();KillTimer(w,1);for(int i=1;i<=4;++i) UnregisterHotKey(w,i);ReleaseViews();if(banner) DestroyWindow(banner);banner=nullptr;PostQuitMessage(0);return 0;}
  return DefWindowProcW(w,msg,wp,lp);
}
bool GeometryTest() {
  const RECT r{0,0,1280,720};const auto a=Slice(r,0,false),b=Slice(r,1,false);
  const auto fit=Fit(a,1920,1080);
  const auto odd0=Slice({0,0,1281,721},0,false),odd1=Slice({0,0,1281,721},1,false);
  const auto side=Slice(r,1,true);
  return a.top==0&&a.bottom==360&&b.top==360&&b.bottom==720&&fit.left==0&&fit.right==1920&&fit.top==270&&fit.bottom==810&&odd0.bottom==odd1.top&&odd1.bottom==721&&side.left==640&&side.right==1280;
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR command,int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const auto folder=Folder();log_file.open(folder/L"FM3-dual-monitor.log",std::ios::app);
  Log("--- dual-monitor companion started ---");
  if(std::wstring(command)==L"--self-test") {const bool ok=GeometryTest();Log(ok?"geometry tests PASS":"geometry tests FAIL");return ok?0:1;}
  std::wstring args(command);
  if(const auto at=args.find(L" --fill");at!=std::wstring::npos) {fill=true;args.erase(at,7);}
  const bool automatic=args.starts_with(L"--auto ");
  if(automatic) {
    wchar_t* end=nullptr;requested_pid=wcstoul(args.c_str()+7,&end,10);
    if(!requested_pid||*end) {Log("invalid autostart process ID");return 1;}
  }
  ReadMonitors();
  if(automatic&&monitors.size()<2) {Log("autostart skipped: fewer than two monitors");return 0;}
  expected=(folder/L"FM3-career-research"/L"fm3.exe").wstring();
  // Started by a game: follow that game, wherever its folder is.
  if(automatic) if(HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,requested_pid)) {
    wchar_t path[32768];DWORD length=32768;
    if(QueryFullProcessImageNameW(p,0,path,&length)) expected=path;
    CloseHandle(p);
  }
  EnumWindows(SourceEnum,0);
  if(automatic&&!source) {
    HANDLE game=OpenProcess(SYNCHRONIZE,FALSE,requested_pid);
    if(!game) {Log("autostart game already closed");return 0;}
    // This wait is only in the companion process, never the game's UI thread.
    for(unsigned i=0;i<300&&!source;++i) {
      if(WaitForSingleObject(game,100)!=WAIT_TIMEOUT) break;
      EnumWindows(SourceEnum,0);
    }
    CloseHandle(game);
  }
  Log("source found="+std::to_string(source!=nullptr));
  if(args==L"--probe") return monitors.size()>=2&&source?0:1;
  if(automatic&&!source) {Log("autostart skipped: no game window");return 0;}
  if(monitors.size()<2||!source) {MessageBoxW(nullptr,L"Open the Career game in a window and connect two monitors, then launch this companion again.",L"FM3 Dual Monitor",MB_OK|MB_ICONINFORMATION);return 1;}
  DWORD source_pid=0;GetWindowThreadProcessId(source,&source_pid);source_process=source_pid;
  const auto lock_name=L"Local\\FM3DualMonitor_"+std::to_wstring(source_pid);
  HANDLE single=CreateMutexW(nullptr,FALSE,lock_name.c_str());
  const auto lock_error=GetLastError();
  if(!single) {Log("could not create companion instance lock");return 4;}
  if(lock_error==ERROR_ALREADY_EXISTS) {
    Log("companion already running for this game");CloseHandle(single);
    if(!automatic) {
      if(auto existing=FindWindowW(L"FM3DualMonitorControl",nullptr)) {
        ShowWindow(existing,SW_RESTORE);SetForegroundWindow(existing);
      }
    }
    return 0;
  }
  BOOL composition=FALSE;
  if(FAILED(DwmIsCompositionEnabled(&composition))||!composition) return 2;
  WNDCLASSW wc{};wc.hInstance=instance;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.lpfnWndProc=ViewProc;wc.lpszClassName=L"FM3PlayerMonitor";wc.hbrBackground=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));RegisterClassW(&wc);
  for(auto& v:views) {
    v.window=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,L"FM3PlayerMonitor",L"FM3 Player monitor",WS_POPUP,0,0,640,360,nullptr,nullptr,instance,nullptr);
    const HRESULT hr=v.window?DwmRegisterThumbnail(v.window,source,&v.thumbnail):E_FAIL;
    if(FAILED(hr)) {Log("DwmRegisterThumbnail failed="+std::to_string(hr));ReleaseViews();return 3;}
  }
  wc.lpfnWndProc=BannerProc;wc.lpszClassName=L"FM3Player2Result";RegisterClassW(&wc);
  banner=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOPMOST,L"FM3Player2Result",L"FM3 Player 2 result",WS_POPUP,0,0,800,120,nullptr,nullptr,instance,nullptr);
  if(banner) SetLayeredWindowAttributes(banner,0,238,LWA_ALPHA);
  wc.lpfnWndProc=PanelProc;wc.lpszClassName=L"FM3DualMonitorControl";wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassW(&wc);
  panel=CreateWindowExW(0,wc.lpszClassName,L"FM3 - Dual monitor controls",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|(automatic?0:WS_VISIBLE),CW_USEDEFAULT,CW_USEDEFAULT,620,400,nullptr,nullptr,instance,nullptr);
  if(!panel) {ReleaseViews();return 4;}
  auto control=[&](const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id) {
    auto c=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,panel,reinterpret_cast<HMENU>(INT_PTR(id)),instance,nullptr);SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);return c;
  };
  control(L"STATIC",L"One player per monitor",0,20,15,570,25,0);
  std::wstring details=L"P1: primary monitor. P2: second monitor. "+std::to_wstring(monitors.size())+L" displays detected.\nKeeps the entire view and HUD; black bars preserve the picture's shape.";
  control(L"STATIC",details.c_str(),0,20,45,570,45,0);
  control(L"BUTTON",L"Show race views",WS_TABSTOP,20,100,175,36,101);
  control(L"BUTTON",L"Show whole menus",WS_TABSTOP,205,100,175,36,102);
  control(L"BUTTON",L"Hide displays",WS_TABSTOP,390,100,175,36,103);
  control(L"BUTTON",L"Swap players / monitors",WS_TABSTOP,20,148,220,32,104);
  control(L"BUTTON",L"Game uses left/right split",WS_TABSTOP|BS_AUTOCHECKBOX,260,148,300,32,105);
  control(L"BUTTON",L"Fill each monitor (needs split_fullscreen_views = true in the game)",WS_TABSTOP|BS_AUTOCHECKBOX,20,186,560,26,107);
  if(fill) SendDlgItemMessageW(panel,107,BM_SETCHECK,BST_CHECKED,0);
  control(L"STATIC",L"Ctrl+Alt+F8: race views / hide     Ctrl+Alt+F7: whole menus\nCtrl+Alt+F9: swap players            Ctrl+Alt+F6: hide\nKeep the game restored. Click a player view to return game input focus.",0,20,222,570,60,0);
  status=control(L"STATIC",L"Ready. Load a two-player race, then choose Show race views.",0,20,293,570,38,0);
  const UINT keys[]={VK_F8,VK_F7,VK_F9,VK_F6};bool hotkeys=true;
  for(int i=0;i<4;++i) if(!RegisterHotKey(panel,i+1,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,keys[i])) hotkeys=false;
  if(!hotkeys) TextStatus(L"Some shortcuts are in use. Use the buttons in this window instead.");
  SetTimer(panel,1,200,nullptr);Log("ready; hotkeys="+std::to_string(hotkeys)+" fill="+std::to_string(fill));
  if(automatic) {ShowWindow(panel,SW_SHOWMINNOACTIVE);Log("autostart ready; controls minimized; race views hidden");}
  MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0) {if(!IsDialogMessageW(panel,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
  CloseHandle(single);Log("companion closed; game unchanged");return 0;
}
