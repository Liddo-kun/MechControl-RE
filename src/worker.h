#pragma once
#include "log.h"
#include "win_handle.h"
#include "ec_session.h"
#include "controller.h"
#include <future>
#include <windows.h>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace mech {
inline constexpr UINT resultMessage = WM_APP + 1;
enum class ResultKind { sample, command, ble, battery, failure, presentation, selection };
struct ModelSelection {
    std::vector<std::filesystem::path> paths;
    std::optional<std::filesystem::path> chosen;
    Handle completed{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
};
struct Result {
    ResultKind kind;
    std::uint64_t sequence = 0;
    std::wstring text;
    std::optional<Sensors> sensors;
    std::optional<ControlResult> control;
    std::shared_ptr<ModelSelection> selection;
};
// Producers own this mailbox, never the window or Application. The UI detaches before destruction.
class Delivery {
public:
    void attach(HWND window);
    void close();
    void post(Result result);
    std::vector<Result> take();
private:
    std::mutex mutex_;
    HWND window_ = nullptr;
    bool open_ = true;
    bool notified_ = false;
    std::vector<Result> pending_;
};
struct Command { std::uint64_t id; int value; std::chrono::milliseconds duration{0}; };
struct WorkerOptions {
    // Only hardware-free check executables set fixture. Production has no bypass switch.
    bool fixture=false;
    bool once=false;
    std::filesystem::path report;
    std::function<std::unique_ptr<EcChannel>(const std::wstring&,HANDLE)> channel_factory;
    std::optional<config::Identity> identity;
    bool read_only=false; // Injected read-only session checks; one-shot diagnostics always force this.
    std::filesystem::path state_directory=config::executable_directory();
    std::function<void(unsigned)> wait;
    bool nvidia_hardware=true; // Injected EC fixtures disable physical NVIDIA startup too.
    bool peripheral_hardware=true;
    std::function<std::unique_ptr<nv::Power>()> nvidia_factory; // Controller/restart workflow fixtures only.
    Lighting::Send lighting_send; // Hardware-free delayed HID and controller recovery fixtures only.
};
class ControlWorker {
public:
    ControlWorker(std::shared_ptr<Delivery> delivery, Log& log,WorkerOptions options={});
    ~ControlWorker();
    bool submit(Command command);
    std::future<ControlResult> request(ControlCommand command,std::uint64_t sequence=0);
    void stop();
    int wait();
private:
    void run(std::stop_token stop);
    std::shared_ptr<Delivery> delivery_;
    Log& log_;
    WorkerOptions options_;
    int outcome_=0;
    std::mutex mutex_;
    struct Pending { ControlCommand command; std::promise<ControlResult> reply; std::uint64_t sequence=0; };
    std::deque<std::variant<Command,Pending>> commands_;
    bool closing_ = false;
    Handle wake_;
    Handle stopping_;
    Handle timer_;
    std::jthread thread_;
};
}
