#include "protocol.h"
#include <iostream>
#include <string_view>
#include <thread>

namespace {
constexpr auto usage = R"(mcctl - control a running MechControl (elevated shell required)

  mcctl status [--json]
  mcctl watch [--interval ms] [--json]  (default 1000, minimum 250)
  mcctl gpu [--tgp W] [--db W|on|off] [--nvvdd A] [--msvdd A] [--board-input A] [--json]
  mcctl cpu [--pl1 W] [--pl2 W] [--pl4 W] [--temp-limit C] [--json]
  mcctl fan boost on|off [--json]
  mcctl ec-bypass on|off [--json]    (EC power-limit bypass; turns itself off on sleep, display off, AC loss, exit)
  mcctl cooler-flag on|off [--json]  (EC liquid cooler connected flag)

Changes are live and unsaved. The running controller validates commands and ranges.
Exit codes: 0 done, 1 refused or failed, 2 usage error, 3 MechControl not reachable.
)";
std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    int length =
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string text(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), text.data(), length, nullptr,
                        nullptr);
    return text;
}
int print(mech::cli::Reply reply) {
    if (!reply.text.empty()) (reply.code == 0 ? std::cout : std::cerr) << reply.text << '\n';
    return reply.code;
}
}
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(utf8(argv[i]));
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h" || args[0] == "/?") {
        std::cout << usage;
        return args.empty() ? 2 : 0;
    }
    try {
        if (args[0] != "watch") return print(mech::cli::exchange(args));
        int interval = 1000;
        bool json = false;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--json")
                json = true;
            else if (args[i] == "--interval" && i + 1 < args.size()) {
                std::size_t parsed = 0;
                interval = std::stoi(args[++i], &parsed);
                if (parsed != args[i].size() || interval < 250)
                    return print({2, "Usage: mcctl watch [--interval ms] [--json] (interval 250 ms or more)"});
            } else
                return print({2, "Usage: mcctl watch [--interval ms] [--json] (interval 250 ms or more)"});
        }
        // Keep the default Ctrl+C behavior, including when a controller is not responding.
        for (;;) {
            auto reply = mech::cli::exchange(json ? std::vector<std::string>{"status", "--json"}
                                                  : std::vector<std::string>{"status"});
            auto code = reply.code;
            if (!json && code == 0) {
                SYSTEMTIME time{};
                GetLocalTime(&time);
                char text[16];
                sprintf_s(text, "%02u:%02u:%02u", time.wHour, time.wMinute, time.wSecond);
                std::cout << text << '\n';
            }
            print(std::move(reply));
            if (code) return code;
            if (!json) std::cout << '\n';
            std::cout.flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        }
    } catch (const std::invalid_argument&) {
        return print({2, "Invalid watch interval."});
    } catch (const std::out_of_range&) {
        return print({2, "Invalid watch interval."});
    } catch (const std::exception& error) {
        return print({3, error.what()});
    }
}
