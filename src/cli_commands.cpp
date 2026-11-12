#include "controller.h"
#include "config_text.h"
#include <algorithm>
#include <charconv>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

// mcctl requests: arguments become the typed commands the UI sends, and replies format the status JSON.
namespace mech {
using namespace config;
static int whole(const std::string& text) {
    int v = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty() || text[0] == '-' || v < 0)
        throw std::invalid_argument("Expected a nonnegative whole number");
    return v;
}
static int milliamps(std::string text) {
    int scale = 1000;
    auto lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](char c) { return static_cast<char>(tolower(static_cast<unsigned char>(c))); });
    if (lower.ends_with("ma")) {
        text.resize(text.size() - 2);
        scale = 1;
    } else if (lower.ends_with('a'))
        text.pop_back();
    // Decimal arithmetic: accepting a binary float and rounding would lose exact mA.
    auto dot = text.find('.');
    auto integral = dot == 0 ? 0 : whole(text.substr(0, dot));
    int64_t value = static_cast<int64_t>(integral) * scale;
    if (dot != std::string::npos) {
        auto fraction = text.substr(dot + 1);
        int places = scale == 1000 ? 3 : 0;
        for (char c : fraction) {
            if (c < '0' || c > '9') throw std::invalid_argument("Invalid current");
            if (places > 0) {
                scale /= 10;
                value += (c - '0') * scale;
                --places;
            } else if (c != '0')
                throw std::invalid_argument("Current must be exactly representable to 1 mA");
        }
    }
    if (value <= 0 || value > INT_MAX) throw std::invalid_argument("Current must be positive and fit mA");
    return static_cast<int>(value);
}
static std::string text_status(const Json& j) {
    std::ostringstream out;
    auto amps = [&](const Json& value) {
        return value.is_null() ? "--" : decimal_text(value.get<int>() / 1000.0, 3, true) + " A";
    };
    auto watts = [&](const Json& value) {
        return value.is_null() ? "--" : decimal_text(value.get<int64_t>() / 1000.0, 1) + " W";
    };
    auto mhz = [](const Json& value) { return value.is_null() ? "--" : std::to_string(value.get<int>()) + " MHz"; };
    auto setting_text = [](const Json& v, const char* unit) {
        return std::to_string(v.at("value").get<int>()) + " " + unit + " (" + std::to_string(v.at("min").get<int>()) +
               "-" + std::to_string(v.at("max").get<int>()) + " " + unit + ")";
    };
    auto advanced = [&] {
        auto& b = j.at("ecBypass");
        auto& f = j.at("coolerFlag");
        auto on = [](bool v) { return v ? "on" : "off"; };
        auto status = b.at("status").get<std::string>();
        out << "EC    power-limit bypass "
            << (!b.at("available").get<bool>() ? "unavailable (" + b.at("reason").get<std::string>() + ")"
                : !status.empty()              ? status
                                               : "Off")
            << "; liquid cooler flag ";
        if (!f.at("available").get<bool>())
            out << "unavailable";
        else
            out << on(f.at("connected").get<bool>());
    };
    if (j.contains("model")) {
        out << "Model " << j.at("model").get<std::string>()
            << "; thermal failsafe: " << j.at("failsafe").at("state").get<std::string>() << "\r\n";
        for (const char* group : {"gpu", "cpu", "fans"})
            out << (j.at(group).is_null()
                        ? std::string(group == std::string("gpu") ? "GPU" : "CPU") + "   power control unavailable"
                        : text_status(j.at(group)))
                << "\r\n";
        advanced();
    } else if (j.contains("ecBypass")) {
        advanced();
    } else if (j.contains("pl1")) {
        out << "CPU   PL1 " << setting_text(j.at("pl1"), "W") << "; PL2 " << setting_text(j.at("pl2"), "W") << "; PL4 "
            << setting_text(j.at("pl4"), "W") << "; temp limit " << setting_text(j.at("tempLimit"), "C");
    } else if (j.contains("tgp")) {
        out << "GPU   TGP " << setting_text(j.at("tgp"), "W") << "; Dynamic Boost "
            << (j.at("dynamicBoost").get<bool>() ? "on" : "off") << ", " << setting_text(j.at("dynamicBoostWatts"), "W")
            << "; unlock "
            << (j.at("unlocked").get<bool>()          ? "on"
                : j.at("unlockAvailable").get<bool>() ? "off"
                                                      : "unavailable");
        if (!j.at("nvidia").is_null())
            out << "\r\n      NVIDIA: " << j.at("nvidia").get<std::string>();
        else {
            out << "\r\n      NVIDIA ceiling " << j.at("ceilingWatts") << " W; enforced " << j.at("enforcedWatts")
                << " W; requested "
                << (j.at("requestedWatts").is_null() ? "none"
                                                     : std::to_string(j.at("requestedWatts").get<int>()) + " W");
            for (auto [key, name] :
                 {std::pair("nvvdd", "NVVDD"), std::pair("msvdd", "MSVDD"), std::pair("boardInput", "Board input")}) {
                const auto& r = j.at(key);
                out << "\r\n      " << name;
                if (r.is_null()) {
                    out << " current limit: not available on this GPU";
                    continue;
                }
                out << " limit " << amps(r.at("limitMa")) << " ("
                    << decimal_text(r.at("minMa").get<int>() / 1000.0, 3, true) << '-' << amps(r.at("maxMa"))
                    << ", stock " << amps(r.at("stockMa")) << "); measured " << amps(r.at("measuredMa"));
                if (std::string_view(key) != "boardInput") out << "; at clock cap " << amps(r.at("atClockCapMa"));
            }
            out << "\r\n      Clock caps: current controller " << mhz(j.at("currentCapMhz")) << "; power controller "
                << mhz(j.at("powerCapMhz")) << "; max " << mhz(j.at("maxClockMhz"));
            Json other = nullptr;
            if (!j.at("gpuPowerMw").is_null() && !j.at("corePowerMw").is_null())
                other = j.at("gpuPowerMw").get<int64_t>() - j.at("corePowerMw").get<int64_t>();
            out << "\r\n      Clock " << mhz(j.at("clockMhz")) << "; power " << watts(j.at("gpuPowerMw"))
                << " (core rails " << watts(j.at("corePowerMw")) << ", memory and other " << watts(other)
                << "); board input limiter (IPC) active ";
            if (j.at("boardInputLimiterActivity").is_null())
                out << "--";
            else
                out << std::fixed << std::setprecision(1) << j.at("boardInputLimiterActivity").get<double>() * 100
                    << '%';
        }
    } else {
        out << "Fans  boost " << (j.at("boost").is_null() ? "unavailable" : j.at("boost").get<bool>() ? "on" : "off");
        for (const char* prefix : {"cpu", "gpu"}) {
            std::string p = prefix;
            out << "; " << (p == "cpu" ? "CPU" : "GPU") << ' ' << j.at(p + "Rpm").get<int>() << " RPM, "
                << j.at(p + "DutyPercent").get<int>() << "%, " << j.at(p + "TempC").get<int>() << " C";
        }
    }
    return out.str();
}
cli::Reply Controller::cli(const std::vector<std::string>& arguments) {
    std::vector<std::string> args;
    bool json = false;
    for (const auto& a : arguments)
        if (a == "--json")
            json = true;
        else
            args.push_back(a);
    if (args.size() == 1 && args[0] == "status") {
        auto j = status();
        return {0, json ? j.dump() : text_status(j)};
    }
    std::optional<ControlCommand> command;
    std::string group;
    try {
        if (args.empty()) throw std::invalid_argument("Expected status, cpu, gpu, fan boost, ec-bypass or cooler-flag");
        if (args[0] == "fan") {
            if (args.size() != 3 || args[1] != "boost" || (args[2] != "on" && args[2] != "off"))
                throw std::invalid_argument("Usage: fan boost on|off");
            command = FanBoost{args[2] == "on"};
            group = "fans";
        } else if (args[0] == "ec-bypass" || args[0] == "cooler-flag") {
            if (args.size() != 2 || (args[1] != "on" && args[1] != "off"))
                throw std::invalid_argument("Usage: " + args[0] + " on|off");
            if (args[0] == "ec-bypass")
                command = EcBypassApply{args[1] == "on"};
            else
                command = CoolerFlag{args[1] == "on"};
            group = "ec";
        } else if (args[0] == "cpu" || args[0] == "gpu") {
            bool is_cpu = args[0] == "cpu";
            if (args.size() < 3 || args.size() % 2 == 0)
                throw std::invalid_argument("Expected one or more option/value pairs");
            std::map<std::string, std::string> options;
            const std::set<std::string> cpu_options = {"--pl1", "--pl2", "--pl4", "--temp-limit"},
                                        gpu_options = {"--tgp", "--db", "--nvvdd", "--msvdd", "--board-input"};
            const auto& allowed = is_cpu ? cpu_options : gpu_options;
            for (size_t i = 1; i < args.size(); i += 2) {
                if (!allowed.contains(args[i]) || !options.emplace(args[i], args[i + 1]).second)
                    throw std::invalid_argument("Unknown or duplicate option: " + args[i]);
            }
            // Match source availability/read/usage ordering, but preflight all values before writes.
            if (is_cpu) {
                if (!features_.cpu_power()) return {1, "CPU control unavailable"};
                auto c = cpu();
                for (const auto& [key, value] : options) {
                    int n = whole(value);
                    if (key == "--pl1")
                        c.pl1 = n;
                    else if (key == "--pl2")
                        c.pl2 = n;
                    else if (key == "--pl4")
                        c.pl4 = n;
                    else
                        c.temp_limit = n;
                }
                command = CpuApply{c};
                group = "cpu";
            } else {
                if (!features_.gpu_power()) return {1, "GPU control unavailable"};
                auto g = gpu(true); // Intentional Apply reads the current F8 request; passive status does not.
                if (auto it = options.find("--tgp"); it != options.end()) g.tgp = whole(it->second);
                if (auto it = options.find("--db"); it != options.end()) {
                    auto& value = it->second;
                    g.dynamic_boost_enabled = value != "off";
                    if (value != "on" && value != "off") g.dynamic_boost_watts = whole(value);
                }
                bool power = options.contains("--tgp") || options.contains("--db");
                if (power) gpu_plan(g);
                if (options.contains("--nvvdd") || options.contains("--msvdd") || options.contains("--board-input")) {
                    if (!nvidia_ || nvidia_busy_) return {1, nvidia_unavailable()};
                    // Read availability first, as the source does; parse each current once.
                    nvidia_work([&] { nvidia_->currents(); });
                    if (auto it = options.find("--nvvdd"); it != options.end()) g.nvvdd_ma = milliamps(it->second);
                    if (auto it = options.find("--msvdd"); it != options.end()) g.msvdd_ma = milliamps(it->second);
                    if (auto it = options.find("--board-input"); it != options.end())
                        g.board_input_ma = milliamps(it->second);
                }
                command = GpuApply{g, power};
                group = "gpu";
            }
        } else
            throw std::invalid_argument("Unknown command: " + args[0]);
    } catch (const std::invalid_argument& ex) {
        return {2, ex.what()};
    } catch (const std::exception& ex) {
        diagnostic_("[CLI] Not applied: " + args[0] + "; " + ex.what());
        return {1, ex.what()};
    }
    auto result = execute(*command);
    diagnostic_(std::string(result.code ? "[CLI] Not applied: " : "[CLI] Applied: ") + args[0] +
                (result.code ? "; " + result.text : ""));
    if (result.code) return {result.code, result.text};
    auto j = group == "cpu"   ? cpu_status()
             : group == "gpu" ? gpu_status()
             : group == "ec"  ? advanced_status()
                              : fans_status();
    return {0, json ? j.dump() : text_status(j)};
}
ControlResult Controller::run(const CliCommand& c) {
    auto reply = cli(c.arguments);
    return {.code = reply.code,
            .text = reply.text,
            .applied = reply.code == 0 && writes_started_,
            .partial = reply.code != 0 && writes_started_};
}
}
