#pragma once
#include "worker.h"

namespace mech {
class CliServer {
public:
    CliServer(ControlWorker&, Log&, std::wstring name = L"MechControl");
    ~CliServer();

private:
    void run();
    ControlWorker& worker_;
    Log& log_;
    std::wstring name_;
    Handle stop_;
    std::mutex mutex_;
    Handle pipe_;
    std::jthread thread_;
};
}
