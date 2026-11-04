#include "worker.h"
#include "fan_chart.h"
#include "config_text.h"
#ifdef MECHCONTROL_WITH_CLI
#include "cli_server.h"
#endif
#include <commctrl.h>
#include <shellapi.h>
#include <commdlg.h>
#include <tlhelp32.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <iomanip>
#include <sstream>
#include <tuple>
#ifdef MECHCONTROL_PAGE_CHECK
#include <dwmapi.h>
#include "../tests/ui_pages_fixture.h"
#endif

namespace {
using namespace mech;
using namespace mech::config;
constexpr wchar_t mainClass[]=L"MechControl.Native.Main";
constexpr UINT trayMessage=WM_APP+2,fanEditMessage=WM_APP+4;
enum Id {
    tabsId=100,saveId=110,defaultsId,renameId,clearId,showId,exitId,readId,
    slotId=120,defaultId=140,
    pl1Id=200,pl2Id,pl4Id,tempId,tgpId,dbId,nvId,msId,inputId,dbOnId,unlockId,dlppcId,powerApplyId,powerReadId,
    boostId=300,switchId,switchMsId,switchApplyId,curveId,linkId,cpuChartId,gpuChartId,fanApplyId,fanTextId,debugId,debugCpuId,debugGpuId,boostApplyId,
    protectionId=400,thresholdId,reduceId,fanBoostId,showWindowId,protectionApplyId,resetId,testId,thermalTextId,historyId,previewId,thermalHintId,detailsId,detailsTextId,
    batteryModeId=500,batteryApplyId,batteryTextId,
    zoneBaseId=600,idleId=660,dimmingId,lightingReadId,
    infoId=700,
    devicesId=800,scanId,connectId,autoConnectId,retryId,coolerTextId,
    coolerFanOnId,coolerFanId,coolerFanApplyId,coolerPumpOnId,coolerPumpId,voltageId,coolerPumpApplyId,
    coolerRgbOnId,coolerRId,coolerGId,coolerBId,rgbModeId,coolerRgbApplyId
};
std::wstring w(const std::string& value) {return wide(value);}
std::string utf8(const std::wstring& value) {
    int n=WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);
    std::string text(n,'\0');WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),text.data(),n,nullptr,nullptr);return text;
}
std::wstring text(HWND window) {int n=GetWindowTextLengthW(window);std::wstring s(n+1,L'\0');GetWindowTextW(window,s.data(),n+1);s.resize(n);return s;}
std::wstring decimal(double value,int places=1) {std::wostringstream out;out.imbue(std::locale::classic());out<<std::fixed<<std::setprecision(places)<<value;return out.str();}
// .NET "0.###": up to three decimals without trailing zeros.
std::wstring compact(double value) {auto s=decimal(value,3);s.erase(s.find_last_not_of(L'0')+1);if(s.back()==L'.') s.pop_back();return s;}
constexpr COLORREF okColor=RGB(46,125,50),warnColor=RGB(230,124,0),errorColor=RGB(198,40,40),frameColor=RGB(180,180,180);
// Green up to normal, amber midway, red from limiting; unknown keeps the normal text color.
COLORREF tint(std::optional<double> value,double normal,double limiting) {
    if(!value) return GetSysColor(COLOR_BTNTEXT);
    auto mix=[](COLORREF a,COLORREF b,double x) {auto c=[x](int p,int q){return int(p+(q-p)*x);};return RGB(c(GetRValue(a),GetRValue(b)),c(GetGValue(a),GetGValue(b)),c(GetBValue(a),GetBValue(b)));};
    double f=std::clamp((*value-normal)/(limiting-normal),0.0,1.0);
    return f<0.5?mix(okColor,warnColor,f*2):mix(warnColor,errorColor,f*2-1);
}
// GPU Limits (live): the four headings, then the seven rows, each with the reference tooltip.
constexpr const wchar_t* liveCaptions[]{L"Setting",L"Now",L"Of limit",L"Controller cap (above clock)",
    L"GPU clock",L"GPU power",L"Core rails",L"Memory and other",L"NVVDD current",L"MSVDD current",L"Board input current"};
constexpr const wchar_t* railTip=L"The current controller lowers its clock cap until neither rail's current would exceed its limit at that cap.";
constexpr const wchar_t* liveTips[]{
    L"The active limit (for GPU power, the enforced limit: TGP + Dynamic Boost, or the driver's lower budget).",
    L"The driver's latest reading (it updates every 20 ms); refreshed twice a second while this tab is shown.",
    L"The reading as a share of its setting.",
    L"The highest clock the driver controller enforcing this limit allows (power or current controller); the lower cap applies. In brackets: how far the cap is above the actual clock. Board input has no controller: the GPU's inline power controller (IPC) slows the clock below the caps.",
    L"The graphics clock (NVML) against the driver's maximum.",
    L"Total GPU power (TGP) against the enforced limit.",
    L"The rails the power controller manages and predicts (NVVDD and MSVDD supply).",
    L"GPU power outside the core rails: memory, PCIe and the rest of the board. The core gets the power limit minus this, so raising TGP raises core power by less than the full amount.",
    railTip,railTip,
    L"Board input has no controller or cap: the GPU's inline power controller (IPC) slows the clock below every cap. IPC active: the share of time it acted since the last reading. It also clips power peaks, so it can act without board input."};
std::wstring dlppcTip(const Json& detected) {
    std::wstring tip=L"DLPPC (likely Deep Learning Power/Performance Controller) is an NVIDIA firmware controller that uses a neural-network power model to pick the GPU memory clock under load. When the GPU is power-limited it caps memory a step down (P2) to give the core more of the power limit. Disabling it keeps memory at full speed (P0) in games; idle downclocking is unaffected. Applies now and at every startup, and is reapplied after a GPU restart.";
    if(!detected.is_object()) return tip;
    std::wstring controllers;auto mask=detected.value("controllers",0u);for(int i=0;i<32;++i) if(mask&(1u<<i)) controllers+=(controllers.empty()?L"":L", ")+std::to_wstring(i);
    return tip+L"\nDetected: PERF_CF policy "+std::to_wstring(detected.value("policy",0))+L", controller "+controllers+L"; currently "+(detected.value("active",false)?L"enabled":L"disabled")+L".";
}
std::wstring caption(std::string name) {
    std::wstring unit;
    for(auto [suffix,label]:{std::pair("Celsius",L" (°C)"),std::pair("Watts",L" (W)"),std::pair("Percent",L" (%)"),std::pair("Mah",L" (mAh)"),std::pair("Mhz",L" (MHz)"),std::pair("Mw",L" (mW)"),std::pair("Ma",L" (mA)"),std::pair("Mv",L" (mV)")}) {
        if(name.ends_with(suffix)) {name.resize(name.size()-strlen(suffix));unit=label;break;}
    }
    if(name=="tgp") return L"TGP"+unit;if(name=="nvvdd") return L"NVVDD"+unit;if(name=="msvdd") return L"MSVDD"+unit;if(name=="dlppc") return L"DLPPC"+unit;
    std::wstring out;for(size_t i=0;i<name.size();++i) {auto c=name[i];if(i && c>='A' && c<='Z' && name[i-1]>='a' && name[i-1]<='z') out+=L' ';out+=w(std::string(1,i==0 && c>='a' && c<='z'?char(c-'a'+'A'):c));}return out+unit;
}
std::wstring describe(const Json& value,const std::wstring& prefix=L"") {
    std::wstring out;
    if(value.is_object()) for(auto i=value.begin();i!=value.end();++i) {if(i.key()=="boardInputLimiterActivity" && i->is_number()) out+=prefix+L"Board input limiter active: "+decimal(i->get<double>()*100)+L"%\r\n";else out+=describe(i.value(),prefix+caption(i.key())+L": ");}
    else if(value.is_array()) {out=prefix;for(const auto& v:value) out+=v.is_null()?L"-- ":w(v.is_string()?v.get<std::string>():v.dump())+L" ";out+=L"\r\n";}
    else out=prefix+(value.is_null()?L"--":value.is_string()?w(value.get<std::string>()):value.is_number_float()?decimal(value.get<double>()):w(value.dump()))+L"\r\n";
    return out;
}
struct Row {HWND label=nullptr,control=nullptr,number=nullptr;int id=0,y=0,height=36;bool slider=false,shown=true;};
struct Page {HWND window=nullptr;std::vector<Row> rows;int height=0,scroll=0;};
struct PowerText {RECT rect;std::wstring text;COLORREF color;};
struct BarText {RECT rect;std::wstring text;bool bold;COLORREF color;};

// Small native edit dialog, shared only by the existing slot rename workflow.
struct RenameDialog {std::wstring value;HFONT font=nullptr;HWND edit=nullptr;};
INT_PTR CALLBACK renameProcedure(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto data=reinterpret_cast<RenameDialog*>(GetWindowLongPtrW(window,DWLP_USER));
    if(message==WM_INITDIALOG) {
        data=reinterpret_cast<RenameDialog*>(lp);SetWindowLongPtrW(window,DWLP_USER,lp);
        SetWindowTextW(window,L"Rename Profile");
        auto scale=[dpi=GetDpiForWindow(window)](int n){return MulDiv(n,dpi,96);};
        RECT owner{};GetWindowRect(GetParent(window),&owner);SetWindowPos(window,nullptr,(owner.left+owner.right-scale(360))/2,(owner.top+owner.bottom-scale(150))/2,scale(360),scale(150),SWP_NOZORDER);
        CreateWindowW(L"STATIC",L"Profile name:",WS_CHILD|WS_VISIBLE,scale(12),scale(12),scale(330),scale(22),window,nullptr,nullptr,nullptr);
        data->edit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",data->value.c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,scale(12),scale(38),scale(330),scale(26),window,reinterpret_cast<HMENU>(10),nullptr,nullptr);
        SendMessageW(data->edit,EM_SETLIMITTEXT,256,0);
        CreateWindowW(L"BUTTON",L"OK",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,scale(176),scale(80),scale(80),scale(28),window,reinterpret_cast<HMENU>(IDOK),nullptr,nullptr);
        CreateWindowW(L"BUTTON",L"Cancel",WS_CHILD|WS_VISIBLE|WS_TABSTOP,scale(264),scale(80),scale(80),scale(28),window,reinterpret_cast<HMENU>(IDCANCEL),nullptr,nullptr);
        EnumChildWindows(window,[](HWND child,LPARAM font)->BOOL {SendMessageW(child,WM_SETFONT,WPARAM(font),TRUE);return TRUE;},reinterpret_cast<LPARAM>(data->font));
        SetFocus(data->edit);SendMessageW(data->edit,EM_SETSEL,0,-1);return FALSE;
    }
    if(message==WM_COMMAND && (LOWORD(wp)==IDOK || LOWORD(wp)==IDCANCEL)) {if(LOWORD(wp)==IDOK) data->value=text(data->edit);EndDialog(window,LOWORD(wp));return TRUE;}
    return FALSE;
}
std::optional<std::wstring> rename(HWND owner,std::wstring name,HFONT font) {
    struct Template {DLGTEMPLATE dialog;WORD menu=0,type=0,title=0;} layout{};
    layout.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_CENTER;
    layout.dialog.cx=240;layout.dialog.cy=85;
    RenameDialog data{std::move(name),font};
    if(DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&layout.dialog,owner,renameProcedure,reinterpret_cast<LPARAM>(&data))==IDOK) return data.value;
    return {};
}

struct ModelDialog {const std::vector<std::filesystem::path>& paths;HFONT font;HWND list=nullptr;int chosen=-1;};
INT_PTR CALLBACK modelProcedure(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto data=reinterpret_cast<ModelDialog*>(GetWindowLongPtrW(window,DWLP_USER));
    if(message==WM_INITDIALOG) {
        data=reinterpret_cast<ModelDialog*>(lp);SetWindowLongPtrW(window,DWLP_USER,lp);SetWindowTextW(window,L"MechControl — Select Model");
        auto scale=[dpi=GetDpiForWindow(window)](int n){return MulDiv(n,dpi,96);};RECT owner{};GetWindowRect(GetParent(window),&owner);
        SetWindowPos(window,nullptr,(owner.left+owner.right-scale(760))/2,(owner.top+owner.bottom-scale(470))/2,scale(760),scale(470),SWP_NOZORDER);
        CreateWindowW(L"STATIC",L"Automatic detection failed. Select your model's full profile path, or cancel to exit.",WS_CHILD|WS_VISIBLE,scale(12),scale(12),scale(720),scale(40),window,nullptr,nullptr,nullptr);
        data->list=CreateWindowW(L"LISTBOX",L"Model profile paths",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|WS_VSCROLL|WS_HSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,scale(12),scale(56),scale(720),scale(320),window,reinterpret_cast<HMENU>(10),nullptr,nullptr);
        auto ok=CreateWindowW(L"BUTTON",L"&Use selected profile",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,scale(384),scale(394),scale(220),scale(30),window,reinterpret_cast<HMENU>(IDOK),nullptr,nullptr);
        CreateWindowW(L"BUTTON",L"&Cancel",WS_CHILD|WS_VISIBLE|WS_TABSTOP,scale(616),scale(394),scale(116),scale(30),window,reinterpret_cast<HMENU>(IDCANCEL),nullptr,nullptr);
        EnumChildWindows(window,[](HWND child,LPARAM font)->BOOL {SendMessageW(child,WM_SETFONT,WPARAM(font),TRUE);return TRUE;},reinterpret_cast<LPARAM>(data->font));
        auto dc=GetDC(data->list);auto previous=SelectObject(dc,data->font);int width=0;
        for(auto& path:data->paths) {auto name=path.wstring();SendMessageW(data->list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));SIZE size{};GetTextExtentPoint32W(dc,name.c_str(),int(name.size()),&size);width=std::max(width,int(size.cx));}
        SelectObject(dc,previous);ReleaseDC(data->list,dc);SendMessageW(data->list,LB_SETHORIZONTALEXTENT,width+scale(12),0);EnableWindow(ok,FALSE);SetFocus(data->list);return FALSE;
    }
    if(message==WM_COMMAND) {
        if(LOWORD(wp)==IDCANCEL) {EndDialog(window,IDCANCEL);return TRUE;}
        if(LOWORD(wp)==10 && HIWORD(wp)==LBN_SELCHANGE) {EnableWindow(GetDlgItem(window,IDOK),SendMessageW(data->list,LB_GETCURSEL,0,0)!=LB_ERR);return TRUE;}
        if(LOWORD(wp)==IDOK || (LOWORD(wp)==10 && HIWORD(wp)==LBN_DBLCLK)) {
            auto selected=SendMessageW(data->list,LB_GETCURSEL,0,0);if(selected>=0 && size_t(selected)<data->paths.size()) {data->chosen=int(selected);EndDialog(window,IDOK);}return TRUE;
        }
    }
    return FALSE;
}

