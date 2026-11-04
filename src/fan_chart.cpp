#include "fan_chart.h"
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <stdexcept>
#include <string>

namespace mech {
namespace {
struct Chart {
    config::Curve curve; // Empty until the owner supplies real data. Fixture seeds live only in the fixture.
    ChartOptions options;
    int selected=0,hover=-1,temperature=-1;
    bool dragging=false;
    UINT dpi=96;
    std::wstring title,tip;
    HWND tooltip=nullptr;
    int scale(int n) const {return MulDiv(n,dpi,96);}
    int count() const {return int(curve.temp_up.size());}
    int duty_index(int i) const {return options.interpolated?i:std::min(i+1,count()-1);}
    int duty(int i) const {return curve.duty[i]/2;}
    RECT plot(HWND window) const {RECT r{};GetClientRect(window,&r);r.left+=scale(44);r.top+=scale(30);r.right-=scale(14);r.bottom-=scale(32);return r;}
    POINT pixel(RECT r,int temp,int percent) const {return {r.left+MulDiv(temp,r.right-r.left,105),r.bottom-MulDiv(percent,r.bottom-r.top,100)};}
    POINT handle(RECT r,int i) const {return pixel(r,curve.temp_up[i],duty(duty_index(i)));}
    int hit(RECT r,int x,int y) const {
        for(int i=count()-1;i>=0;--i) {auto p=handle(r,i);int dx=p.x-x,dy=p.y-y;if(dx*dx+dy*dy<=scale(10)*scale(10)) return i;}return -1;
    }
    void accessible(HWND window) const {
        auto name=title;
        if(count()) name+=L", point "+std::to_wstring(selected+1)+L" of "+std::to_wstring(count())+L", "+std::to_wstring(curve.temp_up[selected])+L" degrees C, "+std::to_wstring(duty(duty_index(selected)))+L" percent";
        else name+=options.read_only?L", no completed table write":L", unavailable";
        SetWindowTextW(window,name.c_str());NotifyWinEvent(EVENT_OBJECT_NAMECHANGE,window,OBJID_CLIENT,CHILDID_SELF);
    }
    void normalize() {curve.temp_down.clear();for(int i=0;i<count();++i) curve.temp_down.push_back(i?std::max(0,curve.temp_up[i-1]-4):0);}
    void move(HWND window,int temp,int percent) {
        if(options.read_only || !count()) return;
        int lo=selected?curve.temp_up[selected-1]+4:0,hi=selected<count()-1?curve.temp_up[selected+1]-4:105;
        if(lo<=hi) curve.temp_up[selected]=std::clamp(temp,lo,hi);
        int index=duty_index(selected);curve.duty[index]=index?std::clamp(percent,1,100)*2:0;
        for(int i=index+1;i<count() && curve.duty[i]<curve.duty[i-1];++i) curve.duty[i]=curve.duty[i-1];
        for(int i=index-1;i>=0 && curve.duty[i]>curve.duty[i+1];--i) curve.duty[i]=i?std::max(2,curve.duty[i+1]):0;
        normalize();accessible(window);InvalidateRect(window,nullptr,FALSE);
    }
    void changed(HWND window) {SendMessageW(GetParent(window),WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(window),EN_CHANGE),reinterpret_cast<LPARAM>(window));}
    void hover_tip(HWND window,int x,int y) {
        int index=hit(plot(window),x,y);if(index==hover) return;hover=index;tip.clear();
        if(index>=0) {int d=duty_index(index);wchar_t address[40]{};if(options.duty_base) swprintf_s(address,L" | Register 0x%03X",options.duty_base+d);
            tip=std::to_wstring(curve.temp_up[index])+L" °C → "+std::to_wstring(duty(d))+L"%"+address+L" (Raw "+std::to_wstring(curve.duty[d])+L")";}
        TOOLINFOW tool{sizeof tool};tool.hwnd=window;tool.uId=reinterpret_cast<UINT_PTR>(window);tool.lpszText=tip.data();SendMessageW(tooltip,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&tool));
        if(index<0) SendMessageW(tooltip,TTM_POP,0,0);
    }
    void paint(HWND window,HDC target) const {
        RECT client{};GetClientRect(window,&client);auto dc=CreateCompatibleDC(target);auto bitmap=CreateCompatibleBitmap(target,std::max(1L,client.right),std::max(1L,client.bottom));auto oldBitmap=SelectObject(dc,bitmap);
        FillRect(dc,&client,GetSysColorBrush(COLOR_WINDOW));SetBkMode(dc,TRANSPARENT);SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
        auto font=CreateFontW(-scale(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");auto oldFont=SelectObject(dc,font);
        RECT heading{scale(8),scale(3),client.right-scale(8),scale(27)};DrawTextW(dc,title.c_str(),-1,&heading,DT_SINGLELINE|DT_END_ELLIPSIS);
        auto r=plot(window);auto grid=CreatePen(PS_SOLID,1,RGB(215,215,215));auto oldPen=SelectObject(dc,grid);
        for(int n=0;n<=100;n+=20) {auto p=pixel(r,n,n);MoveToEx(dc,p.x,r.top,nullptr);LineTo(dc,p.x,r.bottom);MoveToEx(dc,r.left,p.y,nullptr);LineTo(dc,r.right,p.y);
            auto s=std::to_wstring(n);RECT x{p.x-scale(12),r.bottom+scale(2),p.x+scale(14),r.bottom+scale(19)};DrawTextW(dc,s.c_str(),-1,&x,DT_CENTER|DT_SINGLELINE);
            RECT y{0,p.y-scale(8),r.left-scale(4),p.y+scale(10)};DrawTextW(dc,(s+L"%").c_str(),-1,&y,DT_RIGHT|DT_SINGLELINE);}
        auto line=CreatePen(PS_SOLID,scale(2),RGB(0,100,180));auto hysteresis=CreatePen(PS_DASH,1,RGB(125,150,200));auto marker=CreatePen(PS_DOT,1,RGB(210,70,70));
        if(count()) {
            SelectObject(dc,line);auto p=pixel(r,curve.temp_up[0],duty(0));MoveToEx(dc,p.x,p.y,nullptr);
            for(int i=1;i<count();++i) {auto q=pixel(r,curve.temp_up[i],duty(i));if(!options.interpolated) LineTo(dc,p.x,q.y);LineTo(dc,q.x,q.y);p=q;}
            if(!options.interpolated) {SelectObject(dc,hysteresis);MoveToEx(dc,p.x,p.y,nullptr);for(int i=count()-1;i>=1;--i) {auto q=pixel(r,curve.temp_down[i],duty(i));LineTo(dc,q.x,q.y);q=pixel(r,curve.temp_down[i],duty(i-1));LineTo(dc,q.x,q.y);}p=pixel(r,curve.temp_up[0],0);LineTo(dc,p.x,p.y);}
        }
        if(temperature>0 && temperature<=105) {SelectObject(dc,marker);auto p=pixel(r,temperature,0);MoveToEx(dc,p.x,r.top,nullptr);LineTo(dc,p.x,r.bottom);}
        SelectObject(dc,line);auto oldBrush=SelectObject(dc,GetStockObject(WHITE_BRUSH));
        for(int i=0;i<count();++i) {auto p=handle(r,i);SelectObject(dc,GetStockObject(i==selected?BLACK_BRUSH:WHITE_BRUSH));int radius=scale(i==selected?5:4);Ellipse(dc,p.x-radius,p.y-radius,p.x+radius,p.y+radius);}
        SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(grid);DeleteObject(line);DeleteObject(hysteresis);DeleteObject(marker);
        auto help=count()?std::to_wstring(curve.temp_up[selected])+L" °C → "+std::to_wstring(duty(duty_index(selected)))+L"%   "+(options.read_only?L"Last complete write, not hardware readback":L"Drag corners; arrows edit; PgUp/PgDn select"):options.read_only?L"No completed table write":L"Fan curve unavailable";
        RECT footer{scale(8),client.bottom-scale(16),client.right-scale(8),client.bottom};DrawTextW(dc,help.c_str(),-1,&footer,DT_SINGLELINE|DT_END_ELLIPSIS);
        if(GetFocus()==window) DrawFocusRect(dc,&client);SelectObject(dc,oldFont);DeleteObject(font);BitBlt(target,0,0,client.right,client.bottom,dc,0,0,SRCCOPY);SelectObject(dc,oldBitmap);DeleteObject(bitmap);DeleteDC(dc);
    }
};
LRESULT CALLBACK chartProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto c=reinterpret_cast<Chart*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE) {c=new Chart;c->title=reinterpret_cast<CREATESTRUCTW*>(lp)->lpszName;c->dpi=GetDpiForWindow(window);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(c));}
    if(!c) return DefWindowProcW(window,message,wp,lp);
    switch(message) {
    case WM_CREATE: {
        c->tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,window,nullptr,GetModuleHandleW(nullptr),nullptr);
        TOOLINFOW tool{sizeof tool};tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tool.hwnd=window;tool.uId=reinterpret_cast<UINT_PTR>(window);tool.lpszText=c->tip.data();SendMessageW(c->tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));return 0;}
    case WM_GETDLGCODE:return DLGC_WANTARROWS;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{PAINTSTRUCT ps;auto dc=BeginPaint(window,&ps);c->paint(window,dc);EndPaint(window,&ps);return 0;}
    case WM_SETFOCUS:case WM_KILLFOCUS:case WM_SIZE:InvalidateRect(window,nullptr,FALSE);return 0;
    case chartDpiMessage:c->dpi=UINT(wp);InvalidateRect(window,nullptr,FALSE);return 0;
    case chartPointMessage:return c->count()?MAKELRESULT(c->curve.temp_up[c->selected],c->duty(c->duty_index(c->selected))):0;
    case chartOptionsMessage: {auto o=*reinterpret_cast<const ChartOptions*>(lp);if(o.read_only!=c->options.read_only || o.interpolated!=c->options.interpolated || o.duty_base!=c->options.duty_base) {c->options=o;c->hover=-1;c->accessible(window);InvalidateRect(window,nullptr,FALSE);}return 0;}
    case chartTemperatureMessage: {int temp=int(wp)-1;if(c->temperature!=temp) {c->temperature=temp;if(IsWindowVisible(window)) InvalidateRect(window,nullptr,FALSE);}return 0;}
    case chartSetMessage: {config::Curve value;if(lp) value=*reinterpret_cast<const config::Curve*>(lp);if(value.temp_up==c->curve.temp_up && value.duty==c->curve.duty) return 0;c->curve=std::move(value);c->selected=0;c->hover=-1;c->normalize();c->accessible(window);InvalidateRect(window,nullptr,FALSE);return 0;}
    case chartGetMessage:*reinterpret_cast<config::Curve*>(lp)=c->curve;return 0;
    case WM_LBUTTONDOWN: {SetFocus(window);int hit=c->hit(c->plot(window),GET_X_LPARAM(lp),GET_Y_LPARAM(lp));if(hit>=0) {c->selected=hit;c->dragging=!c->options.read_only;if(c->dragging) SetCapture(window);}c->accessible(window);InvalidateRect(window,nullptr,FALSE);return 0;}
    case WM_MOUSEMOVE: {
        if(c->dragging) {auto r=c->plot(window);if(r.right>r.left && r.bottom>r.top) c->move(window,MulDiv(GET_X_LPARAM(lp)-r.left,105,r.right-r.left),MulDiv(r.bottom-GET_Y_LPARAM(lp),100,r.bottom-r.top));}
        else c->hover_tip(window,GET_X_LPARAM(lp),GET_Y_LPARAM(lp));return 0;}
    case WM_LBUTTONUP:if(c->dragging) {c->dragging=false;ReleaseCapture();c->changed(window);}return 0;
    case WM_CAPTURECHANGED:c->dragging=false;return 0;
    case WM_KEYDOWN: {
        if(!c->count()) return 0;int temp=c->curve.temp_up[c->selected],percent=c->duty(c->duty_index(c->selected));bool edit=true;
        switch(wp) {case VK_LEFT:--temp;break;case VK_RIGHT:++temp;break;case VK_UP:++percent;break;case VK_DOWN:--percent;break;
        case VK_HOME:c->selected=0;edit=false;break;case VK_END:c->selected=c->count()-1;edit=false;break;case VK_PRIOR:c->selected=std::max(0,c->selected-1);edit=false;break;case VK_NEXT:c->selected=std::min(c->count()-1,c->selected+1);edit=false;break;default:return DefWindowProcW(window,message,wp,lp);}
        if(edit && !c->options.read_only) {c->move(window,temp,percent);c->changed(window);}else {c->accessible(window);InvalidateRect(window,nullptr,FALSE);}return 0;}
    case WM_NCDESTROY:if(c->tooltip) DestroyWindow(c->tooltip);delete c;SetWindowLongPtrW(window,GWLP_USERDATA,0);break;
    }
    return DefWindowProcW(window,message,wp,lp);
}
}
void registerFanChart(HINSTANCE instance) {WNDCLASSW type{};type.hInstance=instance;type.lpfnWndProc=chartProc;type.lpszClassName=chartClass;type.hCursor=LoadCursorW(nullptr,IDC_ARROW);if(!RegisterClassW(&type)) throw std::runtime_error("Cannot register fan chart");}
}
