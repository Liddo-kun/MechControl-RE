#pragma once
#include "win_handle.h"
#include <memory>
#include <algorithm>
#include <stdexcept>
#include <winrt/Windows.Foundation.h>

namespace mech::ble_detail {
struct Event {
    Handle handle{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    Event(){if(!handle) throw std::runtime_error("Cannot create BLE event");}
};
template<class Op> auto await(const Op& op,HANDLE stopped,HANDLE superseded,ULONGLONG deadline) {
    auto done=std::make_shared<Event>();
    // A late WinRT completion owns only its event, never connection/owner state.
    op.Completed([done](auto const&,winrt::Windows::Foundation::AsyncStatus){SetEvent(done->handle.get());});
    HANDLE waits[]{stopped,superseded,done->handle.get()};auto now=GetTickCount64();
    auto result=WaitForMultipleObjects(3,waits,FALSE,DWORD(deadline>now?std::min<ULONGLONG>(deadline-now,MAXDWORD-1):0));
    if(result!=WAIT_OBJECT_0+2) {
        try {op.Cancel();} catch(...) {}
        throw std::runtime_error(result==WAIT_TIMEOUT?"BLE operation timed out":"BLE operation superseded or stopping");
    }
    return op.GetResults(); // Completion can be success, cancellation or error.
}
}
