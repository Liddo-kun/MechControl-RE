#include "vf_tab.h"
#include "config_json.h"
#include "config_text.h"
#include "theme.h"
#include "vf_chart.h"
#include "vf_numbers.h"
#include "vf_tuner.h"
#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

namespace mech {
using namespace nv::vf;
namespace {
constexpr wchar_t rootClass[] = L"MechControl.Native.VfTab", panelClass[] = L"MechControl.Native.VfPanel";
constexpr UINT_PTR readTimer = 1, zoneTimer = 2;
constexpr int zoneSteps = 32, sixty = 5;                   // positions a zone tries; the 60 °C column
constexpr int hues[]{275, 225, 190, 140, 75, 38, 15, 350}; // one color per column, cold to hot
constexpr const wchar_t* modes[]{L"Clock (MHz)", L"Difference from stock", L"Difference from the reference curve"};
constexpr const char* modeNames[]{"abs", "diff", "base"};
using Clock = std::chrono::steady_clock;

std::wstring w(const std::string& s) { return config::wide(s); }
std::wstring plural(size_t n, const std::wstring& one) {
    return std::to_wstring(n) + L" " + one + (n == 1 ? L"" : L"s");
}
std::wstring join(const std::vector<std::wstring>& parts, const wchar_t* separator) {
    std::wstring s;
    for (auto& p : parts) s += (s.empty() ? L"" : separator) + p;
    return s;
}
double f32(double v) { return double(float(v)); }
// An equation's min or max; none at or beyond 3e38, which is no limit.
std::optional<double> limit(double v) { return std::abs(v) < unbounded ? std::optional(v) : std::nullopt; }
// Local time as the page saved it: 2026-11-19T18:09:18.
std::string now() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char b[32];
    snprintf(b, sizeof b, "%04u-%02u-%02uT%02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}
// An equation number: nine significant digits of the 32-bit value, which read back as the same value, with an
// exponent below 0.001.
std::wstring fmt(std::optional<double> v) {
    if (!v) return L"";
    double x = f32(*v);
    wchar_t b[40];
    if (x != 0 && std::abs(x) < 1e-3) {
        swprintf(b, 40, L"%.8e", x);
        std::wstring s = b, mantissa = s.substr(0, s.find(L'e'));
        mantissa.erase(mantissa.find_last_not_of(L'0') + 1);
        if (mantissa.back() == L'.') mantissa.pop_back();
        return mantissa + L"e" + std::to_wstring(_wtoi(s.c_str() + s.find(L'e') + 1));
    }
    swprintf(b, 40, L"%.9g", x);
    return b;
}
COLORREF hsl(double h, double s, double l) {
    auto f = [&](double n) {
        double k = std::fmod(n + h / 30, 12), a = s * std::min(l, 1 - l);
        return int((l - a * std::max(-1.0, std::min({k - 3, 9 - k, 1.0}))) * 255 + 0.5);
    };
    return RGB(f(0), f(8), f(4));
}
// A column's color; lighter on a dark background.
COLORREF hue(int column) { return hsl(hues[column % 8], 0.85, palette().dark ? 0.64 : 0.40); }
std::wstring windowText(HWND window) {
    int n = GetWindowTextLengthW(window);
    std::wstring s(n + 1, L'\0');
    s.resize(GetWindowTextW(window, s.data(), n + 1));
    return s;
}
std::optional<double> parse(const std::wstring& s) {
    wchar_t* end = nullptr;
    double v = wcstod(s.c_str(), &end);
    if (end == s.c_str() || s.find_first_not_of(L" \t", end - s.c_str()) != std::wstring::npos || !std::isfinite(v))
        return {};
    return v;
}

// A card slider (vfe-ui.html PARAMS): the term of a main formula it needs, its range on the foot and elsewhere.
struct Param {
    double Card::* field;
    int Slots::* slot;
    const wchar_t *name, *unit;
    double lo, hi, footLo, footHi;
    const wchar_t* hint;
};
// The foot is about 30 MHz per mV, so its ends need far more to move it: +4000 at the left end starts it about
// 110 mV earlier, -1000 at the right end about 25 mV later.
const Param params[]{
    {&Card::l, &Slots::c, L"Left end", L"MHz", -800, 600, -1000, 4000,
     L"moves the left end of the line up or down; the right end stays"},
    {&Card::r, &Slots::v, L"Right end", L"MHz", -800, 600, -1000, 2000,
     L"moves the right end of the line up or down; the left end stays"},
    {&Card::bend, &Slots::v2, L"Bend", L"MHz", -150, 150, -500, 500, L"raises the middle by this much; both ends stay"},
    {&Card::tl, &Slots::t, L"Temp, left", L"MHz/10 °C", -40, 40, -200, 200,
     L"adds this much per 10 °C warmer at the left end; no change in the 60 °C column"},
    {&Card::tr, &Slots::vt, L"Temp, right", L"MHz/10 °C", -40, 40, -200, 200,
     L"adds this much per 10 °C warmer at the right end; no change in the 60 °C column"},
    {&Card::tbend, &Slots::t2, L"Temp bend", L"MHz", -40, 40, -40, 40,
     L"adds about this much one column away from 60 °C and four times as much two columns away; no change in the 60 °C column"},
};
}

struct VfTab::Impl {
    struct Slider {
        HWND track = nullptr, edit = nullptr;
        std::wstring name, unit, hint;
        double lo = 0, hi = 0, step = 2.5;
        std::function<double&(Tune&)> value;
        bool doubleStep = false; // Afterburner Style OC: kept on two clock steps
        std::string card;        // whose lines it moves (its zone); empty: no zone
        int p0 = 0, p1 = 0;      // the points it can move
        std::vector<char> zone;  // per segment: moving the slider there changes the curve
        bool stale = true;
        RECT label{}, strip{};
        int barLeft = 0, barRight = 0, barWidth = -1; // the bar's ends in a track this wide
    };
    // Floor or Ceiling: the clock equation's limit, typed in.
    struct Limit {
        HWND edit = nullptr;
        std::optional<double> Tune::* value;
        double Equation::* stock;
        std::wstring name, hint;
    };
    struct View {
        std::array<bool, 8> stock{}, fresh{}, band{};
        bool lines = true;
        int mode = 0;
    };
    struct Readback {
        std::wstring what, verdict, offsets, more;
        bool ok = true;
    };
    // One driver command at a time, the one of `sequence`. A write that changed equations waits 5 s for the driver's
    // fallback curve to pass, then reads back.
    enum class Job { none, opening, reading, loading, writing, restoring, waiting, verifying };

    HINSTANCE instance;
    VfHost host;
    PageFonts fonts{};
    HWND root = nullptr, panel = nullptr;
    Page page; // the scrolling cards in panel
    HWND applyButton = nullptr, restoreButton = nullptr, resetButton = nullptr, importButton = nullptr,
         exportButton = nullptr;
    HWND issueLink = nullptr, modeBox = nullptr, linesCheck = nullptr, referenceButton = nullptr, numbersLink = nullptr,
         structureLink = nullptr;
    std::unique_ptr<VfChart> chart;
    std::unique_ptr<VfNumbers> numbers, structure;

    std::shared_ptr<const Tuner> tuner;
    std::shared_ptr<const VfReading> reading;
    Tune tune;
    Equations equations;  // what Apply writes
    std::wstring invalid; // why the tune cannot be written
    std::vector<double> offsets;
    std::array<std::optional<Calc>, 8> stockCalc, tunedCalc;
    View view;
    std::array<std::vector<double>, 8> sheet; // the reference curve per column; empty where it has none
    std::wstring sheetName;
    std::optional<Live> onGpu;    // the equations on the GPU, from the last read
    std::vector<int> liveOffsets; // kHz
    std::string hot;              // the card the user works on

    Job job = Job::opening;
    std::uint64_t sequence = 0;
    std::wstring waitWhat; // what the readback after the wait reports
    // A profile was loaded: the next read shows the tune of the curve on the GPU and reads back if the equations
    // changed. Unapplied changes go, as on the other tabs.
    bool loaded = false;
    std::wstring openError, readError;
    std::optional<Readback> readback;

    std::vector<Slider> sliders;
    std::vector<Limit> limits;
    std::vector<HWND> ruleChecks;               // "Remove the correction" per rule
    std::array<std::array<HWND, 8>, 3> shows{}; // stock, new, reference by column
    std::array<RECT, 3> showLabels{};
    bool numbersOpen = false, structureOpen = false;

    std::vector<PageFrame> lastFrames; // what the layout before painted
    std::vector<PageText> lastTexts;
    std::vector<std::pair<size_t, std::string>> cards;   // the frame of each section card and its id
    std::vector<double> pointMv;                         // each point's voltage
    std::vector<std::pair<int, std::wstring>> lineCards; // each card line's card name
    std::vector<std::pair<RECT, std::wstring>> tips;     // painted parts with a tooltip
    // The widths of the fixed texts (names, units, labels) by font, and the sizes of the tables' links.
    std::map<std::pair<HFONT, std::wstring>, int> widths;
    std::map<std::pair<std::wstring, int>, int> heights; // of the Show rows' names, by wrapping width
    std::map<HWND, SIZE> linkSizes;
    std::wstring tipText;
    size_t tools = 0;
    RECT statusRect{};
    std::wstring statusText;
    COLORREF statusColor = 0;

    struct {
        std::vector<size_t> order;
        std::vector<int> columns;
        size_t k = 0;
        int j = 0;
        std::vector<std::vector<double>> signatures;
        bool pending = false, running = false;
    } zones;
    HWND downTrack = nullptr;
    DWORD downTime = 0;