class Application {
public:
    explicit Application(HINSTANCE instance,WorkerOptions options={}):instance_(instance),log_(executable_directory()/L"mechcontrol.log"),delivery_(std::make_shared<Delivery>()),workerOptions_(std::move(options)) {checkOem();}
    ~Application() {shutdown();for(auto font:{font_,bold_,title_}) if(font) DeleteObject(font);for(auto icon:slotIcons_) if(icon) DestroyIcon(icon);for(auto icon:animation_) DestroyIcon(icon);}
    HWND create() {auto dpi=GetDpiForSystem();return CreateWindowExW(WS_EX_CONTROLPARENT,mainClass,L"MechControl",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,MulDiv(960,dpi,96),MulDiv(760,dpi,96),nullptr,nullptr,instance_,this);}
#ifdef MECHCONTROL_PAGE_CHECK
    void checkPages(const std::filesystem::path&,const std::shared_ptr<UiPageState>&);
#endif
    static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp) {
        auto app=reinterpret_cast<Application*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE) {app=static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);app->window_=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}
        if(!app) return DefWindowProcW(window,message,wp,lp);
        try {return app->message(message,wp,lp);}catch(const std::exception& e) {
            app->log_.write(e.what());app->setStatus(w(e.what()));
            if(GetTickCount64()-app->lastError_>300000 || !app->lastError_) {app->lastError_=GetTickCount64();MessageBoxW(window,(w(e.what())+L"\nSee mechcontrol.log for details.").c_str(),L"MechControl — Error",MB_OK|MB_ICONERROR);}return message==WM_CREATE?-1:0;
        }
    }
