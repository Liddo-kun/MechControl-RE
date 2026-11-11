#include "protocol.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <thread>

namespace mech::cli {
std::array<std::uint8_t, 8> header(std::int32_t first, std::uint32_t length) {
    std::array<std::uint8_t, 8> bytes{};
    for (unsigned i = 0; i != 4; ++i) {
        bytes[i] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(first) >> (8 * i));
        bytes[i + 4] = static_cast<std::uint8_t>(length >> (8 * i));
    }
    return bytes;
}
std::int32_t integer(const std::uint8_t* bytes) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i != 4; ++i) value |= static_cast<std::uint32_t>(bytes[i]) << (8 * i);
    return static_cast<std::int32_t>(value);
}
std::vector<std::uint8_t> request(const std::vector<std::string>& args) {
    std::string payload;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) payload += '\0';
        payload += args[i];
    }
    if (payload.size() > maxRequestBytes) throw std::runtime_error("CLI request exceeds 64 KiB.");
    auto prefix = header(version, static_cast<std::uint32_t>(payload.size()));
    std::vector<std::uint8_t> bytes(prefix.begin(), prefix.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}
void transfer(HANDLE pipe, std::span<std::uint8_t> bytes, bool writing, Deadline deadline) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) throw std::runtime_error("Cannot allocate CLI I/O event.");
    while (!bytes.empty()) {
        OVERLAPPED io{};
        io.hEvent = event.get();
        ResetEvent(event.get());
        DWORD done = 0;
        DWORD length = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), MAXDWORD));
        BOOL ok = writing ? WriteFile(pipe, bytes.data(), length, &done, &io)
                          : ReadFile(pipe, bytes.data(), length, &done, &io);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                    .count();
            DWORD wait = WaitForSingleObject(event.get(),
                                             static_cast<DWORD>(std::clamp<std::int64_t>(remaining, 0, MAXDWORD - 1)));
            if (wait != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &io);
                GetOverlappedResult(pipe, &io, &done, TRUE); // Stack/event stay alive until cancellation completes.
                throw std::runtime_error("MechControl did not answer within the I/O timeout.");
            }
            ok = GetOverlappedResult(pipe, &io, &done, FALSE);
        }
        if (!ok || done == 0) throw std::runtime_error("Lost the connection to MechControl (incomplete frame).");
        bytes = bytes.subspan(done);
    }
}
Reply exchange(const std::vector<std::string>& args, const std::wstring& pipeName) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    auto bytes = request(args);
    const auto name = L"\\\\.\\pipe\\" + pipeName;
    auto connectDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    Handle pipe;
    do {
        pipe = Handle(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED, nullptr));
        if (pipe) break;
        auto error = GetLastError();
        if (error == ERROR_ACCESS_DENIED)
            return {3, "Access denied: run mcctl from an elevated (administrator) shell."};
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) return {3, "Cannot open the MechControl pipe."};
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    } while (std::chrono::steady_clock::now() < connectDeadline);
    if (!pipe) return {3, "MechControl is not running (or not responding)."};
    transfer(pipe.get(), bytes, true, deadline);
    std::array<std::uint8_t, 8> prefix;
    transfer(pipe.get(), prefix, false, deadline);
    auto length = integer(prefix.data() + 4);
    if (length < 0) throw std::runtime_error("Invalid response size.");
    std::vector<std::uint8_t> output;
    // Read incrementally instead of trusting a peer-supplied length for a large upfront allocation.
    std::array<std::uint8_t, 4096> chunk;
    while (length > 0) {
        auto size = std::min<std::size_t>(length, chunk.size());
        transfer(pipe.get(), std::span(chunk).first(size), false, deadline);
        output.insert(output.end(), chunk.begin(), chunk.begin() + size);
        length -= static_cast<std::int32_t>(size);
    }
    return {integer(prefix.data()), std::string(output.begin(), output.end())};
}
}