    Impl(HINSTANCE i, HWND parent, VfHost h) : instance(i), host(std::move(h)) {
        for (auto [name, procedure] :
             {std::pair(rootClass, &Impl::procedure<false>), {panelClass, &Impl::procedure<true>}}) {
            WNDCLASSW c{};
            if (GetClassInfoW(instance, name, &c)) continue;
            c.lpfnWndProc = procedure;
            c.hInstance = instance;
            c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            c.lpszClassName = name;
            RegisterClassW(&c);
        }
        root =
            CreateWindowExW(WS_EX_CONTROLPARENT, rootClass, L"V/F Curve", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                            0, 0, 0, 0, parent, nullptr, instance, this);
        if (!root) throw std::runtime_error("Cannot create the V/F Curve tab");
        applyButton = make(root, L"BUTTON", L"Apply to GPU", WS_TABSTOP | BS_PUSHBUTTON);
        restoreButton = make(root, L"BUTTON", L"Restore stock", WS_TABSTOP | BS_PUSHBUTTON);
        resetButton = make(root, L"BUTTON", L"Reset all", WS_TABSTOP | BS_PUSHBUTTON);
        importButton = make(root, L"BUTTON", L"Import…", WS_TABSTOP | BS_PUSHBUTTON);
        exportButton = make(root, L"BUTTON", L"Export…", WS_TABSTOP | BS_PUSHBUTTON);
        chart = std::make_unique<VfChart>(instance, root);
        chart->bump = [this](const std::set<int>& points, int steps) { bump(points, steps); };
        panel =
            CreateWindowExW(WS_EX_CONTROLPARENT, panelClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                            0, 0, 0, 0, root, nullptr, instance, this);
        if (!panel) throw std::runtime_error("Cannot create the V/F Curve tab");
        issueLink = make(panel, WC_LINK, L"To get this GPU supported, <a>open an issue</a> with mechcontrol.log.",
                         WS_TABSTOP | LWS_TRANSPARENT | LWS_NOPREFIX);
        ShowWindow(issueLink, SW_HIDE);
        buttons();
        status();
        sequence = host.submit(VfRead{.open = true}, false);
    }
    ~Impl() {
        chart.reset();
        if (root) DestroyWindow(root);
    }

    int ref(int n) const { return pageRef(fonts.dpi, n); }
    int scale(int n) const { return MulDiv(n, fonts.dpi, 96); }
    HWND owner() const { return GetAncestor(root, GA_ROOT); }
    HWND make(HWND parent, const wchar_t* type, const std::wstring& title, DWORD style) {
        auto child = pageControl(instance, parent, type, title, style);
        if (fonts.font) SendMessageW(child, WM_SETFONT, WPARAM(fonts.font), FALSE);
        return child;
    }
    void tip(HWND child, const std::wstring& s) { addTip(host.tooltip, panel, UINT_PTR(child), s, TTF_IDISHWND); }
    void error(const std::wstring& s) {
        MessageBoxW(owner(), s.c_str(), L"MechControl — V/F Curve", MB_OK | MB_ICONERROR);
    }
    bool confirm(const std::wstring& s) {
        return MessageBoxW(owner(), s.c_str(), L"MechControl — V/F Curve", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) ==
               IDYES;
    }

    // ---- the tuner's controls, made once it is open ----
    void build() {
        auto& t = *tuner;
        tune = {};
        view = {};
        for (int p = 0; p < t.points(); ++p) pointMv.push_back(t.mv(p));
        for (auto& s : t.sections)
            for (int h : s.heads) lineCards.push_back({h, w(s.name)});
        view.stock[sixty] = view.fresh[sixty] = view.band[sixty] = true;
        modeBox = make(panel, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL);
        linesCheck = make(root, L"BUTTON", L"Draw Equation Guidelines", WS_TABSTOP | BS_AUTOCHECKBOX);
        referenceButton = make(panel, L"BUTTON", L"Reference CSV…", WS_TABSTOP | BS_PUSHBUTTON);
        tip(referenceButton,
            L"A curve measured with another tool (a CSV with the columns domain, index and actual_<band>), shown as crosses. Once one is loaded, the plot can show the difference from it.");
        for (auto& row : shows)
            for (auto& box : row) box = make(panel, L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX);
        auto slider = [&](std::wstring name, std::wstring unit, double lo, double hi, double step,
                          std::function<double&(Tune&)> value, std::wstring hint, std::string card, int p0, int p1) {
            Slider s{nullptr, nullptr, std::move(name), std::move(unit), std::move(hint), lo,
                     hi,      step,    std::move(value)};
            s.card = std::move(card);
            s.p0 = p0;
            s.p1 = p1;
            s.track = make(panel, TRACKBAR_CLASSW, s.name, WS_TABSTOP | TBS_NOTICKS);
            SendMessageW(s.track, TBM_SETRANGEMIN, FALSE, LPARAM(std::ceil(lo / step - 1e-9)));
            SendMessageW(s.track, TBM_SETRANGEMAX, FALSE, LPARAM(std::floor(hi / step + 1e-9)));
            SendMessageW(s.track, TBM_SETPAGESIZE, 0, LPARAM(std::max(1.0, std::round(10 / step))));
            SetWindowSubclass(s.track, trackProcedure, 1, DWORD_PTR(this));
            s.edit = make(panel, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_RIGHT);
            styleFrame(s.edit);
            SetWindowSubclass(s.edit, editProcedure, 1, DWORD_PTR(this));
            tip(s.track, s.hint);
            sliders.push_back(std::move(s));
        };
        int last = t.points() - 1;
        double step = t.meta.step;
        slider(
            L"Native VF Curve", L"MHz", -300, 600, 2.5, [](Tune& x) -> double& { return x.global; },
            L"Moves the whole equation curve.", "whole", 0, last);
        slider(
            L"Afterburner Style OC", L"MHz", -300, 450, 2 * step, [](Tune& x) -> double& { return x.po; },
            L"Added to every point, in " + number(2 * step) +
                L" MHz steps. Single points: click them on the chart (Shift: a range, Ctrl: add one), then press the up or down arrow (" +
                number(step) + L" MHz each).",
            "", 0, 0);
        sliders.back().doubleStep = true;
        for (auto [value, stock, name, hint] : {std::tuple(&Tune::floor, &Equation::lo, L"Floor", L"lowest clock"),
                                                {&Tune::ceiling, &Equation::hi, L"Ceiling", L"highest clock"}}) {
            Limit l{make(panel, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_RIGHT), value, stock, name,
                    hint};
            styleFrame(l.edit);
            SetWindowSubclass(l.edit, editProcedure, 1, DWORD_PTR(this));
            tip(l.edit, std::wstring(L"The ") + hint + L" the curve can give (equation " +
                            std::to_wstring(t.meta.root) + L"); empty: no limit.");
            limits.push_back(l);
        }
        for (auto& r : t.rules) {
            std::vector<std::wstring> equs;
            for (int i : r.equs) equs.push_back(std::to_wstring(i));
            auto c =
                make(panel, L"BUTTON", L"Remove the correction (written with Apply)", WS_TABSTOP | BS_AUTOCHECKBOX);
            tip(c, std::wstring(L"Apply sets equation") + (equs.size() > 1 ? L"s " : L" ") + join(equs, L", ") +
                       L" to 0.");
            ruleChecks.push_back(c);
        }
        for (auto& s : t.sections)
            for (auto& p : params) {
                if (!std::ranges::any_of(s.heads, [&](int h) { return t.slots.at(h).*p.slot >= 0; })) continue;
                bool foot = s.id == "foot";
                auto id = s.id;
                auto field = p.field;
                slider(
                    p.name, p.unit, foot ? p.footLo : p.lo, foot ? p.footHi : p.hi, 2.5,
                    [id, field](Tune& x) -> double& { return x.cards[id].*field; },
                    std::wstring(p.name) + L" " + p.hint + L". Left end: " + number(s.lo) + L" mV, right end: " +
                        number(s.hi) + L" mV.",
                    s.id, s.p0, s.p1);
            }
        numbersLink = make(panel, WC_LINK, L"<a>Show</a>", WS_TABSTOP | LWS_TRANSPARENT);
        structureLink = make(panel, WC_LINK, L"<a>Show</a>", WS_TABSTOP | LWS_TRANSPARENT);
        numbers = std::make_unique<VfNumbers>(instance, panel);
        structure = std::make_unique<VfNumbers>(instance, panel);
        SendMessageW(modeBox, CB_ADDSTRING, 0, LPARAM(modes[0]));
        SendMessageW(modeBox, CB_ADDSTRING, 0, LPARAM(modes[1]));
        recalc();
        sync();
        style(fonts);
    }

    // ---- the calculation ----
    const Calc& stock(int c) {
        if (!stockCalc[c]) stockCalc[c] = tuner->calc(tuner->m.E, columns[c].calc);
        return *stockCalc[c];
    }
    const Calc& tuned(int c) {
        if (!tunedCalc[c]) tunedCalc[c] = tuner->calc(equations, columns[c].calc);
        return *tunedCalc[c];
    }
    // The new curve with the offsets.
    std::vector<double> newCurve(int c) {
        auto& t = tuned(c).clock;
        return std::ranges::any_of(offsets, [](double o) { return o != 0; })
                   ? with_offsets(t, offsets, tuner->meta.step)
                   : t;
    }
    bool have(int c) const { return !sheet[c].empty(); }
    void recalc() {
        try {
            equations = tuner->equations(tune);
            invalid.clear();
        } catch (const std::exception& e) {
            invalid = w(e.what());
        }
        offsets = tuner->offsets(tune);
        tunedCalc = {};
    }
    // The columns of the new curve shown, or 60 °C and the coldest, at most three.
    std::vector<int> shownNew() const {
        std::vector<int> c;
        for (int i = 0; i < 8; ++i)
            if (view.fresh[i]) c.push_back(i);
        if (c.empty()) c = {sixty, 0, 1};
        c.resize(std::min<size_t>(c.size(), 3));
        return c;
    }
    std::wstring runs(const std::vector<int>& ps) const { return mech::runs(pointMv, ps); }

