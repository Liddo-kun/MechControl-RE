#include "settings.h"
#include "config_text.h"
#include <windows.h>
#include <iostream>
#include <fstream>

using namespace mech::config;
static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
template <class F> static std::string fails(F f) {
    try {
        f();
    } catch (const std::exception& e) {
        return e.what();
    }
    throw std::runtime_error("Expected failure");
}
static void raw(const std::filesystem::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
    if (!f) throw std::runtime_error("Fixture write failed");
}
static ThermalState hold(int actions) {
    ThermalState s;
    s.stage = Tripped;
    s.held = actions;
    return s;
}
struct Lock {
    HANDLE h;
    explicit Lock(const std::filesystem::path& path, bool allow_write = true)
        : h(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | (allow_write ? FILE_SHARE_WRITE : 0), nullptr,
                        OPEN_EXISTING, 0, nullptr)) {
        require(h != INVALID_HANDLE_VALUE, "Lock fixture failed");
    }
    ~Lock() { CloseHandle(h); }
};
int main(int argc, char** argv) try {
    require(argc == 3, "Usage: configuration-check profiles scratch");
    std::filesystem::path profiles = argv[1], scratch = argv[2];
    // Unique child prevents deletion or overwriting of prior evidence/user files.
    std::filesystem::create_directories(scratch);
    auto base = scratch;
    auto stem = std::to_string(GetCurrentProcessId());
    scratch = base / stem;
    for (unsigned suffix = 1; !std::filesystem::create_directory(scratch); ++suffix)
        scratch = base / (stem + "-" + std::to_string(suffix));
    auto area = [&](const char* name) {
        auto p = scratch / name;
        std::filesystem::create_directories(p);
        return p;
    };
    auto encoding = area("encoding");
    raw(encoding / "utf8.json", std::string("\xEF\xBB\xBF") + "{\"pl1\":80}");
    require(read_json(encoding / "utf8.json")["pl1"] == 80, "UTF-8 BOM input");
    auto files = manual_profiles(profiles);
    require(files.size() == 46, "Runtime profile count");
    for (const auto& f : files) {
        if (f.stem() == "IDY_7Y_defaults") {
            fails([&] { load_machine(f); });
            continue;
        }
        auto p = load_machine(f);
        apply_universal_defaults(p);
        require(!p.expanded_allowlist().empty(), "Allowlist");
        p.expanded_skip_verify();
        p.expanded_redirects();
        for (const auto& [_, b] : p.bitfields) {
            hex(b.reg);
            b.get_mask();
        }
    }
    auto select = [&](const std::string& board, std::optional<std::string> vendor, std::optional<int> raw_id) {
        Identity i;
        i.baseboard = board;
        i.cpu_vendor = vendor;
        if (raw_id) i.project_id = static_cast<uint8_t>(*raw_id);
        return select_profile(profiles, i);
    };
    require(select("YAOSHI Series-X6AR55xY", "intel", 26).path.stem() == "IDY_7Y", "Intel pattern/collision");
    require(select("X6FR5xxY", "amd", 26).path.stem() == "X6FR5xxY", "AMD pattern/collision");
    require(select("", "intel", 26).path.stem() == "IDY_7Y", "Intel shifted ID");
    require(select("", "amd", 26).path.stem() == "X6FR5xxY", "AMD shifted ID");
    require(select("", "intel", 13).method == "EC direct", "Direct before shifted");
    require(select("IDY_7Y", "amd", {}).path.empty(), "Filename vendor mismatch");
    require(select("xx-IDY_7Y-xx", {}, {}).method == "baseboard filename", "Unknown vendor permissive");
    require(select("xxIDY_7Yxx", {}, {}).path.empty(), "Filename bounded token");
    Identity unknown;
    unknown.cpu_vendor = "amd";
    auto manual = select_profile(profiles, unknown, [&](const auto& list) -> std::optional<std::filesystem::path> {
        require(list.size() == 46, "Unfiltered manual list");
        return profiles / "IDY_7Y.json";
    });
    require(manual.method == "manual selection", "Manual selection");
    require(select_profile(profiles, unknown).path.empty(), "Cancel manual selection");
    auto matching = area("matching");
    auto machine = read_json(profiles / "IDY_7Y.json");
    machine["cpuVendor"] = "intel";
    machine["baseboardPatterns"] = {"short"};
    machine["projectIdValue"] = 41;
    write_json(matching / "A.json", machine, false);
    machine["baseboardPatterns"] = {"short-long"};
    machine["projectIdValue"] = 42;
    write_json(matching / "B.json", machine, false);
    write_json(matching / "C.json", machine, false);
    raw(matching / "broken.json", "{");
    Identity match;
    match.baseboard = "short-long";
    match.cpu_vendor = "intel";
    match.project_id = uint8_t{41};
    require(select_profile(matching, match).path.stem() == "B", "Longest pattern before ID; ordinal tie");
    auto unicode_dir = area("unicode-matching");
    auto unicode_profile = machine;
    unicode_profile["baseboardPatterns"] = {"ÄΣK"};
    write_json(unicode_dir / L"Ä.JSON", unicode_profile, false);
    Identity unicode_id;
    unicode_id.cpu_vendor = "INTEL";
    unicode_id.baseboard = "äσk";
    require(select_profile(unicode_dir, unicode_id).path.filename() == L"Ä.JSON",
            "Case-insensitive Unicode pattern and upper-case extension");
    unicode_id.baseboard = "-ä-";
    require(select_profile(unicode_dir, unicode_id).method == "baseboard filename", "Unicode filename casing");
    unicode_id.baseboard = "界ä";
    require(select_profile(unicode_dir, unicode_id).path.empty(), "Unicode letter is not a filename-token boundary");
    unicode_id.baseboard = "٥ä";
    require(select_profile(unicode_dir, unicode_id).path.empty(), "Unicode decimal digit is not a token boundary");
    auto length_dir = area("utf16-pattern-length");
    unicode_profile["baseboardPatterns"] = {"é"};
    write_json(length_dir / "A.json", unicode_profile, false);
    unicode_profile["baseboardPatterns"] = {"ab"};
    write_json(length_dir / "B.json", unicode_profile, false);
    unicode_id.baseboard = "éab";
    require(select_profile(length_dir, unicode_id).path.stem() == "B",
            "Longest match counts UTF-16 units, not UTF-8 bytes");
    require(hex("0x10000") == 65536 && hex("ff") == 255 && hex("0XFF") == 255, "Hex with or without the prefix");
    for (const char* text : {"FFFFFFFF", " FF ", "-1", "+FF", "0x0xFF", "0x", ""}) fails([&] { hex(text); });
    Bitfield mask;
    mask.mask = "0x1FF";
    mask.bit = 0;
    require(mask.get_mask() == 255, "Explicit mask takes priority and casts to byte");
    auto invalid = area("invalid");
    raw(invalid / "oem.json", R"({"projectId":"archive","registers":{"x":"0x740"}})");
    fails([&] { load_machine(invalid / "oem.json"); });
    auto minimal = machine;
    minimal["limits"] = Json::object();
    minimal["bitfields"] = Json::object();
    minimal["verifyRedirect"] = Json::object();
    auto p = minimal.get<MachineProfile>();
    apply_universal_defaults(p);
    require(p.tgp_fallback && p.cpu_fallback && p.limits.size() == 9, "Whole-map conservative defaults");
    auto partial = machine;
    partial["limits"] = {{"PL1", {{"max", 100}}}};
    partial["verifyRedirect"] = {{"0x123", {{"readAddr", "0x124"}}}};
    auto pp = partial.get<MachineProfile>();
    apply_universal_defaults(pp);
    require(pp.limits.size() == 1 && pp.redirects.size() == 1 && !pp.tgp_fallback, "No individual map-entry merging");
    fails([&] {
        auto bad = machine;
        bad["limits"] = nullptr;
        bad.get<MachineProfile>();
    });
    auto tolerant = parse("{/*c*/\"cpu\":{\"pl1\":123},\"CPU\":{\"pl1\":44},\"unknown\":3}").get<UserProfile>();
    require(tolerant.cpu->pl1 == 123 && !Json(tolerant).contains("unknown"), "JSON comments/case/unknown");
    for (const auto& text :
         {R"({"cpu":{"pl1":null}})", R"({"cpu":{"pl1":true}})", R"({"cpu":{"pl1":1.0}})",
          R"({"cpu":{"pl1":2147483648}})", R"({"lighting":{"logoR":256}})", R"({"liquidCooler":{"pumpVoltage":"V8"}})"})
        fails([&] { parse(text).get<UserProfile>(); });
    for (const auto& text : {"[1,]", "{\"a\":1,}", "{\"a\":,}", "/*unterminated"}) fails([&] { parse(text); });
    auto defaults = parse("{}").get<UserProfile>();
    defaults.preflight("IDY_7Y");
    require(defaults.fan->switch_ms == 100 && defaults.protection->threshold == 50 && !defaults.cooler,
            "Omitted defaults");
    auto null_cpu = parse(R"({"cpu":null})").get<UserProfile>();
    require(!null_cpu.cpu, "Explicit null preserved");
    null_cpu.preflight("IDY_7Y"); // Saved while CPU control was unavailable; a load skips the group.
    fails([&] { parse(R"({"gpuFailsafe":null})").get<UserProfile>().preflight("IDY_7Y"); });
    fails([&] { parse(R"({"fan":{"cpuCurve":null}})").get<UserProfile>().preflight("IDY_7Y"); });
    auto foreign = defaults;
    foreign.project_id = "X6FR5xxY";
    fails([&] { foreign.preflight("IDY_7Y"); });
    foreign.project_id = "idy_7y";
    foreign.preflight("IDY_7Y");
    foreign.project_id = "";
    foreign.preflight("IDY_7Y");
    foreign.project_id = "Ä_MODEL";
    foreign.preflight("ä_model");
    foreign.project_id = "K";
    fails([&] { foreign.preflight("k"); });
    foreign.project_id = "\xC2\xA0";
    foreign.preflight("IDY_7Y");
    require(!defaults.gpu->bios_override_enabled && !defaults.gpu->nvvdd_ma &&
                Json(defaults)["gpu"]["boardInputMilliamps"].is_null(),
            "Nullable GPU rails preserved");
    Hardware h = collect_hardware(
        [](int a) -> uint8_t {
            if (a == 0x727) return 128;
            if (a == 0x7A4) return 128;
            if (a == 0x730) return 80;
            if (a == 0x731) return 100;
            if (a == 0x732) return 100;
            if (a == 0x7D8) return 95;
            return 0;
        },
        [](int i) -> uint32_t {
            const uint32_t b[]{150, 160, 180, 95, 50, 25, 87, 90, 255, 187, 0, 0};
            return b[i];
        });
    require(h.decoded() && h.liquid_cooling() && h.pl4_doubled() == true, "Hardware capture and capabilities");
    p = load_machine(profiles / "IDY_7Y.json");
    apply_universal_defaults(p);
    auto enriched = enrich(p, h);
    require(p.cpu_encoding() == "direct" && p.tgp_offset() == 90 && p.limits.at("PL4").max == 360 &&
                p.redirects.at("0x786").transform == "direct" && enriched.cpu_encoding_confirmed,
            "ECSpec enrichment");
    require(p.modes && p.modes->at("gaming").tcc_offset == 95 && p.modes->at("gaming").pl4 == 200,
            "EC mode defaults decode");
    p = load_machine(profiles / "IDY_7Y.json");
    p.verified = true;
    p.modes = std::map<std::string, Mode>{};
    apply_universal_defaults(p);
    auto verified_max = p.limits.at("PL1").max;
    enrich(p, h);
    require(p.limits.at("PL1").max == verified_max && p.cpu_encoding() == "direct" && p.modes->empty(),
            "Verified limits; hardware encoding; empty modes not replaced");
    h.ecspec->at(9) = 0;
    require(h.extract_limits().empty(), "ECSpec marker validated");
    auto read = [](int a) -> uint8_t {
        if (a == 0x463) return 10;
        if (a == 0x7A6) return 8;
        if (a == 0x743) return 7;
        if (a == 0x744) return 0;
        if (a == 0x746) return 25;
        if (a >= 0x769 && a <= 0x76B) return 0;
        return 100;
    };
    p = load_machine(profiles / "IDY_7Y.json");
    p.verified = true;
    apply_universal_defaults(p);
    require(!validate_startup(p, read, 1, {}, "amd").cpu_power(), "Verified vendor contradiction gated");
    require(validate_startup(p, read, 1, {}, "intel").cpu_power(), "Matching vendor enabled");
    p.verified = false;
    require(!validate_startup(p, read, 0, {}, "intel").gpu_power(), "Absent GPU gated");
    auto sentinel = [](int) -> uint8_t { return 0; };
    require(!validate_startup(p, sentinel, 1, {}, "intel").cpu_power(), "Unverified sanity gate");
    Enrichment ec;
    ec.limits = {"PL1"};
    require(validate_startup(p, sentinel, 1, ec, "intel").cpu_power(), "ECSpec stronger than sentinel");
    require(encoding::cpu_temperature(95, false) == 0x8A && encoding::cpu_temperature(95, true) == 0xDF, "CPU encode");
    require(encoding::decode_cpu_temperature(0x8A, false) == 95 && encoding::decode_cpu_temperature(0xDF, true) == 95,
            "CPU read high bit mask");
    require(encoding::pl4(200, true) == 100 && encoding::pl4(200, false) == 200 && encoding::battery(2, 0) == 32 &&
                encoding::battery(2, 8) == 40,
            "Runtime encodings");
    require(encoding::tgp(150, 90) == 60 && encoding::switch_speed(true, 300) == 0x83 && encoding::pl1(210) == 210 &&
                encoding::dynamic_boost(25) == 25,
            "Other encodings");
    fails([&] { encoding::pl1(211); });
    fails([&] { encoding::pl4(422, true); });
    fails([&] { encoding::tgp(89, 90); });
    fails([&] { encoding::dynamic_boost(26); });
    fails([&] { encoding::cpu_temperature(64, false); });
    fails([&] { encoding::cpu_temperature(121, true); });
    h.ecspec = std::array<uint32_t, 12>{253, 253, 250, 95, 50, 50, 87, 90, 255, 187, 0, 0};
    auto capped = h.extract_limits();
    require(capped.at("PL1").max == 210 && capped.at("PL2").max == 210 && capped.at("PL4").max == 420 &&
                capped.at("dynamicBoost").max == 25,
            "ECSpec maxima above the register ranges are lowered to them");

    auto persistence = area("persistence");
    ProfileStore store(persistence);
    require(store.settings().thermal.held == 0 && store.settings().last_slot == 0, "Missing settings clean");
    auto slot = defaults;
    slot.name = "Native fixture \xE2\x98\x83";
    slot.project_id = "IDY_7Y";
    slot.saved_at = "2026-11-03T12:00:00+00:00";
    slot.cpu = CpuSettings{80, 100, 200, 95};
    slot.cooler = CoolerSettings{};
    slot.cooler->voltage = PumpVoltage::V7;
    slot.cooler->rgb_mode = CoolerRgbMode::BreatheColor;
    slot.gpu->msvdd_ma = 120000;
    slot.gpu->bios_override_enabled = false;
    for (int i = 1; i <= 5; ++i) {
        store.save(i, slot);
        require(store.exists(i) && store.load(i)->cpu->pl1 == 80, "Five slots real round trip");
    }
    auto slot_before = read_json(persistence / "userprofile_4.json");
    {
        Lock lock(persistence / "userprofile_4.json");
        auto changed = slot;
        changed.cpu->pl1 = 123;
        fails([&] { store.save(4, changed); });
    }
    require(read_json(persistence / "userprofile_4.json") == slot_before && store.settings().last_slot == 5,
            "Failed slot replacement preserves committed slot and selection");
    store.rename(3, "Renamed");
    require(store.load(3)->name == slot.name, "Rename independent of slot contents");
    store.set_disable_dlppc(true);
    require(ProfileStore(persistence).settings().disable_dlppc, "Global DLPPC persistence");
    store.rename(5, "Keep name");
    auto before = read_json(persistence / "settings.json");
    {
        Lock lock(persistence / "settings.json");
        require(fails([&] { store.save(2, slot); }).find("was saved, but selected-slot") != std::string::npos,
                "Partial slot/selection error");
        require(store.settings().last_slot == 5 && store.load(2).has_value(),
                "Selection unchanged after slot committed");
        fails([&] { store.rename(1, "Failed"); });
        require(store.settings().names[0] == "Profile 1", "Rename rollback");
        fails([&] { store.set_disable_dlppc(false); });
        require(store.settings().disable_dlppc, "DLPPC rollback");
        require(read_json(persistence / "settings.json") == before, "Committed settings retained on replace failure");
        fails([&] { store.clear(5); });
        require(store.exists(5) && store.settings().last_slot == 5 && store.settings().names[4] == "Keep name",
                "Failed clear restores only changed selection/name and retains file");
    }
    // A slot temporary file is never a load candidate.
    write_json(persistence / "userprofile_1.json.tmp", slot, false);
    store.clear(1);
    require(!store.load(1), "Slot pending not auto-loaded");
    store.select(3);
    store.rename(3, "Delete failure");
    {
        Lock lock(persistence / "userprofile_3.json");
        fails([&] { store.clear(3); });
    }
    require(store.exists(3) && store.settings().last_slot == 0 && store.settings().names[2] == "Profile 3",
            "Clear deletion failure is not a cross-file transaction");
    auto legacy = Json(slot);
    legacy["gpuFailsafe"] = {{"enabled", false},
                             {"thresholdC", 35},
                             {"fanBoost", true},
                             {"reduceCpuPower", true},
                             {"showMainWindow", false}};
    write_json(persistence / "userprofile_1.json", legacy, false);
    fails([&] { store.load(1); });
    auto invalid_threshold = legacy;
    invalid_threshold["gpuFailsafe"]["thresholdC"] = 96;
    write_json(persistence / "userprofile_1.json", invalid_threshold, false);
    fails([&] { store.load(1); });
    write_json(persistence / "userprofile_1.json", legacy, false);
    auto operational = area("operational");
    ProfileStore thermal(operational);
    auto trip = hold(Cpu | Fan);
    Trip history;
    history.reason = "native fixture";
    history.actions = Cpu | Fan;
    history.time = "2026-11-03T12:00:00+00:00";
    trip.last_trip = history;
    thermal.save_thermal(trip);
    {
        Lock lock(operational / "settings.json");
        fails([&] { thermal.save_thermal({}); });
        require(thermal.settings().thermal.held == (Cpu | Fan), "Failed release retains write authority");
        require(ProfileStore(operational).settings().thermal.held == (Cpu | Fan),
                "Pending release cannot erase committed hold");
    }
    thermal.save_thermal({});
    require(ProfileStore(operational).settings().thermal.held == 0, "Durable release");
    {
        Lock lock(operational / "settings.json");
        fails([&] { thermal.save_thermal(hold(Gpu)); });
        require(thermal.settings().thermal.held == Gpu, "Failed trip retains in-memory hold");
        require(ProfileStore(operational).settings().thermal.held == Gpu, "Pending trip recovered");
    }
    thermal.save_thermal(trip);
    write_json(operational / "settings.json.tmp", Json{{"thermalProtection", hold(Gpu)}}, false);
    require(ProfileStore(operational).settings().thermal.held == All, "Union committed and pending obligations");
    auto uncertain_pending = area("uncertain-pending");
    write_json(uncertain_pending / "settings.json", Json{{"thermalProtection", ThermalState{}}}, false);
    ThermalState invalid_pending;
    invalid_pending.stage = Released;
    invalid_pending.held = Cpu | Fan;
    write_json(uncertain_pending / "settings.json.tmp", Json{{"thermalProtection", invalid_pending}}, false);
    require(ProfileStore(uncertain_pending, false).settings().thermal.held == (Cpu | Fan),
            "Invalid pending record cannot discard known non-GPU obligations");
    auto failed_create = area("failed-create");
    ProfileStore create_store(failed_create);
    create_store.save_thermal(hold(Cpu));
    std::filesystem::create_directory(failed_create / "settings.json.tmp");
    fails([&] { create_store.save_thermal({}); });
    require(create_store.settings().thermal.held == Cpu, "Failed temp creation retains release authority");
    require(ProfileStore(failed_create, false).settings().thermal.held == Cpu,
            "Unreadable pending state preserves known hold without GPU fallback");
    auto inspect = [&](const std::string& text, bool gpu = true) {
        auto d =
            area(("case-" + std::to_string(GetTickCount64()) + "-" + std::to_string(std::hash<std::string>{}(text)))
                     .c_str());
        raw(d / "settings.json", text);
        return ProfileStore(d, gpu).settings();
    };
    require(inspect(R"({"lastProfile":"bad","profileNames":[null," ","Custom"],"disableDlppc":null})").thermal.held ==
                0,
            "Cosmetic corruption not operational");
    require(inspect(R"({"profileNames":["\u00a0","Custom","\u2007","\u3000","\u202f"]})").names[0] == "Profile 1",
            "Unicode whitespace-only names repaired");
    require(
        inspect(
            R"({"thermalProtection":{"Stage":2,"Held":2,"LastTrip":{"Time":"bad","ThresholdC":"bad","Results":null}}})")
                .thermal.held == Cpu,
        "Optional-history corruption retains exact hold");
    require(inspect(R"({"thermalProtection":{"Held":2}})").thermal.held == (Gpu | Cpu),
            "Missing stage uncertain retains known held");
    require(inspect(R"({"thermalProtection":null})").thermal.held == Gpu, "Null operational group uncertain");
    require(inspect(R"({"thermalProtection":{"Stage":3,"Held":4}})").thermal.held == (Gpu | Fan),
            "Illegal operational stage preserves known hold");
    require(inspect(R"({"thermalProtection":{"Stage":2,"Held":8}})").thermal.held == Gpu,
            "Invalid action mask becomes recovery hold");
    require(inspect("{").thermal.held == Gpu && inspect("{", false).thermal.held == 0,
            "Corrupt settings model-derived fallback");
    require(inspect(R"({"thermalProtection":{"Stage":2,"Held":4}})", false).thermal.held == Fan,
            "Known held without GPU fallback");
    auto bounded = hold(Fan);
    bounded.recovery_details = std::string(9000, 'x');
    require(recover(bounded, {}, false, true).recovery_details.size() == 8192, "Bound recovery details");
    std::vector<std::string> diagnostics;
    ProfileStore state_store(persistence, true, [&](const std::string& message) { diagnostics.push_back(message); });
    auto warning_before = state_store.warning();
    auto diagnostic_count = diagnostics.size();
    LightingState lights;
    lights.keyboard->enabled = true;
    lights.keyboard->g = 128;
    lights.idle_seconds = 30;
    state_store.save_lighting(lights);
    require(state_store.load_lighting().keyboard->enabled, "Lighting direct round trip");
    raw(persistence / "lighting_state.json", R"({"keyboard":null})");
    require(!state_store.load_lighting().keyboard, "Explicit-null lighting defect remains visible");
    raw(persistence / "lighting_state.json", "null");
    require(state_store.load_lighting().keyboard.has_value(), "Whole-null lighting defaults");
    raw(persistence / "liquid_cooler_state.json", "{");
    require(state_store.load_cooler().g == 128, "Corrupt cooler default");
    require(diagnostics.size() == diagnostic_count + 1 && state_store.warning() == warning_before,
            "Peripheral load failure logs only, without global configuration warning");
    CoolerState cooler;
    cooler.address = 0xFEDCBA9876543210ULL;
    cooler.device_name = "Native fixture";
    cooler.voltage = PumpVoltage::V12;
    state_store.save_cooler(cooler);
    require(state_store.load_cooler().address == cooler.address, "Cooler uint64 round trip");
    require(Json(cooler)["pumpVoltage"] == 1 && Json(*slot.cooler)["pumpVoltage"] == 2 &&
                Json(*slot.cooler)["rgbMode"] == 3,
            "Cooler numeric enums wire order");
    {
        Lock lock(persistence / "liquid_cooler_state.json", false);
        state_store.save_cooler(cooler);
    }
    require(diagnostics.size() == diagnostic_count + 2 && diagnostics.back().find("save failed") != std::string::npos &&
                state_store.warning() == warning_before,
            "Direct save failure logs only, with no throw or global warning");
    {
        Lock lock(persistence / "lighting_state.json", false);
        state_store.save_lighting(lights);
    }
    require(diagnostics.size() == diagnostic_count + 3 && state_store.warning() == warning_before,
            "Lighting failure uses the same log-only contract");
    auto identity = collect_identity();
    auto detected = select_profile(profiles, identity);
    Json identity_json = {{"baseboard", identity.baseboard},
                          {"cpuName", identity.cpu_name},
                          {"cpuVendor", nullable(identity.cpu_vendor)},
                          {"selectedProfile", detected.path.filename().string()},
                          {"method", detected.method}};
    write_json(scratch / "identity.json", identity_json, false);
    std::cout
        << "PASS: 45 machine profiles + rejected reference-only file; detection, encodings, model preflight, actual failed/interrupted saves, thermal recovery, peripheral state\n"
        << "Evidence: " << scratch.string() << "\nIdentity: " << identity_json.dump() << "\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
