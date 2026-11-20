void Application::checkPages(const std::filesystem::path& directory, const std::shared_ptr<UiPageState>& fixture) {
    std::ofstream report(directory / L"pages-check.txt");
    auto check = [&](bool ok, const char* message) {
        report << (ok ? "PASS " : "FAIL ") << message << std::endl;
        if (!ok) throw std::runtime_error(message);
    };
    auto pump = [&] {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) throw std::runtime_error("Fixture UI exited unexpectedly");
            if (!IsDialogMessageW(window_, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    };
    auto wait = [&](const auto& condition, ULONGLONG ms = 6000) {
        auto deadline = GetTickCount64() + ms;
        while (!condition() && GetTickCount64() < deadline) {
            pump();
            Sleep(10);
        }
        pump();
        check(condition(), "production page delivery settled");
    };
    auto page = [&](int p) {
        TabCtrl_SetCurSel(tabs_, p);
        selectPage();
        pump();
    };
    auto capture = [&](const std::wstring& name, int cx = 940, int cy = 760) {
        auto foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr), thread = GetCurrentThreadId();
        bool attached = foregroundThread != thread && AttachThreadInput(thread, foregroundThread, TRUE);
        SetWindowPos(window_, HWND_TOPMOST, 80, 40, scale(cx), scale(cy), SWP_SHOWWINDOW);
        SetForegroundWindow(window_);
        BringWindowToTop(window_);
        if (attached) AttachThreadInput(thread, foregroundThread, FALSE);
        pump();
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        pump();
        // BitBlt reads the desktop, not our pending GDI/DWM frame.
        GdiFlush();
        check(SUCCEEDED(DwmFlush()), "desktop composition completed before capture");
        RECT r{};
        GetWindowRect(window_, &r);
        int width = r.right - r.left, height = r.bottom - r.top;
        auto screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, height, 1, 32, BI_RGB};
        void* pixels = nullptr;
        auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        auto old = SelectObject(dc, bitmap);
        BitBlt(dc, 0, 0, width, height, screen, r.left, r.top, SRCCOPY);
        BITMAPFILEHEADER header{0x4d42, DWORD(sizeof header + sizeof(BITMAPINFOHEADER) + width * height * 4), 0, 0,
                                DWORD(sizeof header + sizeof(BITMAPINFOHEADER))};
        std::ofstream file(directory / (name + L".bmp"), std::ios::binary);
        file.write(reinterpret_cast<const char*>(&header), sizeof header);
        file.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
        file.write(static_cast<const char*>(pixels), width * height * 4);
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
    };
    wait([&] { return ready_; });
    applyTheme(false); // Whatever Windows' app mode; the dark pages are captured at the end.
    show();
    wait([&] { return state_.contains("cpu"); });
    CHANGEFILTERSTRUCT filter{sizeof filter};
    check(ChangeWindowMessageFilterEx(window_, taskbarMessage_, MSGFLT_ALLOW, &filter) &&
              filter.ExtStatus == MSGFLTINFO_ALREADYALLOWED_FORWND,
          "Explorer restart message is allowed through elevated-window UIPI");
    NOTIFYICONDATAW removed{sizeof removed};
    removed.hWnd = window_;
    removed.uID = 1;
    check(Shell_NotifyIconW(NIM_DELETE, &removed) != FALSE,
          "remove only the fixture tray icon to reproduce lost shell registration");
    tray();
    check(!trayAdded_, "missing tray icon waits for the shell notification");
    SendMessageW(window_, taskbarMessage_, 0, 0);
    check(trayAdded_, "Explorer notification restores missing tray registration without a timer");
    report << "Initial CPU editor: " << utf8(text(get(pl1Id + 1000)))
           << "; owned value: " << state_["cpu"]["pl1"]["value"] << "; edited controls:";
    for (int id : edited_) report << ' ' << id;
    report << std::endl;
    check(number(pl1Id) == state_["cpu"]["pl1"]["value"] && number(nvId) > 0,
          "first Show binds real CPU/current values without requiring Read");
    auto empty = control(pages_[1].window, chartClass, L"CPU last write — empty fixture probe", 0, 999);
    ChartOptions ro{true, true};
    SendMessageW(empty, chartOptionsMessage, 0, reinterpret_cast<LPARAM>(&ro));
    check(SendMessageW(empty, chartPointMessage, 0, 0) == 0,
          "unseeded production/last-write chart contains no demonstration points");
    DestroyWindow(empty);
    controls_.erase(999);
    wait([&] { return liveReason_.empty() && text(get(inputId + 1000)) == L"11.053 A"; });
    check(milliamps(inputId, number(inputId)) == 11053 && text(get(nvId + 1000)) == L"120 A",
          "current sliders show amps and keep the exact detected limit");
    auto bottom = [&](bool on) {
        SendMessageW(pages_[0].window, WM_VSCROLL, on ? SB_BOTTOM : SB_TOP, 0);
        pump();
    };
    // The live refresh reads only the driver, so EC failures and sleep reach the editor through Read.
    fixture->fail_gpu = true;
    read();
    wait([&] {
        return !busy_ && gpuError_.find(L"GPU read failed") != std::wstring::npos && !IsWindowEnabled(get(nvId));
    });
    bottom(true);
    capture(L"live-failed");
    fixture->fail_gpu = false;
    read();
    wait([&] { return !busy_ && IsWindowEnabled(get(nvId)) != FALSE; });
    fixture->asleep = true;
    read();
    wait([&] { return !busy_ && !IsWindowEnabled(get(nvId)); });
    check(liveReason_.find(L"asleep") != std::wstring::npos && text(get(nvId + 1000)) == L"--",
          "sleeping GPU has unavailable live currents, not fresh cached limits");
    fixture->asleep = false;
    read();
    wait([&] { return !busy_ && IsWindowEnabled(get(nvId)) != FALSE && liveReason_.empty(); });
    capture(L"power-live-bottom");
    bottom(false);
    capture(L"power-live");
    check(IsWindowVisible(get(gpuTargetId)) && IsWindowEnabled(get(gpuTargetId)) &&
              text(get(gpuTargetId + 1000)) == L"83 °C",
          "the GPU temperature target shows the driver's value where the driver offers a range");
    // The advanced controls are collapsed until the link is clicked.
    check(std::ranges::none_of(std::array{unlockId, dlppcId, nvId, msId, inputId, bypassId, coolerFlagId},
                               [&](int id) { return IsWindowVisible(get(id)) != FALSE; }),
          "advanced controls are hidden until expanded");
    NMHDR expand{get(advancedId), UINT_PTR(advancedId), NM_CLICK};
    SendMessageW(window_, WM_NOTIFY, advancedId, reinterpret_cast<LPARAM>(&expand));
    pump();
    check(std::ranges::all_of(std::array{unlockId, dlppcId, nvId, msId, inputId, bypassId, coolerFlagId},
                              [&](int id) { return IsWindowVisible(get(id)) != FALSE; }) &&
              IsWindowEnabled(get(bypassId)) && IsWindowEnabled(get(coolerFlagId)),
          "the advanced link reveals the GPU override, current limits, bypass and cooler-flag controls");
    bottom(true);
    capture(L"power-advanced");
    check(!IsWindowVisible(get(voltageOffsetId)) && !IsWindowVisible(get(voltageApplyId)),
          "the voltage offset is hidden where the BIOS does not offer it");
    // As the controller presents a BIOS that offers it; the fixture reads no firmware variables. The worker's next
    // presentation takes it away again, so the checks do not wait and the capture may rarely miss it.
    auto offered = state_.at("advanced");
    offered["voltageOffset"] = {{"available", true}, {"millivolts", 48}, {"max", 100}, {"note", ""}};
    presentation(Json{{"advanced", offered}}, false);
    check(IsWindowVisible(get(voltageOffsetId)) && IsWindowVisible(get(voltageApplyId)) &&
              number(voltageOffsetId) == 48 && SendMessageW(get(voltageOffsetId), TBM_GETRANGEMAX, 0, 0) == 100 &&
              text(get(voltageOffsetId + 1000)) == L"-48 mV",
          "an offered voltage offset shows the BIOS's request and range");
    capture(L"power-voltage");
    bottom(false);
    SendMessageW(get(coolerFlagId), BM_SETCHECK, BST_CHECKED, 0);
    command(coolerFlagId, BN_CLICKED);
    wait([&] {
        return !busy_ &&
               state_.value("advanced", Json::object()).value("coolerFlag", Json::object()).value("connected", false);
    });
    check(checked(coolerFlagId), "the liquid cooler flag applies and binds its live state");
    page(1);
    wait([&] { return state_.contains("switchMs"); });
    auto chartWindow = get(cpuChartId);
    auto before = fanIntent();
    check(HIWORD(SendMessageW(chartWindow, chartPointMessage, 0, 0)) == before.cpu_curve->duty[1] / 2,
          "step handle zero edits the NEXT duty, not the zero-duty sentinel");
    auto at = SendMessageW(chartWindow, chartPointMessage, 0, 1);
    int x = LOWORD(at), y = HIWORD(at);
    SendMessageW(chartWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    check(GetCapture() == chartWindow, "corner hit testing captures actual shifted handle");
    SendMessageW(chartWindow, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + scale(6), y - scale(6)));
    SendMessageW(chartWindow, WM_LBUTTONUP, 0, MAKELPARAM(x + scale(6), y - scale(6)));
    auto after = fanIntent();
    check(after.cpu_curve->duty[0] == 0 && after.cpu_curve->duty[1] > before.cpu_curve->duty[1] &&
              after.cpu_curve->temp_up[0] > before.cpu_curve->temp_up[0] && edited_.contains(cpuChartId),
          "drag moves the shifted corner, keeps the zero-duty sentinel and marks the editor pending");
    capture(L"fan-pending");
    read();
    wait([&] { return !busy_; });
    check(!IsWindowVisible(get(debugCpuId)), "last-write charts are hidden outside linked mode");
    SendMessageW(get(linkId), BM_SETCHECK, BST_CHECKED, 0);
    command(linkId, BN_CLICKED);
    wait([&] {
        return !busy_ && state_.value("linked", false) && state_.contains("linkedStatus") &&
               state_.contains("writtenCpu");
    });
    pump();
    check(IsWindowVisible(get(debugCpuId)) && linkedBar_.right > linkedBar_.left && !IsWindowEnabled(get(fanApplyId)),
          "Link Fans shows its status and written curves and locks Apply");
    capture(L"fan-linked");
    SendMessageW(pages_[1].window, WM_VSCROLL, SB_BOTTOM, 0);
    pump();
    capture(L"fan-linked-bottom");
    SendMessageW(pages_[1].window, WM_VSCROLL, SB_TOP, 0);
    pump();
    SendMessageW(get(linkId), BM_SETCHECK, BST_UNCHECKED, 0);
    command(linkId, BN_CLICKED);
    wait([&] { return !busy_ && !state_.value("linked", true); });
    auto trigger = [&](int c) {
        SendMessageW(get(thresholdId), TBM_SETPOS, TRUE, c);
        SendMessageW(window_, WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM>(get(thresholdId)));
    };
    page(2);
    wait([&] { return TabCtrl_GetCurSel(tabs_) == 2; });
    trigger(65);
    command(protectionApplyId, BN_CLICKED);
    wait([&] { return !busy_ && state_["protection"]["thresholdC"] == 65; });
    trigger(70);
    target_ = 5;
    command(saveId, BN_CLICKED);
    wait([&] { return !busy_ && state_.contains("saved") && !state_["saved"].is_null(); });
    auto saved = read_json(directory / L"userprofile_5.json");
    check(saved["gpuFailsafe"]["thresholdC"] == 65 && text(get(thresholdId + 1000)) == L"70 °C" &&
              thermalText_[1] == L"Changes not applied",
          "original UI Save persists applied 65 C while unapplied 70 C stays pending");
    NMHDR link{get(detailsId), detailsId, NM_CLICK};
    SendMessageW(window_, WM_NOTIFY, detailsId, reinterpret_cast<LPARAM>(&link));
    capture(L"thermal-pending");
    SendMessageW(pages_[2].window, WM_VSCROLL, SB_BOTTOM, 0);
    pump();
    capture(L"thermal-details");
    SendMessageW(pages_[2].window, WM_VSCROLL, SB_TOP, 0);
    pump();
    request(SlotRename{5, "UI fixture"});
    wait([&] { return !busy_; });
    check(thermalText_[1] == L"Changes not applied", "Rename cannot certify pending thermal edits as applied/saved");
    request(ThermalTest{state_["protection"].get<ProtectionSettings>()});
    wait([&] { return !busy_ && alert_; });
    capture(L"thermal-held");
    check(thermalText_[2].starts_with(L"TRIPPED") &&
              thermalText_[3].find(L"Manual failsafe test") != std::wstring::npos && IsWindowEnabled(get(resetId)),
          "a trip shows its held state, the trip record and Reset");
    request(ThermalReset{});
    wait([&] { return !busy_ && !tripped_ && !alert_; });
    check(!alert_, "cleared trip closes the independent production alert");
    int keyboard = zoneBaseId;
    page(4);
    wait([&] { return peripheralState_.contains("zones") && IsWindowEnabled(get(keyboard + 5)); });
    SendMessageW(get(keyboard + 2), UDM_SETPOS32, 0, 200);
    SendMessageW(get(keyboard + 4), TBM_SETPOS, TRUE, 40);
    SendMessageW(window_, WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM>(get(keyboard + 4)));
    SendMessageW(get(keyboard), BM_SETCHECK, BST_CHECKED, 0);
    command(keyboard + 5, BN_CLICKED);
    wait([&] {
        auto& k = peripheralState_.at("lightingIntent").at("keyboard");
        return !busy_ && k.at("g") == 200 && k.at("brightness") == 40 && k.at("enabled") == true;
    });
    check(text(get(keyboard + 1004)) == L"40 %" && edited_.empty(),
          "keyboard color box and brightness slider apply to the zone");
    capture(L"lighting");
    // The fixture model has no cooler; a connected cooler's snapshot is delivered as the BLE owner would.
    page(5);
    wait([&] { return state_.value("readPage", -3) == 5; });
    check(!IsWindowEnabled(get(scanId)) && !IsWindowEnabled(get(coolerFanApplyId)) &&
              coolerStatus_.starts_with(L"This model"),
          "a model without cooler support disables the Liquid Cooler tab");
    CoolerSettings intent;
    intent.fan_on = intent.pump_on = intent.rgb_on = true;
    intent.voltage = PumpVoltage::V12;
    intent.g = 128;
    peripherals({{"coolerAvailable", true},
                 {"cooler",
                  {{"connected", true},
                   {"status", "Connected to cooler"},
                   {"firmware", "1.2"},
                   {"devices", Json::array({{{"address", 0xC0FFEE123456ull}, {"name", "LCT22002"}}})},
                   {"state", {{"autoConnect", true}, {"savedAddress", 0xC0FFEE123456ull}}},
                   {"intent", intent}}}});
    check(IsWindowEnabled(get(coolerFanApplyId)) && text(get(coolerPumpId + 1000)) == L"60 %" &&
              SendMessageW(get(voltageId), CB_GETCURSEL, 0, 0) == 3 && color(coolerRgbOnId) == RGB(0, 128, 0) &&
              checked(autoConnectId),
          "a connected cooler enables its settings and shows the requested ones");
    capture(L"liquid-cooler");
    fixture->asleep = true;
    int budgets = fixture->budgets;
    page(6);
    wait([&] { return infoText_[0] == L"40°C" && infoText_[7] == L"-- (GPU asleep, not read)"; });
    check(fixture->budgets == budgets,
          "System Info reads the EC sensors but not the NVIDIA budget while the GPU sleeps");
    fixture->asleep = false;
    read();
    wait([&] { return !busy_ && infoText_[7] == L"250 W" && infoText_[8] == L"350 W"; });
    capture(L"system-info");
    // The V/F Curve tab on the fake driver: made when first shown and opened on the worker; then a slider and a point
    // offset, Apply with the readback once the driver settles, every number, and Restore.
    check(vfOffered_ && TabCtrl_GetItemCount(tabs_) == vfPage + 1 && !vf_,
          "an NVIDIA GPU offers the V/F Curve tab, which is made only when shown");
    page(vfPage);
    check(vf_ != nullptr, "showing the V/F Curve tab makes it");
    auto vfText = [&] { return vf_->text(); };
    // Waits until the tab's status and readback start with `start`; a timeout reports what the tab shows.
    auto vfWait = [&](const std::wstring& start) {
        auto done = [&] { return !busy_ && vfText().starts_with(start); };
        for (auto deadline = GetTickCount64() + 20000; !done() && GetTickCount64() < deadline; Sleep(10)) pump();
        pump();
        if (!done()) report << "V/F Curve tab shows: " << utf8(vfText()) << std::endl;
        check(done(), "the V/F Curve tab reaches the expected state");
    };
    vfWait(L"Unapplied (currently stock)\nGPU now: stock equations.\nReadback matches this tab");
    auto vfRoot = vf_->window(), vfPanel = FindWindowExW(vfRoot, nullptr, L"MechControl.Native.VfPanel", nullptr),
         vfChart = FindWindowExW(vfRoot, nullptr, L"MechControl.Native.VfChart", nullptr);
    auto chartShape = [&] {
        RECT r{};
        GetClientRect(vfChart, &r);
        return r.bottom > 0 && std::abs(r.right - 2.5 * r.bottom) <= 3;
    };
    check(vfPanel && vfChart && chartShape(), "the V/F chart is 2.5 times as wide as high");
    capture(L"vf-curve");
    check(SendMessageW(FindWindowExW(vfPanel, nullptr, WC_COMBOBOXW, nullptr), CB_GETCOUNT, 0, 0) == 2,
          "the difference from a reference curve is not offered before one is loaded");
    // Answers the tab's confirmation with Yes from inside its message box.
    auto yes = [] {
        SetTimer(nullptr, 0, 100, [](HWND, UINT, UINT_PTR id, DWORD) {
            if (auto box = FindWindowW(L"#32770", L"MechControl — V/F Curve")) {
                KillTimer(nullptr, id);
                PostMessageW(box, WM_COMMAND, IDYES, 0);
            }
        });
    };
    auto vfButton = [&](const wchar_t* title) { return FindWindowExW(vfRoot, nullptr, L"Button", title); };
    auto track = FindWindowExW(vfPanel, nullptr, TRACKBAR_CLASSW, L"Native VF Curve");
    SendMessageW(track, TBM_SETPOS, TRUE, 12);
    SendMessageW(vfPanel, WM_HSCROLL, MAKEWPARAM(TB_THUMBPOSITION, 12), reinterpret_cast<LPARAM>(track));
    RECT plot{};
    GetClientRect(vfChart, &plot);
    auto middle = MAKELPARAM(plot.right / 2, plot.bottom / 2);
    SendMessageW(vfChart, WM_LBUTTONDOWN, MK_LBUTTON, middle);
    SendMessageW(vfChart, WM_LBUTTONUP, 0, middle);
    SendMessageW(vfChart, WM_KEYDOWN, VK_UP, 0);
    SendMessageW(vfChart, WM_MOUSEMOVE, 0, MAKELPARAM(plot.right / 2 + scale(30), plot.bottom / 2));
    check(vfText().starts_with(L"Unapplied (currently stock)"), "a moved slider and point offset are not applied yet");
    capture(L"vf-curve-edited");
    POINT center{plot.right / 2, plot.bottom / 2};
    ClientToScreen(vfChart, &center);
    SendMessageW(vfChart, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(center.x, center.y));
    capture(L"vf-curve-zoomed");
    SendMessageW(vfChart, WM_LBUTTONDBLCLK, MK_LBUTTON, middle);
    yes();
    SendMessageW(vfRoot, WM_COMMAND, MAKEWPARAM(0, BN_CLICKED), reinterpret_cast<LPARAM>(vfButton(L"Apply to GPU")));
    vfWait(L"Applied to the GPU\nApplied (");
    check(vfText().find(L"1 Afterburner Style OC offset).\nReadback matches this tab") != std::wstring::npos,
          "after Apply and the driver's settling, the readback matches the calculation and the offset");
    target_ = 4;
    command(saveId, BN_CLICKED);
    wait([&] { return !busy_ && std::filesystem::exists(directory / L"userprofile_4.json"); });
    auto vfSaved = read_json(directory / L"userprofile_4.json")["gpu"]["vfCurve"];
    check(vfSaved["tune"]["global"] == 30 && vfSaved["tune"]["pp"].size() == 1 && !vfSaved["tune"].contains("showNew") &&
              !vfSaved["curve"]["records"].empty(),
          "the profile bar's Save stores the applied V/F curve with the tune, without what the chart shows");
    // Both tables: every number by line, then the structure. The links follow the issue link.
    auto numbersLink = FindWindowExW(vfPanel, FindWindowExW(vfPanel, nullptr, WC_LINK, nullptr), WC_LINK, nullptr);
    auto structureLink = FindWindowExW(vfPanel, numbersLink, WC_LINK, nullptr);
    auto numbers = FindWindowExW(vfPanel, nullptr, L"MechControl.Native.VfNumbers", nullptr);
    auto structure = FindWindowExW(vfPanel, numbers, L"MechControl.Native.VfNumbers", nullptr);
    for (auto [show, table, name] : {std::tuple(numbersLink, numbers, L"vf-curve-numbers"),
                                     {structureLink, structure, L"vf-curve-structure"}}) {
        NMHDR click{show, 0, UINT(NM_CLICK)};
        SendMessageW(vfPanel, WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&click));
        auto top = [&] {
            RECT r{};
            GetWindowRect(table, &r);
            MapWindowPoints(nullptr, vfPanel, reinterpret_cast<POINT*>(&r), 2);
            return r.top;
        };
        for (int i = 0; i < 400 && top() > scale(60); ++i) SendMessageW(vfPanel, WM_VSCROLL, SB_LINEDOWN, 0);
        RECT panel{};
        GetClientRect(vfPanel, &panel);
        check(structureLink && IsWindowVisible(table) && top() < panel.bottom - scale(60),
              "both equation tables are shown on request");
        capture(name);
    }
    SendMessageW(vfPanel, WM_VSCROLL, SB_TOP, 0);
    for (auto [name, width, height] :
         {std::tuple(L"vf-curve-small", 760, 480), {L"vf-curve-wide", 1600, 720}, {L"vf-curve-large", 1400, 1050}}) {
        capture(name, width, height);
        check(chartShape(), "the V/F chart keeps its shape at every window size");
    }
    yes();
    SendMessageW(vfRoot, WM_COMMAND, MAKEWPARAM(0, BN_CLICKED), reinterpret_cast<LPARAM>(vfButton(L"Restore stock")));
    vfWait(L"Unapplied (currently stock)\nStock restored (");
    check(vfText().find(L"Readback matches this tab") != std::wstring::npos, "Restore puts the stock curve back");
    // Loading the profile writes its curve and shows its settings in place of the unapplied ones.
    SendMessageW(track, TBM_SETPOS, TRUE, 8);
    SendMessageW(vfPanel, WM_HSCROLL, MAKEWPARAM(TB_THUMBPOSITION, 8), reinterpret_cast<LPARAM>(track));
    request(SlotLoad{4});
    vfWait(L"Applied to the GPU\nGPU now: ");
    check(SendMessageW(track, TBM_GETPOS, 0, 0) == 12 && vfText().find(L"Readback matches this tab") != std::wstring::npos,
          "loading a profile writes its V/F curve and the tab shows the profile's settings and readback");
    // Closed to the tray just after a change, while the slider bars are still being worked out: once the bars' timer
    // has seen the hidden window, nothing of the tab runs.
    SendMessageW(track, TBM_SETPOS, TRUE, 4);
    SendMessageW(vfPanel, WM_HSCROLL, MAKEWPARAM(TB_THUMBPOSITION, 4), reinterpret_cast<LPARAM>(track));
    SendMessageW(window_, WM_CLOSE, 0, 0);
    auto tabTimers = [&](ULONGLONG ms) {
        int n = 0;
        for (auto end = GetTickCount64() + ms; GetTickCount64() < end; Sleep(10))
            for (MSG m{}; PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE);) {
                if (m.message == WM_TIMER && (m.hwnd == vfRoot || IsChild(vfRoot, m.hwnd))) ++n;
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        return n;
    };
    check(tabTimers(1000) > 0 && fixture->vf_calls > 0,
          "the hidden tab's bars timer fires and driver calls are counted");
    int calls = fixture->vf_calls;
    check(!IsWindowVisible(window_) && tabTimers(4000) == 0 && fixture->vf_calls == calls,
          "closed to the tray, the opened V/F tab runs no timer and sends nothing to the driver");
    ShowWindow(window_, SW_SHOW);
    for (UINT dpi : {96u, 144u, 192u}) {
        RECT r{80, 40, 80 + MulDiv(940, dpi, 96), 40 + MulDiv(760, dpi, 96)};
        SendMessageW(window_, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&r));
        for (int p = 0; p <= vfPage; ++p) {
            page(p);
            wait([&] { return !busy_; });
            capture(L"page-" + std::to_wstring(p) + L"-" + std::to_wstring(dpi));
        }
    }
    applyTheme(true);
    for (UINT dpi : {96u, 144u}) {
        RECT r{80, 40, 80 + MulDiv(940, dpi, 96), 40 + MulDiv(760, dpi, 96)};
        SendMessageW(window_, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&r));
        for (int p = 0; p <= vfPage; ++p) {
            page(p);
            wait([&] { return !busy_; });
            capture(L"dark-" + std::to_wstring(p) + L"-" + std::to_wstring(dpi));
        }
    }
    // A device operation disables the pages; re-enabling one must not let the static control paint over its sections.
    // Read from the screen, because capture() repaints everything.
    page(1);
    wait([&] { return !busy_; });
    auto cardPixel = [&] {
        GdiFlush();
        DwmFlush();
        RECT r{};
        GetWindowRect(pages_[1].window, &r);
        auto screen = GetDC(nullptr);
        // Inside the first card, left of its accent mark, where no control sits.
        COLORREF c = GetPixel(screen, r.left + scale(14), r.top + scale(20));
        ReleaseDC(nullptr, screen);
        return c;
    };
    check(cardPixel() == palette().card, "the Fan Control page shows its first section");
    busy(true);
    pump();
    busy(false);
    pump();
    check(cardPixel() == palette().card, "a page keeps its painted sections after a device operation");
    report
        << "Injected DPI/failures and synthetic hardware, not physical qualification. Production widgets/controllers were used.\n";
    DestroyWindow(window_);
}