    // ---- after every change ----
    void changed() {
        recalc();
        sync();
        follow(true);
    }
    // A slider moved. Everything it changes is painted before the next mouse move is taken: Windows paints only
    // when no input waits, and in a fast drag the next move always does.
    void stepped(Slider& s) {
        recalc();
        sync(s);
        follow(false);
        UpdateWindow(chart->window());
        UpdateWindow(panel);
        UpdateWindow(root);
    }
    void follow(bool place) {
        if (numbersOpen || structureOpen) tables();
        shown(place);
        status();
        buttons();
    }
    // What is shown changed: the cards' statuses, the chart and the zones follow. Of the cards, only what changed
    // repaints; a slider step usually changes a few texts. Without `place` the controls stay where they are unless a
    // card changed its size.
    void shown(bool place = true) {
        layout(place);
        auto& texts = page.texts;
        bool same = page.frames == lastFrames && texts.size() == lastTexts.size();
        if (!same && !place) {
            same = slide();
            layout(true);
        }
        if (!same)
            InvalidateRect(panel, nullptr, FALSE);
        else
            for (size_t i = 0; i < texts.size(); ++i)
                if (texts[i] != lastTexts[i]) {
                    InvalidateRect(panel, &lastTexts[i].rect, FALSE);
                    InvalidateRect(panel, &texts[i].rect, FALSE);
                }
        plot();
        scheduleZones();
    }
    // A slider that passes 0 gives Current Changes a line more or less, and every card below moves by that line.
    // Where the new layout is the last one with one card that much higher, what is below is moved as a scroll moves
    // it, controls and pixels; that card and what is above it repaint, and below it the texts that changed. The
    // layout that follows places whatever is not where it belongs (a control below the window is not scrolled).
    bool slide() {
        auto& frames = page.frames;
        auto& texts = page.texts;
        if (frames.size() != lastFrames.size()) return false;
        size_t k = 0;
        while (k < frames.size() && frames[k] == lastFrames[k]) ++k;
        if (k == frames.size()) return false;
        int below = lastFrames[k].rect.bottom, by = frames[k].rect.bottom - below;
        auto moved = [&](auto was) {
            OffsetRect(&was.rect, 0, by);
            return was;
        };
        auto card = lastFrames[k];
        card.rect.bottom += by;
        if (!by || frames[k] != card) return false;
        for (size_t j = k + 1; j < frames.size(); ++j) {
            // A card beside it (GPU Readback) grows with it.
            auto beside = lastFrames[j];
            beside.rect.bottom += by;
            if (lastFrames[j].rect.top == card.rect.top && frames[j] == beside) continue;
            if (lastFrames[j].rect.top < below || frames[j] != moved(lastFrames[j])) return false;
        }
        // The texts below the card, which follow those in and above it.
        auto under = [](const std::vector<PageText>& list, int y) {
            return std::ranges::find_if(list, [&](const PageText& x) { return x.rect.top >= y; });
        };
        auto was = under(lastTexts, below);
        auto now = under(texts, below + by);
        if (lastTexts.end() - was != texts.end() - now) return false;
        RECT client{};
        GetClientRect(panel, &client);
        RECT from{0, below, client.right, client.bottom + 100000},
            into{0, std::min(below, below + by), client.right, client.bottom},
            above{0, 0, client.right, std::max(below, below + by)};
        page.shift(by, &from, &into);
        InvalidateRect(panel, &above, FALSE);
        for (; now != texts.end(); ++now, ++was)
            if (auto old = moved(*was); *now != old) {
                InvalidateRect(panel, &old.rect, FALSE);
                InvalidateRect(panel, &now->rect, FALSE);
            }
        return true;
    }
    void sync(Slider& s) {
        double v = s.value(tune);
        auto pos = LPARAM(std::lround(v / s.step));
        if (SendMessageW(s.track, TBM_GETPOS, 0, 0) != pos) SendMessageW(s.track, TBM_SETPOS, TRUE, pos);
        auto t = w(config::decimal_text(std::round(v * 100) / 100, 2, true));
        if (GetFocus() != s.edit && windowText(s.edit) != t) SetWindowTextW(s.edit, t.c_str());
    }
    void sync() {
        for (auto& s : sliders) sync(s);
        for (auto& l : limits) {
            auto t = fmt(tune.*l.value ? tune.*l.value : stockLimit(l));
            if (GetFocus() != l.edit && windowText(l.edit) != t) SetWindowTextW(l.edit, t.c_str());
        }
        // Only where it changes: setting a check box repaints it, and a slider step changes none.
        auto check = [](HWND box, bool on) {
            if ((Button_GetCheck(box) == BST_CHECKED) != on) Button_SetCheck(box, on);
        };
        for (size_t i = 0; i < ruleChecks.size(); ++i) {
            auto it = tune.rules.find(tuner->rules[i].id);
            check(ruleChecks[i], it != tune.rules.end() && it->second);
        }
        for (int c = 0; c < 8; ++c) {
            check(shows[0][c], view.stock[c]);
            check(shows[1][c], view.fresh[c]);
            check(shows[2][c], view.band[c]);
        }
        check(linesCheck, view.lines);
        modeItems();
    }
    // The difference from the reference curve is offered once one is loaded.
    void modeItems() {
        if (sheetName.empty() && view.mode == 2) view.mode = 0;
        if (!sheetName.empty() && SendMessageW(modeBox, CB_GETCOUNT, 0, 0) == 2)
            SendMessageW(modeBox, CB_ADDSTRING, 0, LPARAM(modes[2]));
        if (SendMessageW(modeBox, CB_GETCURSEL, 0, 0) != view.mode) SendMessageW(modeBox, CB_SETCURSEL, view.mode, 0);
    }
    void touch(Slider& s, double v) {
        s.value(tune) = s.doubleStep ? std::round(v / (2 * tuner->meta.step)) * 2 * tuner->meta.step : v;
        if (!s.card.empty()) hot = s.card;
        stepped(s);
    }
    void bump(const std::set<int>& points, int steps) {
        for (int p : points) {
            double v = (tune.pp.contains(p) ? tune.pp[p] : 0) + steps * tuner->meta.step;
            if (v)
                tune.pp[p] = v;
            else
                tune.pp.erase(p);
        }
        changed();
    }
    void commit(HWND edit) {
        auto s = windowText(edit);
        for (auto& sl : sliders)
            if (sl.edit == edit) {
                auto v = parse(s).value_or(0);
                if (v != sl.value(tune)) touch(sl, v);
                return;
            }
        for (auto& l : limits)
            if (l.edit == edit) {
                auto v = parse(s);
                tune.*l.value = v && f32(*v) != stockLimit(l) ? std::optional(f32(*v)) : std::nullopt;
                changed();
                return;
            }
    }
    // The clock equation's stock floor or ceiling; none where it has no limit.
    std::optional<double> stockLimit(const Limit& l) const { return limit(tuner->m.E[tuner->meta.root].*l.stock); }

    // ---- the chart ----
    void plot() {
        if (!tuner) return;
        auto& t = *tuner;
        int n = t.points();
        VfPlot p;
        for (int i = 0; i < n; ++i) p.mv.push_back(t.mv(i));
        if (reading && int(reading->points.graphics.size()) == n)
            for (auto& g : reading->points.graphics) p.listed.push_back(g.uv / 1000.0);
        p.offsets = offsets;
        p.step = t.meta.step;
        auto base = [&](int c) -> std::optional<std::vector<double>> {
            if (view.mode == 1) return stock(c).clock;
            if (view.mode == 2) {
                if (!have(c)) return {};
                return sheet[c];
            }
            return std::vector<double>(n, 0.0);
        };
        auto series = [&](const std::vector<double>& y, int c) {
            std::vector<std::optional<double>> out(n);
            auto b = base(c);
            for (int i = 0; i < n && i < int(y.size()); ++i)
                if (b && !std::isnan(y[i]) && !std::isnan((*b)[i])) out[i] = y[i] - (*b)[i];
            return out;
        };
        auto name = [](const wchar_t* what, int c) { return what + std::to_wstring(columns[c].celsius) + L" °C"; };
        for (int c = 0; c < 8; ++c)
            if (view.band[c] && have(c)) p.series.push_back({name(L"reference ", c), hue(c), '+', series(sheet[c], c)});
        for (int c = 0; c < 8; ++c)
            if (view.fresh[c]) p.series.push_back({name(L"new ", c), hue(c), 'f', series(newCurve(c), c)});
        for (int c = 0; c < 8; ++c)
            if (view.stock[c]) p.series.push_back({name(L"stock ", c), hue(c), 'o', series(stock(c).clock, c)});
        // Each card's lines behind the new curve: the clock is the lowest of them (then floor, ceiling, never falling).
        if (view.lines)
            for (int c = 0; c < 8; ++c) {
                auto b = base(c);
                if (!view.fresh[c] || !b) continue;
                auto& calc = tuned(c);
                for (auto& s : t.sections)
                    for (int h : s.heads) {
                        VfLine line{hue(c), s.id == hot, std::vector<std::optional<double>>(n)};
                        auto& y = calc.lines.at(h);
                        for (int i = 0; i < n; ++i)
                            if (y[i] && !std::isnan((*b)[i])) line.y[i] = *y[i] - (*b)[i];
                        p.lines.push_back(std::move(line));
                    }
            }
        p.relative = view.mode != 0;
        chart->set(std::move(p));
    }

    // ---- the bars under the sliders ----
    // Blue where moving the slider changes the new curve at a plotted temperature, grey where it changes nothing
    // (another line is lower there, or the curve is at the floor, at the ceiling or held flat). Worked out a little
    // after the last change, a few positions at a time, the card in use first.
    void scheduleZones() {
        zones.order.clear();
        for (bool first : {true, false})
            for (size_t i = 0; i < sliders.size(); ++i)
                if (!sliders[i].card.empty() && (sliders[i].card == hot) == first) {
                    if (!sliders[i].stale) InvalidateRect(panel, &sliders[i].strip, FALSE); // paler until worked out
                    sliders[i].stale = true;
                    zones.order.push_back(i);
                }
        zones.columns = shownNew();
        zones.k = 0;
        zones.j = 0;
        zones.signatures.clear();
        zones.pending = !zones.order.empty();
        zones.running = zones.pending;
        if (zones.pending) SetTimer(panel, zoneTimer, 250, nullptr);
    }
    std::vector<double> signature(const Slider& s, int j) {
        Tune t = tune;
        s.value(t) = s.lo + (s.hi - s.lo) * j / zoneSteps;
        std::vector<double> out;
        try {
            auto E = tuner->equations(t);
            for (int c : zones.columns) {
                double top = s.p0 ? tuned(c).raw[s.p0 - 1] : -INFINITY;
                Env env(tuner->m, E, tuner->meta.uv[s.p0], columns[c].calc);
                for (int p = s.p0; p <= s.p1; ++p) {
                    env.voltage(tuner->meta.uv[p]);
                    top = std::max(top, env.equ(tuner->meta.root));
                    out.push_back(to_step(top, tuner->meta.step));
                }
            }
        } catch (const std::exception&) {
            out.clear();
        }
        return out;
    }
    void zoneTick() {
        // Paused while the tab is hidden; painting it again resumes. A tick ends early when the mouse or a key waits.
        if (!IsWindowVisible(panel)) {
            KillTimer(panel, zoneTimer);
            zones.running = false;
            return;
        }
        SetTimer(panel, zoneTimer, USER_TIMER_MINIMUM, nullptr);
        auto start = Clock::now();
        do {
            if (zones.k >= zones.order.size()) {
                KillTimer(panel, zoneTimer);
                zones.pending = zones.running = false;
                return;
            }
            auto& s = sliders[zones.order[zones.k]];
            zones.signatures.push_back(signature(s, zones.j));
            if (++zones.j > zoneSteps) {
                s.zone.assign(zoneSteps, 0);
                for (int j = 0; j < zoneSteps; ++j) s.zone[j] = zones.signatures[j + 1] != zones.signatures[j];
                s.stale = false;
                InvalidateRect(panel, &s.strip, FALSE);
                ++zones.k;
                zones.j = 0;
                zones.signatures.clear();
            }
        } while (Clock::now() - start < std::chrono::milliseconds(8) && !HIWORD(GetQueueStatus(QS_INPUT)));
    }