private:
    static LRESULT CALLBACK pageProcedure(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
        auto app=reinterpret_cast<Application*>(data);
        if(message==WM_COMMAND || message==WM_HSCROLL) return SendMessageW(app->window_,message,wp,lp);
        if(message==WM_VSCROLL || message==WM_MOUSEWHEEL) {app->scroll(window,message,wp);return 0;}
        if(message==WM_SETFOCUS) {auto next=GetNextDlgTabItem(window,nullptr,FALSE);if(next) SetFocus(next);return 0;}
        if(message==WM_PARENTNOTIFY && LOWORD(wp)==WM_CREATE) return 0;
        if(window==app->pages_[0].window) {
            if(message==WM_ERASEBKGND) return 1;
            if(message==WM_PAINT) {PAINTSTRUCT ps{};auto dc=BeginPaint(window,&ps);app->paintPower(dc,ps.rcPaint);EndPaint(window,&ps);return 0;}
            // A static is transparent to the mouse; the live table's tooltips need it.
            if(message==WM_NCHITTEST) {auto hit=DefSubclassProc(window,message,wp,lp);return hit==HTTRANSPARENT?HTCLIENT:hit;}
        }
        return DefSubclassProc(window,message,wp,lp);
    }
    static LRESULT CALLBACK focusProcedure(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
        auto app=reinterpret_cast<Application*>(data);
        if(message==WM_SETFOCUS) app->reveal(window);
        // Flat buttons shade while the mouse is over them.
        if(message==WM_MOUSEMOVE && app->hot_!=window && app->flat(GetDlgCtrlID(window))) {app->hot_=window;TRACKMOUSEEVENT t{sizeof t,TME_LEAVE,window};TrackMouseEvent(&t);InvalidateRect(window,nullptr,FALSE);}
        if(message==WM_MOUSELEAVE && app->hot_==window) {app->hot_=nullptr;InvalidateRect(window,nullptr,FALSE);}
        return DefSubclassProc(window,message,wp,lp);
    }
    static LRESULT CALLBACK alertProcedure(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
        auto app=reinterpret_cast<Application*>(data);
        if(message==WM_COMMAND) {app->show();TabCtrl_SetCurSel(app->tabs_,2);app->selectPage();DestroyWindow(window);return 0;}
        if(message==WM_CLOSE) {DestroyWindow(window);return 0;}
        if(message==WM_NCDESTROY) app->alert_=nullptr;
        return DefSubclassProc(window,message,wp,lp);
    }
    HWND control(HWND parent,const wchar_t* type,const std::wstring& title,DWORD style,int id) {
        auto child=CreateWindowExW(0,type,title.c_str(),WS_CHILD|WS_VISIBLE|style,0,0,0,0,parent,reinterpret_cast<HMENU>(INT_PTR(id)),instance_,nullptr);
        if(!child) throw std::runtime_error("Cannot create native control");
        if(id) controls_[id]=child;
        SetWindowSubclass(child,focusProcedure,1,reinterpret_cast<DWORD_PTR>(this));return child;
    }
    HWND get(int id) const {auto i=controls_.find(id);return i==controls_.end()?nullptr:i->second;}
    void set(int id,const std::wstring& value) {if(text(get(id))!=value) {SetWindowTextW(get(id),value.c_str());InvalidateRect(get(id),nullptr,TRUE);}}
    int scale(int n) const {return MulDiv(n,dpi_,96);}
    // Power page metrics are the reference PowerTab's pixel constants, which it drew unscaled on its 144 DPI
    // screen. Scaling them from 144 DPI keeps the same proportions to the text at every DPI.
    int ref(int n) const {return MulDiv(n,dpi_,144);}
    void place(HWND child,int x,int y,int width,int height) {MoveWindow(child,x,y,std::max(0,width),std::max(0,height),TRUE);}
    Row& row(int page,int id,const wchar_t* title,const wchar_t* type,DWORD style,int height=36) {
        auto& p=pages_[page];Row r;r.id=id;r.y=p.height;r.height=height;
        r.control=control(p.window,type,title,style,id);p.rows.push_back(r);p.height+=height+6;return p.rows.back();
    }
    void label(int page,int id,const wchar_t* title,int height=48) {row(page,id,title,L"EDIT",ES_MULTILINE|ES_READONLY|WS_VSCROLL,height);}
    void button(int page,int id,const wchar_t* title) {row(page,id,title,L"BUTTON",WS_TABSTOP|BS_PUSHBUTTON);}
    void check(int page,int id,const wchar_t* title) {row(page,id,title,L"BUTTON",WS_TABSTOP|BS_AUTOCHECKBOX,28);}
    void combo(int page,int id,const wchar_t* title,std::initializer_list<const wchar_t*> choices) {
        auto& r=row(page,id,L"",WC_COMBOBOXW,WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL);
        r.label=control(pages_[page].window,L"STATIC",title,0,0);
        for(auto choice:choices) SendMessageW(r.control,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(choice));
        SendMessageW(r.control,CB_SETCURSEL,0,0);
    }
    void slider(int page,int id,const wchar_t* title,int minimum,int maximum,int value) {
        // Power sliders have ticks and a formatted value label instead of an edit box.
        bool power=page==0;
        auto& r=row(page,id,title,TRACKBAR_CLASSW,WS_TABSTOP|(power?TBS_AUTOTICKS:TBS_NOTICKS),44);r.slider=true;
        r.label=control(pages_[page].window,L"STATIC",title,SS_NOPREFIX,0);
        if(power) r.number=control(pages_[page].window,L"STATIC",L"",SS_NOPREFIX,id+1000);
        else {r.number=control(pages_[page].window,L"EDIT",std::to_wstring(value),WS_TABSTOP|WS_BORDER|ES_NUMBER|ES_AUTOHSCROLL,id+1000);SendMessageW(r.number,EM_SETLIMITTEXT,9,0);}
        SendMessageW(r.control,TBM_SETRANGEMIN,FALSE,minimum);SendMessageW(r.control,TBM_SETRANGEMAX,TRUE,maximum);SendMessageW(r.control,TBM_SETPOS,TRUE,value);
    }
    void tip(UINT_PTR id,const std::wstring& text,UINT flags=0,UINT message=TTM_ADDTOOLW) {
        TOOLINFOW tool{sizeof tool};tool.uFlags=flags|TTF_SUBCLASS;tool.hwnd=pages_[0].window;tool.uId=id;tool.lpszText=const_cast<wchar_t*>(text.c_str());
        SendMessageW(tooltip_,message,0,reinterpret_cast<LPARAM>(&tool));
    }
    void initialize() {
        dpi_=GetDpiForWindow(window_);updating_=true;
        tabs_=control(window_,WC_TABCONTROLW,L"",WS_TABSTOP|WS_CLIPSIBLINGS,tabsId);
        const wchar_t* names[]{L"Power",L"Fans",L"Thermal Failsafe",L"Battery",L"Lighting",L"System Info",L"Cooler"};
        for(int i=0;i<7;++i) {
            TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=const_cast<wchar_t*>(names[i]);TabCtrl_InsertItem(tabs_,i,&item);
            pages_[i].window=CreateWindowExW(WS_EX_CONTROLPARENT,L"STATIC",L"",WS_CHILD|WS_VSCROLL|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,0,0,window_,nullptr,instance_,nullptr);
            SetWindowPos(pages_[i].window,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            SetWindowSubclass(pages_[i].window,pageProcedure,1,reinterpret_cast<DWORD_PTR>(this));
        }
        for(int i=0;i<5;++i) control(window_,L"BUTTON",L"Profile "+std::to_wstring(i+1),WS_TABSTOP|BS_OWNERDRAW,slotId+i);
        control(window_,L"BUTTON",L"&Save",WS_TABSTOP|BS_OWNERDRAW,saveId);control(window_,L"BUTTON",L"&Defaults ▼",WS_TABSTOP|BS_OWNERDRAW,defaultsId);
        control(window_,L"BUTTON",L"&Read",WS_TABSTOP|BS_OWNERDRAW,readId);
        slider(0,pl1Id,L"PL1 (Sustained)",0,1,0);slider(0,pl2Id,L"PL2 (Burst)",0,1,0);slider(0,pl4Id,L"PL4 (Peak)",0,1,0);slider(0,tempId,L"CPU Temp Limit",0,1,0);
        slider(0,tgpId,L"GPU TGP (before Dynamic Boost)",0,1,0);check(0,unlockId,L"Unlock GPU power / current limits");check(0,dlppcId,L"Disable DLPPC (AI-controlled GPU memory clock)");
        check(0,dbOnId,L"Enable Dynamic Boost");slider(0,dbId,L"Dynamic Boost Max",0,1,0);
        slider(0,nvId,L"NVVDD / Core current limit",0,1,0);slider(0,msId,L"MSVDD / Fabric current limit",0,1,0);slider(0,inputId,L"Board input current limit",0,1,0);
        SendMessageW(get(inputId),TBM_SETLINESIZE,0,100); // Board input moves in milliamps; arrow keys step 0.1 A.
        button(0,powerApplyId,L"&Apply");button(0,powerReadId,L"Read from EC");
        tooltip_=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,window_,nullptr,instance_,nullptr);
        for(UINT_PTR i=0;i<std::size(liveTips);++i) tip(i+1,liveTips[i]); // Rectangles follow the painted table.
        tip(reinterpret_cast<UINT_PTR>(get(dlppcId)),dlppcTip_=dlppcTip(nullptr),TTF_IDISHWND);
        for(int id:{nvId,msId,inputId}) tip(reinterpret_cast<UINT_PTR>(get(id)),L"Applies live within the active ceiling. The unlock allows up to 140% of the stock current; raising an older ceiling restarts the GPU.",TTF_IDISHWND);
        check(1,boostId,L"Enable Fan Boost");button(1,boostApplyId,L"Apply Boost");check(1,switchId,L"Enable Fan Switch Speed");slider(1,switchMsId,L"Fan switching interval (ms)",100,12700,300);button(1,switchApplyId,L"Apply fan switch speed");
        check(1,curveId,L"Custom Fan Curve");check(1,linkId,L"Link Fans");
        row(1,cpuChartId,L"CPU fan curve",chartClass,WS_TABSTOP,250);row(1,gpuChartId,L"GPU fan curve",chartClass,WS_TABSTOP,250);
        button(1,fanApplyId,L"Apply reviewed fan curves / link settings");label(1,fanTextId,L"Fan editor state",100);
        row(1,debugCpuId,L"CPU last complete table write — read-only, not hardware readback",chartClass,0,250);row(1,debugGpuId,L"GPU last complete table write — read-only, not hardware readback",chartClass,0,250);
        EnableWindow(get(debugCpuId),FALSE);EnableWindow(get(debugGpuId),FALSE);
        for(int id:{debugCpuId,debugGpuId}) {ChartOptions options{true,true};SendMessageW(get(id),chartOptionsMessage,0,reinterpret_cast<LPARAM>(&options));}
        check(2,protectionId,L"Enable GPU temperature detection");slider(2,thresholdId,L"GPU trip threshold (°C)",50,95,50);check(2,reduceId,L"Also reduce CPU power");check(2,fanBoostId,L"Also enable Fan Boost");check(2,showWindowId,L"Show main window when protection trips");
        label(2,previewId,L"On trip: waiting for model",90);label(2,thermalHintId,L"No saved profile loaded",48);
        button(2,protectionApplyId,L"Apply thermal protection settings");button(2,resetId,L"Reset failsafe and reapply active profile");button(2,testId,L"Test failsafe");label(2,thermalTextId,L"Current protection state",120);label(2,historyId,L"Historical trip (not current verification)",220);
        button(2,detailsId,L"Safety details and limitations");label(2,detailsTextId,L"EC temperature checks run about once per second, including in the tray. A trip needs at least three readings and averages up to five, adding about two seconds of delay. Five invalid/unavailable readings trip protection; a sleeping GPU can cause a conservative trip.\r\n\r\nWhile detection is enabled, Reset requires a fresh valid read and a mean below the trigger. Loading a profile clears the trip before applying its settings. Minimum EC/driver requests are not proof of physical power enforcement or cooling. Fan boost is the laptop firmware mode, not the external pump or fans.\r\n\r\nThere is no protection while the application/OS cannot run or hardware cannot accept writes. This is not real-time protection; a plausible frozen sensor cannot be detected from this register alone. BLE connection is not required. Alerts and sound are best effort; closing the alert does not reset protection.",260);
        showRow(2,detailsTextId,false);
        combo(3,batteryModeId,L"Battery charge mode",{L"High Capacity (Performance)",L"Balanced",L"Stationary (Health)"});button(3,batteryApplyId,L"Apply battery mode");label(3,batteryTextId,L"Battery telemetry is read every two seconds only while this page is visible.",400);
        const wchar_t* zones[]{L"Keyboard",L"Light bar",L"Logo (rear cover)"};
        for(int zone=0;zone<3;++zone) {int id=zoneBaseId+zone*16;
            check(4,id,zones[zone]);slider(4,id+1,L"Red",0,255,0);slider(4,id+2,L"Green",0,255,0);slider(4,id+3,L"Blue",0,255,0);slider(4,id+4,L"Brightness (%)",0,100,0);button(4,id+5,L"Apply zone");button(4,id+7,L"Pick Color…");if(zone!=2) button(4,id+6,L"Save to EC");}
        combo(4,idleId,L"Keyboard / light bar idle timeout",{L"Off",L"10 seconds",L"30 seconds",L"1 minute",L"2 minutes",L"5 minutes",L"10 minutes"});check(4,dimmingId,L"Enable Local Dimming");button(4,lightingReadId,L"Read lighting / local dimming from EC");
        label(5,infoId,L"System information",900);
        combo(6,devicesId,L"Bluetooth cooler",{});button(6,scanId,L"Scan for coolers");button(6,connectId,L"Connect / Disconnect");check(6,autoConnectId,L"Auto-connect on startup");button(6,retryId,L"Retry Bluetooth availability");label(6,coolerTextId,L"Disconnected",120);
        check(6,coolerFanOnId,L"Cooler Fan On");slider(6,coolerFanId,L"Fan duty (%)",0,100,50);button(6,coolerFanApplyId,L"Apply cooler fan");
        check(6,coolerPumpOnId,L"Pump On");combo(6,voltageId,L"Pump voltage",{L"7 V",L"8 V",L"11 V",L"12 V"});slider(6,coolerPumpId,L"Pump duty (%)",0,100,60);button(6,coolerPumpApplyId,L"Apply cooler pump");
        check(6,coolerRgbOnId,L"LED On");slider(6,coolerRId,L"Red",0,255,0);slider(6,coolerGId,L"Green",0,255,128);slider(6,coolerBId,L"Blue",0,255,0);combo(6,rgbModeId,L"RGB mode",{L"Static",L"Breathe",L"Colorful",L"Breathe Color"});button(6,coolerRgbApplyId,L"Apply cooler RGB");
        progress_=control(window_,PROGRESS_CLASSW,L"Device operation progress",PBS_MARQUEE,0);ShowWindow(progress_,SW_HIDE);
        applyDpi();selectPage();edited_.clear();updating_=false;delivery_->attach(window_);busy(true);
        worker_=std::make_unique<ControlWorker>(delivery_,log_,std::move(workerOptions_));
#ifdef MECHCONTROL_WITH_CLI
        cli_=std::make_unique<CliServer>(*worker_,log_);
#endif
        taskbarMessage_=RegisterWindowMessageW(L"TaskbarCreated");
        // Explorer is unelevated; allow its restart notification into this elevated window.
        if(taskbarMessage_ && !ChangeWindowMessageFilterEx(window_,taskbarMessage_,MSGFLT_ALLOW,nullptr)) log_.write("Could not allow TaskbarCreated notification");
        createIcons();tray(true);
        log_.write("Native UI ready; DPI="+std::to_string(dpi_));
    }
    void applyDpi() {
        auto make=[&](int points,int weight){return CreateFontW(-MulDiv(points,dpi_,72),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");};
        auto font=make(9,FW_NORMAL),bold=make(9,FW_BOLD),title=make(10,FW_BOLD);
        for(auto [id,window]:controls_) {(void)id;SendMessageW(window,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);}
        for(auto& p:pages_) for(auto& r:p.rows) if(r.label) SendMessageW(r.label,WM_SETFONT,reinterpret_cast<WPARAM>(&p==&pages_[0]?bold:font),TRUE);
        SendMessageW(get(dbOnId),WM_SETFONT,reinterpret_cast<WPARAM>(bold),TRUE);SendMessageW(tooltip_,TTM_SETMAXTIPWIDTH,0,ref(600));
        if(alert_) EnumChildWindows(alert_,[](HWND child,LPARAM value)->BOOL {SendMessageW(child,WM_SETFONT,WPARAM(value),TRUE);return TRUE;},reinterpret_cast<LPARAM>(font));
        for(auto old:{font_,bold_,title_}) if(old) DeleteObject(old);font_=font;bold_=bold;title_=title;
        for(int id:{cpuChartId,gpuChartId,debugCpuId,debugGpuId}) SendMessageW(get(id),chartDpiMessage,dpi_,0);
        layout();
    }
    void layoutPage(Page& p) {
        RECT rc{};GetClientRect(p.window,&rc);int m=scale(10),width=rc.right-2*m;
        if(&p==&pages_[0]) {
            int content=layoutPower(rc.right,0,false);p.scroll=std::clamp(p.scroll,0,std::max(0,content-int(rc.bottom)));
            SCROLLINFO si{sizeof si,SIF_RANGE|SIF_PAGE|SIF_POS,0,std::max(0,content-1),UINT(rc.bottom),p.scroll,0};SetScrollInfo(p.window,SB_VERT,&si,TRUE);
            RECT shown{};GetClientRect(p.window,&shown);if(shown.right!=rc.right) {layoutPage(p);return;} // The scroll bar changed the width.
            layoutPower(rc.right,p.scroll,true);RedrawWindow(p.window,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);return;
        }
        p.height=0;for(auto& r:p.rows) {r.y=p.height;if(r.shown) p.height+=r.height+6;ShowWindow(r.control,r.shown?SW_SHOW:SW_HIDE);if(r.label) ShowWindow(r.label,r.shown?SW_SHOW:SW_HIDE);if(r.number) ShowWindow(r.number,r.shown?SW_SHOW:SW_HIDE);}
        int content=scale(p.height);p.scroll=std::clamp(p.scroll,0,std::max(0,content-int(rc.bottom)));
        SCROLLINFO si{sizeof si,SIF_RANGE|SIF_PAGE|SIF_POS,0,std::max(0,content-1),UINT(rc.bottom),p.scroll,0};SetScrollInfo(p.window,SB_VERT,&si,TRUE);
        for(auto& r:p.rows) {
            if(!r.shown) continue;
            int y=scale(r.y)+m-p.scroll,h=scale(r.height);
            if(r.slider) {int label=std::min(scale(255),width/2),edit=scale(80);
                place(r.label,m,y,label,h);place(r.control,m+label,y,std::max(scale(60),width-label-edit-scale(8)),h);place(r.number,rc.right-m-edit,y+scale(5),edit,scale(27));
            } else if(r.label) {int label=std::min(scale(255),width/2);place(r.label,m,y,label,h);place(r.control,m+label,y,width-label,scale(240));}
            else place(r.control,m,y,width,h);
        }
        RedrawWindow(p.window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    }
    Row& find(int id) {for(auto& r:pages_[0].rows) if(r.id==id) return r;throw std::logic_error("Missing Power control");}
    // CPU Power, GPU Power, Apply/Read and GPU Limits (live), as in the reference PowerTab. Returns the content height.
    int layoutPower(int width,int scroll,bool apply) {
        auto window=pages_[0].window;auto dc=GetDC(window);auto previous=SelectObject(dc,title_);
        TEXTMETRICW metrics{};GetTextMetricsW(dc,&metrics);int titleHeight=metrics.tmHeight;SelectObject(dc,font_);GetTextMetricsW(dc,&metrics);int line=metrics.tmHeight;
        auto extent=[&](HFONT font,const std::wstring& s){SelectObject(dc,font);SIZE size{};GetTextExtentPoint32W(dc,s.c_str(),int(s.size()),&size);return int(size.cx);};
        // pad: the reference's labels inset their text by about a third of the font's em on each side.
        int x=ref(16),right=width-ref(16),left=x+ref(15),end=right-ref(15),y=ref(6)-scroll,slider=std::max(ref(30),ref(8)+line),box=line+ref(4),pad=ref(6);
        int noteLeft=left+ref(3)+pad,wrap=std::max(ref(60),std::min(ref(600)-2*pad,end-noteLeft-pad));
        bool cpu=!(state_.contains("cpu") && state_["cpu"].is_null()),gpu=!(state_.contains("gpu") && state_["gpu"].is_null());
        std::vector<std::pair<RECT,const wchar_t*>> frames;std::vector<PowerText> texts;
        auto put=[&](HWND child,bool shown,int cx,int cy,int w,int h){if(!apply) return;ShowWindow(child,shown?SW_SHOW:SW_HIDE);if(shown) place(child,cx,cy,w,h);};
        auto open=[&](const wchar_t* title){frames.push_back({{x,y,right,0},title});y+=titleHeight+ref(11);};
        auto close=[&]{y+=ref(13);frames.back().first.bottom=y;y+=ref(12);};
        auto note=[&](const std::wstring& s,COLORREF color,int top,int bottom) {
            if(s.empty()) return;SelectObject(dc,font_);RECT r{noteLeft,y+top,noteLeft+wrap,y+top};
            DrawTextW(dc,s.c_str(),-1,&r,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);r.right=noteLeft+wrap;texts.push_back({r,s,color});y=r.bottom+bottom;
        };
        // Each section sizes its label and value columns to its own contents.
        auto columns=[&](std::initializer_list<int> ids,const wchar_t* widest) {int label=0;for(int id:ids) label=std::max(label,extent(bold_,text(find(id).label)));return std::pair(label+2*pad,extent(font_,widest)+2*pad);};
        auto slide=[&](int id,bool shown,std::pair<int,int> c) {
            auto& r=find(id);int track=left+c.first+ref(8),value=end-c.second;
            put(r.label,shown,left+pad,y+ref(8),c.first-pad,line);put(r.control,shown,track,y,value-ref(8)-track,slider);put(r.number,shown,value+pad,y+ref(8),c.second-pad,line);
            if(shown) y+=slider;
        };
        auto check=[&](int id,bool shown,HFONT font){if(shown) y+=ref(6);put(get(id),shown,left,y,extent(font,text(get(id)))+ref(40),box);if(shown) y+=box;};
        open(L"CPU Power");auto c=columns({pl1Id,pl2Id,pl4Id,tempId},L"000 °C");
        for(int id:{pl1Id,pl2Id,pl4Id,tempId}) slide(id,cpu,c);
        note(cpuNote_,errorColor,0,0);close();
        open(L"GPU Power");c=columns({tgpId,dbId,nvId,msId,inputId},L"000.000 A");
        slide(tgpId,gpu,c);check(unlockId,gpu,font_);check(dlppcId,gpu,font_);check(dbOnId,gpu,bold_);
        for(int id:{dbId,nvId,msId,inputId}) slide(id,gpu,c);
        note(gpu?powerHint_:gpuNote_,gpu?GetSysColor(COLOR_GRAYTEXT):errorColor,0,0);close();
        y+=ref(6);put(get(powerApplyId),true,x,y,ref(110),ref(32));put(get(powerReadId),true,x+ref(120),y,ref(140),ref(32));y+=ref(44);
        int liveTop=-1;
        if(gpu) {
            open(L"GPU Limits (live)");liveTop=y;y+=8*(ref(6)+line);auto gray=GetSysColor(COLOR_GRAYTEXT);
            if(!liveReason_.empty()) note(L"-- ("+liveReason_+L")",gray,ref(6),0);
            // Measured on an RTX 5090 Laptop: current = rail power / a fixed voltage, so it reads high.
            note(L"NVVDD and MSVDD currents are calculated by the driver from rail power, not measured, and can read well above the real current. The limits use the same calculated values.",gray,0,0);
            note(L"When software shunt modding, tune the NVVDD, MSVDD and board input current limits so that they are all solid green 99% of the time, and not any higher.",gray,ref(6),0);
            close();
        }
        SelectObject(dc,previous);ReleaseDC(window,dc);
        if(apply) {
            powerFrames_=std::move(frames);powerText_=std::move(texts);liveOrigin_={left,liveTop};
            if(liveTop<0) for(UINT_PTR i=1;i<=std::size(liveTips);++i) {TOOLINFOW tool{sizeof tool};tool.hwnd=window;tool.uId=i;SendMessageW(tooltip_,TTM_NEWTOOLRECTW,0,reinterpret_cast<LPARAM>(&tool));}
        }
        return y+scroll+ref(6);
    }
    void paintPower(HDC target,const RECT& area) {
        RECT rc{};GetClientRect(pages_[0].window,&rc);if(rc.right<=0 || rc.bottom<=0) return;
        auto dc=CreateCompatibleDC(target);auto bitmap=CreateCompatibleBitmap(target,rc.right,rc.bottom);auto oldBitmap=SelectObject(dc,bitmap);
        FillRect(dc,&rc,GetSysColorBrush(COLOR_BTNFACE));SetBkMode(dc,TRANSPARENT);
        auto pen=CreatePen(PS_SOLID,1,frameColor);auto oldPen=SelectObject(dc,pen);auto oldFont=SelectObject(dc,title_);SetTextColor(dc,GetSysColor(COLOR_BTNTEXT));
        for(auto& [r,title]:powerFrames_) {
            // A thin frame whose top edge stops either side of the bold title.
            int n=int(wcslen(title));SIZE size{};GetTextExtentPoint32W(dc,title,n,&size);int titleX=r.left+ref(10),middle=r.top+ref(2)+size.cy/2;
            // The reference measured the title with GDI's default padding, hence the wider gap after it.
            POINT edge[]{{titleX-ref(2),middle},{r.left,middle},{r.left,r.bottom-1},{r.right-1,r.bottom-1},{r.right-1,middle},{titleX+size.cx+ref(15),middle}};
            Polyline(dc,edge,6);TextOutW(dc,titleX,r.top+ref(2),title,n);
        }
        SelectObject(dc,font_);
        for(auto& t:powerText_) {SetTextColor(dc,t.color);RECT r=t.rect;DrawTextW(dc,t.text.c_str(),-1,&r,DT_WORDBREAK|DT_NOPREFIX);}
        if(liveOrigin_.y>=0) paintLive(dc);
        BitBlt(target,area.left,area.top,area.right-area.left,area.bottom-area.top,dc,area.left,area.top,SRCCOPY);
        SelectObject(dc,oldFont);SelectObject(dc,oldPen);DeleteObject(pen);SelectObject(dc,oldBitmap);DeleteObject(bitmap);DeleteDC(dc);
    }
    // Each limit next to the clock: its setting, reading, share of the setting and the clock cap of the driver
    // controller enforcing it. No verdicts: several limits hold the clock back at once, so only numbers are shown.
    void paintLive(HDC dc) {
        const Json& g=gpuTelemetry_;bool live=liveReason_.empty() && g.is_object();
        auto number=[&](const Json& j,const char* key)->std::optional<double> {if(!live || !j.is_object() || !j.contains(key) || !j[key].is_number()) return {};return j[key].get<double>();};
        auto rail=[&](const char* key){return live && g.contains(key)?g[key]:Json(nullptr);};
        auto nv=rail("nvvdd"),ms=rail("msvdd"),input=rail("boardInput");
        auto clock=number(g,"clockMhz"),maxClock=number(g,"maxClockMhz"),power=number(g,"gpuPowerMw"),enforced=number(g,"enforcedMw"),core=number(g,"corePowerMw"),ipc=number(g,"boardInputLimiterActivity");
        auto nvNow=number(nv,"measuredMa"),nvLimit=number(nv,"liveLimitMa"),msNow=number(ms,"measuredMa"),msLimit=number(ms,"liveLimitMa"),inputNow=number(input,"measuredMa"),inputLimit=number(input,"liveLimitMa");
        auto ratio=[](std::optional<double> v,std::optional<double> limit)->std::optional<double> {if(v && limit && *limit>0) return *v / *limit;return {};};
        auto percent=[](std::optional<double> r){return r?std::to_wstring(std::lround(*r*100))+L"%":std::wstring(L"--");};
        auto mhz=[](std::optional<double> v){return v?std::to_wstring(std::lround(*v))+L" MHz":std::wstring(L"--");};
        auto watts=[](std::optional<double> mw){return mw?decimal(*mw/1000)+L" W":std::wstring(L"--");};
        auto amps=[](std::optional<double> ma,int places){return ma?(places?decimal(*ma/1000,places):compact(*ma/1000))+L" A":std::wstring(L"--");};
        auto cap=[&](const wchar_t* controller,std::optional<double> c) {
            if(!c) return std::wstring(L"--");auto s=controller+(L" "+mhz(c));
            if(clock) {auto above=std::lround(*c-*clock);s+=L" (";s+=above>0?L"+":L"";s+=std::to_wstring(above)+L")";}return s;
        };
        std::optional<double> rest;if(power && core && *power>=*core) rest=*power-*core;
        // From the reference, tuned on an RTX 5090 Laptop: GPU power and the core rails against what each reads
        // while limiting. Board input is enforced on fast peaks, so its average stays well below the limit while
        // limiting. IPC reached at most 37% from power peaks alone and 93.5% with board input clearly limiting.
        COLORREF text=GetSysColor(COLOR_BTNTEXT),gray=GetSysColor(COLOR_GRAYTEXT);
        auto powerColor=tint(ratio(power,enforced),0.94,0.99),nvColor=tint(ratio(nvNow,nvLimit),0.89,0.99),msColor=tint(ratio(msNow,msLimit),0.90,0.99),inputColor=tint(ratio(inputNow,inputLimit),0.68,0.81);
        struct Cell {std::wstring text;COLORREF color;};
        std::array<std::array<Cell,5>,8> cells{};
        for(int i=1;i<5;++i) cells[0][i]={liveCaptions[i-1],gray};
        auto row=[&](int r,COLORREF color,std::wstring setting,std::wstring now,std::wstring share,std::wstring limiter) {cells[r]={{{liveCaptions[r+3],color},{setting,color},{now,color},{share,color},{limiter,text}}};};
        row(1,text,maxClock?L"max "+mhz(maxClock):L"--",mhz(clock),percent(ratio(clock,maxClock)),L"");
        row(2,powerColor,watts(enforced),watts(power),percent(ratio(power,enforced)),cap(L"power",number(g,"powerCapMhz")));
        row(3,text,L"",watts(core),L"",L"");
        row(4,text,L"",watts(rest),L"",L"");
        // One current controller caps both rails, so its cap spans the NVVDD and MSVDD rows.
        row(5,nvColor,amps(nvLimit,0),amps(nvNow,1),percent(ratio(nvNow,nvLimit)),cap(L"current",number(g,"currentCapMhz")));
        row(6,msColor,amps(msLimit,0),amps(msNow,1),percent(ratio(msNow,msLimit)),L"");
        row(7,inputColor,amps(inputLimit,0),amps(inputNow,2),percent(ratio(inputNow,inputLimit)),L"IPC active "+(ipc?percent(std::min(*ipc,1.0)):std::wstring(L"--")));
        cells[7][4].color=tint(ipc,0.52,0.80);
        if(!live) for(int r=1;r<8;++r) for(int i=1;i<5;++i) if(r!=6 || i!=4) cells[r][i].text=L"--";
        TEXTMETRICW metrics{};SelectObject(dc,font_);GetTextMetricsW(dc,&metrics);int line=metrics.tmHeight,height=ref(6)+line,pad=ref(6),gap=ref(12);
        auto part=[](int r){return r==3 || r==4;};
        auto font=[&](int r,int i){return i==0 && !part(r)?bold_:font_;};
        auto extent=[&](int r,int i){SelectObject(dc,font(r,i));SIZE size{};GetTextExtentPoint32W(dc,cells[r][i].text.c_str(),int(cells[r][i].text.size()),&size);return int(size.cx);};
        std::array<int,5> column{};int next=liveOrigin_.x;
        for(int i=0;i<5;++i) {int widest=0;for(int r=0;r<8;++r) widest=std::max(widest,extent(r,i)+(i==0 && part(r)?gap:0));column[i]=next+(i?gap:0)+pad;next=column[i]+widest+pad;}
        for(int r=0;r<8;++r) for(int i=0;i<5;++i) {
            auto& cell=cells[r][i];if(cell.text.empty()) continue;
            int cx=column[i]+(i==0 && part(r)?gap:0),cy=liveOrigin_.y+r*height+ref(6);if(r==5 && i==4) cy+=(2*height-ref(6)-line)/2;
            SelectObject(dc,font(r,i));SetTextColor(dc,cell.color);TextOutW(dc,cx,cy,cell.text.c_str(),int(cell.text.size()));
            if(r==0 || i==0) {TOOLINFOW tool{sizeof tool};tool.hwnd=pages_[0].window;tool.uId=r==0?i:r+4;tool.rect={cx,cy,cx+extent(r,i),cy+line};SendMessageW(tooltip_,TTM_NEWTOOLRECTW,0,reinterpret_cast<LPARAM>(&tool));}
        }
    }
    void refreshPower() {
        const Json& g=gpuTelemetry_;auto nvidia=g.is_object()?g.value("nvidia",Json(nullptr)):Json(nullptr);
        cpuNote_=state_.contains("cpu") && state_["cpu"].is_null()?L"CPU power controls disabled — register validation failed. See log for details."
            :state_.contains("cpuError")?L"CPU read failed: "+w(state_["cpuError"].get<std::string>()):L"";
        gpuNote_=state_.contains("gpu") && state_["gpu"].is_null()?L"GPU power controls disabled — register validation failed. See log for details.":L"";
        powerHint_.clear();
        if(gpuError_.empty() && g.is_object()) {
            auto failed=g.value("powerError",std::string{});if(failed.empty()) failed=g.value("currentsError",std::string{});
            auto stock=[&](const char* key){return compact(g[key]["stockMa"].get<int>()/1000.0);};
            if(nvidia.is_string()) powerHint_=w(nvidia.get<std::string>());
            else if(!failed.empty()) powerHint_=L"NVIDIA readback failed: "+w(failed);
            else if(g.value("ceilingWatts",Json(nullptr)).is_number() && g.value("nvvdd",Json(nullptr)).is_object() && g.value("msvdd",Json(nullptr)).is_object())
                powerHint_=L"Driver ceiling "+std::to_wstring(g["ceilingWatts"].get<int>())+L" W · enforced "+std::to_wstring(g["enforcedWatts"].get<int>())+L" W · stock currents "
                    +stock("nvvdd")+L" / "+stock("msvdd")+(g.value("boardInput",Json(nullptr)).is_object()?L" / "+stock("boardInput"):L"")+L" A";
        }
        liveReason_=!gpuError_.empty()?gpuError_:!g.is_object()?L"not read yet":nvidia.is_string()?w(nvidia.get<std::string>())
            :g.contains("telemetryError")?L"NVIDIA readback failed: "+w(g["telemetryError"].get<std::string>()):L"";
        auto dlppc=dlppcTip(g.is_object()?g.value("dlppc",Json(nullptr)):Json(nullptr));
        if(dlppc!=dlppcTip_) {dlppcTip_=std::move(dlppc);tip(reinterpret_cast<UINT_PTR>(get(dlppcId)),dlppcTip_,TTF_IDISHWND,TTM_UPDATETIPTEXTW);}
        // Text that changes the layout relays out the page; live readings only repaint it.
        auto key=cpuNote_+L'\n'+gpuNote_+L'\n'+powerHint_+L'\n'+liveReason_+L'\n'+text(get(unlockId));
        if(key!=powerKey_) {powerKey_=std::move(key);layoutPage(pages_[0]);} else InvalidateRect(pages_[0].window,nullptr,FALSE);
    }
    void showRow(int page,int id,bool shown) {for(auto& r:pages_[page].rows) if(r.id==id && r.shown!=shown) {r.shown=shown;layoutPage(pages_[page]);break;}}
    // The reference MainForm: the profile bar, the tabs and the monitor bar fill the window edge to edge.
    void layout() {
        if(!tabs_) return;RECT r{};GetClientRect(window_,&r);
        auto dc=GetDC(window_);auto previous=SelectObject(dc,font_);SIZE size{};GetTextExtentPoint32W(dc,L"Defaults ▼",10,&size);SelectObject(dc,previous);ReleaseDC(window_,dc);
        int defaults=size.cx+ref(26),bar=layoutBar(r.right,r.bottom);
        for(int i=0;i<5;++i) place(get(slotId+i),ref(7)+i*ref(126),ref(5),ref(120),ref(32));
        place(get(saveId),ref(637),ref(5),ref(80),ref(32));place(get(readId),r.right-defaults-ref(86),ref(5),ref(80),ref(32));place(get(defaultsId),r.right-defaults,0,defaults,ref(40));
        place(tabs_,0,ref(40),r.right,bar-ref(40));RECT page{};GetClientRect(tabs_,&page);TabCtrl_AdjustRect(tabs_,FALSE,&page);MapWindowPoints(tabs_,window_,reinterpret_cast<POINT*>(&page),2);
        for(auto& p:pages_) {place(p.window,page.left,page.top,page.right-page.left,page.bottom-page.top);layoutPage(p);}
        place(progress_,1,bar+1,r.right-2,ref(5));InvalidateRect(window_,&bar_,FALSE);
    }
    // The reference MonitorPanel: bold readings between gray bars, wrapping like its flow layout onto at least two
    // lines, then the latest status in gray. Returns the bar's top.
    int layoutBar(int width,int bottom) {
        auto dc=GetDC(window_);auto previous=SelectObject(dc,bold_);TEXTMETRICW metrics{};GetTextMetricsW(dc,&metrics);
        int line=metrics.tmHeight,pitch=line+ref(2),pad=line/6,left=1+ref(4),right=width-1-ref(4),x=left,y=1+ref(6),lowest=y+pitch+line;
        std::vector<BarText> texts;auto gray=GetSysColor(COLOR_GRAYTEXT);
        auto put=[&](const std::wstring& s,bool bold,COLORREF color,int margin) {
            // The reference measured its labels slightly wider than GDI does, hence the extra space after each.
            SelectObject(dc,bold?bold_:font_);SIZE size{};GetTextExtentPoint32W(dc,s.c_str(),int(s.size()),&size);int space=2*(margin+pad)+ref(3);
            if(x>left && x+size.cx+space>right) {x=left;y+=pitch;}
            RECT r{x+margin+pad,y,right,y+line};
            // Only a long status can be wider than the bar; it wraps on its own lines.
            if(x+size.cx+space>right) {r.right-=margin+pad;DrawTextW(dc,s.c_str(),-1,&r,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);}
            texts.push_back({r,s,bold,color});x+=size.cx+space;lowest=std::max(lowest,int(r.bottom));
        };
        for(size_t i=0;i<monitor_.size();++i) {put(monitor_[i],true,GetSysColor(COLOR_BTNTEXT),ref(2));if(i+1<monitor_.size() || !status_.empty()) put(L"|",true,gray,0);}
        if(!status_.empty()) put(status_,false,gray,ref(2));
        SelectObject(dc,previous);ReleaseDC(window_,dc);
        bar_={0,bottom-lowest-ref(5),width,bottom};
        for(auto& t:texts) OffsetRect(&t.rect,0,bar_.top);barText_=std::move(texts);return bar_.top;
    }
    void updateBar() {RECT r{};GetClientRect(window_,&r);int top=bar_.top;if(layoutBar(r.right,r.bottom)!=top) layout();else InvalidateRect(window_,&bar_,FALSE);}
    void paintBar(HDC target) {
        int width=bar_.right-bar_.left,height=bar_.bottom-bar_.top;if(width<=0 || height<=0) return;
        auto dc=CreateCompatibleDC(target);auto bitmap=CreateCompatibleBitmap(target,width,height);auto oldBitmap=SelectObject(dc,bitmap);
        RECT r{0,0,width,height};FillRect(dc,&r,GetSysColorBrush(COLOR_3DLIGHT));FrameRect(dc,&r,GetSysColorBrush(COLOR_WINDOWFRAME));
        SetBkMode(dc,TRANSPARENT);auto oldFont=SelectObject(dc,font_);
        for(auto& t:barText_) {SelectObject(dc,t.bold?bold_:font_);SetTextColor(dc,t.color);RECT tr=t.rect;OffsetRect(&tr,0,-bar_.top);DrawTextW(dc,t.text.c_str(),-1,&tr,DT_WORDBREAK|DT_NOPREFIX);}
        BitBlt(target,bar_.left,bar_.top,width,height,dc,0,0,SRCCOPY);
        SelectObject(dc,oldFont);SelectObject(dc,oldBitmap);DeleteObject(bitmap);DeleteDC(dc);
    }
    bool flat(int id) const {return id==saveId || id==defaultsId || id==readId || (id>=slotId && id<slotId+5);}
    bool emptySlot(int i) const {return state_.contains("slots") && !state_.at("slots").at(i).get<bool>();}
    // The reference's flat profile buttons: the selected slot is filled with the highlight color, empty slots are gray.
    void drawButton(const DRAWITEMSTRUCT& d) {
        int id=int(d.CtlID);bool slot=id>=slotId && id<slotId+5,active=slot && target_==id-slotId+1,disabled=(d.itemState&ODS_DISABLED)!=0;
        COLORREF fill=GetSysColor(active?COLOR_HIGHLIGHT:COLOR_BTNFACE),ink=GetSysColor(active?COLOR_HIGHLIGHTTEXT:disabled || (slot && emptySlot(id-slotId))?COLOR_GRAYTEXT:COLOR_BTNTEXT);
        double shade=d.itemState&ODS_SELECTED?0.84:hot_==d.hwndItem && !disabled?0.92:1.0;fill=RGB(BYTE(GetRValue(fill)*shade),BYTE(GetGValue(fill)*shade),BYTE(GetBValue(fill)*shade));
        auto brush=CreateSolidBrush(fill);FillRect(d.hDC,&d.rcItem,brush);DeleteObject(brush);brush=CreateSolidBrush(ink);FrameRect(d.hDC,&d.rcItem,brush);DeleteObject(brush);
        auto old=SelectObject(d.hDC,active || id==saveId?bold_:font_);SetBkMode(d.hDC,TRANSPARENT);SetTextColor(d.hDC,ink);
        auto s=text(d.hwndItem);RECT r=d.rcItem;DrawTextW(d.hDC,s.c_str(),-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|(d.itemState&ODS_NOACCEL?DT_HIDEPREFIX:0));
        if((d.itemState&ODS_FOCUS) && !(d.itemState&ODS_NOFOCUSRECT)) {r=d.rcItem;InflateRect(&r,-ref(3),-ref(3));DrawFocusRect(d.hDC,&r);}
        SelectObject(d.hDC,old);
    }
    void scroll(HWND window,UINT message,WPARAM wp) {
        for(auto& p:pages_) if(p.window==window) {
            if(message==WM_MOUSEWHEEL) p.scroll-=GET_WHEEL_DELTA_WPARAM(wp)*scale(54)/WHEEL_DELTA;
            else {SCROLLINFO si{sizeof si,SIF_ALL};GetScrollInfo(window,SB_VERT,&si);
                switch(LOWORD(wp)) {case SB_LINEUP:p.scroll-=scale(28);break;case SB_LINEDOWN:p.scroll+=scale(28);break;case SB_PAGEUP:p.scroll-=int(si.nPage);break;case SB_PAGEDOWN:p.scroll+=int(si.nPage);break;case SB_THUMBTRACK:p.scroll=si.nTrackPos;break;case SB_TOP:p.scroll=0;break;case SB_BOTTOM:p.scroll=si.nMax;break;}}
            layoutPage(p);return;
        }
    }
    void reveal(HWND child) {
        if(auto& power=pages_[0];GetParent(child)==power.window) {
            RECT rc{},r{};GetClientRect(power.window,&rc);GetWindowRect(child,&r);MapWindowPoints(nullptr,power.window,reinterpret_cast<POINT*>(&r),2);
            if(r.top<0) power.scroll+=r.top;else if(r.bottom>rc.bottom) power.scroll+=r.bottom-rc.bottom;else return;
            layoutPage(power);return;
        }
        for(auto& p:pages_) for(auto& r:p.rows) if(r.control==child || r.number==child) {
            RECT rc{};GetClientRect(p.window,&rc);int top=scale(r.y+10),bottom=top+scale(r.height);
            if(top<p.scroll) p.scroll=top;else if(bottom>p.scroll+rc.bottom) p.scroll=bottom-rc.bottom;
            layoutPage(p);return;
        }
    }
    void selectPage() {
        int page=TabCtrl_GetCurSel(tabs_);for(int i=0;i<7;++i) ShowWindow(pages_[i].window,i==page?SW_SHOW:SW_HIDE);
        enable(readId,!busy_ && !(page==1 && state_.value("linked",false)));
        if(ready_ && IsWindowVisible(window_)) bindPage(page);
        if(worker_) {worker_->request(UiPage{page});worker_->request(BatteryPage{page==3});}
    }
    bool checked(int id) const {return SendMessageW(get(id),BM_GETCHECK,0,0)==BST_CHECKED;}
    void checked(int id,bool value) {if(!edited_.contains(id)) SendMessageW(get(id),BM_SETCHECK,value?BST_CHECKED:BST_UNCHECKED,0);}
    int number(int id) {
        if(id>=pl1Id && id<=inputId) return int(SendMessageW(get(id),TBM_GETPOS,0,0)); // Power values are slider positions.
        auto value=utf8(text(get(id+1000)));int n=0;auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),n);
        if(value.empty() || error!=std::errc{} || end!=value.data()+value.size()) throw std::runtime_error("Enter a whole number");
        int lo=int(SendMessageW(get(id),TBM_GETRANGEMIN,0,0)),hi=int(SendMessageW(get(id),TBM_GETRANGEMAX,0,0));
        if(n<lo || n>hi) throw std::runtime_error("Value is outside the displayed model range ("+std::to_string(lo)+"–"+std::to_string(hi)+")");return n;
    }
    void value(int id,int v,int lo,int hi) {
        auto slider=get(id);
        if(SendMessageW(slider,TBM_GETRANGEMIN,0,0)!=lo || SendMessageW(slider,TBM_GETRANGEMAX,0,0)!=hi) {
            bool power=id>=pl1Id && id<=inputId;SendMessageW(slider,TBM_SETRANGEMIN,FALSE,lo);SendMessageW(slider,TBM_SETRANGEMAX,!power,hi);
            if(power) {int tick=std::max(1,(hi-lo)/10);SendMessageW(slider,TBM_SETTICFREQ,tick,0);SendMessageW(slider,TBM_SETPAGESIZE,0,tick);InvalidateRect(slider,nullptr,TRUE);}
        }
        if(!edited_.contains(id)) {SendMessageW(slider,TBM_SETPOS,TRUE,v);set(id+1000,display(id,v));}
    }
    // Current sliders move in whole amps (board input: milliamps). An unmoved slider keeps the exact detected
    // limit, which some BIOSes define in fractional amps.
    int milliamps(int id,int position) const {int step=id==inputId?1:1000;auto i=detectedMa_.find(id);return i!=detectedMa_.end() && position==i->second/step?i->second:position*step;}
    std::wstring display(int id,int v) const {
        if(id==tempId) return std::to_wstring(v)+L" °C";
        if(id>=nvId && id<=inputId) return compact(milliamps(id,v)/1000.0)+L" A";
        if(id>=pl1Id && id<=dbId) return std::to_wstring(v)+L" W";
        return std::to_wstring(v);
    }
    void enable(int id,bool on) {EnableWindow(get(id),on);if(get(id+1000)) EnableWindow(get(id+1000),on);}
    void eraseEdits(int lo,int hi) {for(auto i=edited_.begin();i!=edited_.end();) if(*i>=lo && *i<=hi) i=edited_.erase(i);else ++i;}
    void busy(bool on) {busy_=on;for(auto& p:pages_) EnableWindow(p.window,!on);for(int id:std::array<int,8>{saveId,defaultsId,readId,slotId,slotId+1,slotId+2,slotId+3,slotId+4}) EnableWindow(get(id),!on);ShowWindow(progress_,on?SW_SHOW:SW_HIDE);SendMessageW(progress_,PBM_SETMARQUEE,on,60);}
    void request(ControlCommand command) {
        if(busy_) return;
        operation_=command;operationPage_=TabCtrl_GetCurSel(tabs_);
        busy(true);busySequence_=++sequence_;setStatus(L"Working… device operations may take a few seconds.");worker_->request(std::move(command),busySequence_);
    }
    void setStatus(const std::wstring& value) {status_=value;updateBar();}
    bool confirm(const wchar_t* message,const wchar_t* title=L"MechControl") {return MessageBoxW(window_,message,title,MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)==IDYES;}
    void read() {int page=TabCtrl_GetCurSel(tabs_);const int lo[]{200,300,400,500,600,700,800};eraseEdits(lo[page],lo[page]+99);if(page==1) {boundCharts_.clear();request(FanRead{});}else request(UiRead{page});}
    void selectSlot(int slot) {
        if(busy_ || state_.empty()) return;
        if(!state_["slots"][slot-1].get<bool>()) {target_=slot;updateSlots();updateTray();setStatus(L"Empty slot selected as save target; live hardware unchanged.");return;}
        if(tripped_ && !confirm(L"Loading a profile clears the thermal trip and reapplies that profile. Check your cooling. Continue?",L"Thermal Failsafe Tripped")) return;
        request(UiPreflight{{},slot});
    }
    void updateSlots() {
        if(!state_.contains("names")) return;
        for(int i=0;i<5;++i) {auto name=state_["names"][i];set(slotId+i,name.is_null()?L"Profile "+std::to_wstring(i+1):w(name.get<std::string>()));InvalidateRect(get(slotId+i),nullptr,FALSE);}
    }
    void slotMenu(int slot,POINT p) {
        if(busy_) return;HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,renameId,L"Rename…");AppendMenuW(menu,MF_STRING,clearId,L"Clear Profile");int id=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,window_,nullptr);DestroyMenu(menu);
        if(id==renameId) {auto value=rename(window_,state_.contains("names") && !state_["names"][slot-1].is_null()?w(state_["names"][slot-1].get<std::string>()):L"",font_);if(value) request(SlotRename{slot,utf8(*value)});}
        if(id==clearId && confirm(L"Delete this saved profile and reset its name? Settings currently applied to hardware are not changed.",L"Clear Profile")) request(SlotClear{slot});
    }
    void defaults() {
        HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,defaultId,L"Restore Firmware Defaults");
        if(state_.contains("modes")) for(size_t i=0;i<state_["modes"].size();++i) AppendMenuW(menu,MF_STRING,defaultId+1+UINT(i),w(state_["modes"][i].get<std::string>()).c_str());
        RECT r{};GetWindowRect(get(defaultsId),&r);int id=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,r.left,r.bottom,0,window_,nullptr);DestroyMenu(menu);
        if(id && confirm(id==defaultId?L"Apply firmware defaults? Changes are not saved to a profile. Owned NVIDIA power/current defaults will also be restored; the GPU may restart and the display may blink.":L"Apply this mode's defaults? Changes are not saved to a profile.",L"Restore Defaults")) {edited_.clear();request(DefaultsApply{id==defaultId?"":state_["modes"][id-defaultId-1].get<std::string>()});}
    }
    GpuSettings gpuIntent() {
        GpuSettings g;g.tgp=number(tgpId);g.dynamic_boost_enabled=checked(dbOnId);g.dynamic_boost_watts=number(dbId);
        if(IsWindowEnabled(get(nvId))) g.nvvdd_ma=milliamps(nvId,number(nvId));if(IsWindowEnabled(get(msId))) g.msvdd_ma=milliamps(msId,number(msId));if(IsWindowEnabled(get(inputId))) g.board_input_ma=milliamps(inputId,number(inputId));
        if(state_.contains("gpu") && !state_["gpu"].is_null()) g.bios_override_enabled=state_["gpu"]["unlocked"].get<bool>();return g;
    }
    void applyPower() {
        if(IsWindowEnabled(get(pl1Id))) pendingCpu_=CpuSettings{number(pl1Id),number(pl2Id),number(pl4Id),number(tempId)};
        if(IsWindowEnabled(get(tgpId))) request(UiPreflight{gpuIntent(),0,pendingCpu_});else if(pendingCpu_) {auto c=*pendingCpu_;pendingCpu_.reset();eraseEdits(pl1Id,tempId);request(CpuApply{c});}
    }
    void applyPlan(const Json& plan) {
        bool restart=plan.value("restart",false),unlock=plan.value("unlock",false),was=plan.value("wasUnlocked",false);
        if(restart) {
            std::wstring message=unlock && !was?L"Raise the NVIDIA BIOS power ceiling and allow higher power/current limits? Higher limits increase heat and hardware stress. This is a persistent software override, not a BIOS flash.\n\n":L"Changing NVIDIA power/current limits requires a GPU restart.\n\n";
            message+=L"The GPU will restart; the display may blink and GPU applications may close. Continue?";
            if(!confirm(message.c_str(),L"GPU BIOS Override")) {pendingCpu_.reset();return;}
        }
        int slot=plan.value("slot",0);
        if(slot) {edited_.clear();target_=0;request(SlotLoad{slot});}
        else {eraseEdits(pl1Id,inputId);edited_.erase(dbOnId);auto cpu=pendingCpu_;pendingCpu_.reset();request(GpuApply{plan.at("gpu").get<GpuSettings>(),true,restart,cpu});}
    }
    FanSettings fanIntent() {
        auto s=state_.at("fan").get<FanSettings>();if(boundPages_[1]) {s.curve=checked(curveId);s.linked=checked(linkId);}
        if(boundPages_[1] && IsWindowEnabled(get(cpuChartId))) {Curve c;SendMessageW(get(cpuChartId),chartGetMessage,0,reinterpret_cast<LPARAM>(&c));s.cpu_curve=std::move(c);}
        if(boundPages_[1] && IsWindowEnabled(get(gpuChartId))) {Curve c;SendMessageW(get(gpuChartId),chartGetMessage,0,reinterpret_cast<LPARAM>(&c));s.gpu_curve=std::move(c);}return s;
    }
    ProtectionSettings protectionIntent() {return {checked(protectionId),number(thresholdId),checked(reduceId),checked(fanBoostId),checked(showWindowId)};}
    CoolerSettings coolerIntent() {
        CoolerSettings s;s.fan_on=checked(coolerFanOnId);s.fan_duty=number(coolerFanId);s.pump_on=checked(coolerPumpOnId);s.pump_duty=number(coolerPumpId);
        const PumpVoltage voltages[]{PumpVoltage::V7,PumpVoltage::V8,PumpVoltage::V11,PumpVoltage::V12};s.voltage=voltages[std::clamp(int(SendMessageW(get(voltageId),CB_GETCURSEL,0,0)),0,3)];
        s.rgb_on=checked(coolerRgbOnId);s.r=uint8_t(number(coolerRId));s.g=uint8_t(number(coolerGId));s.b=uint8_t(number(coolerBId));s.rgb_mode=CoolerRgbMode(SendMessageW(get(rgbModeId),CB_GETCURSEL,0,0));return s;
    }
    void coolerApply(CoolerSection section) {
        if(!coolerConnected_) {setStatus(L"Cooler disconnected; Apply refused.");return;}
        CoolerRequest c;c.action=CoolerAction::apply;c.section=section;c.persist=true;c.settings=coolerIntent();request(c);
    }
    void command(int id,int code) {
        if(updating_) return;
        if(code==EN_CHANGE && id>=1200) {edited_.insert(id-1000);hints();return;}
        if(code==EN_CHANGE && (id==cpuChartId || id==gpuChartId)) {edited_.insert(id);boundCharts_.erase(id);if(state_.value("linked",false) && !fanEditQueued_) {fanEditQueued_=true;PostMessageW(window_,fanEditMessage,0,0);}hints();return;}
        if(id==showId) {show();return;}if(id==exitId) {DestroyWindow(window_);return;}
        if(id>=slotId && id<slotId+5) {selectSlot(id-slotId+1);return;}
        if(busy_ || !worker_) return;
        switch(id) {
        case saveId: {SlotSave save{target_?target_:1};save.editor_fan=fanIntent();if(coolerAvailable_ && std::any_of(edited_.begin(),edited_.end(),[](int id){return id>=coolerFanOnId && id<=coolerRgbApplyId;})) save.editor_cooler=coolerIntent();request(std::move(save));break;}
        case readId:case powerReadId:read();break;case defaultsId:defaults();break;
        case powerApplyId:applyPower();break;
        case dbOnId:edited_.insert(id);enable(dbId,checked(id));break;
        case unlockId: {
            bool on=checked(unlockId);auto message=on?L"Raise the NVIDIA BIOS power ceiling and allow lower TGP / higher current limits? Higher limits increase heat and hardware stress. This is a persistent software override, not a BIOS flash. The GPU will restart; the display may blink and GPU applications may close. Continue?":L"Restore original NVIDIA BIOS power/current limits? The GPU will restart; the display may blink and GPU applications may close. Continue?";
            if(confirm(message,L"GPU BIOS Override")) request(GpuUnlock{on});else {updating_=true;SendMessageW(get(id),BM_SETCHECK,on?BST_UNCHECKED:BST_CHECKED,0);updating_=false;}break;
        }
        case dlppcId:request(DlppcApply{checked(id)});break;
        case boostId:edited_.insert(id);hints();break;
        case boostApplyId:edited_.erase(boostId);request(FanBoost{checked(boostId)});break;
        case linkId: {auto s=fanIntent();s.curve=true;updating_=true;SendMessageW(get(curveId),BM_SETCHECK,BST_CHECKED,0);updating_=false;eraseEdits(curveId,gpuChartId);request(FanApply{s});break;}
        case switchApplyId:eraseEdits(switchMsId,switchMsId);request(SwitchApply{checked(switchId),number(switchMsId)});break;
        case fanApplyId: {
            if(!state_.value("curvesSupported",false)) {request(FanMaster{checked(curveId)});break;}
            auto s=fanIntent();
            eraseEdits(curveId,gpuChartId);request(FanApply{s});break;
        }
        case protectionApplyId:request(ProtectionApply{protectionIntent()});break;
        case resetId:if(confirm(L"Check cooling, then reset the failsafe and reapply the active profile's power and fan settings?",L"Reset Thermal Failsafe")) request(ThermalReset{});break;
        case testId: {auto applied=state_.at("protection").get<ProtectionSettings>();auto description=actions(applied)+L"\n\nThis really applies emergency settings and latches protection. ";if(Json(protectionIntent())!=Json(applied)) description+=L"Unapplied control changes are NOT part of this test. ";description+=L"Continue?";if(confirm(description.c_str(),L"Test Failsafe")) request(ThermalTest{applied});break;}
        case detailsId:showRow(2,detailsTextId,!IsWindowVisible(get(detailsTextId)));break;
        case batteryApplyId:edited_.erase(batteryModeId);request(BatteryApply{int(SendMessageW(get(batteryModeId),CB_GETCURSEL,0,0))});break;
        case lightingReadId:eraseEdits(zoneBaseId,dimmingId);request(UiRead{4});break;
        case idleId:if(code==CBN_SELCHANGE) {const int seconds[]{0,10,30,60,120,300,600};request(LightingIdle{seconds[SendMessageW(get(id),CB_GETCURSEL,0,0)]});}break;
        case dimmingId:request(DimmingApply{checked(id)});break;
        case scanId:request(CoolerRequest{});break;
        case connectId: {
            CoolerRequest c;c.action=coolerConnected_?CoolerAction::disconnect:CoolerAction::connect;
            if(!coolerConnected_) {auto i=SendMessageW(get(devicesId),CB_GETCURSEL,0,0);if(i<0 || size_t(i)>=devices_.size()) {setStatus(L"Select a cooler first.");break;}c.address=devices_[size_t(i)].at("address").get<uint64_t>();}request(c);break;
        }
        case autoConnectId: {CoolerRequest c;c.action=CoolerAction::auto_connect;c.auto_connect=checked(id);request(c);break;}
        case retryId: {CoolerRequest c;c.action=CoolerAction::availability;request(c);break;}
        case coolerFanApplyId:coolerApply(CoolerSection::fan);break;case coolerPumpApplyId:coolerApply(CoolerSection::pump);break;case coolerRgbApplyId:coolerApply(CoolerSection::rgb);break;
        default:
            if(id>=zoneBaseId && id<zoneBaseId+48) {
                int zone=(id-zoneBaseId)/16,base=zoneBaseId+zone*16,field=id-base;
                if(field==5 || field==6) {const int wire[]{0,2,3};Zone s{checked(base),uint8_t(number(base+1)),uint8_t(number(base+2)),uint8_t(number(base+3)),number(base+4)};eraseEdits(base,base+4);request(LightingApply{wire[zone],s,field==6});}
                else if(field==7) {COLORREF colors[16]{};CHOOSECOLORW choice{sizeof choice};choice.hwndOwner=window_;choice.rgbResult=RGB(number(base+1),number(base+2),number(base+3));choice.lpCustColors=colors;choice.Flags=CC_FULLOPEN|CC_RGBINIT;if(ChooseColorW(&choice)) {eraseEdits(base+1,base+3);updating_=true;value(base+1,GetRValue(choice.rgbResult),0,255);value(base+2,GetGValue(choice.rgbResult),0,255);value(base+3,GetBValue(choice.rgbResult),0,255);updating_=false;for(int n=1;n<=3;++n) edited_.insert(base+n);}}
                else edited_.insert(id);
            } else if(code==BN_CLICKED || code==CBN_SELCHANGE) edited_.insert(id);
            break;
        }
        hints();
    }
    Json fanShape(FanSettings s) {
        auto normalize=[](std::optional<Curve>& c) {if(c && !c->temp_up.empty()) c=active_curve(*c,int(c->temp_up.size()));};normalize(s.cpu_curve);normalize(s.gpu_curve);
        return {{"curve",s.curve},{"linked",s.linked},{"cpu",nullable(s.cpu_curve)},{"gpu",state_.value("hasGpuFan",false)?nullable(s.gpu_curve):Json(nullptr)}};
    }
    std::wstring actions(const ProtectionSettings& p) const {
        auto limits=state_.value("actionLimits",Json::object());int supported=state_.value("thermalActions",0);
        std::wstring s=L"On trip: ";
        if(supported&Gpu) s+=L"request this model's minimum GPU power ("+std::to_wstring(limits.value("gpu",0))+L" W), disable Dynamic Boost";else s+=L"GPU protection unavailable on this model";
        if(p.reduce_cpu && (supported&Cpu)) s+=L"; CPU PL1/PL2/PL4 → "+std::to_wstring(limits.value("pl1",0))+L"/"+std::to_wstring(limits.value("pl2",0))+L"/"+std::to_wstring(limits.value("pl4",0))+L" W (temperature limit unchanged)";
        if(p.fan_boost && (supported&Fan)) s+=L"; enable laptop Fan Boost";
        if(p.reduce_cpu && !(supported&Cpu)) s+=L"; CPU reduction unavailable";
        if(p.fan_boost && !(supported&Fan)) s+=L"; Fan Boost unavailable";
        return s+L".";
    }
    void hints() {
        if(!ready_ || !IsWindowVisible(window_)) return;int page=TabCtrl_GetCurSel(tabs_);
        try {
            if(page==1 && state_.contains("fan")) {
                auto editor=fanShape(fanIntent()),applied=fanShape(state_["fan"].get<FanSettings>());bool dirty=editor!=applied;
                auto saved=state_.value("saved",Json(nullptr));bool matches=!saved.is_null() && !saved["fan"].is_null() && editor==fanShape(saved["fan"].get<FanSettings>());
                auto status=dirty?L"Changes not applied":state_.value("fanApplication",0)==1?L"Requested curves applied":state_.value("fanApplication",0)==2?L"Partial application — review before saving":L"Requested curves not applied/verified";
                set(fanTextId,std::wstring(status)+L"\r\n"+(matches?L"Editor curves match saved profile":L"Editor curves not saved to profile")+L"; linked: "+(state_.value("linked",false)?L"yes":L"no")+(state_.value("fanReview",false)?L"\r\nGeneric seed: review and Apply before saving":L"")+outcome_[1]);
            }
            if(page==2 && state_.contains("protection")) {
                auto applied=state_["protection"],pending=Json(protectionIntent()),saved=state_.value("saved",Json(nullptr));bool dirty=pending!=applied;
                std::wstring status=dirty?L"Changes not applied. ":L"";
                status+=saved.is_null()?L"No saved profile loaded":saved["gpuFailsafe"]==applied?L"Applied settings match saved profile":L"Applied settings not saved to profile";
                set(thermalHintId,status+outcome_[2]);set(previewId,actions(pending.get<ProtectionSettings>()));
            }
        } catch(const std::exception& e) {set(page==1?fanTextId:thermalHintId,L"Pending editor values are invalid: "+w(e.what()));}
    }
    void chart(int id,const Json& value,bool readOnly,bool linear,int dutyBase) {
        ChartOptions options{readOnly,linear,dutyBase};auto bound=boundCharts_.find(id);
        if(bound!=boundCharts_.end() && bound->second.first==value && bound->second.second==options) return;
        boundCharts_[id]={value,options};SendMessageW(get(id),chartOptionsMessage,0,reinterpret_cast<LPARAM>(&options));std::optional<Curve> curve;if(!value.is_null()) curve=value.get<Curve>();
        SendMessageW(get(id),chartSetMessage,0,curve?reinterpret_cast<LPARAM>(&*curve):0);
    }
    void bindPage(int page) {
        if(!ready_ || !IsWindowVisible(window_)) return;updating_=true;
        auto features=state_.value("enabled",std::set<std::string>{});bool linked=state_.value("linked",false);
        enable(readId,!busy_ && !(page==1 && linked));
        if(page==0) {
            bool cpu=state_.contains("cpu") && !state_["cpu"].is_null();for(int id:{pl1Id,pl2Id,pl4Id,tempId}) enable(id,cpu);
            if(cpu) for(auto [id,key]:{std::pair(pl1Id,"pl1"),std::pair(pl2Id,"pl2"),std::pair(pl4Id,"pl4"),std::pair(tempId,"tempLimit")}) {auto& v=state_["cpu"][key];value(id,v["value"],v["min"],v["max"]);}
            bool gpu=!gpuTelemetry_.is_null() && gpuError_.empty();for(int id:{tgpId,dbOnId}) enable(id,gpu);
            if(gpu) gpuLive(gpuTelemetry_,powerRead_);else for(int id:{nvId,msId,inputId,unlockId,dlppcId}) enable(id,false);
            enable(dbId,gpu && checked(dbOnId));powerRead_=false;enable(powerApplyId,cpu || gpu);refreshPower();
        }
        if(page==1 && state_.contains("fan")) {
            auto f=state_["fan"].get<FanSettings>();bool supported=state_.value("curvesSupported",false),hasGpu=state_.value("hasGpuFan",false);
            checked(curveId,f.curve);checked(linkId,linked);enable(cpuChartId,supported && f.cpu_curve && !f.cpu_curve->temp_up.empty());enable(gpuChartId,supported && hasGpu && f.gpu_curve && !f.gpu_curve->temp_up.empty());
            enable(linkId,state_.value("linkAvailable",false));for(int id:{curveId,fanApplyId}) enable(id,features.contains("fanControl") && !linked);
            for(int id:{boostId,boostApplyId,switchId,switchMsId,switchApplyId}) enable(id,features.contains("fanControl"));
            if(!edited_.contains(cpuChartId)) chart(cpuChartId,supported?nullable(f.cpu_curve):Json(nullptr),false,linked,state_.value("dutyBaseCpu",0));
            if(!edited_.contains(gpuChartId)) chart(gpuChartId,supported && hasGpu?nullable(f.gpu_curve):Json(nullptr),false,linked,state_.value("dutyBaseGpu",0));
            showRow(1,gpuChartId,hasGpu);showRow(1,debugCpuId,linked);showRow(1,debugGpuId,linked && hasGpu);
            if(linked) for(auto [id,key,base]:{std::tuple(debugCpuId,"writtenCpu","dutyBaseCpu"),std::tuple(debugGpuId,"writtenGpu","dutyBaseGpu")}) {auto value=state_.value(key,Json(nullptr));EnableWindow(get(id),TRUE);chart(id,value,true,true,state_.value(base,0));}
            if(state_.contains("boost")) checked(boostId,state_["boost"]);if(state_.contains("switchEnabled")) checked(switchId,state_["switchEnabled"]);if(state_.contains("switchMs")) value(switchMsId,std::max(100,state_["switchMs"].get<int>()),100,12700);
        }
        if(page==2 && state_.contains("protection")) {
            auto p=state_["protection"].get<ProtectionSettings>();checked(protectionId,p.enabled);value(thresholdId,p.threshold,50,95);checked(reduceId,p.reduce_cpu);checked(fanBoostId,p.fan_boost);checked(showWindowId,p.show_window);
            int a=state_.value("thermalActions",0);for(int id:{protectionId,thresholdId,protectionApplyId,showWindowId}) enable(id,(a&Gpu)!=0);enable(testId,(a&Gpu)!=0 && !tripped_);enable(resetId,tripped_);enable(reduceId,(a&Cpu)!=0);enable(fanBoostId,(a&Fan)!=0);
            set(thermalTextId,w(state_.value("thermalStatus",std::string{}))+L"\r\nCurrent GPU temperature: "+describe(state_["temperature"])+L"Mean: "+describe(state_["average"])+L"Peak: "+describe(state_["peak"])+w(warning_));
            auto history=state_.value("thermal",Json(nullptr));if(history!=boundHistory_) {boundHistory_=history;set(historyId,L"Historical state (not current hardware verification):\r\n"+describe(history));}
        }
        if(page==3) {for(int id:{batteryModeId,batteryApplyId}) enable(id,features.contains("battery"));if(state_.contains("batteryMode") && !edited_.contains(batteryModeId)) SendMessageW(get(batteryModeId),CB_SETCURSEL,state_["batteryMode"].get<int>(),0);}
        if(page==5) updateInfo();
        if(page==3 || page==4 || page==6) bindPeripherals(page);
        boundPages_[page]=true;updating_=false;hints();
    }
    void presentation(const Json& j,bool initial) {
        int readPage=j.value("readPage",-3),old=state_.value("activeSlot",-1);
        if(j.contains("fan") && state_.contains("fan") && j["fan"]!=state_["fan"] && !std::any_of(edited_.begin(),edited_.end(),[](int id){return id>=curveId && id<=gpuChartId;})) boundPages_[1]=false;
        if(readPage==0) {for(auto key:{"cpu","cpuError","gpu","gpuError"}) state_.erase(key);powerRead_=true;}
        if(readPage==1) state_.erase("fanError");if(readPage==3) state_.erase("batteryError");
        for(auto i=j.begin();i!=j.end();++i) state_[i.key()]=i.value();
        if(initial && !ready_) {ready_=true;busy(false);setStatus(L"Controller ready.");}
        if(j.contains("identity")) {identity_=j["identity"];SetWindowTextW(window_,(L"MechControl — "+w(state_.value("model",std::string{}))).c_str());}
        bool selection=old!=state_.value("activeSlot",0);if(selection) target_=state_.value("activeSlot",0);if(selection || j.contains("slots") || j.contains("names")) updateSlots();
        bool wasTripped=tripped_;tripped_=state_.value("tripped",false);warning_=state_.value("warning",std::string{});if(!configurationWarning_.empty()) warning_+=(warning_.empty()?"":"\n")+configurationWarning_;
        if(j.contains("gpu") || j.contains("gpuLive")) {gpuTelemetry_=j.contains("gpuLive")?j["gpuLive"]:j["gpu"];gpuError_.clear();state_.erase("gpuError");}
        if(j.contains("gpuError")) gpuError_=L"Live read failed: "+w(j["gpuError"].get<std::string>());
        if(IsWindowVisible(window_)) {int page=TabCtrl_GetCurSel(tabs_);if(readPage>=0 || readPage==-1 || initial || page==1 || page==2 || j.contains("gpuLive") || j.contains("gpuError")) bindPage(page);}
        updateTray();if(tripped_ && !wasTripped) alert();
        if(alert_ && !tripped_) DestroyWindow(alert_);
        if(alert_) SetWindowTextW(alertText_,(w(state_.value("thermalStatus",std::string{}))+L"\r\n\r\nCheck the cooler. Closing this alert does NOT reset the failsafe.").c_str());
    }
    void gpuLive(const Json& gpu,bool editor) {
        if(gpu.is_null()) return;
        gpuTelemetry_=gpu;
        for(auto [id,key]:{std::pair(tgpId,"tgp"),std::pair(dbId,"dynamicBoostWatts")}) {auto& v=gpu.at(key);value(id,v["value"],v["min"],v["max"]);}
        checked(dbOnId,gpu.value("dynamicBoost",false));checked(unlockId,gpu.value("unlocked",false));checked(dlppcId,state_.value("dlppcDisabled",false));enable(unlockId,gpu.value("unlockAvailable",false));enable(dlppcId,gpu.contains("dlppc") && !gpu["dlppc"].is_null());
        if(gpu.contains("unlockMin") && gpu.contains("unlockMax")) set(unlockId,L"Unlock GPU power / current limits (TGP "+std::to_wstring(gpu["unlockMin"].get<int>())+L"-"+std::to_wstring(gpu["unlockMax"].get<int>())+L" W)");
        for(auto [id,key]:{std::pair(nvId,"nvvdd"),std::pair(msId,"msvdd"),std::pair(inputId,"boardInput")}) {
            auto& rail=gpu.at(key);bool previouslyEnabled=IsWindowEnabled(get(id))!=FALSE;enable(id,!rail.is_null());
            if(rail.is_null()) {set(id+1000,L"--");continue;}
            if(editor || !previouslyEnabled) {
                // Positions round down, so the detected limit never sits above the slider maximum.
                int step=id==inputId?1:1000,limit=rail["limitMa"],lo=(rail["minMa"].get<int>()+step-1)/step;if(!edited_.contains(id)) detectedMa_[id]=limit;
                value(id,limit/step,lo,std::max(lo,rail["maxMa"].get<int>()/step));
            }
            set(id+1000,display(id,number(id)));
        }
        if(TabCtrl_GetCurSel(tabs_)==5) updateInfo();
    }
    void updateInfo() {
        std::wstring sensors;if(lastSensors_) {auto& s=*lastSensors_;auto v=[](auto x){return x?std::to_wstring(*x):L"--";};
            sensors=L"CPU: "+v(s.cpu_temp)+L" °C; GPU: "+v(s.gpu_temp)+L" °C; CPU limit: "+v(s.cpu_limit)+L" °C\r\n";
            sensors+=L"System: "+v(s.system_watts)+L" W; Adapter: "+(s.adapter_amps?decimal(*s.adapter_amps):L"--")+L" A; TGP: "+v(s.tgp_live)+L" W; Dynamic Boost: "+v(s.db_live)+L" W\r\n";}
        set(infoId,L"System and model detection:\r\n"+describe(identity_)+L"\r\nLive sensors:\r\n"+sensors+L"\r\nGPU limits:\r\n"+(gpuError_.empty()?L"":L"STALE — "+gpuError_+L"\r\n")+describe(gpuTelemetry_)+L"\r\nDisabled features:\r\n"+(state_.contains("disabled")?describe(state_["disabled"]):L""));
    }
    void peripherals(const Json& j) {
        if(j.contains("battery")) peripheralState_.erase("batteryError");if(j.contains("lightingIntent")) for(auto key:{"logoError","dimmingError"}) peripheralState_.erase(key);
        for(auto i=j.begin();i!=j.end();++i) if(i.key()!="ui") peripheralState_[i.key()]=i.value();
        if(j.contains("coolerAvailable")) coolerAvailable_=j["coolerAvailable"].get<bool>();
        if(IsWindowVisible(window_)) {int page=TabCtrl_GetCurSel(tabs_);if((page==3 && (j.contains("battery") || j.contains("batteryError"))) || (page==4 && j.contains("lightingIntent")) || (page==6 && (j.contains("cooler") || j.contains("coolerAvailable")))) bindPeripherals(page);}
    }
    void bindPeripherals(int page) {
        const auto& j=peripheralState_;
        updating_=true;
        if(page==3 && j.contains("battery")) {const auto& b=j["battery"];auto voltage=b.value("designVoltageMv",0)/1000.0;
            auto capacity=[&](const char* key){int n=b.value(key,0);return std::to_wstring(n)+L" mAh (~"+decimal(n*voltage/1000)+L" Wh)";};
            set(batteryTextId,L"Power source: "+std::wstring(b.value("onAC",false)?L"AC (plugged in)":L"Battery")+L"\r\nCharge: "+std::to_wstring(b.value("chargePercent",0))+L"%\r\nTemperature: "+decimal(b.value("temperatureCelsius",0.0))+L" °C\r\nCycle count: "+std::to_wstring(b.value("cycleCount",0))+L"\r\nDesign capacity: "+capacity("designCapacityMah")+L"\r\nFull charge capacity: "+capacity("fullChargeCapacityMah")+L"\r\nRemaining capacity: "+capacity("remainingCapacityMah")+L"\r\nVoltage: "+std::to_wstring(b.value("presentVoltageMv",0))+L" mV (design "+std::to_wstring(b.value("designVoltageMv",0))+L" mV)\r\nCharge rate: "+std::to_wstring(b.value("chargeRateMa",0))+L" mA\r\nHealth: "+std::to_wstring(b.value("healthPercent",0))+L"%");}
        if(page==3 && j.contains("batteryError")) set(batteryTextId,L"Battery read failed; previous telemetry is stale: "+w(j["batteryError"].get<std::string>()));
        if(page==4 && j.contains("lightingIntent")) {
            auto s=j["lightingIntent"].get<LightingState>();std::optional<Zone> zones[]{s.keyboard,s.light_bar,s.logo};
            if(j.contains("logo")) zones[2]=j["logo"].get<Zone>();
            for(int i=0;i<3;++i) {int base=zoneBaseId+i*16;bool available=j.contains("zones") && j["zones"][i].get<bool>() && zones[i].has_value();
                if(i==2 && j.contains("logoError")) available=false;
                for(int n=0;n<=7;++n) enable(base+n,available);
                if(zones[i]) {auto z=*zones[i];checked(base,z.enabled);value(base+1,z.r,0,255);value(base+2,z.g,0,255);value(base+3,z.b,0,255);value(base+4,z.brightness,0,100);}}
            const int seconds[]{0,10,30,60,120,300,600};for(int i=0;i<7;++i) if(seconds[i]==s.idle_seconds) SendMessageW(get(idleId),CB_SETCURSEL,i,0);
        }
        if(page==4) {
            if(j.contains("logoError")) setStatus(L"Logo read failed independently: "+w(j["logoError"].get<std::string>()));
            if(j.contains("dimmingError")) setStatus(L"Local dimming read failed independently: "+w(j["dimmingError"].get<std::string>()));
            if(j.contains("dimmingAvailable")) enable(dimmingId,j["dimmingAvailable"].get<bool>() && !j.contains("dimmingError"));if(j.contains("localDimming")) checked(dimmingId,j["localDimming"]);
        }
        if(page==6) {
            for(int id:{scanId,connectId,autoConnectId,retryId}) enable(id,coolerAvailable_);
            for(int id:{coolerFanApplyId,coolerPumpApplyId,coolerRgbApplyId}) enable(id,coolerAvailable_ && coolerConnected_);
            if(!coolerAvailable_) {set(coolerTextId,L"Model/runtime cooler support unavailable");showRow(6,retryId,false);}else if(j.contains("cooler")) cooler(j["cooler"]);
        }
        updating_=false;
    }
    void cooler(const Json& j) {
        coolerConnected_=j.value("connected",false);set(coolerTextId,w(j.value("status",std::string{}))+L"\r\nFirmware: "+w(j.value("firmware",std::string{}))+(j.value("completed",false)?j.value("ok",true)?L"\r\nCompleted":L"\r\nFailed":L""));
        set(connectId,coolerConnected_?L"Disconnect":L"Connect");for(int id:{coolerFanApplyId,coolerPumpApplyId,coolerRgbApplyId}) enable(id,coolerConnected_);
        auto devices=j.value("devices",Json::array());auto saved=j.value("state",Json::object());auto address=saved.value("savedAddress",uint64_t{});
        if(address && std::none_of(devices.begin(),devices.end(),[&](const auto& d){return d["address"]==address;})) devices.push_back({{"address",address},{"name",saved.value("savedDeviceName",Json(nullptr)).is_string()?saved["savedDeviceName"].get<std::string>():"Cooler (saved)"}});
        if(devices!=devices_) {uint64_t selected=0;auto index=SendMessageW(get(devicesId),CB_GETCURSEL,0,0);if(index>=0 && size_t(index)<devices_.size()) selected=devices_[size_t(index)]["address"].get<uint64_t>();devices_=std::move(devices);SendMessageW(get(devicesId),CB_RESETCONTENT,0,0);int choose=0,i=0;
            for(auto& d:devices_) {auto a=d["address"].get<uint64_t>();wchar_t mac[24]{};swprintf_s(mac,L"%02X:%02X:%02X:%02X:%02X:%02X",unsigned(a>>40)&255,unsigned(a>>32)&255,unsigned(a>>24)&255,unsigned(a>>16)&255,unsigned(a>>8)&255,unsigned(a)&255);auto name=w(d.value("name",std::string{}))+L" ("+mac+L")"+(a==address?L" [saved]":L"");SendMessageW(get(devicesId),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));if(a==selected || (!selected && a==address)) choose=i;++i;}if(!devices_.empty()) SendMessageW(get(devicesId),CB_SETCURSEL,choose,0);}
        bool unavailable=j.contains("bluetoothAvailable") && j["bluetoothAvailable"]==false;showRow(6,retryId,unavailable);
        if(unavailable) set(coolerTextId,L"Bluetooth is off or unavailable.\r\nEnable Bluetooth in Windows Settings or Device Manager, then press Retry.\r\n"+w(j.value("status",std::string{})));enable(scanId,coolerAvailable_ && !unavailable);enable(connectId,coolerAvailable_ && !unavailable);
        if(j.contains("state")) checked(autoConnectId,j["state"].value("autoConnect",false));
        if(j.contains("intent")) {
            auto s=j["intent"].get<CoolerSettings>();checked(coolerFanOnId,s.fan_on);value(coolerFanId,s.fan_duty,0,100);checked(coolerPumpOnId,s.pump_on);value(coolerPumpId,s.pump_duty,0,100);
            int voltage=s.voltage==PumpVoltage::V7?0:s.voltage==PumpVoltage::V8?1:s.voltage==PumpVoltage::V11?2:3;if(!edited_.contains(voltageId)) SendMessageW(get(voltageId),CB_SETCURSEL,voltage,0);
            checked(coolerRgbOnId,s.rgb_on);value(coolerRId,s.r,0,255);value(coolerGId,s.g,0,255);value(coolerBId,s.b,0,255);if(!edited_.contains(rgbModeId)) SendMessageW(get(rgbModeId),CB_SETCURSEL,int(s.rgb_mode),0);
        }
    }
    void sensors(const Sensors& s) {
        lastSensors_=s;
        const wchar_t* modes[]{L"High Capacity",L"Balanced",L"Stationary"};auto mode=s.battery_mode && *s.battery_mode>=0 && *s.battery_mode<3?modes[*s.battery_mode]:L"--";wchar_t raw[40]{};if(s.battery_raw) swprintf_s(raw,L" [0x%03X=0x%02X]",state_.value("batteryRegister",0),*s.battery_raw);
        auto percent=[](auto x){return x?std::to_wstring(*x)+L"%":std::wstring(L"--");};auto on=[](auto x){return x?std::wstring(*x?L"ON":L"OFF"):L"--";};
        std::array<std::wstring,6> monitor{L"CPU Fan: "+percent(s.cpu_percent)+L" ("+std::to_wstring(s.cpu_rpm)+L" RPM)",L"GPU Fan: "+percent(s.gpu_percent)+L" ("+std::to_wstring(s.gpu_rpm)+L" RPM)",
            L"CPU Temp Limit: "+(s.cpu_limit?std::to_wstring(*s.cpu_limit)+L"°C":L"--"),L"Battery: "+percent(s.battery_soc)+L" ("+mode+L")"+raw,L"Fan Boost: "+on(s.boost),L"Fan Curve: "+on(s.curve_enabled)};
        if(monitor!=monitor_) {monitor_=std::move(monitor);updateBar();}
        if(TabCtrl_GetCurSel(tabs_)==5) updateInfo();
        if(TabCtrl_GetCurSel(tabs_)==1) for(auto [id,temp]:{std::pair(cpuChartId,s.cpu_temp),std::pair(debugCpuId,s.cpu_temp),std::pair(gpuChartId,s.gpu_temp),std::pair(debugGpuId,s.gpu_temp)}) if(IsWindowVisible(get(id))) SendMessageW(get(id),chartTemperatureMessage,temp?*temp+1:0,0);
    }
    void results() {
        for(auto& result:delivery_->take()) {
            if(result.kind==ResultKind::selection) {selectModel(result.selection);continue;}
            if(result.kind==ResultKind::failure) {MessageBoxW(window_,(result.text+L"\nMake sure UWACPIDriver is installed and run as Administrator. See mechcontrol.log.").c_str(),L"MechControl — Controller Error",MB_OK|MB_ICONERROR);DestroyWindow(window_);PostQuitMessage(1);return;}
            bool own=result.sequence && result.sequence==busySequence_;
            if(own) busy(false);
            if(result.control) {
                auto& r=*result.control;
                if(r.peripherals) {auto& p=*r.peripherals;
                    if(p.contains("ui")) {presentation(p["ui"],p["ui"].contains("identity"));if(own) setStatus(L"Read completed. Independent errors are shown without replacing successful values.");}
                    peripherals(p);
                    if(p.contains("plan") && own) {applyPlan(p["plan"]);continue;}
                }
                if(own && (!r.peripherals || !r.peripherals->contains("ui"))) {
                    bool catalog=operation_ && (std::holds_alternative<SlotSave>(*operation_) || std::holds_alternative<SlotRename>(*operation_) || std::holds_alternative<SlotClear>(*operation_));
                    if(!catalog) outcome_[operationPage_]=L"\r\nLast action: "+result.text+(r.partial?L" [partial; review readback]":r.applied?L" [applied]":L"");
                    if(operation_ && (std::holds_alternative<SlotSave>(*operation_) || std::holds_alternative<SlotLoad>(*operation_))) {if(r.code || r.partial) configurationWarning_=r.text;else configurationWarning_.clear();}
                    if(!r.code && operation_ && std::holds_alternative<ProtectionApply>(*operation_)) eraseEdits(protectionId,showWindowId);
                    if(!r.code && operation_ && (std::holds_alternative<ThermalReset>(*operation_) || std::holds_alternative<SlotLoad>(*operation_) || std::holds_alternative<DefaultsApply>(*operation_))) {eraseEdits(pl1Id,showWindowId);boundPages_[1]=false;boundCharts_.clear();}
                    setStatus(result.text+(r.slot_committed?L" [slot committed]":L"")+(r.partial?L" [partial]":L"")+(r.applied?L" [applied]":L"")+(r.saved?L" [saved]":L""));
                    if(r.code) {pendingCpu_.reset();MessageBoxW(window_,result.text.c_str(),L"MechControl — Not completed",MB_OK|MB_ICONERROR);}
                    hints();
                }
            } else if(result.kind!=ResultKind::sample && !result.text.empty()) setStatus(result.text);
            if(result.sensors && IsWindowVisible(window_)) sensors(*result.sensors);
        }
    }
    void selectModel(const std::shared_ptr<ModelSelection>& selection) {
        struct Template {DLGTEMPLATE dialog;WORD menu=0,type=0,title=0;} layout{};layout.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_CENTER;layout.dialog.cx=510;layout.dialog.cy=310;
        ModelDialog data{selection->paths,font_};auto result=DialogBoxIndirectParamW(instance_,&layout.dialog,window_,modelProcedure,reinterpret_cast<LPARAM>(&data));
        if(result==IDOK && data.chosen>=0) selection->chosen=selection->paths[size_t(data.chosen)];
        if(result==-1) log_.write("Manual model selection dialog failed: "+std::to_string(GetLastError()));
        SetEvent(selection->completed.get());if(!selection->chosen) {log_.write("Manual model selection canceled or failed");DestroyWindow(window_);}
    }
    void checkOem() {
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));if(!snapshot) return;PROCESSENTRY32W p{sizeof p};std::wstring conflicts;
        if(Process32FirstW(snapshot.get(),&p)) do {if(_wcsicmp(p.szExeFile,L"GCUService.exe")==0 || _wcsicmp(p.szExeFile,L"GCUBridge.exe")==0) conflicts+=std::wstring(p.szExeFile)+L" (PID "+std::to_wstring(p.th32ProcessID)+L")\n";}while(Process32NextW(snapshot.get(),&p));
        if(!conflicts.empty()) {log_.write("OEM conflicts: "+utf8(conflicts));MessageBoxW(window_,(L"The following OEM processes are running and may overwrite your settings:\n\n"+conflicts+L"\nConsider stopping them before using MechControl.").c_str(),L"MechControl — OEM Conflict Warning",MB_OK|MB_ICONWARNING);}
    }
    void createIcons() {
        for(int i=0;i<5;++i) {
            HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,32,32),mask=CreateBitmap(32,32,1,1,nullptr);auto old=SelectObject(dc,bitmap);
            RECT rect{0,0,32,32};HBRUSH brush=CreateSolidBrush(RGB(30+i*35,100,190-i*25));FillRect(dc,&rect,brush);DeleteObject(brush);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));auto digit=std::to_wstring(i+1);DrawTextW(dc,digit.c_str(),-1,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            ICONINFO info{TRUE,0,0,mask,bitmap};slotIcons_[i]=CreateIconIndirect(&info);SelectObject(dc,old);DeleteObject(bitmap);DeleteObject(mask);DeleteDC(dc);ReleaseDC(nullptr,screen);
        }
        for(int i=0;i<40;++i) {auto resource=FindResourceW(instance_,MAKEINTRESOURCEW(1000+i),RT_RCDATA);if(!resource) continue;auto data=LoadResource(instance_,resource);auto bytes=static_cast<PBYTE>(LockResource(data));auto icon=CreateIconFromResourceEx(bytes,SizeofResource(instance_,resource),TRUE,0x30000,32,32,LR_DEFAULTCOLOR);if(icon) animation_.push_back(icon);}
    }
    void tray(bool add=false) {
        if(closing_) return;
        NOTIFYICONDATAW icon{sizeof icon};icon.hWnd=window_;icon.uID=1;icon.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;icon.uCallbackMessage=trayMessage;
        auto tip=tripped_?L"MechControl — THERMAL PROTECTION HOLD":!warning_.empty()?L"MechControl — CONFIGURATION WARNING":L"MechControl";
        wcscpy_s(icon.szTip,tip);icon.hIcon=LoadIconW(nullptr,tripped_?IDI_ERROR:!warning_.empty()?IDI_WARNING:IDI_APPLICATION);
        if(!tripped_ && warning_.empty() && target_>=1 && target_<=5) icon.hIcon=target_==5 && !animation_.empty()?animation_[animationFrame_]:slotIcons_[target_-1];
        const bool wasAdded=trayAdded_,adding=add || !wasAdded;
        trayAdded_=Shell_NotifyIconW(adding?NIM_ADD:NIM_MODIFY,&icon)!=FALSE;
        if(trayAdded_) {
            if(adding) {icon.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&icon);log_.write("Tray icon registration succeeded");}
        } else if(add || wasAdded) log_.write("Tray icon unavailable; waiting for Explorer's TaskbarCreated notification");
    }
    void updateTray() {
        int state=tripped_?-2:!warning_.empty()?-1:target_;if(state==trayState_) return;
        trayState_=state;KillTimer(window_,1);animationFrame_=0;
        if(state==5 && animation_.size()>1) SetTimer(window_,1,67,nullptr);
        TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=const_cast<LPWSTR>(tripped_?L"Thermal Failsafe (!)":!warning_.empty()?L"Thermal Failsafe (configuration !)":L"Thermal Failsafe");TabCtrl_SetItem(tabs_,2,&item);
        if(trayAdded_) tray();
    }
    void balloon(const std::wstring& title,const std::wstring& text,DWORD flags) {NOTIFYICONDATAW icon{sizeof icon};icon.hWnd=window_;icon.uID=1;icon.uFlags=NIF_INFO;icon.dwInfoFlags=flags;wcsncpy_s(icon.szInfoTitle,title.c_str(),_TRUNCATE);wcsncpy_s(icon.szInfo,text.c_str(),_TRUNCATE);Shell_NotifyIconW(NIM_MODIFY,&icon);}
    void alert() {
        balloon(L"MechControl Thermal Failsafe",w(state_.value("thermalStatus",std::string{})),NIIF_ERROR);MessageBeep(MB_ICONHAND);
        if(state_.at("protection").get<ProtectionSettings>().show_window) {show();TabCtrl_SetCurSel(tabs_,2);selectPage();}
        if(alert_) return;
        alert_=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,L"STATIC",L"MechControl — Thermal Failsafe",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_VISIBLE, CW_USEDEFAULT,CW_USEDEFAULT,scale(560),scale(200),window_,nullptr,instance_,nullptr);
        SetWindowSubclass(alert_,alertProcedure,1,reinterpret_cast<DWORD_PTR>(this));
        alertText_=CreateWindowW(L"STATIC",L"Check the cooler. Closing this alert does NOT reset the failsafe.",WS_CHILD|WS_VISIBLE,scale(12),scale(12),scale(520),scale(100),alert_,nullptr,instance_,nullptr);
        auto open=CreateWindowW(L"BUTTON",L"Open Failsafe Settings",WS_CHILD|WS_VISIBLE|WS_TABSTOP,scale(12),scale(118),scale(520),scale(30),alert_,reinterpret_cast<HMENU>(1),instance_,nullptr);
        for(auto child:{alertText_,open}) SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
    }
    void show() {ShowWindow(window_,SW_RESTORE);SetForegroundWindow(window_);if(worker_) {worker_->request(Visibility{true});worker_->request(UiPage{TabCtrl_GetCurSel(tabs_)});}}
    void menu() {
        HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,showId,L"Show");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        for(int i=0;i<5;++i) {auto name=std::to_wstring(i+1)+L": "+text(get(slotId+i))+(emptySlot(i)?L" (empty)":L"");AppendMenuW(menu,MF_STRING|(target_==i+1?MF_CHECKED:0),slotId+i,name.c_str());}
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,exitId,L"Exit");POINT p{};GetCursorPos(&p);SetForegroundWindow(window_);auto id=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,window_,nullptr);DestroyMenu(menu);PostMessageW(window_,WM_NULL,0,0);if(id) command(id,0);
    }
    void shutdown() {
        if(closing_) return;closing_=true;delivery_->close();
#ifdef MECHCONTROL_WITH_CLI
        cli_.reset();
#endif
        if(worker_) worker_->stop();
        if(trayAdded_) {NOTIFYICONDATAW icon{sizeof icon};icon.hWnd=window_;icon.uID=1;Shell_NotifyIconW(NIM_DELETE,&icon);trayAdded_=false;}
        log_.write("Orderly shutdown completed");
    }
    LRESULT message(UINT message,WPARAM wp,LPARAM lp) {
        if(taskbarMessage_ && message==taskbarMessage_) {tray(true);return 0;}
        switch(message) {
        case WM_CREATE:initialize();return 0;
        case WM_TIMER:if(wp==1) {if(trayState_!=5 || ++animationFrame_>=animation_.size()) {KillTimer(window_,1);animationFrame_=animation_.empty()?0:animation_.size()-1;}else tray();}return 0;
        case WM_SHOWWINDOW:if(worker_) worker_->request(Visibility{wp!=0});if(wp && ready_) bindPage(TabCtrl_GetCurSel(tabs_));break;
        case WM_SIZE:if(wp==SIZE_MINIMIZED) ShowWindow(window_,SW_HIDE);else layout();return 0;
        // The window paints only the monitor bar; its children cover the rest.
        case WM_ERASEBKGND: {RECT r{};GetClientRect(window_,&r);r.bottom=bar_.top;FillRect(reinterpret_cast<HDC>(wp),&r,GetSysColorBrush(COLOR_BTNFACE));return 1;}
        case WM_PAINT: {PAINTSTRUCT ps{};auto dc=BeginPaint(window_,&ps);paintBar(dc);EndPaint(window_,&ps);return 0;}
        case WM_DRAWITEM:drawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));return TRUE;
        case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize={scale(760),scale(480)};return 0;
        case WM_DPICHANGED: {dpi_=HIWORD(wp);auto r=reinterpret_cast<RECT*>(lp);SetWindowPos(window_,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);applyDpi();return 0;}
        case WM_NOTIFY:if(reinterpret_cast<NMHDR*>(lp)->idFrom==tabsId && reinterpret_cast<NMHDR*>(lp)->code==TCN_SELCHANGE) selectPage();return 0;
        case WM_MOUSEWHEEL:scroll(pages_[TabCtrl_GetCurSel(tabs_)].window,message,wp);return 0;
        case WM_HSCROLL:if(!updating_ && lp) {int id=GetDlgCtrlID(reinterpret_cast<HWND>(lp));if(id>=200 && id<1000) {updating_=true;set(id+1000,display(id,int(SendMessageW(reinterpret_cast<HWND>(lp),TBM_GETPOS,0,0))));updating_=false;edited_.insert(id);hints();}}return 0;
        case WM_COMMAND:command(LOWORD(wp),HIWORD(wp));return 0;
        case WM_CONTEXTMENU: {int id=GetDlgCtrlID(reinterpret_cast<HWND>(wp));if(id>=slotId && id<slotId+5) {POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};if(p.x==-1) {RECT r{};GetWindowRect(get(id),&r);p={r.left,r.bottom};}slotMenu(id-slotId+1,p);}return 0;}
        case resultMessage:results();return 0;
        case fanEditMessage:fanEditQueued_=false;if(worker_ && state_.value("linked",false)) {auto intent=fanIntent();worker_->request(FanEdit{intent});state_["fan"]=intent;state_["fanApplication"]=0;hints();}return 0;
        case trayMessage:if(LOWORD(lp)==WM_CONTEXTMENU) menu();else if(LOWORD(lp)==NIN_SELECT || LOWORD(lp)==NIN_KEYSELECT) show();return 0;
        case WM_POWERBROADCAST:
            if(worker_ && wp==PBT_APMPOWERSTATUSCHANGE) {SYSTEM_POWER_STATUS s{};if(GetSystemPowerStatus(&s) && s.ACLineStatus!=255) worker_->request(AcEvent{s.ACLineStatus==1});}
            if(worker_ && (wp==PBT_APMSUSPEND || wp==PBT_APMRESUMEAUTOMATIC)) worker_->request(PowerEvent{wp==PBT_APMSUSPEND});return TRUE;
        case WM_CLOSE:ShowWindow(window_,SW_HIDE);return 0;
        case WM_QUERYENDSESSION:return TRUE;
        case WM_ENDSESSION:if(wp) DestroyWindow(window_);return 0;
        case WM_DESTROY:shutdown();PostQuitMessage(0);return 0;
        }
        return DefWindowProcW(window_,message,wp,lp);
    }
    HINSTANCE instance_;HWND window_=nullptr,tabs_=nullptr,progress_=nullptr,alert_=nullptr,alertText_=nullptr;
    HWND tooltip_=nullptr,hot_=nullptr;RECT bar_{};std::vector<BarText> barText_;std::wstring status_;
    std::array<std::wstring,6> monitor_{L"CPU Fan: --",L"GPU Fan: --",L"CPU Temp Limit: --",L"Battery: --",L"Fan Boost: --",L"Fan Curve: --"};HFONT font_=nullptr,bold_=nullptr,title_=nullptr;UINT dpi_=96,taskbarMessage_=0;
    std::vector<std::pair<RECT,const wchar_t*>> powerFrames_;std::vector<PowerText> powerText_;POINT liveOrigin_{0,-1};std::map<int,int> detectedMa_;
    std::wstring cpuNote_,gpuNote_,powerHint_,liveReason_,powerKey_,dlppcTip_;std::array<Page,7> pages_;std::map<int,HWND> controls_;std::set<int> edited_;
    std::array<HICON,5> slotIcons_{};std::vector<HICON> animation_;size_t animationFrame_=0;int trayState_=-3;bool closing_=false,trayAdded_=false,updating_=false,busy_=false,ready_=false,tripped_=false,coolerConnected_=false,coolerAvailable_=false,fanEditQueued_=false,powerRead_=false;
    int target_=0;std::uint64_t sequence_=0,busySequence_=0;ULONGLONG lastError_=0;
    Json state_=Json::object(),peripheralState_=Json::object(),identity_,gpuTelemetry_,boundHistory_,devices_=Json::array();std::map<int,std::pair<Json,ChartOptions>> boundCharts_;std::array<bool,7> boundPages_{};std::array<std::wstring,7> outcome_;
    std::string warning_,configurationWarning_;std::wstring gpuError_;std::optional<CpuSettings> pendingCpu_;std::optional<Sensors> lastSensors_;std::optional<ControlCommand> operation_;int operationPage_=0;
    Log log_;std::shared_ptr<Delivery> delivery_;WorkerOptions workerOptions_;std::unique_ptr<ControlWorker> worker_;
