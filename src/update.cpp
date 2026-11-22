#include "update.h"
#include "config_text.h"
#include <winhttp.h>
#include <charconv>
#include <thread>

namespace mech::update {
namespace {
struct Close {
    void operator()(HINTERNET h) const { WinHttpCloseHandle(h); }
};
using Internet = std::unique_ptr<void, Close>;
// Only the log shows it; WinHTTP's codes are listed in winhttp.h (12007: the name did not resolve).
[[noreturn]] void fail(const char* step) {
    throw std::runtime_error(std::string(step) + ": error " + std::to_string(GetLastError()));
}
Internet require(HINTERNET h, const char* step) {
    if (!h) fail(step);
    return Internet(h);
}
void require(BOOL ok, const char* step) {
    if (!ok) fail(step);
}
// GitHub's API wants a User-Agent, which WinHTTP sends from the agent name.
std::string latestReply() {
    auto agent = L"MechControl/" + text(current());
    auto session = require(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0),
                           "Opening WinHTTP");
    require(WinHttpSetTimeouts(session.get(), 15000, 15000, 15000, 15000), "Setting timeouts");
    auto connection =
        require(WinHttpConnect(session.get(), L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0), "Connecting");
    auto request =
        require(WinHttpOpenRequest(connection.get(), L"GET", L"/repos/Liddo-kun/MechControl-RE/releases/latest",
                                   nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE),
                "Opening the request");
    require(WinHttpSendRequest(request.get(), L"Accept: application/vnd.github+json", DWORD(-1),
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0),
            "Sending the request");
    require(WinHttpReceiveResponse(request.get(), nullptr), "Receiving the reply");
    DWORD status = 0, size = sizeof status;
    require(WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX),
            "Reading the status");
    if (status != 200) throw std::runtime_error("GitHub answered HTTP " + std::to_string(status));
    std::string reply;
    for (;;) {
        char buffer[8192];
        DWORD read = 0;
        require(WinHttpReadData(request.get(), buffer, sizeof buffer, &read), "Reading the reply");
        if (!read) break;
        reply.append(buffer, read);
        if (reply.size() > (1 << 20)) throw std::runtime_error("GitHub's reply is too long");
    }
    return reply;
}
}
std::optional<Version> parse(std::string_view tag) {
    if (tag.starts_with('v')) tag.remove_prefix(1);
    Version v{};
    for (size_t i = 0; i < v.size(); ++i) {
        if (tag.empty() || tag.front() < '0' || tag.front() > '9') return {};
        auto [end, error] = std::from_chars(tag.data(), tag.data() + tag.size(), v[i]);
        if (error != std::errc{} || end == tag.data()) return {};
        tag.remove_prefix(size_t(end - tag.data()));
        if (tag.empty()) return i ? std::optional(v) : std::nullopt; // At least major.minor.
        if (tag.front() != '.' || i == v.size() - 1) return {};
        tag.remove_prefix(1);
    }
    return {};
}
std::wstring text(const Version& v) {
    return std::to_wstring(v[0]) + L"." + std::to_wstring(v[1]) + L"." + std::to_wstring(v[2]);
}
Version current() {
    static const auto v = parse(MECHCONTROL_VERSION);
    return v.value();
}
std::string latestTag(std::string_view reply) {
    auto j = config::Json::parse(reply, nullptr, false);
    if (!j.is_object() || !j.contains("tag_name") || !j["tag_name"].is_string())
        throw std::runtime_error("GitHub's reply names no release");
    return j["tag_name"].get<std::string>();
}
void check(std::shared_ptr<Delivery> delivery) {
    std::thread([delivery] {
        Result r{ResultKind::update};
        try {
            r.text = config::wide(latestTag(latestReply()));
        } catch (const std::exception& e) {
            r.kind = ResultKind::updateFailed;
            r.text = config::wide(e.what());
        }
        delivery->post(std::move(r));
    }).detach();
}
}
