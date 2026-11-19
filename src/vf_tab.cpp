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
#include <charconv>
#include <chrono>
#include <cmath>

namespace mech {
using namespace nv::vf;
namespace {
constexpr wchar_t rootClass[] = L"MechControl.Native.VfTab", panelClass[] = L"MechControl.Native.VfPanel";
constexpr UINT_PTR settleTimer = 1, zoneTimer = 2;
constexpr int zoneSteps = 32, sixty = 5;                   // positions a zone tries; the 60 °C column
constexpr int hues[]{275, 225, 190, 140, 75, 38, 15, 350}; // one color per column, cold to hot
constexpr const wchar_t* modes[]{L"Clock (MHz)", L"Difference from stock", L"Difference from the reference curve"};
constexpr const char* modeNames[]{"abs", "diff", "base"};
using Clock = std::chrono::steady_clock;

std::wstring w(const std::string& s) { return config::wide(s); }
// The shortest text that reads back as the same value, as the page prints numbers.
std::wstring number(double v) {
    char b[32];
    auto end = std::to_chars(b, b + sizeof b, v).ptr;
    return std::wstring(b, end);
}
std::wstring sign(double v) { return (v > 0 ? L"+" : L"") + number(v); }
std::wstring plural(size_t n, const std::wstring& one) {
    return std::to_wstring(n) + L" " + one + (n == 1 ? L"" : L"s");
}
std::wstring join(const std::vector<std::wstring>& parts, const wchar_t* separator) {
    std::wstring s;
    for (auto& p : parts) s += (s.empty() ? L"" : separator) + p;
    return s;
}
double f32(double v) { return double(float(v)); }
// Local time as the page saved it: 2026-11-19T18:09:18.
std::string now() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char b[32];
    snprintf(b, sizeof b, "%04u-%02u-%02uT%02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}
// An equation number as the page's tables show it: nine significant digits of the 32-bit value, an exponent below
// 0.001 (vfe-ui.html fmt).
std::wstring fmt(std::optional<double> v) {
    if (!v) return L"";
    char b[40];
    snprintf(b, sizeof b, "%.9g", f32(*v));
    double x = std::strtod(b, nullptr);
    auto end = x != 0 && std::abs(x) < 1e-3 ? std::to_chars(b, b + sizeof b, x, std::chars_format::scientific).ptr
                                            : std::to_chars(b, b + sizeof b, x).ptr;
    std::string s(b, end);
    if (auto e = s.find("e-0"); e != std::string::npos) s.erase(e + 2, 1);
    return w(s);
}
COLORREF mix(COLORREF a, COLORREF b, double x) {
    auto c = [x](int p, int q) { return int(p + (q - p) * x + 0.5); };
    return RGB(c(GetRValue(a), GetRValue(b)), c(GetGValue(a), GetGValue(b)), c(GetBValue(a), GetBValue(b)));
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
bool truthy(const config::Json& v) {
    return v.is_boolean() ? v.get<bool>() : v.is_number() ? v.get<double>() != 0 : false;
}
std::wstring windowText(HWND window) {
    int n = GetWindowTextLengthW(window);
    std::wstring s(n + 1, L'\0');
    s.resize(GetWindowTextW(window, s.data(), n + 1));
    return s;
}
std::optional<double> parse(const std::wstring& s) {
    auto t = config::utf8(s);
    t.erase(0, t.find_first_not_of(" \t"));
    t.erase(t.find_last_not_of(" \t") + 1);
    double v = 0;
    auto [end, error] = std::from_chars(t.data(), t.data() + t.size(), v);
    if (t.empty() || error != std::errc{} || end != t.data() + t.size() || !std::isfinite(v)) return {};
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
    };
    struct Limit {
        HWND edit = nullptr;
        std::string field;
        std::wstring name, hint;
    };
    struct RuleChecks {
        HWND preview = nullptr, remove = nullptr;
    };
    struct CellRef {
        int equ;
        std::string field;
        char kind; // 'n' number, 'f' comparison, 'm' choice, 'a' fixed amount (both limits)
    };
    struct Text {
        RECT r;
        std::wstring s;
        COLORREF color;
        HFONT font;
        UINT format = DT_WORDBREAK | DT_NOPREFIX;
    };
    struct Frame {
        RECT r;
        std::wstring title;
        COLORREF fill, edge;
        std::string card; // a section's id
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
    enum class Job { none, opening, reading, writing, settling };

    HINSTANCE instance;
    VfHost host;
    VfFonts fonts{};
    HWND root = nullptr, panel = nullptr, tooltip = nullptr;
    HWND applyButton = nullptr, restoreButton = nullptr, resetButton = nullptr, importButton = nullptr,
         exportButton = nullptr;
    HWND issueLink = nullptr, modeBox = nullptr, linesCheck = nullptr, referenceButton = nullptr, numbersLink = nullptr,
         structureLink = nullptr;
    std::unique_ptr<VfChart> chart;
    std::unique_ptr<VfNumbers> numbers, structure;

    std::shared_ptr<const Tuner> tuner;
    std::shared_ptr<const VfReading> reading;
    Tune tune;
    Fields fields;
    Equations equations;
    std::wstring invalid; // why the tune cannot be written
    std::vector<double> offsets;
    std::array<std::optional<Calc>, 8> stockCalc, tunedCalc;
    View view;
    std::map<std::string, std::vector<double>> sheet;
    std::wstring sheetName;
    std::optional<Fields> liveFields;
    std::vector<int> liveOffsets; // kHz
    std::string hot;              // the card the user works on

    Job job = Job::opening;
    std::uint64_t openSeq = 0, readSeq = 0, applySeq = 0, settleSeq = 0;
    bool restoring = false;
    std::optional<Settle> settle;
    Clock::time_point settleStart;
    std::wstring settleWhat;
    std::wstring openError, readError;
    std::vector<int> undescribed;
    std::optional<Readback> readback;

    std::vector<Slider> sliders;
    std::vector<Limit> limits;
    std::vector<RuleChecks> ruleChecks;
    std::array<std::array<HWND, 8>, 3> shows{}; // stock, new, reference by column
    std::array<RECT, 3> showLabels{};
    std::vector<CellRef> cells;
    bool numbersOpen = false, structureOpen = false;

    std::vector<Frame> frames;
    std::vector<Text> texts;
    std::vector<std::pair<RECT, std::wstring>> tips; // painted parts with a tooltip
    std::map<HWND, std::wstring> controlTips;
    std::wstring tipText;
    size_t tools = 0;
    int scroll = 0;
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
        tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0,
                                  0, 0, 0, root, nullptr, instance, nullptr);
        issueLink = make(panel, WC_LINK, L"To get this GPU supported, <a>open an issue</a> with mechcontrol.log.",
                         WS_TABSTOP | LWS_TRANSPARENT | LWS_NOPREFIX);
        ShowWindow(issueLink, SW_HIDE);
        buttons();
        status();
        openSeq = host.submit(VfRead{.open = true}, false);
    }
    ~Impl() {
        if (tooltip) DestroyWindow(tooltip);
        chart.reset();
        if (root) DestroyWindow(root);
    }

    int ref(int n) const { return MulDiv(n, fonts.dpi ? fonts.dpi : 96, 144); }
    int scale(int n) const { return MulDiv(n, fonts.dpi ? fonts.dpi : 96, 96); }
    HWND owner() const { return GetAncestor(root, GA_ROOT); }
    HWND make(HWND parent, const wchar_t* type, const std::wstring& title, DWORD style) {
        auto child = CreateWindowExW(0, type, title.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent, nullptr,
                                     instance, nullptr);
        if (!child) throw std::runtime_error("Cannot create native control");
        if (!wcscmp(type, TRACKBAR_CLASSW) || !wcscmp(type, WC_LINK)) paintWhole(child);
        if (fonts.font) SendMessageW(child, WM_SETFONT, WPARAM(fonts.font), FALSE);
        return child;
    }
    void addTip(HWND child, const std::wstring& s) {
        controlTips[child] = s;
        TOOLINFOW t{sizeof t, TTF_IDISHWND | TTF_SUBCLASS, panel, UINT_PTR(child)};
        t.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(tooltip, TTM_ADDTOOLW, 0, LPARAM(&t));
    }
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
        tune = t.tune(config::Json::object());
        view = {};
        view.stock[sixty] = view.fresh[sixty] = view.fresh[sixty + 1] = view.band[sixty] = true;
        modeBox = make(panel, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL);
        linesCheck = make(panel, L"BUTTON", L"Draw the lines behind the new curve", WS_TABSTOP | BS_AUTOCHECKBOX);
        referenceButton = make(panel, L"BUTTON", L"Reference CSV…", WS_TABSTOP | BS_PUSHBUTTON);
        addTip(
            referenceButton,
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
            addTip(s.track, s.hint);
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
        for (auto [field, name, hint] :
             {std::tuple("lo", L"Floor", L"lowest clock"), {"hi", L"Ceiling", L"highest clock"}}) {
            Limit l{make(panel, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_RIGHT), field, name, hint};
            styleFrame(l.edit);
            SetWindowSubclass(l.edit, editProcedure, 1, DWORD_PTR(this));
            addTip(l.edit, std::wstring(L"The ") + hint + L" the curve can give (equation " +
                               std::to_wstring(t.meta.root) + L"); empty: no limit.");
            limits.push_back(l);
        }
        for (auto& r : t.rules) {
            RuleChecks c;
            std::vector<std::wstring> equs;
            for (int i : r.equs) equs.push_back(std::to_wstring(i));
            if (r.kind == "diff") {
                c.preview =
                    make(panel, L"BUTTON", L"Show the corrected curve (plot only)", WS_TABSTOP | BS_AUTOCHECKBOX);
                addTip(c.preview, L"Draws every curve as if channel " + std::to_wstring(r.hot) +
                                      L" were hotter than channel " + std::to_wstring(r.cold) +
                                      L". Nothing is written.");
            }
            c.remove =
                make(panel, L"BUTTON", L"Remove the correction (written with Apply)", WS_TABSTOP | BS_AUTOCHECKBOX);
            addTip(c.remove, std::wstring(L"Apply sets equation") + (equs.size() > 1 ? L"s " : L" ") +
                                 join(equs, L", ") + L" to 0.");
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
        numbers->edited = structure->edited = [this](int cell, const std::wstring& s) { edited(cell, s); };
        recalc();
        tables();
        sync();
        style(fonts);
    }

    // ---- the calculation ----
    std::map<int, double> hotter() const {
        std::map<int, double> d;
        for (auto& r : tuner->rules)
            if (r.kind == "diff" && tune.preview.contains(r.id) && tune.preview.at(r.id)) d[r.hot] = 1;
        return d;
    }
    const Calc& stock(int c) {
        if (!stockCalc[c]) stockCalc[c] = tuner->calc(tuner->m.E, columns[c].calc, hotter());
        return *stockCalc[c];
    }
    const Calc& tuned(int c) {
        if (!tunedCalc[c]) tunedCalc[c] = tuner->calc(equations, columns[c].calc, hotter());
        return *tunedCalc[c];
    }
    // The new curve with the offsets.
    std::vector<double> newCurve(int c) {
        auto& t = tuned(c).clock;
        return std::ranges::any_of(offsets, [](double o) { return o != 0; })
                   ? with_offsets(t, offsets, tuner->meta.step)
                   : t;
    }
    bool have(int c) const {
        auto it = *columns[c].band ? sheet.find(columns[c].band) : sheet.end();
        return it != sheet.end() && std::ranges::any_of(it->second, [](double v) { return !std::isnan(v); });
    }
    void recalc() {
        try {
            fields = tuner->fields(tune);
            equations = tuner->edited(fields);
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
    std::wstring runs(const std::vector<int>& ps) const {
        std::vector<std::wstring> out;
        for (size_t k = 0; k < ps.size(); ++k) {
            size_t j = k;
            while (j + 1 < ps.size() && ps[j + 1] == ps[j] + 1) ++j;
            out.push_back(number(tuner->mv(ps[k])) + (j > k ? L"–" + number(tuner->mv(ps[j])) : L""));
            k = j;
        }
        return join(out, L", ") + L" mV";
    }

    // ---- after every change ----
    void changed() {
        recalc();
        sync();
        if (numbersOpen || structureOpen) refreshCells();
        shown();
        status();
        buttons();
    }
    // What is shown changed: the cards' statuses, the chart and the zones follow.
    void shown() {
        layout(true);
        InvalidateRect(panel, nullptr, FALSE);
        plot();
        scheduleZones();
    }
    void sync() {
        for (auto& s : sliders) {
            double v = s.value(tune);
            auto pos = LPARAM(std::lround(v / s.step));
            if (SendMessageW(s.track, TBM_GETPOS, 0, 0) != pos) SendMessageW(s.track, TBM_SETPOS, TRUE, pos);
            auto t = w(config::decimal_text(std::round(v * 100) / 100, 2, true));
            if (GetFocus() != s.edit && windowText(s.edit) != t) SetWindowTextW(s.edit, t.c_str());
        }
        for (auto& l : limits) {
            auto key = std::to_string(tuner->meta.root) + "." + l.field;
            auto m = tune.manual.find(key);
            auto v = m != tune.manual.end() ? m->second : tuner->stock_value(tuner->meta.root, l.field);
            auto t = v ? number(*v) : L"";
            if (GetFocus() != l.edit && windowText(l.edit) != t) SetWindowTextW(l.edit, t.c_str());
        }
        for (size_t i = 0; i < ruleChecks.size(); ++i) {
            auto& id = tuner->rules[i].id;
            auto on = [&](auto& map) { return map.contains(id) && map.at(id); };
            if (ruleChecks[i].preview) Button_SetCheck(ruleChecks[i].preview, on(tune.preview));
            Button_SetCheck(ruleChecks[i].remove, on(tune.rules));
        }
        for (int c = 0; c < 8; ++c) {
            Button_SetCheck(shows[0][c], view.stock[c]);
            Button_SetCheck(shows[1][c], view.fresh[c]);
            Button_SetCheck(shows[2][c], view.band[c]);
        }
        Button_SetCheck(linesCheck, view.lines);
        modeItems();
    }
    // The difference from the reference curve is offered once one is loaded.
    void modeItems() {
        int n = sheet.empty() ? 2 : 3;
        if (view.mode >= n) view.mode = 0;
        for (int i = int(SendMessageW(modeBox, CB_GETCOUNT, 0, 0)); i > n; --i)
            SendMessageW(modeBox, CB_DELETESTRING, i - 1, 0);
        for (int i = int(SendMessageW(modeBox, CB_GETCOUNT, 0, 0)); i < n; ++i)
            SendMessageW(modeBox, CB_ADDSTRING, 0, LPARAM(modes[i]));
        SendMessageW(modeBox, CB_SETCURSEL, view.mode, 0);
    }
    void touch(Slider& s, double v) {
        s.value(tune) = s.doubleStep ? std::round(v / (2 * tuner->meta.step)) * 2 * tuner->meta.step : v;
        if (!s.card.empty()) hot = s.card;
        changed();
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
                auto key = std::to_string(tuner->meta.root) + "." + l.field;
                auto v = parse(s);
                auto b = tuner->stock_value(tuner->meta.root, l.field);
                if (!v || (b && f32(*v) == *b))
                    tune.manual.erase(key);
                else
                    tune.manual[key] = f32(*v);
                changed();
                return;
            }
    }

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
                return sheet.at(columns[c].band);
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
            if (view.band[c] && have(c))
                p.series.push_back({name(L"reference ", c), hue(c), '+', series(sheet.at(columns[c].band), c)});
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
        p.axis = view.mode == 1   ? L"MHz above stock at the same temperature"
                 : view.mode == 2 ? L"MHz above the reference curve at the same temperature"
                                  : L"MHz";
        p.relative = view.mode != 0;
        for (auto& r : t.rules)
            if (r.kind == "diff" && tune.preview.contains(r.id) && tune.preview.at(r.id))
                p.notes.push_back(L"Shown as if channel " + std::to_wstring(r.hot) + L" is hotter than channel " +
                                  std::to_wstring(r.cold));
        p.notes.resize(std::min<size_t>(p.notes.size(), 3));
        chart->set(std::move(p));
    }

    // ---- the bars under the sliders ----
    // Blue where moving the slider changes the new curve at a plotted temperature, grey where it changes nothing
    // (another line is lower there, or the curve is at the floor, at the ceiling or held flat). Worked out a little
    // after the last change, a few positions at a time, the card in use first.
    void scheduleZones() {
        zones.order.clear();
        for (size_t i = 0; i < sliders.size(); ++i)
            if (!sliders[i].card.empty()) {
                sliders[i].stale = true;
                zones.order.push_back(i);
            }
        std::ranges::stable_sort(
            zones.order, [&](size_t a, size_t b) { return (sliders[a].card == hot) > (sliders[b].card == hot); });
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
            auto E = tuner->edited(tuner->fields(t));
            auto heat = hotter();
            for (int c : zones.columns) {
                double top = s.p0 ? tuned(c).raw[s.p0 - 1] : -INFINITY;
                for (int p = s.p0; p <= s.p1; ++p) {
                    top = std::max(top,
                                   Env(tuner->m, E, tuner->meta.uv[p], columns[c].calc, 0, heat).equ(tuner->meta.root));
                    out.push_back(to_step(top, tuner->meta.step));
                }
            }
        } catch (const std::exception&) {
            out.clear();
        }
        return out;
    }
    void zoneTick() {
        // Paused while the tab is hidden; painting it again resumes.
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
        } while (Clock::now() - start < std::chrono::milliseconds(8));
    }