#ifdef MECHCONTROL_WITH_CLI
    std::unique_ptr<CliServer> cli_;
#endif
};
#ifdef MECHCONTROL_PAGE_CHECK
#include "../tests/ui_pages_checks.h"
#endif
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    try {
        int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);if(!argv) throw std::runtime_error("Cannot parse command line");
#ifdef MECHCONTROL_PAGE_CHECK
        if(argc!=3 || std::wstring_view(argv[1])!=L"--ui-check") {LocalFree(argv);return 2;}
        std::filesystem::path checks=argv[2];LocalFree(argv);auto fixture=std::make_shared<UiPageState>();
#else
        std::filesystem::path diagnostic;bool invalid=false;
        for(int i=1;i<argc;++i) {
            if(std::wstring_view(argv[i])==L"--diagnose") {if(argc==3 && i==1 && argv[2][0] && !std::wstring_view(argv[2]).starts_with(L"--")) diagnostic=argv[++i];else invalid=true;}
            else invalid=true;
        }
        LocalFree(argv);if(invalid) return 2; // Unknown/invalid diagnostics can never start controlling hardware.
        if(!diagnostic.empty()) {Log log(executable_directory()/L"mechcontrol.log");ControlWorker worker(std::make_shared<Delivery>(),log,{.once=true,.report=diagnostic});return worker.wait();}
        Handle singleton(CreateMutexW(nullptr,TRUE,L"Global\\MechControl"));if(!singleton) throw std::runtime_error("Cannot acquire Global\\MechControl");if(GetLastError()==ERROR_ALREADY_EXISTS) return 0;
