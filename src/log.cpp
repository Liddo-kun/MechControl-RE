#include "log.h"
#include <windows.h>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <vector>

namespace mech {
namespace {
Log* crash_log=nullptr;
// Native crashes otherwise leave nothing in the log the user sends back.
LONG WINAPI unhandled(EXCEPTION_POINTERS* info) {
    auto address=info->ExceptionRecord->ExceptionAddress;HMODULE module=nullptr;char name[MAX_PATH]="unknown";
    if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,static_cast<LPCSTR>(address),&module)) GetModuleFileNameA(module,name,MAX_PATH);
    char text[MAX_PATH+96];
    snprintf(text,sizeof text,"[ERROR] Unhandled exception 0x%08lX at %s+0x%llX",info->ExceptionRecord->ExceptionCode,name,
        static_cast<unsigned long long>(static_cast<const char*>(address)-reinterpret_cast<const char*>(module)));
    if(crash_log) crash_log->crash(text);
    return EXCEPTION_CONTINUE_SEARCH;
}
[[noreturn]] void terminated() {
    std::string text="[ERROR] Terminated by an uncaught exception";
    try {if(auto e=std::current_exception()) std::rethrow_exception(e);} catch(const std::exception& ex) {text+=": ";text+=ex.what();} catch(...) {}
    if(crash_log) crash_log->crash(text);
    std::abort();
}
}
Log::Log(const std::filesystem::path& path) {
    const std::string separator(72,'=');
    // Opportunistic retention: the first session (the machine's original state) plus the five most recent.
    // Anchored on both lines so ordinary log text cannot split sessions.
    try {
        std::ifstream input(path);std::vector<std::string> lines;std::vector<size_t> starts;std::string line;
        while(std::getline(input,line)) {if(!line.empty() && line.back()=='\r') line.pop_back();lines.push_back(line);}
        for(size_t i=0;i+1<lines.size();++i) if(lines[i]==separator && lines[i+1].starts_with("Session started:")) starts.push_back(i);
        if(starts.size()>6) {
            auto temporary=path;temporary+=L".retention";
            std::ofstream output(temporary,std::ios::trunc);
            for(size_t i=starts.front();i<starts[1];++i) output<<lines[i]<<'\n';
            for(size_t i=starts[starts.size()-5];i<lines.size();++i) output<<lines[i]<<'\n';
            output.close();input.close();
            if(output) MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
        }
    } catch(const std::exception&) {} // A retention failure must not prevent startup.
    file_.open(path,std::ios::app);
    if (!file_) throw std::runtime_error("Cannot open the diagnostic log.");
    SYSTEMTIME time{};GetLocalTime(&time);char start[80];
    std::snprintf(start,sizeof start,"Session started: %04u-%02u-%02u %02u:%02u:%02u",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond);
    file_<<'\n'<<separator<<'\n'<<start<<'\n'<<separator<<'\n';file_.flush();
    if(!crash_log) {crash_log=this;SetUnhandledExceptionFilter(unhandled);std::set_terminate(terminated);}
}
Log::~Log() {if(crash_log==this) crash_log=nullptr;}
void Log::write(std::string_view message) {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char prefix[64];
    std::snprintf(prefix, sizeof prefix, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu] ",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
        time.wMilliseconds, GetCurrentThreadId());
    std::lock_guard lock(mutex_);
    file_ << prefix << message << '\n';
    file_.flush();
}
void Log::crash(std::string_view message) {
    // The crashing thread may already hold the lock; give up rather than deadlock.
    for(int i=0;i<20;++i) {if(mutex_.try_lock()) {mutex_.unlock();write(message);return;}Sleep(10);}
}
}
