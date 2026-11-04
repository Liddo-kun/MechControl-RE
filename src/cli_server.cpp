#include "cli_server.h"
#include <sddl.h>

namespace mech {
CliServer::CliServer(ControlWorker& worker,Log& log,std::wstring name):worker_(worker),log_(log),name_(std::move(name)),
    stop_(CreateEventW(nullptr,TRUE,FALSE,nullptr)) {
    if(!stop_) throw std::runtime_error("Cannot create CLI stop event");
    thread_=std::jthread([this]{run();});
}
CliServer::~CliServer() {
    SetEvent(stop_.get());
    {std::lock_guard lock(mutex_);if(pipe_) CancelIoEx(pipe_.get(),nullptr);}
    if(thread_.joinable()) thread_.join();
}
void CliServer::run() {
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;BA)(A;;GA;;;SY)",SDDL_REVISION_1,&descriptor,nullptr)) {
        log_.write("CLI ACL creation failed");return;
    }
    struct Free {PSECURITY_DESCRIPTOR p;~Free(){LocalFree(p);}} free{descriptor};
    SECURITY_ATTRIBUTES security{sizeof security,descriptor,FALSE};
    std::string last_error;
    while(WaitForSingleObject(stop_.get(),0)!=WAIT_OBJECT_0) {
        try {
            Handle connected(CreateEventW(nullptr,TRUE,FALSE,nullptr));
            HANDLE pipe;
            {
                std::lock_guard lock(mutex_);
                pipe_=Handle(CreateNamedPipeW((L"\\\\.\\pipe\\"+name_).c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
                    PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,&security));
                if(!pipe_) throw std::runtime_error("CLI listener unavailable (name owned or access denied)");
                pipe=pipe_.get();
            }
            OVERLAPPED io{};io.hEvent=connected.get();
            BOOL ok=ConnectNamedPipe(pipe,&io);auto error=ok?ERROR_SUCCESS:GetLastError();
            if(!ok && error==ERROR_IO_PENDING) {
                HANDLE waits[]{stop_.get(),connected.get()};
                auto ready=WaitForMultipleObjects(2,waits,FALSE,INFINITE);
                if(ready!=WAIT_OBJECT_0+1) {CancelIoEx(pipe,&io);DWORD done;GetOverlappedResult(pipe,&io,&done,TRUE);break;}
                DWORD done;if(!GetOverlappedResult(pipe,&io,&done,FALSE)) throw std::runtime_error("CLI connect failed");
            } else if(!ok && error!=ERROR_PIPE_CONNECTED) throw std::runtime_error("CLI connect failed");
            if(WaitForSingleObject(stop_.get(),0)==WAIT_OBJECT_0) break;
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            std::array<uint8_t,8> header;cli::transfer(pipe,header,false,deadline);
            int version=cli::integer(header.data()),length=cli::integer(header.data()+4);
            if(length<0 || length>static_cast<int>(cli::maxRequestBytes)) throw std::runtime_error("CLI invalid request length");
            std::vector<uint8_t> bytes(length);cli::transfer(pipe,bytes,false,deadline);
            cli::Reply reply;
            if(version!=cli::version) reply={2,"Protocol mismatch. Use the mcctl shipped beside this controller."};
            else {
                std::vector<std::string> args;
                if(!bytes.empty()) {
                    size_t start=0;
                    for(size_t i=0;i<=bytes.size();++i) if(i==bytes.size() || bytes[i]==0) {
                        // Encoding.UTF8 in the reference replaces malformed sequences.
                        auto data=reinterpret_cast<const char*>(bytes.data()+start);auto byte_count=static_cast<int>(i-start);
                        int count=MultiByteToWideChar(CP_UTF8,0,data,byte_count,nullptr,0);
                        std::wstring wide(count,L'\0');MultiByteToWideChar(CP_UTF8,0,data,byte_count,wide.data(),count);
                        count=WideCharToMultiByte(CP_UTF8,0,wide.data(),static_cast<int>(wide.size()),nullptr,0,nullptr,nullptr);
                        std::string text(count,'\0');WideCharToMultiByte(CP_UTF8,0,wide.data(),static_cast<int>(wide.size()),text.data(),count,nullptr,nullptr);
                        args.push_back(std::move(text));start=i+1;
                    }
                }
                auto future=worker_.request(CliCommand{std::move(args)});
                while(future.wait_for(std::chrono::milliseconds(100))!=std::future_status::ready)
                    if(WaitForSingleObject(stop_.get(),0)==WAIT_OBJECT_0) return;
                auto result=future.get();reply={result.code,std::move(result.text)};
            }
            if(WaitForSingleObject(stop_.get(),0)==WAIT_OBJECT_0) break;
            auto prefix=cli::header(reply.code,static_cast<uint32_t>(reply.text.size()));
            deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            cli::transfer(pipe,prefix,true,deadline);
            cli::transfer(pipe,std::span(reinterpret_cast<uint8_t*>(reply.text.data()),reply.text.size()),true,deadline);
            // Do not disconnect before the client has read/closed. A one-byte read
            // completes with broken pipe when the adjacent client closes its handle.
            std::array<uint8_t,1> closed;
            try {cli::transfer(pipe,closed,false,deadline);} catch(...) {}
            last_error.clear();
        } catch(const std::exception& ex) {
            if(WaitForSingleObject(stop_.get(),0)==WAIT_OBJECT_0) break;
            if(last_error!=ex.what()) {log_.write(ex.what());last_error=ex.what();}
        }
        {std::lock_guard lock(mutex_);if(pipe_) DisconnectNamedPipe(pipe_.get());pipe_.reset();}
        if(last_error.find("listener")!=std::string::npos && WaitForSingleObject(stop_.get(),5000)==WAIT_OBJECT_0) break;
    }
    std::lock_guard lock(mutex_);pipe_.reset();
}
}
