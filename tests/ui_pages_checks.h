void Application::checkPages(const std::filesystem::path& directory,const std::shared_ptr<UiPageState>& fixture) {
    std::ofstream report(directory/L"pages-check.txt");
    auto check=[&](bool ok,const char* message) {report<<(ok?"PASS ":"FAIL ")<<message<<std::endl;if(!ok) throw std::runtime_error(message);};
    auto pump=[&] {MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {if(msg.message==WM_QUIT) throw std::runtime_error("Fixture UI exited unexpectedly");if(!IsDialogMessageW(window_,&msg)) {TranslateMessage(&msg);DispatchMessageW(&msg);}}};
    auto wait=[&](const auto& condition) {auto deadline=GetTickCount64()+6000;while(!condition() && GetTickCount64()<deadline) {pump();Sleep(10);}pump();check(condition(),"production page delivery settled");};
    auto page=[&](int p) {TabCtrl_SetCurSel(tabs_,p);selectPage();pump();};
    auto capture=[&](const std::wstring& name) {
        auto foregroundThread=GetWindowThreadProcessId(GetForegroundWindow(),nullptr),thread=GetCurrentThreadId();bool attached=foregroundThread!=thread && AttachThreadInput(thread,foregroundThread,TRUE);
        SetWindowPos(window_,HWND_TOPMOST,80,40,scale(940),scale(760),SWP_SHOWWINDOW);SetForegroundWindow(window_);BringWindowToTop(window_);if(attached) AttachThreadInput(thread,foregroundThread,FALSE);pump();RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_UPDATENOW|RDW_ALLCHILDREN);pump();
        // BitBlt reads the desktop, not our pending GDI/DWM frame.
        GdiFlush();check(SUCCEEDED(DwmFlush()),"desktop composition completed before capture");
        RECT r{};GetWindowRect(window_,&r);int width=r.right-r.left,height=r.bottom-r.top;auto screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),width,height,1,32,BI_RGB};void* pixels=nullptr;auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);auto old=SelectObject(dc,bitmap);BitBlt(dc,0,0,width,height,screen,r.left,r.top,SRCCOPY);
        BITMAPFILEHEADER header{0x4d42,DWORD(sizeof header+sizeof(BITMAPINFOHEADER)+width*height*4),0,0,DWORD(sizeof header+sizeof(BITMAPINFOHEADER))};std::ofstream file(directory/(name+L".bmp"),std::ios::binary);file.write(reinterpret_cast<const char*>(&header),sizeof header);file.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(BITMAPINFOHEADER));file.write(static_cast<const char*>(pixels),width*height*4);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(nullptr,screen);
    };
    wait([&]{return ready_;});show();wait([&]{return state_.contains("cpu");});
    CHANGEFILTERSTRUCT filter{sizeof filter};
    check(ChangeWindowMessageFilterEx(window_,taskbarMessage_,MSGFLT_ALLOW,&filter) && filter.ExtStatus==MSGFLTINFO_ALREADYALLOWED_FORWND,"Explorer restart message is allowed through elevated-window UIPI");
    NOTIFYICONDATAW removed{sizeof removed};removed.hWnd=window_;removed.uID=1;
    check(Shell_NotifyIconW(NIM_DELETE,&removed)!=FALSE,"remove only the fixture tray icon to reproduce lost shell registration");
    tray();check(!trayAdded_,"missing tray icon waits for the shell notification");
    SendMessageW(window_,taskbarMessage_,0,0);check(trayAdded_,"Explorer notification restores missing tray registration without a timer");
    report<<"Initial CPU editor: "<<utf8(text(get(pl1Id+1000)))<<"; owned value: "<<state_["cpu"]["pl1"]["value"]<<"; edited controls:";for(int id:edited_) report<<' '<<id;report<<std::endl;
    check(number(pl1Id)==state_["cpu"]["pl1"]["value"] && number(nvId)>0,"first Show binds real CPU/current values without requiring Read");
    auto empty=control(pages_[1].window,chartClass,L"CPU last write — empty fixture probe",0,999);
    ChartOptions ro{true,true};SendMessageW(empty,chartOptionsMessage,0,reinterpret_cast<LPARAM>(&ro));check(SendMessageW(empty,chartPointMessage,0,0)==0 && text(empty).find(L"no completed")!=std::wstring::npos,"unseeded production/last-write chart contains no demonstration points");DestroyWindow(empty);controls_.erase(999);
    wait([&]{return liveReason_.empty() && text(get(inputId+1000))==L"11.053 A";});check(milliamps(inputId,number(inputId))==11053 && text(get(nvId+1000))==L"120 A","current sliders show amps and keep the exact detected limit");
    auto bottom=[&](bool on){SendMessageW(pages_[0].window,WM_VSCROLL,on?SB_BOTTOM:SB_TOP,0);pump();};
    fixture->fail_gpu=true;wait([&]{return liveReason_.find(L"Live read failed")!=std::wstring::npos && !IsWindowEnabled(get(nvId));});bottom(true);capture(L"live-failed");
    fixture->fail_gpu=false;wait([&]{return IsWindowEnabled(get(nvId))!=FALSE;});fixture->asleep=true;wait([&]{return !IsWindowEnabled(get(nvId));});check(liveReason_.find(L"asleep")!=std::wstring::npos && text(get(nvId+1000))==L"--","sleeping GPU has unavailable live currents, not fresh cached limits");fixture->asleep=false;wait([&]{return IsWindowEnabled(get(nvId))!=FALSE && liveReason_.empty();});
    capture(L"power-live-bottom");bottom(false);capture(L"power-live");
    page(1);wait([&]{return state_.contains("switchMs");});
    auto before=fanIntent();auto point=SendMessageW(get(cpuChartId),chartPointMessage,0,0);check(HIWORD(point)==before.cpu_curve->duty[1]/2,"step handle zero edits the NEXT duty, not the zero-duty sentinel");
    SendMessageW(get(cpuChartId),WM_KEYDOWN,VK_UP,0);auto after=fanIntent();check(after.cpu_curve->duty[0]==0 && after.cpu_curve->duty[1]==before.cpu_curve->duty[1]+2 && text(get(fanTextId)).find(L"Changes not applied")!=std::wstring::npos,"chart keyboard edit preserves sentinel and marks pending state");
    auto chartWindow=get(cpuChartId);RECT plot{};GetClientRect(chartWindow,&plot);plot.left+=scale(44);plot.right-=scale(14);plot.top+=scale(30);plot.bottom-=scale(32);auto selected=SendMessageW(chartWindow,chartPointMessage,0,0);int x=plot.left+MulDiv(LOWORD(selected),plot.right-plot.left,105),y=plot.bottom-MulDiv(HIWORD(selected),plot.bottom-plot.top,100);
    SendMessageW(chartWindow,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));check(GetCapture()==chartWindow,"corner hit testing captures actual shifted handle");SendMessageW(chartWindow,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x+scale(6),y-scale(4)));SendMessageW(chartWindow,WM_LBUTTONUP,0,MAKELPARAM(x+scale(6),y-scale(4)));check(SendMessageW(chartWindow,chartPointMessage,0,0)!=selected,"drag changes shifted corner and hysteresis thresholds");capture(L"fan-pending");
    read();wait([&]{return !busy_;});check(!IsWindowVisible(get(debugCpuId)),"last-write charts are hidden outside linked mode");
    page(2);wait([&]{return TabCtrl_GetCurSel(tabs_)==2;});SetWindowTextW(get(thresholdId+1000),L"65");command(protectionApplyId,BN_CLICKED);wait([&]{return !busy_ && state_["protection"]["thresholdC"]==65;});SetWindowTextW(get(thresholdId+1000),L"70");target_=5;command(saveId,BN_CLICKED);wait([&]{return !busy_ && state_.contains("saved") && !state_["saved"].is_null();});
    auto saved=read_json(directory/L"userprofile_5.json");check(saved["gpuFailsafe"]["thresholdC"]==65 && text(get(thresholdId+1000))==L"70" && text(get(thermalHintId)).find(L"Changes not applied")!=std::wstring::npos,"original UI Save persists applied 65 C while unapplied 70 C stays pending");
    command(detailsId,BN_CLICKED);capture(L"thermal-pending");reveal(get(detailsTextId));capture(L"thermal-details");request(SlotRename{5,"UI fixture"});wait([&]{return !busy_;});check(text(get(thermalHintId)).find(L"Changes not applied")!=std::wstring::npos,"Rename cannot certify pending thermal edits as applied/saved");
    request(ThermalTest{state_["protection"].get<ProtectionSettings>()});wait([&]{return !busy_ && alert_;});capture(L"thermal-held");request(ThermalReset{});wait([&]{return !busy_ && !tripped_ && !alert_;});check(!alert_,"cleared trip closes the independent production alert");
    for(UINT dpi:{96u,144u,192u}) {RECT r{80,40,80+MulDiv(940,dpi,96),40+MulDiv(760,dpi,96)};SendMessageW(window_,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&r));for(int p=0;p<7;++p) {page(p);wait([&]{return !busy_;});capture(L"page-"+std::to_wstring(p)+L"-"+std::to_wstring(dpi));}}
    report<<"Injected DPI/failures and synthetic hardware, not physical qualification. Production widgets/controllers were used.\n";DestroyWindow(window_);
}