    // ---- the tables of every number ----
    void tables() {
        auto& t = *tuner;
        // A number `get` reads from equation i, as Apply writes it; highlighted where it differs from stock.
        auto cell = [&](int i, auto get) -> VfItem {
            std::optional<double> v = get(equations.at(i)), b = get(t.m.E.at(i));
            return {fmt(v), false, false, true, v != b};
        };
        auto lo = [](const Equation& e) { return limit(e.lo); };
        auto hi = [](const Equation& e) { return limit(e.hi); };
        auto label = [](std::wstring s, bool muted = false) { return VfItem{std::move(s), muted, true}; };
        auto text = [](std::wstring s, bool muted = false) { return VfItem{std::move(s), muted}; };
        auto target = [&](int i) {
            auto& e = t.m.E.at(i);
            return e.leaf() && e.lo == 0 && e.hi == 0 && e.next < 0 ? std::wstring(L"nothing (0)")
                                                                    : L"equation " + std::to_wstring(i);
        };
        auto what = [&](const Row& r, const wchar_t* kind) {
            std::vector<VfItem> v{label(kind)};
            if (!r.when.empty()) v.push_back(label(L"only when " + w(r.when), true));
            if (!r.used.empty()) v.push_back(label(w(r.used), true));
            return v;
        };
        // A comparison's or choice's setting.
        auto setting = [&](int v, std::vector<const wchar_t*> names) {
            return std::wstring(names[std::clamp(v, 0, int(names.size()) - 1)]);
        };
        // One row (vfe-ui.html rowFor); a test or choice spans the columns from 2.
        auto rowFor = [&](const Row& r, bool head) {
            auto& e = t.m.E.at(r.equ);
            auto& now = equations.at(r.equ);
            VfRow row;
            row.spans.push_back({0, {text(std::to_wstring(r.equ))}});
            if (r.kind == "leaf") {
                bool fixed = std::abs(e.lo) < unbounded && e.lo == e.hi;
                row.spans.push_back({1, what(r, head    ? L"main formula"
                                                : fixed ? L"fixed amount (MHz)"
                                                        : L"correction, kept between min and max")});
                if (fixed) {
                    row.spans.push_back({2, {cell(r.equ, lo)}});
                    row.spans.push_back({3, {text(L"stays this value", true)}});
                } else {
                    for (int j = 0; j < 10; ++j)
                        row.spans.push_back({2 + j, j < e.nk ? std::vector{cell(
                                                                   r.equ, [j](const Equation& x) { return x.k[j]; })}
                                                             : std::vector<VfItem>{}});
                    row.spans.push_back({12, {cell(r.equ, lo)}});
                    row.spans.push_back({13, {cell(r.equ, hi)}});
                }
            } else if (r.kind == "cmp") {
                // The value it compares with; none while it compares with another equation.
                auto crit = [](const Equation& x) { return x.ref ? std::nullopt : std::optional(x.crit); };
                row.spans.push_back({1, what(r, L"test")});
                row.spans.push_back({2,
                                     {text(L"if " + w(r.var) + L" " + setting(now.func, {L"=", L"≥", L">"}) + L" "),
                                      cell(r.equ, crit), text(L" then " + target(r.yes) + L", else " + target(r.no))}});
            } else {
                row.spans.push_back({1, what(r, L"choice")});
                row.spans.push_back({2,
                                     {text(setting(now.max, {L"lowest of", L"highest of"}) + L" equation " +
                                           std::to_wstring(r.a) + L" and equation " + std::to_wstring(r.b))}});
            }
            return row;
        };
        std::vector<VfTable> lines;
        for (auto& s : t.sections)
            for (auto& [head, rows] : s.chains) {
                VfTable table{&s.chains.front().second == &rows
                                  ? w(s.name) + L" · " + number(s.lo) + L"–" + number(s.hi) + L" mV"
                                  : L"",
                              "ew" + std::string(12, 'n')};
                VfRow header{{}, true};
                header.spans.push_back({0, {label(L"equ")}});
                header.spans.push_back({1, {label(L"what it is")}});
                for (int j = 0; j < 10; ++j)
                    header.spans.push_back({2 + j, {label(L"k" + std::to_wstring(j) + L" · " + w(t.knames[j]))}});
                header.spans.push_back({12, {label(L"min")}});
                header.spans.push_back({13, {label(L"max")}});
                table.rows.push_back(std::move(header));
                for (auto& r : rows) table.rows.push_back(rowFor(r, r.equ == head));
                lines.push_back(std::move(table));
            }
        VfTable tests{L"", "ewsnn"};
        VfRow header{{}, true};
        for (auto [i, s] :
             {std::pair(0, L"equ"), {1, L"what it is"}, {2, L"setting"}, {3, L"result min"}, {4, L"result max"}})
            header.spans.push_back({i, {label(s)}});
        tests.rows.push_back(std::move(header));
        for (auto& r : t.structure) {
            auto row = rowFor(r, false);
            if (r.kind == "leaf") row.spans.resize(2);
            row.spans.push_back({3, {cell(r.equ, lo)}});
            row.spans.push_back({4, {cell(r.equ, hi)}});
            tests.rows.push_back(std::move(row));
        }
        numbers->set(std::move(lines));
        structure->set({std::move(tests)});
    }

    // ---- the driver ----
    // A busy command is not sent while another operation runs; the job in progress then stays.
    void submit(ControlCommand command, Job next, bool busy = false) {
        if (auto s = host.submit(std::move(command), busy)) {
            sequence = s;
            job = next;
        }
    }
    void delivered(std::uint64_t seq, const ControlResult& r) {
        if (!seq || seq != sequence) return;
        sequence = 0;
        auto done = std::exchange(job, Job::none);
        bool failed = r.code || !r.vf;
        auto message = w(r.text);
        if (done == Job::opening) {
            if (failed)
                openError = L"The V/F curve tuner could not open: " + message;
            else {
                openError.clear();
                tuner = r.vf->tuner;
                build();
                layoutRoot(); // for the guidelines check box
                appliedTune(r.vf->tune);
                read(r.vf, L"");
            }
        } else if (done == Job::writing || done == Job::restoring) {
            if (failed) {
                readback = Readback{L"The write stopped.", message, L"", L"", false};
                submit(VfRead{}, Job::reading);
            } else {
                auto what =
                    done == Job::restoring
                        ? L"Stock restored (" + plural(r.vf->equations, L"equation") + L")."
                        : L"Applied (" + plural(r.vf->equations, L"equation") +
                              (r.vf->offsets ? L", " + plural(r.vf->offsets, L"Afterburner Style OC offset") : L"") +
                              L").";
                readBack(r.vf, r.vf->equations, what);
            }
        } else if (failed)
            readError = L"Could not read the driver: " + message;
        else if (done == Job::loading) {
            appliedTune(r.vf->tune);
            readBack(r.vf, !reading || reading->records != r.vf->records, L"");
        } else
            read(r.vf, done == Job::verifying ? waitWhat : L"");
        if (loaded && job == Job::none) refresh();
        buttons();
        status();
        layout(true);
        InvalidateRect(panel, nullptr, FALSE);
    }
    // After an equation write the driver shows a slower fallback curve for a few seconds; the readback reads once more
    // 5 s after the write.
    void readBack(const std::shared_ptr<const VfReading>& r, bool wrote, const std::wstring& what) {
        if (wrote && live(r)) {
            waitWhat = what;
            job = Job::waiting;
            SetTimer(root, readTimer, 5000, nullptr);
        } else
            read(r, what);
    }
    // A profile was loaded (VfTab::loaded); reads the driver once the tab is free.
    void refresh() {
        if (!tuner || job != Job::none) return;
        loaded = false;
        submit(VfRead{}, Job::loading);
    }
    // The tune of the curve this run last wrote, from a profile or this tab. What is shown stays as it is.
    void appliedTune(const config::Json& state) {
        if (!state.is_object()) return;
        try {
            take(tuner->tune(state));
        } catch (const std::exception& e) {
            error(L"The profile's V/F curve settings cannot be shown: " + w(e.what()));
        }
    }
    bool live(const std::shared_ptr<const VfReading>& r) {
        reading = r;
        try {
            onGpu = tuner->live(r->records);
            liveOffsets.clear();
            for (auto& p : r->points.graphics) liveOffsets.push_back(p.offset_khz);
            readError.clear();
            return true;
        } catch (const std::exception& e) {
            onGpu.reset();
            readError = L"Could not read the driver: " + w(e.what());
            return false;
        }
    }
    void read(const std::shared_ptr<const VfReading>& r, std::wstring what) {
        if (!live(r)) return;
        if (what.empty())
            what = onGpu->changed.empty() ? L"GPU now: stock equations."
                                          : L"GPU now: " + plural(onGpu->changed.size(), L"equation") + L" changed.";
        verify(what);
        plot();
    }
    // The driver's curve, read back, against the calculation with the equations on the GPU (vfe-ui.html verify).
    void verify(const std::wstring& what) {
        Readback b{what};
        int n = tuner->points();
        auto of = L" of " + std::to_wstring(n) + L" points";
        try {
            auto fit = tuner->fit(onGpu->E, reading->points);
            b.ok = fit.equal == n;
            b.verdict = b.ok ? L"Readback matches this tab: all " + std::to_wstring(n) + L" points equal"
                             : L"Readback differs from this tab at " + std::to_wstring(n - fit.equal) + of + L".";
            b.more = L"Compared at " + number(fit.celsius) + L" °C";
            if (fit.offsets)
                b.offsets = L"With the Afterburner Style OC offsets, the final clock differs from this tab at " +
                            std::to_wstring(fit.offsets) + of + L".";
        } catch (const std::exception& e) {
            b.verdict = w(e.what());
            b.ok = false;
        }
        readback = std::move(b);
    }

