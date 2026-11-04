#include "log.h"
#include <windows.h>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace mech {
Log::Log(const std::filesystem::path& path) {
    const std::string separator(72,'=');
    // Opportunistic retention, anchored on both lines so ordinary log text cannot split sessions.
    try {
        std::ifstream input(path);std::vector<std::string> lines;std::vector<size_t> starts;std::string line;
        while(std::getline(input,line)) {if(!line.empty() && line.back()=='\r') line.pop_back();lines.push_back(line);}
        for(size_t i=0;i+1<lines.size();++i) if(lines[i]==separator && lines[i+1].starts_with("Session started:")) starts.push_back(i);
        if(starts.size()>5) {
            auto temporary=path;temporary+=L".retention";
            std::ofstream output(temporary,std::ios::trunc);
            for(size_t i=starts.front();i<starts[1];++i) output<<lines[i]<<'\n';
            for(size_t i=starts[starts.size()-4];i<lines.size();++i) output<<lines[i]<<'\n';
            output.close();input.close();
            if(output) MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
        }
    } catch(const std::exception&) {} // A retention failure must not prevent startup.
    file_.open(path,std::ios::app);
    if (!file_) throw std::runtime_error("Cannot open the diagnostic log.");
    SYSTEMTIME time{};GetLocalTime(&time);char start[80];
    std::snprintf(start,sizeof start,"Session started: %04u-%02u-%02u %02u:%02u:%02u",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond);
    file_<<'\n'<<separator<<'\n'<<start<<'\n'<<separator<<'\n';file_.flush();
}
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
}
