#include "worker.h"
#include "counters.h"
#include "config_text.h"
#include <algorithm>
#include <utility>
#include <stdexcept>

namespace mech {
void Delivery::attach(HWND window) {
    std::lock_guard lock(mutex_);
    window_ = window;
}
void Delivery::close() {
    std::lock_guard lock(mutex_);
    open_ = false;
    window_ = nullptr;
    pending_.clear();
}
void Delivery::post(Result result) {
    std::lock_guard lock(mutex_);
    if (!open_) return;
    if (result.kind == ResultKind::sample || result.kind == ResultKind::battery ||
        result.kind == ResultKind::presentation) {
        auto previous =
            std::find_if(pending_.begin(), pending_.end(), [&](const Result& r) { return r.kind == result.kind; });
        if (previous != pending_.end())
            *previous = std::move(result);
        else
            pending_.push_back(std::move(result));
    } else
        pending_.push_back(std::move(result));
    if (window_ && !notified_) notified_ = PostMessageW(window_, resultMessage, 0, 0) != FALSE;
}
std::vector<Result> Delivery::take() {
    std::lock_guard lock(mutex_);
    notified_ = false;
    return std::exchange(pending_, {});
}
ControlWorker::ControlWorker(std::shared_ptr<Delivery> delivery, Log& log, WorkerOptions options)
    : delivery_(std::move(delivery)), log_(log), options_(std::move(options)),
      wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)), stopping_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      timer_(options_.once ? nullptr : CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS)) {
    if (!wake_ || !stopping_ || (!options_.once && !timer_))
        throw std::runtime_error("Cannot create worker wait handles.");
    if (!options_.once) {
        LARGE_INTEGER due{};
        due.QuadPart = -1;
        // One ordinary-resolution timer. Windows may coalesce its wakeup by up to 100 ms.
        if (!SetWaitableTimerEx(timer_.get(), &due, 1000, nullptr, nullptr, nullptr, 100))
            throw std::runtime_error("Cannot schedule the control worker.");
    }
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}
ControlWorker::~ControlWorker() { stop(); }
std::future<ControlResult> ControlWorker::request(ControlCommand command, std::uint64_t sequence) {
    std::promise<ControlResult> promise;
    auto future = promise.get_future();
    std::lock_guard lock(mutex_);
    if (closing_ || options_.once || options_.read_only)
        promise.set_value({1, "Controller unavailable or stopping"});
    else {
        // Clearing the EC bypass before sleep, display off or exit goes ahead of queued commands.
        bool first = std::holds_alternative<PowerTransition>(command);
        Pending pending{std::move(command), std::move(promise), sequence};
        if (first)
            commands_.push_front(std::move(pending));
        else
            commands_.push_back(std::move(pending));
        SetEvent(wake_.get());
    }
    return future;
}
// Answers the queued commands and accepts no more.
void ControlWorker::refuse(const char* reason) {
    std::lock_guard lock(mutex_);
    closing_ = true;
    for (auto& p : commands_) p.reply.set_value({1, reason});
    commands_.clear();
}
void ControlWorker::stop() {
    refuse("Controller stopping");
    thread_.request_stop();
    SetEvent(stopping_.get());
    if (thread_.joinable()) thread_.join();
}
int ControlWorker::wait() {
    if (thread_.joinable()) thread_.join();
    return outcome_;
}
void ControlWorker::run(std::stop_token stop) {
    try {
        std::unique_ptr<EcSession> session;
        std::unique_ptr<Controller> controller;
        unsigned dirty_pages = 0x7f;
        std::optional<Counters> counters;
        unsigned published_state = 0;
        auto presentation = [&](bool live = false) {
            if (!controller) return;
            ControlResult r;
            r.peripherals = config::Json{{"ui", controller->presentation(false, live, controller->ui_page())}};
            delivery_->post({ResultKind::presentation, 0, L"", {}, std::move(r)});
        };
        auto page_read = [&](int page) {
            auto r = controller->execute(UiRead{page});
            delivery_->post({ResultKind::command, 0, L"", {}, std::move(r)});
            if (page >= 0) dirty_pages &= ~(1u << page);
        };
        auto refresh_visible = [&] {
            if (controller && controller->visible() && (dirty_pages & (1u << controller->ui_page())))
                page_read(controller->ui_page());
        };
        auto publish_state = [&] {
            if (!controller) return;
            // Lost-protection alerts count too: protection can be lost and restored within one command.
            unsigned state = (controller->thermal().tripped() ? 1u : 0u) |
                             (controller->thermal().complete() ? 2u : 0u) | (controller->monitor_stopped() ? 4u : 0u) |
                             (unsigned(controller->thermal().lost_alerts()) << 3);
            if (state == published_state) return;
            bool monitor_changed = ((state ^ published_state) & 4) != 0;
            published_state = state;
            std::string text = controller->thermal().status();
            if (monitor_changed)
                text += controller->monitor_stopped() ? "; EC monitoring stopped; retry in 60 s"
                                                      : "; EC monitoring restarted";
            delivery_->post({ResultKind::command, 0, config::wide(text)});
            page_read(2);
            presentation();
        };
        std::uint64_t samples = 0;
        unsigned failures = 0;
        auto sample = [&] {
            auto shared = controller ? controller->tick() : std::optional(session->sample(Detail::rpm));
            if (controller)
                if (auto battery = controller->take_battery()) {
                    ControlResult result;
                    result.peripherals = config::Json{{"battery", *battery}};
                    delivery_->post({ResultKind::battery, 0, L"Battery telemetry", {}, std::move(result)});
                }
            publish_state();
            if (!shared) return;
            failures = 0;
            counters->publish(shared->cpu_rpm, shared->gpu_rpm);
            ++samples;
            if (!controller || controller->visible())
                delivery_->post(
                    {ResultKind::sample, samples,
                     L"EC fan RPM: " + std::to_wstring(shared->cpu_rpm) + L" / " + std::to_wstring(shared->gpu_rpm),
                     *shared});
            if (controller && controller->visible()) presentation(controller->gpu_presentation());
        };
        ChannelFactory factory = [this](const std::wstring& path) {
            return options_.channel_factory ? options_.channel_factory(path, stopping_.get())
                                            : open_ec_channel(path, stopping_.get());
        };
        session = std::make_unique<EcSession>(
            factory, [this](const std::string& text) { log_.write(text); }, options_.once || options_.read_only,
            [this](unsigned ms) {
                if (WaitForSingleObject(stopping_.get(), ms) == WAIT_OBJECT_0)
                    throw std::system_error(ERROR_OPERATION_ABORTED, std::system_category(), "EC shutdown");
            },
            config::profiles_directory(), options_.identity,
            [&](const std::vector<std::filesystem::path>& paths) -> std::optional<std::filesystem::path> {
                if (options_.once || options_.read_only)
                    throw std::runtime_error(
                        "Automatic model detection failed; read-only diagnostics cannot select a model interactively");
                auto selection = std::make_shared<ModelSelection>();
                selection->paths = paths;
                if (!selection->completed) throw std::runtime_error("Cannot create model selection event");
                Result result{ResultKind::selection};
                result.selection = selection;
                delivery_->post(std::move(result));
                HANDLE waits[]{selection->completed.get(), stopping_.get()};
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0)
                    throw std::runtime_error("Model selection canceled during shutdown");
                return selection->chosen;
            });
        if (!options_.report.empty()) config::write_json(options_.report, session->diagnostic_report(), false);
        delivery_->post(
            {ResultKind::command, 0,
             (options_.once || options_.read_only ? L"Read-only EC session: " : L"Preparing core controller: ") +
                 config::wide(session->profile.project_id)});
        if (options_.once) return;
        counters.emplace();
        if (!options_.read_only) {
            // Only once the OEM EC driver has opened and the model is identified.
            if (options_.peripheral_hardware) ensure_uefi_flags([this](const std::string& text) { log_.write(text); });
            auto run_blocking = [&](std::function<void()> work) {
                Handle completed(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                if (!completed) throw std::runtime_error("Cannot create device-operation completion event");
                std::exception_ptr failure;
                std::jthread operation([&] {
                    try {
                        work();
                    } catch (...) {
                        failure = std::current_exception();
                    }
                    SetEvent(completed.get());
                });
                HANDLE waits[]{completed.get(), timer_.get(), stopping_.get(), wake_.get()};
                for (;;) {
                    auto ready = WaitForMultipleObjects(4, waits, FALSE, INFINITE);
                    if (ready == WAIT_OBJECT_0) break;
                    if (ready == WAIT_OBJECT_0 + 2 || ready == WAIT_FAILED) {
                        WaitForSingleObject(completed.get(), INFINITE);
                        break;
                    }
                    // Reuse the existing joined NVIDIA wait for blocking HID/WMI too.
                    // Only the operation moves; EC access and the shared timer stay here.
                    // Ordinary feature writes stay queued; lifecycle notifications
                    // remain responsive without touching the in-flight device owner.
                    if (ready == WAIT_OBJECT_0 + 3 && controller) {
                        std::deque<Pending> notifications;
                        {
                            std::lock_guard lock(mutex_);
                            for (auto i = commands_.begin(); i != commands_.end();) {
                                auto& c = i->command;
                                if (std::holds_alternative<Visibility>(c) || std::holds_alternative<UiPage>(c) ||
                                    std::holds_alternative<BatteryPage>(c) || std::holds_alternative<AcEvent>(c) ||
                                    std::holds_alternative<Resume>(c) || std::holds_alternative<PowerTransition>(c)) {
                                    notifications.push_back(std::move(*i));
                                    i = commands_.erase(i);
                                } else
                                    ++i;
                            }
                        }
                        for (auto& p : notifications) p.reply.set_value(controller->execute(p.command));
                    }
                    if (ready == WAIT_OBJECT_0 + 1 && controller) {
                        try {
                            sample();
                        } catch (const std::exception& ex) {
                            log_.write(ex.what());
                        }
                    }
                }
                operation.join();
                {
                    std::lock_guard lock(mutex_);
                    if (!commands_.empty()) SetEvent(wake_.get());
                }
                if (failure) std::rethrow_exception(failure);
                if (stop.stop_requested()) throw std::runtime_error("Controller stopping after device operation");
            };
            auto wait = options_.wait;
            if (!wait)
                wait = [this](unsigned ms) {
                    if (WaitForSingleObject(stopping_.get(), ms) == WAIT_OBJECT_0)
                        throw std::system_error(ERROR_OPERATION_ABORTED, std::system_category(), "Controller shutdown");
                };
            auto cooler_delivery = [delivery = delivery_](config::Json value) {
                ControlResult result;
                result.code = value.value("ok", true) ? 0 : 1;
                result.text = value.value("status", std::string{});
                result.peripherals = config::Json{{"cooler", std::move(value)}};
                delivery->post({ResultKind::ble, 0, config::wide(result.text), {}, std::move(result)});
            };
            controller = std::make_unique<Controller>(
                *session, options_.state_directory, [this](const std::string& text) { log_.write(text); }, wait,
                run_blocking, options_.nvidia_hardware,
                options_.nvidia_factory ? options_.nvidia_factory() : std::unique_ptr<nv::Power>{},
                options_.peripheral_hardware, cooler_delivery, options_.lighting_send);
            auto init_errors = controller->start();
            delivery_->post(
                {ResultKind::command, 0, L"Core controller ready: " + config::wide(session->profile.project_id)});
            if (!init_errors.empty()) {
                std::string text = "EC initialization had " + std::to_string(init_errors.size()) +
                                   " error(s). Some features may not work:\n";
                for (const auto& e : init_errors) text += "\n" + e;
                delivery_->post({ResultKind::warning, 0, config::wide(text)});
            }
            auto initial = controller->execute(UiRead{});
            delivery_->post({ResultKind::command, 0, L"Controller ready", {}, std::move(initial)});
        }
        log_.write("Control worker started; native counter status=" + std::to_string(counters->error()));
        publish_state();
        HANDLE waits[]{stopping_.get(), timer_.get(), wake_.get()};
        while (!stop.stop_requested()) {
            auto ready = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
            if (ready == WAIT_OBJECT_0) break;
            if (ready == WAIT_OBJECT_0 + 1) {
                // An auto-reset timer retains one signal, not one queued callback per missed period.
                try {
                    sample();
                } catch (const std::exception& ex) {
                    if (stop.stop_requested()) break;
                    log_.write(std::string("EC sample failed: ") + ex.what());
                    if (++failures >= 5 && !controller) {
                        // No control subscriptions exist in this read-only stage.
                        delivery_->post(
                            {ResultKind::command, 0, L"EC monitoring stopped after five consecutive read failures."});
                        break;
                    }
                }
                continue;
            }
            if (ready != WAIT_OBJECT_0 + 2) {
                log_.write("Control worker wait failed");
                break;
            }
            std::optional<Pending> queued;
            {
                std::lock_guard lock(mutex_);
                if (commands_.empty()) continue;
                queued = std::move(commands_.front());
                commands_.pop_front();
                if (!commands_.empty()) SetEvent(wake_.get());
            }
            auto pending = &*queued;
            auto result =
                controller ? controller->execute(pending->command) : ControlResult{1, "Controller unavailable"};
            pending->reply.set_value(result);
            publish_state();
            // Status/watch is a query, not a UI refresh subscription. Writes,
            // including partial writes, still notify the application owner.
            if (!std::holds_alternative<FanEdit>(pending->command) &&
                !std::holds_alternative<Visibility>(pending->command) &&
                !std::holds_alternative<UiPage>(pending->command) &&
                !std::holds_alternative<BatteryPage>(pending->command) &&
                !std::holds_alternative<Resume>(pending->command) &&
                !std::holds_alternative<PowerTransition>(pending->command) &&
                (!std::holds_alternative<CliCommand>(pending->command) || result.applied || result.partial))
                delivery_->post({ResultKind::command, pending->sequence, config::wide(result.text), {}, result});
            if (controller) {
                unsigned changed = std::visit(
                    [&](const auto& c) -> unsigned {
                        using T = std::decay_t<decltype(c)>;
                        if constexpr (std::is_same_v<T, CpuApply> || std::is_same_v<T, GpuApply> ||
                                      std::is_same_v<T, GpuUnlock> || std::is_same_v<T, DlppcApply> ||
                                      std::is_same_v<T, EcBypassApply> || std::is_same_v<T, CoolerFlag>)
                            return 1;
                        else if constexpr (std::is_same_v<T, FanApply> || std::is_same_v<T, FanMaster> ||
                                           std::is_same_v<T, FanBoost> || std::is_same_v<T, SwitchApply>)
                            return 2;
                        else if constexpr (std::is_same_v<T, ProtectionApply>)
                            return 4;
                        else if constexpr (std::is_same_v<T, BatteryApply>)
                            return 8;
                        else if constexpr (std::is_same_v<T, LightingApply> || std::is_same_v<T, LightingIdle> ||
                                           std::is_same_v<T, DimmingApply>)
                            return 16;
                        else if constexpr (std::is_same_v<T, SlotLoad> || std::is_same_v<T, DefaultsApply> ||
                                           std::is_same_v<T, ThermalReset> || std::is_same_v<T, ThermalTest>)
                            return 0x7f;
                        else if constexpr (std::is_same_v<T, CliCommand>)
                            return result.applied || result.partial
                                       ? (!c.arguments.empty() && c.arguments[0] == "fan" ? 2 : 1)
                                       : 0;
                        else
                            return 0;
                    },
                    pending->command);
                if (result.applied || result.partial) dirty_pages |= changed;
                if (auto read = std::get_if<UiRead>(&pending->command); read && read->page >= 0)
                    dirty_pages &= ~(1u << read->page);
                if (std::holds_alternative<FanRead>(pending->command) && !result.code) dirty_pages &= ~2u;
                if (std::holds_alternative<SlotSave>(pending->command) ||
                    std::holds_alternative<SlotLoad>(pending->command) ||
                    std::holds_alternative<SlotRename>(pending->command) ||
                    std::holds_alternative<SlotClear>(pending->command))
                    page_read(-1);
                if (controller->visible() && (changed || std::holds_alternative<SlotSave>(pending->command)))
                    presentation();
                refresh_visible();
            }
        }
        log_.write("Control worker stopped; samples=" + std::to_string(samples));
    } catch (const std::exception& ex) {
        if (!stop.stop_requested()) {
            outcome_ = 1;
            log_.write(std::string("EC startup/work failed: ") + ex.what());
            delivery_->post({ResultKind::failure, 0, L"EC startup/work failed; see the diagnostic log."});
        }
    }
    refuse("Controller stopped");
}
}