    // ---- the buttons ----
    void buttons() {
        bool idle = tuner && job == Job::none;
        EnableWindow(applyButton, idle && tuner->refusal.empty() && invalid.empty());
        EnableWindow(restoreButton, idle);
        for (auto b : {resetButton, importButton, exportButton}) EnableWindow(b, tuner != nullptr);
    }
    void apply() {
        Curve curve;
        try {
            curve = tuner->compile(tune, reading->gpu);
        } catch (const std::exception& e) {
            error(w(e.what()));
            return;
        }
        std::vector<std::wstring> ids;
        for (auto& [i, record] : curve.records) ids.push_back(std::to_wstring(i));
        size_t moved = 0;
        for (size_t p = 0; p < curve.offsets_khz.size(); ++p)
            moved += curve.offsets_khz[p] != (p < liveOffsets.size() ? liveOffsets[p] : 0);
        if (!confirm(
                L"Write up to " + plural(ids.size(), L"equation") + L" to the GPU (" + join(ids, L", ") + L")" +
                (moved ? L" and " + plural(moved, L"Afterburner Style OC offset") : L"") +
                L"?\n\nThe GPU drops to a slower fallback curve for a few seconds after each write. Do this at idle, not in a game."))
            return;
        submitWrite(std::move(curve));
    }
    void restore() {
        if (!confirm(
                L"Write NVIDIA's stock equations back and set the Afterburner Style OC offsets to 0 (slow curve on again, none of your changes)?\n\nThe settings on this tab stay as they are."))
            return;
        submitWrite({});
    }
    // The tune goes with the write, so that a profile Save stores it beside the curve; what is shown does not.
    void submitWrite(std::optional<Curve> curve) {
        auto next = curve ? Job::writing : Job::restoring;
        submit(VfApply{std::move(curve), tuner->state(tune)}, next, true);
        buttons();
        status();
    }
    void reset() {
        if (!confirm(
                L"Set every slider and Afterburner Style OC offset to 0, clear Floor and Ceiling and untick the corrections? (Nothing is written to the GPU.)"))
            return;
        take({});
    }
    std::optional<std::filesystem::path> file(bool save, const wchar_t* filter, const wchar_t* extension) {
        wchar_t path[MAX_PATH]{};
        if (save) wcscpy_s(path, L"vfe-tune.json");
        OPENFILENAMEW f{sizeof f};
        f.hwndOwner = owner();
        f.lpstrFilter = filter;
        f.lpstrFile = path;
        f.nMaxFile = MAX_PATH;
        f.lpstrDefExt = extension;
        f.Flags = save ? OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR : OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (!(save ? GetSaveFileNameW(&f) : GetOpenFileNameW(&f))) return {};
        return std::filesystem::path(path);
    }
    // What the chart shows, kept beside the tune in an exported file (vfe-ui.html's keys).
    config::Json viewJson() const {
        auto columnsOf = [](const std::array<bool, 8>& on) {
            auto j = config::Json::object();
            for (int c = 0; c < 8; ++c)
                if (on[c]) j[std::to_string(columns[c].celsius)] = true;
            return j;
        };
        return {{"showStock", columnsOf(view.stock)},
                {"showNew", columnsOf(view.fresh)},
                {"showBand", columnsOf(view.band)},
                {"lines", view.lines},
                {"ymode", modeNames[view.mode]}};
    }
    // What an imported tune shows.
    void loadView(const config::Json& v) {
        for (auto [key, on] :
             {std::pair("showStock", &view.stock), {"showNew", &view.fresh}, {"showBand", &view.band}}) {
            if (!v.contains(key) || !v[key].is_object()) continue;
            for (int c = 0; c < 8; ++c) (*on)[c] = v[key].value(std::to_string(columns[c].celsius), false);
        }
        view.lines = v.value("lines", view.lines);
        for (int m = 0; m < 3; ++m)
            if (v.value("ymode", "") == modeNames[m]) view.mode = m;
    }
    void importTune() {
        auto path = file(false, L"Tune (*.json)\0*.json\0All files\0*.*\0", L"json");
        if (!path) return;
        try {
            auto j = config::read_json(*path);
            auto& state = j.contains("state") && j["state"].is_object() ? j["state"] : j;
            auto t = tuner->tune(state);
            loadView(state);
            take(t);
        } catch (const std::exception& e) {
            error(L"Could not import " + path->filename().wstring() + L": " + w(e.what()));
        }
    }
    void take(const Tune& t) {
        tune = t;
        chart->clear_selection();
        changed();
    }
    // The same file as the page's Save (vfe-tune.json), which `vfe-ui.py apply` can write without this tab.
    void exportTune() {
        if (!invalid.empty()) {
            error(L"The settings cannot be exported: " + invalid);
            return;
        }
        auto path = file(true, L"Tune (*.json)\0*.json\0", L"json");
        if (!path) return;
        auto state = tuner->state(tune);
        state.update(viewJson());
        auto j = config::Json{{"saved", now()}, {"state", std::move(state)}};
        j["offsets"] = config::Json::array();
        for (double o : offsets) j["offsets"].push_back(std::round(o * 1000) / 1000);
        j["fields"] = tuner->fields(equations);
        try {
            config::write_json(*path, j, true);
        } catch (const std::exception& e) {
            error(L"Could not export the settings: " + w(e.what()));
        }
    }
    void reference() {
        auto path = file(false, L"CSV (*.csv)\0*.csv\0All files\0*.*\0", L"csv");
        if (!path) return;
        try {
            auto bands = tuner->reference(*path);
            for (int c = 0; c < 8; ++c) {
                auto it = *columns[c].band ? bands.find(columns[c].band) : bands.end();
                bool any =
                    it != bands.end() && std::ranges::any_of(it->second, [](double v) { return !std::isnan(v); });
                sheet[c] = any ? it->second : std::vector<double>{};
            }
            sheetName = path->filename().wstring();
            modeItems();
        } catch (const std::exception& e) {
            error(L"Could not read " + path->filename().wstring() + L": " + w(e.what()));
            return;
        }
        shown();
    }

    // ---- the status beside the buttons ----
    void status() {
        auto& p = palette();
        auto before = std::pair(statusText, statusColor);
        statusColor = p.muted;
        if (!tuner)
            statusText = job == Job::opening ? L"Opening…" : L"Not open";
        else if (job == Job::writing || job == Job::restoring)
            statusText = L"Writing…";
        else if (job == Job::waiting || job == Job::verifying)
            statusText = L"Reading back…";
        else if (job != Job::none)
            statusText = L"Reading…";
        else if (!onGpu)
            statusText = L"GPU not read";
        else {
            bool same = invalid.empty() && equations == onGpu->E && onGpu->undescribed.empty() &&
                        offsets.size() == liveOffsets.size();
            for (size_t i = 0; same && i < offsets.size(); ++i) same = std::lround(offsets[i] * 1000) == liveOffsets[i];
            bool stockNow = onGpu->changed.empty() && std::ranges::all_of(liveOffsets, [](int o) { return o == 0; });
            statusText = same ? L"Applied to the GPU" : stockNow ? L"Unapplied (currently stock)" : L"Unapplied";
            statusColor = same ? p.ok : p.warn;
        }
        if (before != std::pair(statusText, statusColor)) InvalidateRect(root, &statusRect, FALSE);
    }

