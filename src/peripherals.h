#pragma once
#include "ec_session.h"
#include "settings.h"
#include "win_handle.h"

namespace mech {
using HidReport=std::array<uint8_t,9>;
class Lighting {
public:
    using Send=std::function<void(int,const HidReport&)>;
    Lighting(EcSession&,config::ProfileStore&,std::function<void(const std::string&)>,bool hardware,Send send={},
        std::function<void(std::function<void()>)> run_io={},std::function<void(const std::vector<EcWrite>&)> batch={});
    bool open();
    bool available(int) const;
    void zone(int,const config::Zone&,bool flash=false,bool color_only=false);
    void change(int,const config::Zone&,bool flash=false,bool color_only=false);
    config::Zone logo();
    void apply(const config::LightingSettings&);
    void capture(config::LightingSettings&);
    void idle(int);
    void resume(ULONGLONG now);
    void tick(ULONGLONG now,std::optional<DWORD> idle_ms={});
    const config::LightingState& intent() const {return state_;}
private:
    bool open_device();
    void io(const std::function<void()>&);
    void send(int,const HidReport&);
    void restore(bool reopen);
    EcSession& session_;config::ProfileStore& store_;
    std::function<void(const std::string&)> log_;
    std::array<Handle,4> handles_;
    bool hardware_,sleeping_=false;ULONGLONG resume_at_=0;
    Send output_;
    std::function<void(std::function<void()>)> run_io_;
    std::function<void(const std::vector<EcWrite>&)> batch_;
    bool io_busy_=false;
    config::LightingState state_;
};
bool local_dimming_available();
void set_local_dimming(bool);
void ensure_uefi_flags(const std::function<void(const std::string&)>&);
config::Json battery_telemetry(EcTransport&);
}