#endif
        winrt::init_apartment(winrt::apartment_type::single_threaded);struct Apartment {~Apartment(){winrt::uninit_apartment();}} apartment;
        INITCOMMONCONTROLSEX cc{sizeof cc,ICC_TAB_CLASSES|ICC_BAR_CLASSES|ICC_PROGRESS_CLASS};if(!InitCommonControlsEx(&cc)) throw std::runtime_error("Cannot initialize native controls");registerFanChart(instance);
        WNDCLASSW type{};type.hInstance=instance;type.lpfnWndProc=Application::procedure;type.lpszClassName=mainClass;type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.hIcon=LoadIconW(nullptr,IDI_APPLICATION);type.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);if(!RegisterClassW(&type)) throw std::runtime_error("Cannot register main window");
#ifdef MECHCONTROL_PAGE_CHECK
        Application app(instance,ui_page_options(checks,fixture));
#else
        Application app(instance);
#endif
        auto window=app.create();if(!window) throw std::runtime_error("Cannot create main window");
#ifdef MECHCONTROL_PAGE_CHECK
        app.checkPages(checks,fixture);return 0;
#else
        MSG message{};int result;while((result=GetMessageW(&message,nullptr,0,0))>0) if(!IsDialogMessageW(window,&message)) {TranslateMessage(&message);DispatchMessageW(&message);}return result==-1?1:int(message.wParam);
#endif
    }catch(const winrt::hresult_error& e) {MessageBoxW(nullptr,e.message().c_str(),L"MechControl",MB_OK|MB_ICONERROR);return 1;}
    catch(const std::exception& e) {MessageBoxW(nullptr,w(e.what()).c_str(),L"MechControl",MB_OK|MB_ICONERROR);return 1;}
}