    // ---- the tables of every number ----
    void tables() {
        auto& t = *tuner;
        cells.clear();
        auto cell = [&](int i, const std::string& f, char kind) {
            cells.push_back({i, f, kind});
            return VfItem{L"", int(cells.size()) - 1};
        };
        auto label = [](std::wstring s, bool muted = false) { return VfItem{std::move(s), -1, muted, true}; };
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
        // One row (vfe-ui.html rowFor); a test or choice spans the columns from 2.
        auto rowFor = [&](const Row& r, bool head) {
            auto& e = t.m.E.at(r.equ);
            VfRow row;
            row.spans.push_back({0, {VfItem{std::to_wstring(r.equ)}}});
            if (r.kind == "leaf") {
                bool fixed = std::abs(e.lo) < unbounded && e.lo == e.hi;
                row.spans.push_back({1, what(r, head    ? L"main formula"
                                                : fixed ? L"fixed amount (MHz)"
                                                        : L"correction, kept between min and max")});
                if (fixed) {
                    row.spans.push_back({2, {cell(r.equ, "lo", 'a')}});
                    row.spans.push_back({3, {VfItem{L"stays this value", -1, true}}});
                } else {
                    for (int j = 0; j < 10; ++j)
                        row.spans.push_back({2 + j, j < e.nk ? std::vector{cell(r.equ, "k" + std::to_string(j), 'n')}
                                                             : std::vector<VfItem>{}});
                    row.spans.push_back({12, {cell(r.equ, "lo", 'n')}});
                    row.spans.push_back({13, {cell(r.equ, "hi", 'n')}});
                }
            } else if (r.kind == "cmp") {
                row.spans.push_back({1, what(r, L"test")});
                row.spans.push_back(
                    {2,
                     {VfItem{L"if " + w(r.var) + L" "}, cell(r.equ, "func", 'f'), VfItem{L" "},
                      cell(r.equ, "crit", 'n'), VfItem{L" then " + target(r.yes) + L", else " + target(r.no)}}});
            } else {
                row.spans.push_back({1, what(r, L"choice")});
                row.spans.push_back({2,
                                     {cell(r.equ, "max", 'm'), VfItem{L" equation " + std::to_wstring(r.a) +
                                                                      L" and equation " + std::to_wstring(r.b)}}});
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
        size_t lineCells = cells.size();
        VfTable tests{L"", "ewsnn"};
        VfRow header{{}, true};
        for (auto [i, s] :
             {std::pair(0, L"equ"), {1, L"what it is"}, {2, L"setting"}, {3, L"result min"}, {4, L"result max"}})
            header.spans.push_back({i, {label(s)}});
        tests.rows.push_back(std::move(header));
        for (auto& r : t.structure) {
            auto row = rowFor(r, false);
            if (r.kind == "leaf") row.spans.resize(2);
            row.spans.push_back({3, {cell(r.equ, "lo", 'n')}});
            row.spans.push_back({4, {cell(r.equ, "hi", 'n')}});
            tests.rows.push_back(std::move(row));
        }
        auto values = cellTexts();
        numbers->set(std::move(lines), std::vector<VfCell>(values.begin(), values.begin() + lineCells));
        // The structure's cells follow the lines' in `cells`; its own list starts at 0.
        structureOffset = lineCells;
        structure->set({std::move(tests)}, std::vector<VfCell>(values.begin() + lineCells, values.end()));
    }
    size_t structureOffset = 0;
    std::vector<VfCell> cellTexts() const {
        auto add = tuner->additions(tune);
        std::vector<VfCell> out;
        for (auto& c : cells) {
            auto v = tuner->value(tune, add, c.equ, c.field);
            auto b = tuner->stock_value(c.equ, c.field);
            VfCell cell{L"", v != b};
            if (c.kind == 'f' || c.kind == 'm') {
                cell.choices = c.kind == 'f' ? std::vector<std::wstring>{L"=", L"≥", L">"}
                                             : std::vector<std::wstring>{L"lowest of", L"highest of"};
                cell.text = cell.choices.at(std::clamp(int(v.value_or(0)), 0, int(cell.choices.size()) - 1));
            } else
                cell.text = fmt(v);
            out.push_back(std::move(cell));
        }
        return out;
    }
    void refreshCells() {
        auto values = cellTexts();
        numbers->cells(std::vector<VfCell>(values.begin(), values.begin() + structureOffset));
        structure->cells(std::vector<VfCell>(values.begin() + structureOffset, values.end()));
    }
    // A number typed into a cell, or a choice made (vfe-ui.html cell and amountCell).
    void edited(int index, const std::wstring& s) {
        auto& c = cells.at(index);
        auto key = std::to_string(c.equ) + "." + c.field;
        auto b = tuner->stock_value(c.equ, c.field);
        if (c.kind == 'a') {
            auto v = parse(s);
            for (auto f : {"lo", "hi"}) {
                auto k = std::to_string(c.equ) + "." + f;
                if (!v || f32(*v) == tuner->stock_value(c.equ, f))
                    tune.manual.erase(k);
                else
                    tune.manual[k] = f32(*v);
            }
            changed();
            return;
        }
        std::optional<double> v;
        if (c.kind == 'f' || c.kind == 'm')
            v = std::stoi(s);
        else if (s.find_first_not_of(L" \t") == std::wstring::npos)
            v = c.field == "lo" || c.field == "hi" ? std::nullopt : b;
        else if (auto x = parse(s))
            v = f32(*x);
        else {
            refreshCells();
            return;
        }
        if (tuner->slider(c.equ, c.field)) {
            auto add = tuner->additions(tune);
            double sum = b.value_or(NAN) + (add.contains(key) ? add.at(key) : 0);
            if (!v || f32(sum) == *v)
                tune.mdelta.erase(key);
            else
                tune.mdelta[key] = *v - sum;
        } else if (v == b)
            tune.manual.erase(key);
        else
            tune.manual[key] = v;
        changed();
    }

    // ---- the driver ----
    void delivered(std::uint64_t seq, const ControlResult& r) {
        if (!seq || (seq != openSeq && seq != readSeq && seq != applySeq && seq != settleSeq)) return;
        bool failed = r.code || !r.vf;
        auto message = w(r.text);
        if (seq == openSeq) {
            openSeq = 0;
            job = Job::none;
            if (failed)
                openError = L"The V/F curve tuner could not open: " + message;
            else {
                openError.clear();
                tuner = r.vf->tuner;
                build();
                read(r.vf, L"");
            }
        } else if (seq == readSeq) {
            readSeq = 0;
            job = Job::none;
            if (failed)
                readError = L"Could not read the driver: " + message;
            else
                read(r.vf, L"");
        } else if (seq == applySeq) {
            applySeq = 0;
            job = Job::none;
            if (failed) {
                readback = Readback{L"The write stopped.", message, L"", L"", false};
                readSeq = host.submit(VfRead{}, false);
                if (readSeq) job = Job::reading;
            } else {
                auto what =
                    restoring
                        ? L"Stock restored (" + plural(r.vf->equations, L"equation") + L")."
                        : L"Applied (" + plural(r.vf->equations, L"equation") +
                              (r.vf->offsets ? L", " + plural(r.vf->offsets, L"Afterburner Style OC offset") : L"") +
                              L").";
                if (r.vf->equations && live(r.vf)) {
                    // After an equation write the driver shows a slower fallback curve for a few seconds; the readback
                    // waits until it is gone (vf_tuner.h Settle), reading every 0.5 s.
                    settle = tuner->settling(liveEquations());
                    settleStart = Clock::now();
                    settleWhat = what;
                    settled(r.vf->points);
                    job = Job::settling;
                    SetTimer(root, settleTimer, 500, nullptr);
                } else
                    read(r.vf, what);
            }
        } else {
            settleSeq = 0;
            job = Job::none;
            if (failed) {
                readError = L"Could not read the driver: " + message;
                settle.reset();
            } else if (!settled(r.vf->points)) {
                reading = r.vf;
                job = Job::settling;
                SetTimer(root, settleTimer, 500, nullptr);
            } else {
                settle.reset();
                read(r.vf, settleWhat);
            }
        }
        buttons();
        status();
        layout(true);
        InvalidateRect(panel, nullptr, FALSE);
    }
    bool settled(const Points& points) {
        try {
            return settle->done(points, std::chrono::duration<double>(Clock::now() - settleStart).count());
        } catch (const std::exception&) {
            return true; // verify() says why the curve cannot be compared
        }
    }
    // The equations now on the GPU, as far as the tuner can describe them.
    Equations liveEquations() const {
        Fields known = *liveFields;
        for (auto& [i, d] : known) d.erase("other");
        return tuner->edited(known);
    }
    bool live(const std::shared_ptr<const VfReading>& r) {
        reading = r;
        try {
            liveFields = tuner->live_fields(r->records);
            liveOffsets.clear();
            for (auto& p : r->points.graphics) liveOffsets.push_back(p.offset_khz);
            undescribed.clear();
            for (auto& [i, d] : *liveFields)
                if (d.contains("other")) undescribed.push_back(i);
            readError.clear();
            return true;
        } catch (const std::exception& e) {
            liveFields.reset();
            readError = L"Could not read the driver: " + w(e.what());
            return false;
        }
    }
    void read(const std::shared_ptr<const VfReading>& r, std::wstring what) {
        if (!live(r)) return;
        if (what.empty())
            what = liveFields->empty() ? L"GPU now: stock equations."
                                       : L"GPU now: " + plural(liveFields->size(), L"equation") + L" changed.";
        verify(what);
        plot();
    }
    // The driver's curve, read back, against the calculation with the equations on the GPU (vfe-ui.html verify).
    void verify(const std::wstring& what) {
        Readback b{what};
        auto& t = *tuner;
        int n = t.points();
        double step = t.meta.step;
        try {
            auto& points = reading->points;
            auto driver = driver_stock(points);
            if (int(driver.size()) != n)
                throw std::runtime_error("The driver's curve has " + std::to_string(driver.size()) +
                                         " points; the tuner expects " + std::to_string(n) + ".");
            for (auto& x : driver) x = snap(x, step);
            auto fit = t.fit(liveEquations(), driver);
            std::vector<Fit::Diff> bad, edge;
            for (auto& d : fit.diffs) (d.edge ? edge : bad).push_back(d);
            auto list = [&](const std::vector<Fit::Diff>& a) {
                std::vector<std::wstring> s;
                for (size_t i = 0; i < a.size() && i < 15; ++i)
                    s.push_back(number(t.mv(a[i].point)) + L" mV: driver " + number(a[i].driver) + L", tab " +
                                number(a[i].computed));
                return join(s, L"; ") + (a.size() > 15 ? L"; …" : L"");
            };
            auto apart = L"one " + number(step) + L" MHz step apart (rounding)";
            b.ok = bad.empty();
            if (!bad.empty())
                b.verdict = L"Readback differs from this tab at " + std::to_wstring(bad.size()) + L" of " +
                            std::to_wstring(n) + L" points: " + list(bad) +
                            (edge.empty() ? L"" : L" · " + std::to_wstring(edge.size()) + L" more " + apart);
            else
                b.verdict = L"Readback matches this tab: " +
                            (edge.empty() ? L"all " + std::to_wstring(n) + L" points equal"
                                          : std::to_wstring(n - edge.size()) + L" of " + std::to_wstring(n) +
                                                L" points equal, " + std::to_wstring(edge.size()) + L" " + apart);
            b.more = L"Compared at " + number(fit.celsius) +
                     L" °C, the temperature at which the calculation fits the driver's curve" +
                     (fit.lo != fit.hi ? L" (" + number(fit.lo) + L" to " + number(fit.hi) + L" °C fit equally)" : L"");
            if (auto temp = host.gpu_temp ? host.gpu_temp() : std::nullopt)
                b.more += L"; the EC reads " + std::to_wstring(*temp) + L" °C for the GPU";
            b.more += L".";
            if (!edge.empty()) b.more += L"\nOne step apart: " + list(edge) + L".";
            if (!t.meta.recorded.empty())
                b.more +=
                    L"\nNVIDIA's stock equations were recorded on " + w(t.meta.recorded).replace(10, 1, L" ") + L".";
            // The final clock: the driver's own clock plus its offsets by the measured rule, against what it reports.
            if (std::ranges::any_of(points.graphics, [](auto& p) { return p.offset_khz != 0; })) {
                std::vector<double> o;
                for (auto& p : points.graphics) o.push_back(p.offset_khz / 1000.0);
                auto predicted = with_offsets(driver, o, step);
                std::vector<std::wstring> s;
                int count = 0;
                for (int p = 0; p < n; ++p) {
                    double final = snap(points.graphics[p].final_khz / 1000.0, step);
                    if (final == predicted[p]) continue;
                    if (++count <= 15)
                        s.push_back(number(t.mv(p)) + L" mV: driver " + number(final) + L", tab " +
                                    number(predicted[p]));
                }
                if (count)
                    b.offsets = L"With the Afterburner Style OC offsets, the final clock differs from this tab at " +
                                std::to_wstring(count) + L" of " + std::to_wstring(n) + L" points: " + join(s, L"; ") +
                                (count > 15 ? L"; …" : L"");
            }
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
        for (auto& [i, d] : fields) ids.push_back(std::to_wstring(i));
        size_t moved = 0;
        for (size_t p = 0; p < curve.offsets_khz.size(); ++p)
            moved += curve.offsets_khz[p] != (p < liveOffsets.size() ? liveOffsets[p] : 0);
        if (!confirm(
                L"Write up to " + plural(ids.size(), L"equation") + L" to the GPU (" + join(ids, L", ") + L")" +
                (moved ? L" and " + plural(moved, L"Afterburner Style OC offset") : L"") +
                L"?\n\nThe GPU drops to a slower fallback curve for a few seconds after each write. Do this at idle, not in a game."))
            return;
        submitWrite(VfApply{std::move(curve)}, false);
    }
    void restore() {
        if (!confirm(
                L"Write NVIDIA's stock equations back and set the Afterburner Style OC offsets to 0 (slow curve on again, none of your changes)?\n\nThe settings on this tab stay as they are."))
            return;
        submitWrite(VfApply{}, true);
    }
    void submitWrite(ControlCommand command, bool stock) {
        applySeq = host.submit(std::move(command), true);
        if (!applySeq) return;
        restoring = stock;
        job = Job::writing;
        buttons();
        status();
    }
    void reset() {
        if (!confirm(
                L"Set every slider and Afterburner Style OC offset to 0, clear every edited number and untick the corrections? (Nothing is written to the GPU.)"))
            return;
        auto keep = tune.view;
        tune = tuner->tune(config::Json::object());
        tune.view = keep;
        chart->clear_selection();
        changed();
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
    // What a saved tune shows; a state saved by an older page (other temperatures, the reference by band name) goes to
    // the nearest column.
    void loadView(const config::Json& v) {
        auto nearest = [](double celsius) {
            int best = 0;
            for (int c = 1; c < 8; ++c)
                if (std::abs(columns[c].celsius - celsius) < std::abs(columns[best].celsius - celsius)) best = c;
            return best;
        };
        for (auto [key, on] :
             {std::pair("showStock", &view.stock), {"showNew", &view.fresh}, {"showBand", &view.band}}) {
            if (!v.contains(key) || !v[key].is_object()) continue;
            on->fill(false);
            for (auto& [k, x] : v[key].items()) {
                if (!truthy(x)) continue;
                auto band = std::ranges::find_if(columns, [&](auto& c) { return *c.band && k == c.band; });
                if (band != columns.end())
                    (*on)[band - columns.begin()] = true;
                else if (auto celsius = parse(w(k)))
                    (*on)[nearest(*celsius)] = true;
            }
        }
        if (v.contains("lines")) view.lines = truthy(v["lines"]);
        if (v.contains("ymode") && v["ymode"].is_string())
            for (int m = 0; m < 3; ++m)
                if (v["ymode"] == modeNames[m]) view.mode = m;
    }
    void importTune() {
        auto path = file(false, L"Tune (*.json)\0*.json\0All files\0*.*\0", L"json");
        if (!path) return;
        try {
            auto j = config::read_json(*path);
            auto& state = j.contains("state") && j["state"].is_object() ? j["state"] : j;
            tune = tuner->tune(state);
            loadView(tune.view);
        } catch (const std::exception& e) {
            error(L"Could not import " + path->filename().wstring() + L": " + w(e.what()));
            return;
        }
        chart->clear_selection();
        sync();
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
        Tune t = tune;
        t.view = viewJson();
        auto j = config::Json{{"saved", now()}, {"state", tuner->state(t)}};
        j["offsets"] = config::Json::array();
        for (double o : offsets) j["offsets"].push_back(std::round(o * 1000) / 1000);
        j["fields"] = config::Json::object();
        for (auto& [i, d] : fields)
            for (auto& [f, v] : d)
                j["fields"][std::to_string(i)][f] =
                    (f == "lo" || f == "hi") && std::abs(v) >= unbounded ? config::Json(nullptr) : config::Json(v);
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
            sheet = tuner->reference(*path);
            sheetName = path->filename().wstring();
            modeItems();
        } catch (const std::exception& e) {
            error(L"Could not read " + path->filename().wstring() + L": " + w(e.what()));
            return;
        }
        layout(true);
        shown();
    }

    // ---- the status beside the buttons ----
    void status() {
        auto& p = palette();
        statusColor = p.muted;
        if (!tuner)
            statusText = job == Job::opening ? L"Opening…" : L"Not open";
        else if (job == Job::writing)
            statusText = L"Writing…";
        else if (job == Job::settling)
            statusText = L"Reading back…";
        else if (job == Job::reading)
            statusText = L"Reading…";
        else if (!liveFields)
            statusText = L"GPU not read";
        else {
            bool same = invalid.empty() && fields == *liveFields && offsets.size() == liveOffsets.size();
            for (size_t i = 0; same && i < offsets.size(); ++i) same = std::lround(offsets[i] * 1000) == liveOffsets[i];
            bool stockNow = liveFields->empty() && std::ranges::all_of(liveOffsets, [](int o) { return o == 0; });
            statusText = same ? L"Applied to the GPU" : stockNow ? L"Unapplied (currently stock)" : L"Unapplied";
            statusColor = same ? p.ok : p.warn;
        }
        InvalidateRect(root, &statusRect, FALSE);
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
        ReleaseDC(root, dc);
        statusRect = {x + ref(8), y, rc.right - ref(16), y + h};
        // The chart keeps its shape: 2.5 times as wide as high, and at most half of the height below the buttons.
        int top = y + h + ref(10), width = rc.right - 2 * ref(16), height = std::max(0, int(rc.bottom) - top);
        int chartHeight = std::min(int(width / 2.5), height / 2), chartWidth = std::min(width, int(chartHeight * 2.5));
        MoveWindow(chart->window(), (rc.right - chartWidth) / 2, top, chartWidth, chartHeight, TRUE);
        int below = top + chartHeight + ref(6);
        MoveWindow(panel, 0, below, rc.right, std::max(0, int(rc.bottom) - below), TRUE);
        InvalidateRect(root, nullptr, FALSE);
    }
    // One pass over the scrolling cards: measures them and, with apply, also places the controls. y runs down the
    // scrolled panel.
    void layout(bool apply) {
        RECT rc{};
        GetClientRect(panel, &rc);
        if (rc.right <= 0 || !fonts.font) return;
        auto& pal = palette();
        auto dc = GetDC(panel);
        auto height = [&](HFONT f) {
            SelectObject(dc, f);
            TEXTMETRICW m{};
            GetTextMetricsW(dc, &m);
            return int(m.tmHeight);
        };
        auto extent = [&](HFONT f, const std::wstring& s) {
            SelectObject(dc, f);
            SIZE size{};
            GetTextExtentPoint32W(dc, s.c_str(), int(s.size()), &size);
            return int(size.cx);
        };
        int line = height(fonts.font), titleHeight = height(fonts.title), box = line + ref(4), row = ref(32),
            gap = ref(12);
        std::vector<Frame> fr;
        std::vector<Text> tx;
        std::vector<std::pair<RECT, std::wstring>> tp;
        int x = ref(16), right = rc.right - ref(16), y = ref(8) - scroll;
        auto put = [&](HWND child, int cx, int cy, int cw, int ch, bool show = true) {
            if (!apply || !child) return;
            if (!show) {
                ShowWindow(child, SW_HIDE);
                return;
            }
            RECT now{};
            GetWindowRect(child, &now);
            MapWindowPoints(nullptr, panel, reinterpret_cast<POINT*>(&now), 2);
            if (now.left != cx || now.top != cy || now.right - now.left != cw || now.bottom - now.top != ch)
                SetWindowPos(child, nullptr, cx, cy, std::max(0, cw), std::max(0, ch), SWP_NOZORDER | SWP_NOACTIVATE);
            if (!IsWindowVisible(child)) ShowWindow(child, SW_SHOW);
        };
        auto say = [&](const std::wstring& s, int from, int to, int& at, COLORREF color, HFONT f = nullptr) {
            if (s.empty()) return RECT{from, at, from, at};
            f = f ? f : fonts.font;
            RECT r{from, at, to, at};
            SelectObject(dc, f);
            DrawTextW(dc, s.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            r.right = to;
            tx.push_back({r, s, color, f});
            at = r.bottom;
            return r;
        };
        auto open = [&](const std::wstring& title, int from, int to, int& at) {
            fr.push_back({{from, at, to, 0}, title, pal.card, pal.border});
            at += ref(10) + titleHeight + ref(8);
        };
        auto close = [&](int& at, size_t frame) {
            at += ref(14);
            fr[frame].r.bottom = at;
        };
        auto check = [&](HWND c, int from, int& at) {
            at += ref(6);
            put(c, from, at, extent(fonts.font, windowText(c)) + ref(40), box);
            at += box;
        };
        auto link = [&](HWND l, int from, int to, int& at) {
            SIZE size{};
            SendMessageW(l, LM_GETIDEALSIZE, std::max(0, to - from), LPARAM(&size));
            put(l, from, at, to - from, size.cy);
            at += size.cy;
        };
        // Banners: what keeps the tab from working as it should.
        std::vector<std::wstring> banners;
        if (!openError.empty()) banners.push_back(openError);
        if (tuner && !tuner->refusal.empty()) banners.push_back(L"Apply is off on this GPU: " + w(tuner->refusal));
        bool issue = !banners.empty();
        if (!undescribed.empty()) {
            std::vector<std::wstring> ids;
            for (int i : undescribed) ids.push_back(std::to_wstring(i));
            banners.push_back(L"The driver has changes in equation " + join(ids, L", ") +
                              L" that this tab cannot show. \"Restore stock\" removes them.");
        }
        if (!readError.empty()) banners.push_back(readError);
        if (!invalid.empty()) banners.push_back(L"These settings cannot be written: " + invalid);
        for (size_t i = 0; i < banners.size(); ++i) {
            fr.push_back({{x, y, right, 0}, L"", mix(pal.card, pal.error, pal.dark ? 0.22 : 0.10), pal.error});
            size_t frame = fr.size() - 1;
            y += ref(10);
            say(banners[i], x + ref(15), right - ref(15), y, pal.text);
            if (i == 0 && issue) {
                y += ref(4);
                link(issueLink, x + ref(15), right - ref(15), y);
            }
            y += ref(10);
            fr[frame].r.bottom = y;
            y += gap;
        }
        if (!issue) put(issueLink, 0, 0, 0, 0, false);
        if (!tuner) {
            size_t frame = fr.size();
            open(L"V/F Curve", x, right, y);
            say(job == Job::opening
                    ? L"Opening the tuner: reading the driver's equations and finding its clock curve. The first time on a GPU this takes a few seconds."
                    : L"The tuner is not open. Read (above) tries again.",
                x + ref(15), right - ref(15), y, pal.muted);
            close(y, frame);
            finish(fr, tx, tp, y, apply, dc);
            return;
        }
        auto& t = *tuner;
        int left = x + ref(15), end = right - ref(15);
        // Current changes.
        {
            std::vector<std::wstring> ids, parts, removed;
            for (auto& [i, d] : fields)
                if (!t.switch_fields.contains(i)) ids.push_back(std::to_wstring(i));
            for (auto& r : t.rules)
                if (tune.rules.contains(r.id) && tune.rules.at(r.id)) {
                    auto s = w(r.title.substr(0, r.title.find(',')));
                    std::ranges::transform(s, s.begin(), towlower);
                    removed.push_back(s + L" removed");
                }
            bool sw = !t.switch_fields.empty();
            size_t moved = std::ranges::count_if(offsets, [](double o) { return o != 0; });
            if (sw) parts.push_back(L"Slow curve disabled");
            if (!ids.empty()) parts.push_back(plural(ids.size(), L"equation") + L" changed");
            if (moved) parts.push_back(L"Afterburner Style OC at " + plural(moved, L"point"));
            auto summary = parts.empty() ? L"None" : join(parts, L" + ") + (sw && parts.size() == 1 ? L" only" : L"");
            if (!sw && !parts.empty()) summary[0] = towupper(summary[0]);
            if (!removed.empty()) summary += L" (" + join(removed, L", ") + L")";
            size_t frame = fr.size();
            open(L"Current Changes", x, right, y);
            say(summary, left, end, y, pal.text);
            for (auto& r : t.rules)
                if (r.kind == "diff" && tune.preview.contains(r.id) && tune.preview.at(r.id))
                    say(L"Shown as if channel " + std::to_wstring(r.hot) + L" is hotter than channel " +
                            std::to_wstring(r.cold) + L" (plot only)",
                        left, end, y, pal.text, fonts.bold);
            if (!ids.empty()) {
                std::vector<std::wstring> sws;
                for (auto& [i, d] : t.switch_fields) sws.push_back(std::to_wstring(i));
                say(L"Equations " + join(ids, L", ") +
                        (sw ? L", and " + join(sws, L", ") + L" to disable the slow curve" : L"") + L".",
                    left, end, y, pal.muted);
            }
            close(y, frame);
            y += gap;
        }
        if (readback) {
            size_t frame = fr.size();
            open(L"GPU Readback", x, right, y);
            say(readback->what, left, end, y, pal.text, fonts.bold);
            say(readback->verdict, left, end, y, readback->ok ? pal.ok : pal.error);
            say(readback->offsets, left, end, y, pal.error);
            if (!readback->more.empty()) y += ref(4);
            say(readback->more, left, end, y, pal.muted);
            close(y, frame);
            y += gap;
        }
        // Plot: what the chart shows.
        {
            size_t frame = fr.size();
            open(L"Plot", x, right, y);
            int widest = 0;
            for (auto m : modes) widest = std::max(widest, extent(fonts.font, m));
            int at = left, top = y;
            // A drop-down list's window includes its list; it is centered by its closed height.
            auto flow = [&](HWND c, int width, int height, int list = 0) {
                if (at > left && at + width > end) {
                    at = left;
                    top += row + ref(6);
                }
                put(c, at, top + (row - height) / 2, width, height + list);
                at += width + ref(16);
            };
            flow(modeBox, widest + ref(60), box, ref(200));
            flow(linesCheck, extent(fonts.font, windowText(linesCheck)) + ref(40), box);
            flow(referenceButton, extent(fonts.font, windowText(referenceButton)) + ref(30), row);
            auto name = sheetName.empty() ? L"No reference curve" : sheetName;
            int nameWidth = extent(fonts.font, name);
            if (at > left && at + nameWidth > end) {
                at = left;
                top += row + ref(6);
            }
            tx.push_back({{at, top, end, top + row},
                          name,
                          pal.muted,
                          fonts.font,
                          DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS});
            y = top + row + ref(10);
            // Show: per row and column. A click on a row's name turns the whole row on or off.
            bool band = std::ranges::any_of(std::array{0, 1, 2, 3, 4, 5, 6, 7}, [&](int c) { return have(c); });
            const std::wstring rows[]{L"○ Stock curve (NVIDIA's)", L"● New curve (slow curve disabled + your changes)",
                                      L"+ Reference curve (" + sheetName + L")"};
            int cell = std::max(extent(fonts.bold, L"80 °C") + ref(18), ref(60)), labels = 0;
            for (auto& s : rows) labels = std::max(labels, extent(fonts.font, s) + ref(16));
            labels = std::min(labels, std::max(ref(150), end - left - 8 * cell));
            tx.push_back(
                {{left, y, left + labels, y + line}, L"Show", pal.text, fonts.bold, DT_SINGLELINE | DT_NOPREFIX});
            for (int c = 0; c < 8; ++c)
                tx.push_back({{left + labels + c * cell, y, left + labels + (c + 1) * cell, y + line},
                              std::to_wstring(columns[c].celsius) + L" °C",
                              hue(c),
                              fonts.bold,
                              DT_SINGLELINE | DT_CENTER | DT_NOPREFIX});
            y += line + ref(4);
            for (int r = 0; r < 3; ++r) {
                bool shownRow = r < 2 || band;
                if (!shownRow) {
                    showLabels[r] = {};
                    for (auto b : shows[r]) put(b, 0, 0, 0, 0, false);
                    continue;
                }
                RECT label{left, y, left + labels - ref(8), y};
                SelectObject(dc, fonts.font);
                DrawTextW(dc, rows[r].c_str(), -1, &label, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                int h = std::max(int(label.bottom - label.top), box) + ref(6);
                label.bottom = label.top + h;
                label.right = left + labels - ref(8);
                tx.push_back({label, rows[r], pal.text, fonts.font, DT_WORDBREAK | DT_NOPREFIX});
                showLabels[r] = label;
                tp.push_back({label, L"Click to turn the whole row on or off."});
                for (int c = 0; c < 8; ++c) {
                    bool on = r < 2 || have(c);
                    int size = scale(18);
                    put(shows[r][c], left + labels + c * cell + (cell - size) / 2, y, size, box, on);
                }
                y += h;
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
            y += ref(4);
            say(note, left, end, y, pal.muted);
            close(y, frame);
            y += gap;
        }
        // The cards: as many columns as fit, at least about 400 pixels wide each.
        {
            int minimum = scale(400), count = std::max(1, (right - x + gap) / (minimum + gap)),
                width = (right - x - (count - 1) * gap) / count;
            struct Item {
                std::function<void(int from, int to, int& at)> fill;
            };
            std::vector<Item> items;
            auto sliderRows = [&](std::vector<Slider*> list, int from, int to, int& at) {
                int labelWidth = 0, unitWidth = 0;
                for (auto s : list) {
                    labelWidth = std::max(labelWidth, extent(fonts.bold, s->name) + ref(12));
                    unitWidth = std::max(unitWidth, extent(fonts.font, s->unit) + ref(6));
                }
                for (auto& l : limits) labelWidth = std::max(labelWidth, extent(fonts.bold, l.name) + ref(12));
                int numberWidth = extent(fonts.font, L"-0000.00") + ref(20);
                for (auto s : list) {
                    double v = s->value(tune);
                    int trackLeft = from + labelWidth, trackRight = to - unitWidth - numberWidth - ref(16);
                    s->label = {from, at, trackLeft, at + row};
                    tx.push_back({s->label, s->name, v != 0 ? pal.accent : pal.text, fonts.bold,
                                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS});
                    tp.push_back({s->label, s->hint});
                    put(s->track, trackLeft, at, trackRight - trackLeft, row);
                    put(s->edit, trackRight + ref(8), at + (row - box) / 2, numberWidth, box);
                    tx.push_back({{trackRight + ref(16) + numberWidth, at, to, at + row},
                                  s->unit,
                                  pal.muted,
                                  fonts.font,
                                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX});
                    at += row;
                    if (!s->card.empty()) {
                        // Under the track, from the thumb's leftmost to its rightmost center.
                        RECT channel{}, thumb{};
                        SendMessageW(s->track, TBM_GETCHANNELRECT, 0, LPARAM(&channel));
                        SendMessageW(s->track, TBM_GETTHUMBRECT, 0, LPARAM(&thumb));
                        int half = (thumb.right - thumb.left) / 2;
                        s->strip = {trackLeft + channel.left + half, at, trackLeft + channel.right - half,
                                    at + scale(5)};
                        tp.push_back(
                            {s->strip,
                             L"Grey: moving the slider here changes nothing at the plotted temperatures (another line is lower, or the curve is at the floor, at the ceiling or held flat)."});
                        at += scale(5);
                    }
                    at += ref(6);
                }
                return std::pair(labelWidth, numberWidth + unitWidth);
            };
            items.push_back({[&](int from, int to, int& at) {
                size_t frame = fr.size();
                open(L"Global Offset", from, to, at);
                int l = from + ref(15), r = to - ref(15);
                auto [labelWidth, tail] = sliderRows({&sliders[0], &sliders[1]}, l, r, at);
                int numberWidth = extent(fonts.font, L"-0000.00") + ref(20), unitWidth = tail - numberWidth;
                for (auto& lim : limits) {
                    bool set = tune.manual.contains(std::to_string(t.meta.root) + "." + lim.field);
                    int trackRight = r - unitWidth - numberWidth - ref(16);
                    tx.push_back({{l, at, l + labelWidth, at + row},
                                  lim.name,
                                  set ? pal.accent : pal.text,
                                  fonts.bold,
                                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX});
                    tx.push_back({{l + labelWidth, at, trackRight, at + row},
                                  lim.hint,
                                  pal.muted,
                                  fonts.font,
                                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX});
                    put(lim.edit, trackRight + ref(8), at + (row - box) / 2, numberWidth, box);
                    tx.push_back({{trackRight + ref(16) + numberWidth, at, r, at + row},
                                  L"MHz",
                                  pal.muted,
                                  fonts.font,
                                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX});
                    at += row + ref(6);
                }
                at += ref(4);
                say(w(t.switch_text), l, r, at, pal.muted);
                close(at, frame);
            }});
            if (!t.rules.empty())
                items.push_back({[&](int from, int to, int& at) {
                    size_t frame = fr.size();
                    open(L"NVIDIA's Temperature Corrections", from, to, at);
                    int l = from + ref(15), r = to - ref(15);
                    for (size_t i = 0; i < t.rules.size(); ++i) {
                        if (i) at += ref(10);
                        say(w(t.rules[i].title), l, r, at, pal.text, fonts.bold);
                        at += ref(2);
                        say(w(t.rules[i].text), l, r, at, pal.muted);
                        if (ruleChecks[i].preview) check(ruleChecks[i].preview, l, at);
                        check(ruleChecks[i].remove, l, at);
                    }
                    close(at, frame);
                }});
            items.push_back({[&](int from, int to, int& at) {
                size_t frame = fr.size();
                open(L"The Lines, from Low to High Voltage", from, to, at);
                say(L"The clock follows the lowest line (thin lines on the chart).\nGrey bar under a slider: moving it there changes nothing.\nTemp sliders: MHz per 10 °C warmer.",
                    from + ref(15), to - ref(15), at, pal.muted);
                close(at, frame);
            }});
            auto calc = shownNew();
            auto& win = tuned(calc[0]).win;
            for (auto& s : t.sections)
                items.push_back({[&, s = &s](int from, int to, int& at) {
                    size_t frame = fr.size();
                    int top = at;
                    open(w(s->name), from, to, at);
                    fr.back().card = s->id;
                    // Where its lines set the new curve now, at the first plotted temperature.
                    std::map<int, std::wstring> names;
                    for (auto& o : t.sections)
                        for (int h : o.heads) names[h] = w(o.name);
                    std::vector<int> mine;
                    std::vector<std::pair<std::wstring, std::vector<int>>> others;
                    for (int p = 0; p < t.points(); ++p) {
                        if (t.mv(p) < s->lo || t.mv(p) > s->hi) continue;
                        if (std::ranges::find(s->heads, win[p]) != s->heads.end()) {
                            mine.push_back(p);
                            continue;
                        }
                        auto k = win[p] == Calc::held_flat ? L"held flat"
                                 : win[p] == Calc::floor   ? L"floor"
                                 : win[p] == Calc::ceiling ? L"ceiling"
                                 : names.contains(win[p])  ? names[win[p]]
                                                           : L"another equation";
                        auto it = std::ranges::find_if(others, [&](auto& o) { return o.first == k; });
                        if (it == others.end()) it = others.insert(others.end(), {k, {}});
                        it->second.push_back(p);
                    }
                    std::vector<std::wstring> by;
                    for (auto& [k, ps] : others) by.push_back(k + L": " + runs(ps));
                    int titleRight = from + ref(29) + extent(fonts.title, w(s->name));
                    RECT title{from + ref(15), top + ref(10), titleRight, top + ref(10) + titleHeight};
                    std::vector<std::wstring> heads;
                    for (int h : s->heads) heads.push_back(std::to_wstring(h));
                    tp.push_back({title, L"Left end " + number(s->lo) + L" mV, right end " + number(s->hi) +
                                             L" mV. Equation" + (heads.size() > 1 ? L"s " : L" ") + join(heads, L", ") +
                                             L". " + w(s->note)});
                    RECT state{titleRight + ref(12), title.top, to - ref(15), title.bottom};
                    tx.push_back({state, mine.empty() ? L"not active" : L"active at " + runs(mine),
                                  mine.empty() ? pal.error : pal.muted, fonts.font,
                                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS});
                    tp.push_back(
                        {state, L"At " + std::to_wstring(columns[calc[0]].celsius) + L" °C. " + join(by, L"; ")});
                    std::vector<Slider*> list;
                    for (auto& sl : sliders)
                        if (sl.card == s->id) list.push_back(&sl);
                    sliderRows(list, from + ref(15), to - ref(15), at);
                    close(at, frame);
                }});
            int bottom = y;
            for (size_t i = 0; i < items.size(); ++i) {
                int column = int(i % count), from = x + column * (width + gap);
                if (!column) y = bottom + (i ? gap : 0);
                int at = y;
                items[i].fill(from, from + width, at);
                bottom = std::max(bottom, at);
            }
            y = bottom + ref(8);
            say(L"Double-click a slider to set it to 0. Every number behind the sliders is in the tables below.", x,
                right, y, pal.muted);
            y += gap;
        }
        // Every number, and how the lines are combined.
        for (auto [title, linkControl, table, isOpen] :
             {std::tuple(L"Every Number, by Line", numbersLink, numbers.get(), numbersOpen),
              {L"Structure: How the Lines Are Combined", structureLink, structure.get(), structureOpen}}) {
            size_t frame = fr.size();
            int top = y;
            open(title, x, right, y);
            SIZE size{};
            SendMessageW(linkControl, LM_GETIDEALSIZE, ref(200), LPARAM(&size));
            put(linkControl, right - ref(15) - size.cx, top + ref(10) + (titleHeight - size.cy) / 2, size.cx, size.cy);
            if (isOpen) {
                say(table == numbers.get()
                        ? w(t.formnote) +
                              L" Each line is its main formula plus the rows under it. Highlighted cells differ from stock; empty a cell to put the stock value back. The sliders change the constant, V, V², T, T² and V·T terms of the main formulas."
                        : L"Tests on voltage and temperature and the lowest-of choices above the lines, for this chip. Equation " +
                              std::to_wstring(t.meta.root) +
                              L" is the result: its min and max are the lowest and highest clock the curve can give. Writing a test or a choice to the GPU is a kind of edit that has not been tried yet. " +
                              w(t.switch_detail),
                    left, end, y, pal.muted);
                y += ref(6);
                int h = table->height(end - left);
                put(table->window(), left, y, end - left, h);
                y += h;
            } else
                put(table->window(), 0, 0, 0, 0, false);
            close(y, frame);
            y += gap;
        }
        finish(fr, tx, tp, y, apply, dc);
    }
    void finish(std::vector<Frame>& fr, std::vector<Text>& tx, std::vector<std::pair<RECT, std::wstring>>& tp, int y,
                bool apply, HDC dc) {
        ReleaseDC(panel, dc);
        frames = std::move(fr);
        texts = std::move(tx);
        tips = std::move(tp);
        RECT rc{};
        GetClientRect(panel, &rc);
        int content = y + scroll;
        int top = std::max(0, content - int(rc.bottom));
        if (scroll > top) {
            scroll = top;
            layout(true);
            InvalidateRect(panel, nullptr, FALSE);
            return;
        }
        SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, content - 1), UINT(rc.bottom), scroll};
        SetScrollInfo(panel, SB_VERT, &si, TRUE);
        // Painted parts with a tooltip get one tool each, numbered from 1.
        for (size_t i = 0; i < tools; ++i) {
            TOOLINFOW t{sizeof t, 0, panel, i + 1};
            SendMessageW(tooltip, TTM_DELTOOLW, 0, LPARAM(&t));
        }
        for (size_t i = 0; i < tips.size(); ++i) {
            TOOLINFOW t{sizeof t, TTF_SUBCLASS, panel, i + 1, tips[i].first};
            t.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(tooltip, TTM_ADDTOOLW, 0, LPARAM(&t));
        }
        tools = tips.size();
        (void)apply;
    }
    void scrollTo(int target) {
        SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE};
        GetScrollInfo(panel, SB_VERT, &si);
        target = std::clamp(target, 0, std::max(0, si.nMax + 1 - int(si.nPage)));
        if (target == scroll) return;
        ScrollWindowEx(panel, 0, scroll - target, nullptr, nullptr, nullptr, nullptr,
                       SW_SCROLLCHILDREN | SW_INVALIDATE);
        scroll = target;
        layout(false);
    }
    void style(const VfFonts& f) {
        fonts = f;
        auto font = [](HWND child, LPARAM value) -> BOOL {
            SendMessageW(child, WM_SETFONT, WPARAM(value), TRUE);
            return TRUE;
        };
        EnumChildWindows(root, font, LPARAM(fonts.font));
        if (numbers) numbers->style(fonts.font, fonts.bold, fonts.dpi);
        if (structure) structure->style(fonts.font, fonts.bold, fonts.dpi);
        chart->dpi(fonts.dpi);
        SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, ref(600));
        systemTheme(tooltip, L"DarkMode_Explorer");
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
    COLORREF backdrop(POINT at) const {
        for (auto& f : frames)
            if (PtInRect(&f.r, at)) return f.fill;
        return palette().window;
    }
    COLORREF backdrop(HWND child) const {
        RECT r{};
        GetWindowRect(child, &r);
        MapWindowPoints(nullptr, panel, reinterpret_cast<POINT*>(&r), 2);
        return backdrop(POINT{(r.left + r.right) / 2, (r.top + r.bottom) / 2});
    }
    void paintPanel(HDC target, const RECT& area) {
        if (IsRectEmpty(&area)) return;
        Buffer buffer(target, area);
        auto dc = buffer.dc;
        auto& p = palette();
        FillRect(dc, &area, brush(p.window));
        SelectObject(dc, fonts.title);
        for (auto& f : frames) {
            RECT visible{};
            if (!IntersectRect(&visible, &f.r, &area)) continue;
            roundRect(dc, f.r, ref(12), f.fill, f.edge);
            if (f.title.empty()) continue;
            // A card with a short accent mark before its bold title, as on the other tabs.
            SIZE size{};
            GetTextExtentPoint32W(dc, f.title.c_str(), int(f.title.size()), &size);
            int top = f.r.top + ref(10);
            RECT mark{f.r.left + ref(15), top + size.cy / 5, f.r.left + ref(20), top + size.cy - size.cy / 5};
            roundRect(dc, mark, ref(3), p.accent, p.accent);
            SetTextColor(dc, p.text);
            SelectObject(dc, fonts.title);
            TextOutW(dc, f.r.left + ref(29), top, f.title.c_str(), int(f.title.size()));
        }
        for (auto& t : texts) {
            RECT visible{};
            if (!IntersectRect(&visible, &t.r, &area)) continue;
            SelectObject(dc, t.font);
            SetTextColor(dc, t.color);
            RECT r = t.r;
            DrawTextW(dc, t.s.c_str(), -1, &r, t.format);
        }
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
        for (size_t i = 0; i < ruleChecks.size(); ++i) {
            auto& id = tuner->rules[i].id;
            if (from == ruleChecks[i].preview) {
                tune.preview[id] = on;
                stockCalc = {};
                changed();
            }
            if (from == ruleChecks[i].remove) {
                tune.rules[id] = on;
                changed();
            }
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
            if (!wcscmp(name, WC_LINK)) {
                if (draw.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (draw.dwDrawStage == CDDS_ITEMPREPAINT)
                    SetTextColor(draw.hdc, draw.dwItemSpec != UINT_PTR(-1) ? palette().link : palette().text);
                return CDRF_DODEFAULT;
            }
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
                if (open) refreshCells();
                layout(true);
                InvalidateRect(panel, nullptr, FALSE);
            }
        }
        if (n->code == TTN_GETDISPINFOW) {
            auto& d = *reinterpret_cast<NMTTDISPINFOW*>(n);
            if (d.uFlags & TTF_IDISHWND) {
                auto it = controlTips.find(reinterpret_cast<HWND>(n->idFrom));
                tipText = it != controlTips.end() ? it->second : L"";
            } else
                tipText = n->idFrom && n->idFrom <= tips.size() ? tips[n->idFrom - 1].second : L"";
            d.lpszText = tipText.data();
        }
        return 0;
    }
    LRESULT colors(UINT message, HDC dc, HWND child) const {
        auto& p = palette();
        COLORREF back = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX ? p.field
                        : GetParent(child) == panel                                 ? backdrop(child)
                                                                                    : p.window;
        SetTextColor(dc, p.text);
        SetBkColor(dc, back);
        return reinterpret_cast<LRESULT>(brush(back));
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
            if (wp == settleTimer) {
                KillTimer(root, settleTimer);
                settleSeq = host.submit(VfRead{}, false);
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
            PAINTSTRUCT ps{};
            auto dc = BeginPaint(panel, &ps);
            paintPanel(dc, ps.rcPaint);
            EndPaint(panel, &ps);
            if (zones.pending && !zones.running) {
                zones.running = true;
                SetTimer(panel, zoneTimer, 250, nullptr);
            }
            return 0;
        }
        case WM_PRINTCLIENT: {
            auto dc = reinterpret_cast<HDC>(wp);
            RECT r{};
            GetClipBox(dc, &r);
            FillRect(dc, &r, brush(backdrop(POINT{(r.left + r.right) / 2, (r.top + r.bottom) / 2})));
            return 0;
        }
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
        case WM_MOUSEWHEEL: {
            int target = scroll;
            if (message == WM_MOUSEWHEEL)
                target -= GET_WHEEL_DELTA_WPARAM(wp) * scale(54) / WHEEL_DELTA;
            else {
                SCROLLINFO si{sizeof si, SIF_ALL};
                GetScrollInfo(panel, SB_VERT, &si);
                switch (LOWORD(wp)) {
                case SB_LINEUP: target -= scale(28); break;
                case SB_LINEDOWN: target += scale(28); break;
                case SB_PAGEUP: target -= int(si.nPage); break;
                case SB_PAGEDOWN: target += int(si.nPage); break;
                case SB_THUMBTRACK: target = si.nTrackPos; break;
                case SB_TOP: target = 0; break;
                case SB_BOTTOM: target = si.nMax; break;
                }
            }
            scrollTo(target);
            return 0;
        }
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
                for (auto& f : frames)
                    if (PtInRect(&f.r, at)) card = f.card;
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
void VfTab::style(const VfFonts& fonts) { impl_->style(fonts); }
void VfTab::read() {
    auto& i = *impl_;
    if (i.job != Impl::Job::none) return;
    if (!i.tuner) {
        i.openSeq = i.host.submit(VfRead{.open = true}, false);
        if (i.openSeq) i.job = Impl::Job::opening;
    } else {
        i.readSeq = i.host.submit(VfRead{}, true);
        if (i.readSeq) i.job = Impl::Job::reading;
    }
    i.buttons();
    i.status();
    i.layout(true);
    InvalidateRect(i.panel, nullptr, FALSE);
}
void VfTab::delivered(std::uint64_t sequence, const ControlResult& result) { impl_->delivered(sequence, result); }
std::wstring VfTab::text() const {
    auto& r = impl_->readback;
    return impl_->statusText + (r ? L"\n" + r->what + L"\n" + r->verdict + L"\n" + r->offsets : L"");
}
}
