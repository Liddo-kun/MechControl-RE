#include "protocol.h"
#include <sddl.h>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;
namespace {
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); std::cout << "PASS " << text << '\n'; }
}
int wmain() {
    try {
        auto fixture = mech::cli::request({"status", "--json"});
        require(fixture == std::vector<std::uint8_t>{1,0,0,0,13,0,0,0,'s','t','a','t','u','s',0,'-','-','j','s','o','n'}, "version 1 golden UTF-8/NUL request");
        require(mech::cli::request({}) == std::vector<std::uint8_t>{1,0,0,0,0,0,0,0}, "zero-length request fixture");
        require(mech::cli::request({"", "x", ""}).size() == 11, "empty argument framing preserved");
        const auto name = L"MechControl.Native.ProtocolFixture." + std::to_wstring(GetCurrentProcessId());
        auto path = L"\\\\.\\pipe\\" + name;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr), "administrator/SYSTEM fixture ACL");
        SECURITY_ATTRIBUTES security{sizeof security, descriptor, FALSE};
        mech::Handle pipe(CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, &security));
        LocalFree(descriptor);
        require(static_cast<bool>(pipe), "isolated first-instance pipe created");
        mech::Handle squatter(CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE, 1, 4096, 4096, 0, nullptr));
        require(!squatter, "second first-instance pipe refused");
        auto server = std::async(std::launch::async, [&] {
            for (int round = 0; round != 3; ++round) {
                mech::Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                OVERLAPPED connect{}; connect.hEvent = event.get();
                BOOL ok = ConnectNamedPipe(pipe.get(), &connect);
                auto error = ok ? ERROR_SUCCESS : GetLastError();
                if (!ok && error == ERROR_IO_PENDING) {
                    DWORD done;
                    if (WaitForSingleObject(event.get(), 5000) != WAIT_OBJECT_0) {
                        CancelIoEx(pipe.get(), &connect); GetOverlappedResult(pipe.get(), &connect, &done, TRUE);
                        throw std::runtime_error("fixture connect timeout");
                    }
                    if (!GetOverlappedResult(pipe.get(), &connect, &done, FALSE)) throw std::runtime_error("fixture connect failed");
                } else if (!ok && error != ERROR_PIPE_CONNECTED) throw std::runtime_error("fixture connect failed");
                auto deadline = std::chrono::steady_clock::now() + 5s;
                std::array<std::uint8_t, 8> header;
                mech::cli::transfer(pipe.get(), header, false, deadline);
                auto length = mech::cli::integer(header.data() + 4);
                if (length < 0 || length > 65536) throw std::runtime_error("bad fixture request length");
                std::vector<std::uint8_t> payload(length);
                mech::cli::transfer(pipe.get(), payload, false, deadline);
                auto prefix = mech::cli::header(round == 1 ? 2 : 0, round == 2 ? 5 : 7);
                std::vector<std::uint8_t> response(prefix.begin(), prefix.end());
                std::string text = round == 2 ? "cut" : "fixture";
                response.insert(response.end(), text.begin(), text.end());
                // Force real fragmented response delivery, one byte at a time.
                for (auto& byte : response) mech::cli::transfer(pipe.get(), std::span(&byte, 1), true, deadline);
                if (round != 2) {
                    std::uint8_t eof;
                    try { mech::cli::transfer(pipe.get(), std::span(&eof, 1), false, deadline); } catch (const std::exception&) {}
                }
                DisconnectNamedPipe(pipe.get());
            }
        });
        auto first = mech::cli::exchange({"status", "--json"}, name);
        require(first.code == 0 && first.text == "fixture", "fragmented response round-trip");
        auto second = mech::cli::exchange({"invalid"}, name);
        require(second.code == 2 && second.text == "fixture", "server exit code and pipe reuse");
        bool incomplete = false;
        try { mech::cli::exchange({"status"}, name); } catch (const std::exception&) { incomplete = true; }
        require(incomplete, "truncated response rejected");
        server.get();
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