    // ---- layout ----
    void layoutRoot() {
        if (!chart || !panel) return; // still being made
        RECT rc{};
        GetClientRect(root, &rc);
        auto dc = GetDC(root);
        SelectObject(dc, fonts.font);
        int x = ref(16), y = ref(8), h = ref(32);
        for (auto b : {applyButton, restoreButton, resetButton, importButton, exportButton}) {
            auto s = windowText(b);
            SIZE size{};
            GetTextExtentPoint32W(dc, s.c_str(), int(s.size()), &size);
            int width = std::max(int(size.cx) + ref(30), ref(100));
            MoveWindow(b, x, y, width, h, TRUE);
            x += width + ref(8);
        }
        // The guidelines check box ends the row once the tuner is open; the status fills the space before it.
        int status = rc.right - ref(16);
        if (linesCheck) {
            auto s = windowText(linesCheck);
            SIZE size{};
            GetTextExtentPoint32W(dc, s.c_str(), int(s.size()), &size);
            int width = int(size.cx) + ref(40), box = ref(28);
            status -= width + ref(16);
            MoveWindow(linesCheck, status + ref(16), y + (h - box) / 2, width, box, TRUE);
        }
        ReleaseDC(root, dc);
        statusRect = {x + ref(8), y, status, y + h};
        // The chart keeps its shape: 2.5 times as wide as high, and at most half of the height below the buttons.
        int top = y + h + ref(10), width = rc.right - 2 * ref(16), height = std::max(0, int(rc.bottom) - top);
        int chartHeight = std::min(int(width / 2.5), height / 2), chartWidth = std::min(width, int(chartHeight * 2.5));
        MoveWindow(chart->window(), (rc.right - chartWidth) / 2, top, chartWidth, chartHeight, TRUE);
        int below = top + chartHeight + ref(6);
        MoveWindow(panel, 0, below, rc.right, std::max(0, int(rc.bottom) - below), TRUE);
        InvalidateRect(root, nullptr, FALSE);
    }
    // The scrolling cards: one pass that measures them and, with place, also places the controls. The pass before stays
    // in lastFrames and lastTexts, so that only what changed repaints.
    void layout(bool place) {
        RECT rc{};
        GetClientRect(panel, &rc);
        if (rc.right <= 0 || !fonts.font) return;
        std::vector<std::pair<RECT, std::wstring>> tp;
        int content = 0;
        {
            PageLayout s(page, fonts, rc.right, place);
            cards.clear();
            content = cardsPass(s, tp);
            lastFrames = std::move(s.frames);
            lastTexts = std::move(s.texts);
        }
        if (page.range(content)) {
            layout(true);
            InvalidateRect(panel, nullptr, FALSE);
            return;
        }
        // Painted parts with a tooltip get one tool each, numbered from 1; their texts are asked for when shown.
        bool sameTips = std::ranges::equal(tips, tp, [](auto& a, auto& b) { return EqualRect(&a.first, &b.first); });
        tips = std::move(tp);
        if (sameTips && tools == tips.size()) return;
        for (size_t i = 0; i < tools; ++i) {
            TOOLINFOW t{sizeof t, 0, panel, i + 1};
            SendMessageW(host.tooltip, TTM_DELTOOLW, 0, LPARAM(&t));
        }
        for (size_t i = 0; i < tips.size(); ++i) {
            TOOLINFOW t{sizeof t, TTF_SUBCLASS, panel, i + 1, tips[i].first};
            t.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(host.tooltip, TTM_ADDTOOLW, 0, LPARAM(&t));
        }
        tools = tips.size();
    }
    // Returns the content's height.
    int cardsPass(PageLayout& s, std::vector<std::pair<RECT, std::wstring>>& tp) {
        auto& pal = palette();
        int x = s.x, right = s.right, left = s.left, end = s.end, line = s.line, box = s.box, row = s.button,
            gap = ref(12);
        s.y += ref(2); // the cards start ref(8) down and end at the last card's gap
        auto say = [&](const std::wstring& t, int from, int to, COLORREF color, HFONT f = nullptr) {
            if (!t.empty()) s.y = s.paragraph(t, from, s.y, to, color, f ? f : fonts.font);
        };
        auto extent = [&](HFONT f, const std::wstring& t) {
            auto [known, fresh] = widths.try_emplace({f, t});
            if (fresh) known->second = s.extent(f, t);
            return known->second;
        };
        auto content = [&] { return s.finish() - ref(6); };
        // Banners: what keeps the tab from working as it should.
        std::vector<std::wstring> banners;
        if (!openError.empty()) banners.push_back(openError);
        if (tuner && !tuner->refusal.empty()) banners.push_back(L"Apply is off on this GPU: " + w(tuner->refusal));
        bool issue = !banners.empty();
        if (onGpu && !onGpu->undescribed.empty()) {
            std::vector<std::wstring> ids;
            for (int i : onGpu->undescribed) ids.push_back(std::to_wstring(i));
            banners.push_back(L"The driver has changes in equation " + join(ids, L", ") +
                              L" that this tab cannot show. \"Restore stock\" removes them.");
        }
        if (!readError.empty()) banners.push_back(readError);
        if (!invalid.empty()) banners.push_back(L"These settings cannot be written: " + invalid);
        for (size_t i = 0; i < banners.size(); ++i) {
            s.frames.push_back({{x, s.y, right, 0}, L"", true});
            size_t frame = s.frames.size() - 1;
            s.y += ref(10);
            say(banners[i], left, end, pal.text);
            if (i == 0 && issue) s.link(issueLink, left, ref(4), end - left);
            s.y += ref(10);
            s.frames[frame].rect.bottom = s.y;
            s.y += gap;
        }
        if (!issue) s.put(issueLink, 0, 0, 0, 0, false);
        if (!tuner) {
            s.open(L"V/F Curve");
            say(job == Job::opening
                    ? L"Opening the tuner: reading the driver's equations and finding its clock curve. The first time on a GPU this takes a few seconds."
                    : L"The tuner is not open. Read (above) tries again.",
                left, end, pal.muted);
            s.close();
            return content();
        }
        auto& t = *tuner;
        // Current changes.
        {
            std::vector<std::wstring> ids, parts, removed;
            for (int i = 0; i < int(equations.size()); ++i)
                if (equations[i] != t.m.E[i] && !t.switches.contains(i)) ids.push_back(std::to_wstring(i));
            for (auto& r : t.rules)
                if (tune.rules.contains(r.id) && tune.rules.at(r.id)) {
                    auto n = w(r.title.substr(0, r.title.find(',')));
                    std::ranges::transform(n, n.begin(), towlower);
                    removed.push_back(n + L" removed");
                }
            bool sw = !t.switches.empty();
            size_t moved = std::ranges::count_if(offsets, [](double o) { return o != 0; });
            if (sw) parts.push_back(L"Slow curve disabled");
            if (!ids.empty()) parts.push_back(plural(ids.size(), L"equation") + L" changed");
            if (moved) parts.push_back(L"Afterburner Style OC at " + plural(moved, L"point"));
            auto summary = parts.empty() ? L"None" : join(parts, L" + ") + (sw && parts.size() == 1 ? L" only" : L"");
            if (!sw && !parts.empty()) summary[0] = towupper(summary[0]);
            if (!removed.empty()) summary += L" (" + join(removed, L", ") + L")";
            // Side by side with GPU Readback, both as high as the higher one.
            int top = s.y, mid = readback ? (x + right) / 2 : right + gap / 2, l = left, e = mid - gap / 2 - ref(15);
            s.open(L"Current Changes", x, mid - gap / 2);
            say(summary, l, e, pal.text);
            if (!ids.empty()) {
                std::vector<std::wstring> sws;
                for (auto& [i, fixed] : t.switches) sws.push_back(std::to_wstring(i));
                say(L"Equations " + join(ids, L", ") +
                        (sw ? L", and " + join(sws, L", ") + L" to disable the slow curve" : L"") + L".",
                    l, e, pal.muted);
            }
            s.close();
            if (readback) {
                int bottom = s.y;
                s.y = top;
                l = mid + gap / 2 + ref(15);
                s.open(L"GPU Readback", mid + gap / 2, right);
                say(readback->what, l, end, pal.text, fonts.bold);
                say(readback->verdict, l, end, readback->ok ? pal.ok : pal.error);
                say(readback->offsets, l, end, pal.error);
                say(readback->more, l, end, pal.muted);
                s.close();
                s.y = std::max(s.y, bottom);
                for (auto f : {s.frames.size() - 2, s.frames.size() - 1}) s.frames[f].rect.bottom = s.y - ref(12);
            }
        }
        // Plot: what the chart shows.
        {
            s.open(L"Plot");
            int widest = 0;
            for (auto m : modes) widest = std::max(widest, extent(fonts.font, m));
            int at = left, top = s.y;
            // A drop-down list's window includes its list; it is centered by its closed height.
            auto flow = [&](HWND c, int width, int height, int list = 0) {
                if (at > left && at + width > end) {
                    at = left;
                    top += row + ref(6);
                }
                s.put(c, at, top + (row - height) / 2, width, height + list, true, list != 0);
                at += width + ref(16);
            };
            flow(modeBox, widest + ref(60), box, ref(200));
            flow(referenceButton, extent(fonts.font, windowText(referenceButton)) + ref(30), row);
            auto name = sheetName.empty() ? L"No reference curve" : sheetName;
            int nameWidth = extent(fonts.font, name);
            if (at > left && at + nameWidth > end) {
                at = left;
                top += row + ref(6);
            }
            s.texts.push_back({{at, top, end, top + row},
                               name,
                               pal.muted,
                               fonts.font,
                               DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS});
            s.y = top + row + ref(10);
            // Show: per row and column. A click on a row's name turns the whole row on or off.
            bool band = std::ranges::any_of(std::array{0, 1, 2, 3, 4, 5, 6, 7}, [&](int c) { return have(c); });
            const std::wstring rows[]{L"○ Stock curve (NVIDIA's)", L"● New curve (slow curve disabled + your changes)",
                                      L"+ Reference curve (" + sheetName + L")"};
            int cell = std::max(extent(fonts.bold, L"80 °C") + ref(18), ref(60)), labels = 0;
            for (auto& r : rows) labels = std::max(labels, extent(fonts.font, r) + ref(16));
            labels = std::min(labels, std::max(ref(150), end - left - 8 * cell));
            s.texts.push_back(
                {{left, s.y, left + labels, s.y + line}, L"Show", pal.text, fonts.bold, DT_SINGLELINE | DT_NOPREFIX});
            for (int c = 0; c < 8; ++c)
                s.texts.push_back({{left + labels + c * cell, s.y, left + labels + (c + 1) * cell, s.y + line},
                                   std::to_wstring(columns[c].celsius) + L" °C",
                                   hue(c),
                                   fonts.bold,
                                   DT_SINGLELINE | DT_CENTER | DT_NOPREFIX});
            s.y += line + ref(4);
            for (int r = 0; r < 3; ++r) {
                bool shownRow = r < 2 || band;
                if (!shownRow) {
                    showLabels[r] = {};
                    for (auto b : shows[r]) s.put(b, 0, 0, 0, 0, false);
                    continue;
                }
                RECT label{left, s.y, left + labels - ref(8), s.y};
                auto [known, fresh] = heights.try_emplace({rows[r], label.right - label.left});
                if (fresh) {
                    RECT measured{0, 0, label.right - label.left, 0};
                    SelectObject(s.dc, fonts.font);
                    DrawTextW(s.dc, rows[r].c_str(), -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                    known->second = measured.bottom;
                }
                int h = std::max(known->second, box) + ref(6);
                label.bottom = label.top + h;
                s.texts.push_back({label, rows[r], pal.text, fonts.font});
                showLabels[r] = label;
                tp.push_back({label, L"Click to turn the whole row on or off."});
                for (int c = 0; c < 8; ++c) {
                    bool on = r < 2 || have(c);
                    int size = scale(18);
                    s.put(shows[r][c], left + labels + c * cell + (cell - size) / 2, s.y, size, box, on);
                }
                s.y += h;
            }
            std::wstring note = L"Each column is one 10 °C temperature band.";
            for (auto& c : columns)
                if (c.calc != c.celsius)
                    note += L" The " + std::to_wstring(c.celsius) + L" °C column is calculated at " + number(c.calc) +
                            L" °C" +
                            (*c.band ? L": the stock curve measured at " + w(c.band).replace(2, 1, L"–") +
                                           L" °C equals that."
                                     : L".");
            note +=
                L" Wheel zooms the chart (Shift: voltage only, Alt: clock only), dragging moves it, hovering shows a point. Click points to select them (Shift: a range, Ctrl: add one); the up and down arrows then move their offsets.";
            s.y += ref(4);
            say(note, left, end, pal.muted);
            s.close();
        }
        // The cards: as many columns as fit, at least about 400 pixels wide each.
        {
            int minimum = scale(400), count = std::max(1, (right - x + gap) / (minimum + gap)),
                width = (right - x - (count - 1) * gap) / count;
            std::vector<std::function<void(int from, int to)>> items;
            auto sliderRows = [&](std::vector<Slider*> list, int from, int to) {
                int labelWidth = 0, unitWidth = 0;
                for (auto sl : list) {
                    labelWidth = std::max(labelWidth, extent(fonts.bold, sl->name) + ref(12));
                    unitWidth = std::max(unitWidth, extent(fonts.font, sl->unit) + ref(6));
                }
                for (auto& l : limits) labelWidth = std::max(labelWidth, extent(fonts.bold, l.name) + ref(12));
                int numberWidth = extent(fonts.font, L"-0000.00") + ref(20);
                for (auto sl : list) {
                    double v = sl->value(tune);
                    int trackLeft = from + labelWidth, trackRight = to - unitWidth - numberWidth - ref(16);
                    sl->label = {from, s.y, trackLeft, s.y + row};
                    s.texts.push_back({sl->label, sl->name, v != 0 ? pal.accent : pal.text, fonts.bold,
                                       DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS});
                    tp.push_back({sl->label, sl->hint});
                    s.put(sl->track, trackLeft, s.y, trackRight - trackLeft, row);
                    s.put(sl->edit, trackRight + ref(8), s.y + (row - box) / 2, numberWidth, box);
                    s.texts.push_back({{trackRight + ref(16) + numberWidth, s.y, to, s.y + row},
                                       sl->unit,
                                       pal.muted,
                                       fonts.font,
                                       DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX});
                    s.y += row;
                    if (!sl->card.empty()) {
                        // Under the track, from the thumb's leftmost to its rightmost center; asked of the track
                        // once it has the width.
                        if (s.place && sl->barWidth != trackRight - trackLeft) {
                            RECT channel{}, thumb{};
                            SendMessageW(sl->track, TBM_GETCHANNELRECT, 0, LPARAM(&channel));
                            SendMessageW(sl->track, TBM_GETTHUMBRECT, 0, LPARAM(&thumb));
                            int half = (thumb.right - thumb.left) / 2;
                            sl->barLeft = channel.left + half;
                            sl->barRight = channel.right - half;
                            sl->barWidth = trackRight - trackLeft;
                        }
                        sl->strip = {trackLeft + sl->barLeft, s.y, trackLeft + sl->barRight, s.y + scale(5)};
                        tp.push_back(
                            {sl->strip,
                             L"Grey: moving the slider here changes nothing at the plotted temperatures (another line is lower, or the curve is at the floor, at the ceiling or held flat)."});
                        s.y += scale(5);
                    }
                    s.y += ref(6);
                }
                return std::pair(labelWidth, numberWidth + unitWidth);
            };
            items.push_back([&](int from, int to) {
                s.open(L"Global Offset", from, to);
                int l = from + ref(15), r = to - ref(15);
                auto [labelWidth, tail] = sliderRows({&sliders[0], &sliders[1]}, l, r);
                int numberWidth = extent(fonts.font, L"-0000.00") + ref(20), unitWidth = tail - numberWidth;
                for (auto& lim : limits) {
                    bool set = (tune.*lim.value).has_value();
                    int trackRight = r - unitWidth - numberWidth - ref(16);
                    s.texts.push_back({{l, s.y, l + labelWidth, s.y + row},
                                       lim.name,
                                       set ? pal.accent : pal.text,
                                       fonts.bold,
                                       DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX});
                    s.texts.push_back({{l + labelWidth, s.y, trackRight, s.y + row},
                                       lim.hint,
                                       pal.muted,
                                       fonts.font,
                                       DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX});
                    s.put(lim.edit, trackRight + ref(8), s.y + (row - box) / 2, numberWidth, box);
                    s.texts.push_back({{trackRight + ref(16) + numberWidth, s.y, r, s.y + row},
                                       L"MHz",
                                       pal.muted,
                                       fonts.font,
                                       DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX});
                    s.y += row + ref(6);
                }
                s.y += ref(4);
                say(w(t.switch_text), l, r, pal.muted);
                s.close();
            });
            if (!t.rules.empty())
                items.push_back([&](int from, int to) {
                    s.open(L"NVIDIA's Temperature Corrections", from, to);
                    int l = from + ref(15), r = to - ref(15);
                    for (size_t i = 0; i < t.rules.size(); ++i) {
                        if (i) s.y += ref(10);
                        say(w(t.rules[i].title), l, r, pal.text, fonts.bold);
                        s.y += ref(2);
                        say(w(t.rules[i].text), l, r, pal.muted);
                        s.check(ruleChecks[i], l, true, fonts.font);
                    }
                    s.close();
                });
            items.push_back([&](int from, int to) {
                s.open(L"The Lines, from Low to High Voltage", from, to);
                say(L"The clock follows the lowest line (thin lines on the chart).\nGrey bar under a slider: moving it there changes nothing.\nTemp sliders: MHz per 10 °C warmer.",
                    from + ref(15), to - ref(15), pal.muted);
                s.close();
            });
            auto calc = shownNew();
            auto& win = tuned(calc[0]).win;
            for (auto& section : t.sections)
                items.push_back([&, sec = &section](int from, int to) {
                    int top = s.y;
                    s.open(w(sec->name), from, to);
                    cards.push_back({s.frames.size() - 1, sec->id});
                    // Where its lines set the new curve now, at the first plotted temperature.
                    std::vector<int> mine;
                    std::vector<std::pair<std::wstring, std::vector<int>>> others;
                    for (int p = 0; p < t.points(); ++p) {
                        if (t.mv(p) < sec->lo || t.mv(p) > sec->hi) continue;
                        if (std::ranges::find(sec->heads, win[p]) != sec->heads.end()) {
                            mine.push_back(p);
                            continue;
                        }
                        auto card = std::ranges::find(lineCards, win[p], [](auto& line) { return line.first; });
                        auto k = win[p] == Calc::held_flat ? L"held flat"
                                 : win[p] == Calc::floor   ? L"floor"
                                 : win[p] == Calc::ceiling ? L"ceiling"
                                 : card != lineCards.end() ? card->second
                                                           : L"another equation";
                        auto it = std::ranges::find_if(others, [&](auto& o) { return o.first == k; });
                        if (it == others.end()) it = others.insert(others.end(), {k, {}});
                        it->second.push_back(p);
                    }
                    std::vector<std::wstring> by;
                    for (auto& [k, ps] : others) by.push_back(k + L": " + runs(ps));
                    int titleRight = from + ref(29) + extent(fonts.title, w(sec->name));
                    RECT title{from + ref(15), top + ref(10), titleRight, top + ref(10) + s.titleHeight};
                    std::vector<std::wstring> heads;
                    for (int h : sec->heads) heads.push_back(std::to_wstring(h));
                    tp.push_back({title, L"Left end " + number(sec->lo) + L" mV, right end " + number(sec->hi) +
                                             L" mV. Equation" + (heads.size() > 1 ? L"s " : L" ") + join(heads, L", ") +
                                             L". " + w(sec->note)});
                    RECT state{titleRight + ref(12), title.top, to - ref(15), title.bottom};
                    s.texts.push_back({state, mine.empty() ? L"not active" : L"active at " + runs(mine),
                                       mine.empty() ? pal.error : pal.muted, fonts.font,
                                       DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS});
                    tp.push_back(
                        {state, L"At " + std::to_wstring(columns[calc[0]].celsius) + L" °C. " + join(by, L"; ")});
                    std::vector<Slider*> list;
                    for (auto& sl : sliders)
                        if (sl.card == sec->id) list.push_back(&sl);
                    sliderRows(list, from + ref(15), to - ref(15));
                    s.close();
                });
            // Each card closes with the gap below it; a row of cards ends at its lowest.
            int bottom = s.y, top = s.y;
            for (size_t i = 0; i < items.size(); ++i) {
                int column = int(i % count), from = x + column * (width + gap);
                if (!column) top = bottom + (i ? gap : 0);
                s.y = top;
                items[i](from, from + width);
                bottom = std::max(bottom, s.y - gap);
            }
            s.y = bottom + ref(8);
            say(L"Double-click a slider to set it to 0. Every number behind the sliders is in the tables below.", x,
                right, pal.muted);
            s.y += gap;
        }
        // Every number, and how the lines are combined.
        for (auto [title, linkControl, table, isOpen] :
             {std::tuple(L"Every Number, by Line", numbersLink, numbers.get(), numbersOpen),
              {L"Structure: How the Lines Are Combined", structureLink, structure.get(), structureOpen}}) {
            int top = s.y;
            s.open(title);
            auto& size = linkSizes[linkControl];
            if (!size.cy) SendMessageW(linkControl, LM_GETIDEALSIZE, ref(200), LPARAM(&size));
            s.put(linkControl, right - ref(15) - size.cx, top + ref(10) + (s.titleHeight - size.cy) / 2, size.cx,
                  size.cy);
            if (isOpen) {
                say(table == numbers.get()
                        ? w(t.formnote) +
                              L" Each line is its main formula plus the rows under it. Highlighted cells differ from stock. The sliders change the constant, V, V², T, T² and V·T terms of the main formulas."
                        : L"Tests on voltage and temperature and the lowest-of choices above the lines, for this chip. Equation " +
                              std::to_wstring(t.meta.root) +
                              L" is the result: its min and max are the lowest and highest clock the curve can give. " +
                              w(t.switch_detail),
                    left, end, pal.muted);
                s.y += ref(6);
                int h = table->height(end - left);
                s.put(table->window(), left, s.y, end - left, h);
                s.y += h;
            } else
                s.put(table->window(), 0, 0, 0, 0, false);
            s.close();
        }
        return content();
    }
    void scrollTo(int target) {
        if (page.scrollTo(target)) layout(false);
    }
    void style(const PageFonts& f) {
        fonts = f;
        page.texts.clear(); // measured with the old fonts, whose handles may be reused
        widths.clear();
        heights.clear();
        linkSizes.clear();
        for (auto& s : sliders) s.barWidth = -1;
        auto font = [](HWND child, LPARAM value) -> BOOL {
            SendMessageW(child, WM_SETFONT, WPARAM(value), TRUE);
            return TRUE;
        };
        EnumChildWindows(root, font, LPARAM(fonts.font));
        if (numbers) numbers->style(fonts.font, fonts.bold, fonts.dpi);
        if (structure) structure->style(fonts.font, fonts.bold, fonts.dpi);
        chart->dpi(fonts.dpi);
        systemTheme(panel, L"DarkMode_Explorer");
        for (auto& n : {numbers.get(), structure.get()})
            if (n) systemTheme(n->window(), L"DarkMode_Explorer");
        if (modeBox) {
            systemTheme(modeBox, L"DarkMode_CFD");
            COMBOBOXINFO info{sizeof info};
            if (GetComboBoxInfo(modeBox, &info)) systemTheme(info.hwndList, L"DarkMode_Explorer");
        }
        status();
        layoutRoot();
        layout(true);
        RedrawWindow(root, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        plot();
    }

    // ---- painting ----
    void paintPanel(HDC target, const RECT& area) {
        Buffer buffer(target, area);
        auto dc = buffer.dc;
        auto& p = palette();
        page.paint(dc, area, fonts);
        COLORREF moving = mix(p.card, p.accent, 0.7), idle = mix(p.card, p.border, 0.8);
        for (auto& s : sliders) {
            RECT visible{};
            if (s.card.empty() || !IntersectRect(&visible, &s.strip, &area)) continue;
            int width = s.strip.right - s.strip.left;
            for (int j = 0; j < zoneSteps; ++j) {
                RECT seg{s.strip.left + width * j / zoneSteps, s.strip.top, s.strip.left + width * (j + 1) / zoneSteps,
                         s.strip.bottom};
                COLORREF c = !s.zone.empty() && s.zone[j] ? moving : idle;
                FillRect(dc, &seg, brush(s.stale ? mix(p.card, c, 0.4) : c));
            }
        }
    }
    void paintRoot(HDC target) {
        RECT rc{};
        GetClientRect(root, &rc);
        Buffer buffer(target, rc);
        FillRect(buffer.dc, &rc, brush(palette().window));
        SelectObject(buffer.dc, fonts.bold);
        SetTextColor(buffer.dc, statusColor);
        RECT r = statusRect;
        DrawTextW(buffer.dc, statusText.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    // ---- messages ----
    LRESULT command(HWND from, int code) {
        if (from == applyButton && code == BN_CLICKED) apply();
        if (from == restoreButton && code == BN_CLICKED) restore();
        if (from == resetButton && code == BN_CLICKED) reset();
        if (from == importButton && code == BN_CLICKED) importTune();
        if (from == exportButton && code == BN_CLICKED) exportTune();
        if (!tuner) return 0;
        if (from == referenceButton && code == BN_CLICKED) reference();
        if (from == modeBox && code == CBN_SELCHANGE) {
            view.mode = int(SendMessageW(modeBox, CB_GETCURSEL, 0, 0));
            chart->reset_view();
            shown();
        }
        if (code == EN_KILLFOCUS) commit(from);
        if (code != BN_CLICKED) return 0;
        bool on = Button_GetCheck(from) == BST_CHECKED;
        if (from == linesCheck) {
            view.lines = on;
            plot();
        }
        for (size_t i = 0; i < ruleChecks.size(); ++i)
            if (from == ruleChecks[i]) {
                tune.rules[tuner->rules[i].id] = on;
                changed();
            }
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 8; ++c)
                if (from == shows[r][c]) {
                    (r == 0 ? view.stock : r == 1 ? view.fresh : view.band)[c] = on;
                    shown();
                }
        return 0;
    }
    void toggleRow(int r) {
        auto& map = r == 0 ? view.stock : r == 1 ? view.fresh : view.band;
        std::vector<int> cs;
        for (int c = 0; c < 8; ++c)
            if (r < 2 || have(c)) cs.push_back(c);
        bool on = !std::ranges::all_of(cs, [&](int c) { return map[c]; });
        for (int c : cs) map[c] = on;
        sync();
        shown();
    }
    LRESULT notify(NMHDR* n) {
        if (n->code == NM_CUSTOMDRAW) {
            auto& draw = *reinterpret_cast<NMCUSTOMDRAW*>(n);
            wchar_t name[32]{};
            GetClassNameW(n->hwndFrom, name, 32);
            if (!wcscmp(name, WC_LINK)) return drawLink(draw, palette().text);
            if (!wcscmp(name, L"Button") || !wcscmp(name, TRACKBAR_CLASSW))
                return drawControl(draw, n->hwndFrom == applyButton);
            return CDRF_DODEFAULT;
        }
        if (n->code == NM_CLICK || n->code == NM_RETURN) {
            if (n->hwndFrom == issueLink && host.report)
                host.report(openError.empty() && tuner ? w(tuner->refusal) : openError);
            if (n->hwndFrom == numbersLink || n->hwndFrom == structureLink) {
                bool& open = n->hwndFrom == numbersLink ? numbersOpen : structureOpen;
                open = !open;
                SetWindowTextW(n->hwndFrom, open ? L"<a>Hide</a>" : L"<a>Show</a>");
                linkSizes.erase(n->hwndFrom);
                if (open) tables();
                layout(true);
                InvalidateRect(panel, nullptr, FALSE);
            }
        }
        if (n->code == TTN_GETDISPINFOW) {
            auto& d = *reinterpret_cast<NMTTDISPINFOW*>(n);
            tipText = n->idFrom && n->idFrom <= tips.size() ? tips[n->idFrom - 1].second : L"";
            d.lpszText = tipText.data();
        }
        return 0;
    }
    LRESULT colors(UINT message, HDC dc, HWND child) const {
        return controlColors(message, dc, GetParent(child) == panel ? page.backdrop(child) : palette().window);
    }
    LRESULT rootMessage(UINT message, WPARAM wp, LPARAM lp) {
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            auto dc = BeginPaint(root, &ps);
            paintRoot(dc);
            EndPaint(root, &ps);
            return 0;
        }
        case WM_SIZE: layoutRoot(); return 0;
        case WM_COMMAND: return command(reinterpret_cast<HWND>(lp), HIWORD(wp));
        case WM_NOTIFY: return notify(reinterpret_cast<NMHDR*>(lp));
        case WM_MOUSEWHEEL: SendMessageW(panel, message, wp, lp); return 0;
        case WM_TIMER:
            if (wp == readTimer) {
                KillTimer(root, readTimer);
                submit(VfRead{}, Job::verifying);
            }
            return 0;
        }
        if (message >= WM_CTLCOLORMSGBOX && message <= WM_CTLCOLORSTATIC)
            return colors(message, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp));
        return DefWindowProcW(root, message, wp, lp);
    }
    LRESULT panelMessage(UINT message, WPARAM wp, LPARAM lp) {
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            // A slider step leaves a few small parts to repaint, often far apart.
            paintParts(panel, [&](HDC dc, const RECT& area) { paintPanel(dc, area); });
            if (zones.pending && !zones.running) {
                zones.running = true;
                SetTimer(panel, zoneTimer, 250, nullptr);
            }
            return 0;
        }
        case WM_PRINTCLIENT: page.printClient(reinterpret_cast<HDC>(wp)); return 0;
        case WM_SIZE:
            layout(true);
            InvalidateRect(panel, nullptr, FALSE);
            return 0;
        case WM_COMMAND: return command(reinterpret_cast<HWND>(lp), HIWORD(wp));
        case WM_NOTIFY: return notify(reinterpret_cast<NMHDR*>(lp));
        case WM_HSCROLL:
            for (auto& s : sliders)
                if (s.track == reinterpret_cast<HWND>(lp)) {
                    double v = double(SendMessageW(s.track, TBM_GETPOS, 0, 0)) * s.step;
                    if (std::lround(v / s.step) != std::lround(s.value(tune) / s.step)) touch(s, v);
                }
            return 0;
        case WM_VSCROLL:
        case WM_MOUSEWHEEL: scrollTo(page.target(message, wp, fonts.dpi)); return 0;
        case WM_TIMER:
            if (wp == zoneTimer) zoneTick();
            return 0;
        case WM_SETCURSOR: {
            POINT at{};
            GetCursorPos(&at);
            ScreenToClient(panel, &at);
            // The card under the mouse draws its lines on the chart in front.
            if (tuner) {
                std::string card;
                for (auto& [frame, id] : cards)
                    if (PtInRect(&page.frames[frame].rect, at)) card = id;
                if (!card.empty() && card != hot) {
                    hot = card;
                    plot();
                }
            }
            if (reinterpret_cast<HWND>(wp) == panel && LOWORD(lp) == HTCLIENT)
                for (auto& r : showLabels)
                    if (PtInRect(&r, at)) {
                        SetCursor(LoadCursorW(nullptr, IDC_HAND));
                        return TRUE;
                    }
            break;
        }
        case WM_LBUTTONDOWN: {
            POINT at{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            SetFocus(panel);
            for (int r = 0; r < 3; ++r)
                if (PtInRect(&showLabels[r], at)) toggleRow(r);
            return 0;
        }
        }
        if (message >= WM_CTLCOLORMSGBOX && message <= WM_CTLCOLORSTATIC)
            return colors(message, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp));
        return DefWindowProcW(panel, message, wp, lp);
    }
    template <bool panelWindow> static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            (panelWindow ? self->panel : self->root) = window;
            if (panelWindow) self->page.window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (message == WM_NCDESTROY && self) {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            (panelWindow ? self->panel : self->root) = nullptr;
            return DefWindowProcW(window, message, wp, lp);
        }
        if (!self) return DefWindowProcW(window, message, wp, lp);
        try {
            return panelWindow ? self->panelMessage(message, wp, lp) : self->rootMessage(message, wp, lp);
        } catch (const std::exception& e) {
            self->error(w(e.what()));
            return 0;
        }
    }
    // Double-clicking a slider sets it to 0; the wheel scrolls the cards instead of moving the slider under it.
    static LRESULT CALLBACK trackProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto self = reinterpret_cast<Impl*>(data);
        if (message == WM_MOUSEWHEEL) return SendMessageW(self->panel, message, wp, lp);
        bool twice =
            message == WM_LBUTTONDBLCLK || (message == WM_LBUTTONDOWN && self->downTrack == window &&
                                            GetMessageTime() - LONG(self->downTime) <= LONG(GetDoubleClickTime()));
        if (message == WM_LBUTTONDOWN) {
            self->downTrack = window;
            self->downTime = DWORD(GetMessageTime());
        }
        if (twice) {
            self->downTrack = nullptr;
            try {
                for (auto& s : self->sliders)
                    if (s.track == window) self->touch(s, 0);
            } catch (const std::exception& e) {
                self->error(w(e.what()));
            }
            return 0;
        }
        return DefSubclassProc(window, message, wp, lp);
    }
    // Enter commits a typed number, as leaving the box does.
    static LRESULT CALLBACK editProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto self = reinterpret_cast<Impl*>(data);
        if (message == WM_GETDLGCODE) {
            auto m = reinterpret_cast<MSG*>(lp);
            return DefSubclassProc(window, message, wp, lp) |
                   (m && m->message == WM_KEYDOWN && m->wParam == VK_RETURN ? DLGC_WANTMESSAGE : 0);
        }
        if (message == WM_CHAR && wp == VK_RETURN) return 0;
        if (message == WM_KEYDOWN && wp == VK_RETURN) {
            try {
                self->commit(window);
            } catch (const std::exception& e) {
                self->error(w(e.what()));
            }
            return 0;
        }
        return DefSubclassProc(window, message, wp, lp);
    }
};

VfTab::VfTab(HINSTANCE instance, HWND parent, VfHost host)
    : impl_(std::make_unique<Impl>(instance, parent, std::move(host))) {}
VfTab::~VfTab() = default;
HWND VfTab::window() const { return impl_->root; }
void VfTab::style(const PageFonts& fonts) { impl_->style(fonts); }
void VfTab::read() {
    auto& i = *impl_;
    if (i.job != Impl::Job::none) return;
    if (!i.tuner)
        i.submit(VfRead{.open = true}, Impl::Job::opening);
    else
        i.submit(VfRead{}, Impl::Job::reading, true);
    i.buttons();
    i.status();
    i.layout(true);
    InvalidateRect(i.panel, nullptr, FALSE);
}
void VfTab::loaded() {
    impl_->loaded = true;
    impl_->refresh();
    impl_->buttons();
    impl_->status();
}
void VfTab::delivered(std::uint64_t sequence, const ControlResult& result) { impl_->delivered(sequence, result); }
std::wstring VfTab::text() const {
    auto& r = impl_->readback;
    return impl_->statusText + (r ? L"\n" + r->what + L"\n" + r->verdict + L"\n" + r->offsets : L"");
}
}
