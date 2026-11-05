#include "ec_session.h"
#include "worker.h"
#include <iostream>
#include <atomic>
#include <system_error>

namespace {
using namespace mech;
struct Call { uint32_t code; std::vector<int32_t> input; };
struct State {
    std::map<int32_t,uint8_t> bytes;
    std::vector<Call> calls;
    std::array<uint32_t,12> spec{200,200,200,10,55,25,0,95,0,0xBB,0,0};
    int failures=0,reopens=0,live=0;
    std::set<int32_t> ignored;
    bool spec_fail=false,redirect_high=false;
    bool rpm_fail=false;
    std::optional<int32_t> fail_write;
};
class Fake final : public EcChannel {
public:
    explicit Fake(std::shared_ptr<State> s):s_(std::move(s)){++s_->live;}
    ~Fake(){--s_->live;}
    uint32_t ioctl(uint32_t code,std::span<const int32_t> input) override {
        s_->calls.push_back({code,{input.begin(),input.end()}});
        if(s_->failures-->0 || (code==ecSpecIoctl && s_->spec_fail) || (code==ecReadIoctl && input[0]==0x464 && s_->rpm_fail) ||
            (code==ecWriteIoctl && s_->fail_write==input[0]))
            throw std::system_error(ERROR_GEN_FAILURE,std::system_category(),"injected driver failure");
        if(code==ecSpecIoctl) return s_->spec.at(static_cast<size_t>(input[0]>>8));
        if(code==ecReadIoctl) return 0xABCD0000|s_->bytes[input[0]];
        if(code==ecWriteIoctl && input.size()==2) {
            if(!s_->ignored.contains(input[0])) {
                s_->bytes[input[0]]=static_cast<uint8_t>(input[1]);
                if(input[0]==0x786) s_->bytes[0x463]=static_cast<uint8_t>(s_->redirect_high?input[1]:input[1]&127);
            }
            return 0xFFFFFFFE;
        }
        throw std::runtime_error("Unexpected IOCTL format");
    }
    void reopen() override {++s_->reopens;}
private:
    std::shared_ptr<State> s_;
};
void require(bool v,const char* name) { if(!v) throw std::runtime_error(name); std::cout<<"PASS "<<name<<'\n'; }
template<class F> void refuses(F f,const char* name) { bool failed=false; try{f();}catch(const std::exception&){failed=true;} require(failed,name); }
std::vector<std::pair<int32_t,int32_t>> writes(const State& s) {
    std::vector<std::pair<int32_t,int32_t>> r;
    for(const auto& c:s.calls) if(c.code==ecWriteIoctl) {require(c.input.size()==2,"EC write uses exactly two int32 words");r.emplace_back(c.input[0],c.input[1]);}
    return r;
}
config::MachineProfile profile(const wchar_t* name) {
    auto p=config::load_machine(config::profiles_directory()/name); config::apply_universal_defaults(p); return p;
}
std::shared_ptr<State> machine() {
    auto s=std::make_shared<State>();
    for(auto [a,v]:std::initializer_list<std::pair<int,int>>{{0x740,26},{0x727,128},{0x7D2,1},{0x783,100},{0x784,150},{0x785,100},{0x463,10},
        {0x7A6,0x18},{0x743,7},{0x744,45},{0x746,15},{0x7C5,0xE0},{0x7C6,0xC4}}) s->bytes[a]=static_cast<uint8_t>(v);
    for(int i=0;i<16;++i) {
        s->bytes[0xF00+i]=static_cast<uint8_t>(i<3?40+i*10:255);
        s->bytes[0xF30+i]=255;
    }
    return s;
}
void abi_and_guard() {
    auto p=profile(L"IDY_7Y.json"); p.limits["PL4"].encoding="double";
    auto s=machine(); EcTransport ec(std::make_unique<Fake>(s));
    std::vector<unsigned> delays;
    WriteGuard g(ec,p,false,[&](unsigned ms){delays.push_back(ms);});
    require(ec.read(0x740)==26 && s->calls.back().code==ecReadIoctl && s->calls.back().input==std::vector<int32_t>{0x740},"EC read 4-byte address / low output byte");
    ec.read(INT32_MIN); require(s->calls.back().input[0]==INT32_MIN,"signed int32 address is not truncated or capped at 16 bits");
    require(ec.spec(9)==0xBB && s->calls.back().code==ecSpecIoctl && s->calls.back().input==std::vector<int32_t>{0x9BB},"SMAPC exact 0xBB + index byte input");
    s->failures=1; ec.read(0x740); require(s->reopens==1,"read I/O failure reconnects once");
    s->failures=2; refuses([&]{ec.read(0x740);},"second driver failure propagates"); require(s->reopens==2,"reconnect is bounded");
    s->failures=1; refuses([&]{ec.spec(0);},"SMAPC failure propagates without reconnect"); require(s->reopens==2,"SMAPC preserves reference retry policy");
    s->calls.clear(); g.apply(cpu_writes(p,50,80,181,95));
    require(writes(*s)==std::vector<std::pair<int32_t,int32_t>>{{0x783,50},{0x784,80},{0x785,90},{0x786,0x8A}},"CPU group order, runtime doubled PL4 rounding and Intel offset");
    require(delays==std::vector<unsigned>{450},"redirect waits and verifies payload with commit bit cleared");
    s->calls.clear(); refuses([&]{g.apply(cpu_writes(p,50,80,180,120));},"invalid last CPU value refuses complete group"); require(s->calls.empty(),"CPU bounds failure performs no I/O");
    std::array<EcWrite,2> bad{{{0x783,50},{0x123456,1}}}; refuses([&]{g.apply(bad);},"group allowlist preflight"); require(s->calls.empty(),"all group addresses admitted before first write");
    s->bytes[0x743]=0xA8; g.apply(gpu_writes(p,140,true,15));
    require(writes(*s)==std::vector<std::pair<int32_t,int32_t>>{{0x743,0xAF},{0x744,45},{0x745,255},{0x746,15}},"GPU on sequence preserves unrelated control bits and uses model offset");
    s->calls.clear(); g.apply(gpu_writes(p,140,false,0));
    require(writes(*s)==std::vector<std::pair<int32_t,int32_t>>{{0x743,0xAD},{0x744,45},{0x745,0},{0x746,0}},"GPU off retains OEM bits zero/two but clears enable bit one");
    s->calls.clear(); refuses([&]{g.apply(gpu_writes(p,140,true,26));},"DB bounds rejected before grouped writes"); require(s->calls.empty(),"invalid DB does not partially change TGP");
    s->bytes[0x751]=0x91;
    auto boost=fan_boost_write(p,true); g.apply({&boost,1}); require(s->bytes[0x751]==0xD1,"fan boost preserves mode bits");
    boost=fan_boost_write(p,false); g.apply({&boost,1}); require(s->bytes[0x751]==0x91,"fan boost off preserves mode bits");
    s->calls.clear(); g.write(0x741,5); require(s->calls.size()==1,"init skip-verify exception does not read transforming register");
    WriteGuard ro(ec,p,true); s->calls.clear(); refuses([&]{ro.bitfield(0x751,64,64);},"read-only guard blocks before even RMW read"); require(s->calls.empty(),"read-only guard emits no driver calls");
    s->failures=1; g.write(0x783,30); require(s->reopens==3 && s->bytes[0x783]==30,"write I/O failure reopens and retries through guard");
    s->ignored.insert(0x783); s->calls.clear(); refuses([&]{g.write(0x783,40);},"same-address verify failure propagates"); require(writes(*s).size()==2,"same-address mismatch retries write exactly once");
    int recovery=0; g.recover=[&]{++recovery;s->ignored.erase(0x783);return true;};
    s->calls.clear(); g.write(0x783,40); require(recovery==1 && writes(*s).size()==3,"one recovery after verify retry then one final write");
    s->ignored.insert(0x783); g.recover=[&]{++recovery;g.write(0x783,41);return true;};
    s->calls.clear(); refuses([&]{g.write(0x783,41);},"nested recovery write failure escapes"); require(recovery==2 && writes(*s).size()==4,"recovery hook cannot recurse");
    g.recover=[&]{++recovery;return false;}; refuses([&]{g.write(0x783,41);},"declined recovery retains failure"); require(recovery==3,"recovery scope resets after an exception");
    g.recover={}; s->ignored.insert(0x786); s->calls.clear();delays.clear();
    refuses([&]{g.write(0x786,0x8B);},"redirect mismatch fails after bounded backoff");
    require(delays==std::vector<unsigned>{450,200,350,500} && writes(*s).size()==1,"redirect backoff is 1500 ms and does not rewrite command");
    s->ignored.erase(0x786); s->calls.clear(); g.write(0x786,0x8A);
    require(s->bytes[0x463]==10,"warm readback commit bit masking retained");
    s->redirect_high=true; g.write(0x786,0x8B);
    require(s->bytes[0x463]==0x8B,"cold readback may retain commit bit");
}
void snapshots_and_startup() {
    auto p=profile(L"X6FR5xxY.json"); auto s=machine();
    EcTransport ec(std::make_unique<Fake>(s));
    auto fan=read_fans(ec,p.fan_table);
    require(fan.active_entries()==3 && fan.valid() && fan.gpu_present()==false,"one-pass profile fan snapshot detects active entries and absent GPU");
    require(fan.mode==0xC4 && fan.custom_mode(4),"qualified shared latch bit preserves raw unrelated bits, not whole-byte equality");
    fan.gpu.temp_up.assign(16,0); require(!fan.gpu_present().has_value(),"cold zero GPU table is ambiguous");
    fan.cpu.temp_up.assign(16,0); require(!fan.valid() && fan.active_entries()==0,"cold CPU table is not a usable curve");
    s->bytes[0x464]=0x12;s->bytes[0x465]=0x34;s->bytes[0x46D]=0x78;s->bytes[0x46C]=0x56;s->bytes[0x463]=0xD5;
    p.limits["cpuTempLimit"]={80,100,"direct",{}, {}};
    WriteGuard amd(ec,p,false,[](unsigned){});
    amd.apply(cpu_writes(p,50,80,180,95));
    require(s->bytes[0x786]==0xDF && s->bytes[0x463]==95,"AMD CPU group writes absolute temperature payload through redirect verification");
    s->bytes[0x463]=0xD5;
    s->calls.clear(); auto sample=read_sensors(ec,p,Detail::full);
    require(sample.cpu_rpm==0x1234 && sample.gpu_rpm==0x5678 && sample.cpu_limit==85 && s->calls.size()==18,"X6FR profile RPM byte order, direct high-bit masked temperature and 18-byte full sample");
    s->calls.clear(); sample=read_sensors(ec,p,Detail::rpm); require(s->calls.size()==4 && !sample.cpu_temp,"tray sample reads just four RPM bytes");
    s->calls.clear(); sample=read_sensors(ec,p,Detail::monitor); require(s->calls.size()==11 && sample.curve_enabled && !sample.cpu_temp && !sample.system_watts,"monitor bar sample skips temperatures and System Info sensors");
    s->calls.clear(); sample=read_sensors(ec,p,Detail::temperatures,Sensors{.gpu_temp=50}); require(s->calls.size()==12 && sample.cpu_temp && sample.gpu_temp==50 && !sample.system_watts,"Fans tab sample adds temperatures, reusing a shared reading");
    s=machine(); ChannelFactory factory=[s](const std::wstring& path){require(path==LR"(\\.\ACPIDriver)","profile device path passed to EC owner");return std::make_unique<Fake>(s);};
    {
        EcSession session(factory,{},true,{},config::profiles_directory(),config::Identity{"YAOSHI Series-X6AR55xY","fixture","intel",uint8_t{26}});
        require(session.profile.project_id=="IDY_7Y" && session.selection.method=="baseboard pattern","composed executable-relative startup selects by board before colliding EC ID");
        require(session.profile.pl4_doubled() && session.battery_bit3==8 && session.profile.fan_table.active==3 && !session.profile.fan_table.has_gpu,"runtime flags and snapshot enrich effective model before guard");
        require(session.profile.limits.at("TGP").max==150 && session.profile.limits.at("PL1").max==200,"real transport callbacks feed ECSpec enrichment");
        require(session.validation.enabled.contains("gpuPower"),"GPU capability uses module ID 0x7D2, not LED-support byte");
        require(session.diagnostic_report()["readOnly"]==true && writes(*s).empty(),"diagnostic workflow never activates or writes EC");
    }
    require(s->live==0,"transport handle ownership ends with session");
    refuses([&]{EcSession session(factory,{},true,{},config::profiles_directory(),config::Identity{"unknown","fixture","intel",uint8_t{255}});},"failed automatic detection cancels without choosing first file");
    require(s->live==0,"cancel path cleans transport");
    refuses([&]{EcSession session(factory,{},true,{},config::profiles_directory(),config::Identity{"unknown","fixture","intel",uint8_t{255}},
        [](const auto& files)->std::optional<std::filesystem::path> { for(const auto& file:files) if(file.filename()==L"IDY_7Y_defaults.json") return file; return {}; });},
        "manual invalid reference-only profile refuses startup");
    require(s->live==0,"invalid selected profile cleans transport");
    s->spec_fail=true;
    EcSession fallback(factory,{},true,{},config::profiles_directory(),config::Identity{"X6AR55xY","fixture","intel",uint8_t{26}});
    require(!fallback.hardware.ecspec && !fallback.hardware.ecspec_error.empty(),"unavailable ECSpec records failure and retains profile limits");
}
void worker_owner(const std::filesystem::path& scratch) {
    auto s=machine(); std::thread::id owner;
    Log log(scratch/L"worker.log");
    auto delivery=std::make_shared<Delivery>();
    WorkerOptions options; options.once=true;options.report=scratch/L"diagnostic.json";
    options.identity=config::Identity{"X6AR55xY","fixture","intel",uint8_t{26}};
    options.channel_factory=[&](const std::wstring&,HANDLE){owner=std::this_thread::get_id();return std::make_unique<Fake>(s);};
    ControlWorker worker(delivery,log,std::move(options));
    require(worker.wait()==0 && owner!=std::this_thread::get_id() && s->live==0,"worker owns startup, EC I/O, delivery and destruction without a window");
    require(config::read_json(scratch/L"diagnostic.json")["model"]=="IDY_7Y" && writes(*s).empty(),"worker diagnostic writes report, never hardware");
}
void shutdown_io(const std::filesystem::path& scratch) {
    struct Pending { std::atomic<bool> entered=false,completed=false,destroyed=false; } pending;
    class Blocking final : public EcChannel {
    public:
        Blocking(Pending& p,HANDLE release):p_(p),release_(release){}
        ~Blocking(){p_.destroyed=true;}
        uint32_t ioctl(uint32_t,std::span<const int32_t>) override {
            p_.entered=true;
            WaitForSingleObject(release_,INFINITE);
            p_.completed=true;
            throw std::system_error(ERROR_OPERATION_ABORTED,std::system_category(),"injected synchronous I/O completed during shutdown");
        }
        void reopen() override {throw std::runtime_error("canceled I/O must not reconnect");}
    private:
        Pending& p_; HANDLE release_;
    };
    Log log(scratch/L"shutdown.log");
    auto delivery=std::make_shared<Delivery>();
    Handle release(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    WorkerOptions options;options.nvidia_hardware=false;options.peripheral_hardware=false; options.identity=config::Identity{"X6AR55xY","fixture","intel",uint8_t{26}};
    options.channel_factory=[&](const std::wstring&,HANDLE){return std::make_unique<Blocking>(pending,release.get());};
    ControlWorker worker(delivery,log,std::move(options));
    for(int i=0;i<300 && !pending.entered;++i) Sleep(10);
    require(pending.entered && !pending.destroyed,"pending driver work remains owned on worker");
    auto queued=worker.request(CpuApply{{50,80,180,95}});
    std::atomic<bool> stopped=false;
    std::jthread stopping([&]{delivery->close();worker.stop();stopped=true;});
    Sleep(100);
    bool retained=!stopped && !pending.completed && !pending.destroyed;
    SetEvent(release.get()); // Test releases driver completion, not the worker stop signal.
    stopping.join();
    require(retained,"synchronous shutdown does not abandon or pretend to cancel blocked driver work");
    require(pending.completed && pending.destroyed && stopped && delivery->take().empty(),
        "shutdown joins completed synchronous I/O before transport destruction and rejects late delivery");
    require(queued.get().code==1,"shutdown resolves a queued value-owned promise even while startup I/O is blocked");
}
void fan_batches() {
    auto p=profile(L"IDY_7Y.json");auto s=machine();
    EcTransport ec(std::make_unique<Fake>(s));WriteGuard g(ec,p,false,[](unsigned){});
    config::Curve cpu{{40,50,60},{199,199,199},{66,0,99}},gpu{{45,55},{199,199},{0,200}};
    for(uint8_t mask:{uint8_t{255},uint8_t{4}}) {
        s->calls.clear();s->bytes[0x7C5]=0x60;s->bytes[0x7C6]=0xC4;
        g.apply(fan_curve_writes(p,cpu,gpu,true,mask));
        std::vector<std::pair<int32_t,int32_t>> expected{{0x7C5,0xE0},{0x7C6,mask==4?0xC0:0}};
        const int cpu_up[]{40,50,60},cpu_down[]{36,46,50};
        for(int i=0;i<16;++i) expected.emplace_back(0xF00+i,i<3?cpu_up[i]:255);
        for(int i=1;i<16;++i) expected.emplace_back(0xF10+i,i<=3?cpu_down[i-1]:255);
        for(int i=0;i<16;++i) expected.emplace_back(0xF20+i,i==0?0:i==1?2:98);
        for(int i=0;i<16;++i) expected.emplace_back(0xF30+i,i==0?45:i==1?55:255);
        for(int i=1;i<16;++i) expected.emplace_back(0xF40+i,i==1?41:i==2?45:255);
        for(int i=0;i<16;++i) expected.emplace_back(0xF50+i,i==0?0:200);
        expected.emplace_back(0x7C6,mask==4?0xC4:4);
        require(writes(*s)==expected,"complete fan batch: master, latch, CPU/GPU array order, rebuilt hysteresis/bridge, marker, rounded/minimum/padding duty, final latch");
    }
    s->calls.clear();auto bad=gpu;bad.duty.back()=201;
    refuses([&]{g.apply(fan_curve_writes(p,cpu,bad,true,4));},"invalid GPU tail preflights before master/CPU writes");
    require(s->calls.empty(),"invalid fan group performs no I/O");
    auto misplaced=p;misplaced.fan_table.gpu_duty="0xE00";
    refuses([&]{g.apply(fan_curve_writes(misplaced,cpu,gpu,true,4));},"fan batch admission rejects later unauthorized addresses before master write");
    require(s->calls.empty(),"fan address admission failure leaves hardware unchanged");
    refuses([&]{g.apply(fan_curve_writes(p,cpu,gpu,true,0));},"fan latch interpretation is never implicitly universal");
    auto cpu_only=p;cpu_only.fan_table.has_gpu=false;
    g.apply(fan_curve_writes(cpu_only,cpu,bad,false,4));
    auto w=writes(*s);require(w.size()==50 && w.front()==std::pair<int32_t,int32_t>{0x7C5,0x60},"CPU-only fan batch skips GPU table and preserves master unrelated bits when disabled");
    s->calls.clear();s->fail_write=0xF22;
    refuses([&]{g.apply(fan_curve_writes(p,cpu,gpu,true,4));},"mid-table driver failure propagates rather than reporting complete apply");
    w=writes(*s);require(w.back().first==0xF22 && s->bytes[0x7C6]==0xC0,"failed batch does not re-enable latch or roll back already-written bytes");
    s->fail_write.reset();s->calls.clear();g.apply(fan_curve_writes(p,cpu,gpu,true,4));
    require(writes(*s).size()==97 && s->bytes[0x7C6]==0xC4,"retry resends complete fan batch and restores qualified latch only after success");
}
void recurring_worker(const std::filesystem::path& scratch) {
    for(bool fail:{false,true}) {
        auto s=machine();s->bytes[0x464]=4;s->bytes[0x465]=42;s->bytes[0x46C]=4;s->bytes[0x46D]=20;s->rpm_fail=fail;
        Log log(scratch/(fail?L"sampling-failure.log":L"sampling.log"));
        auto delivery=std::make_shared<Delivery>();
        WorkerOptions options;options.read_only=true;options.identity=config::Identity{"X6AR55xY","fixture","intel",uint8_t{26}};
        options.channel_factory=[s](const std::wstring&,HANDLE){return std::make_unique<Fake>(s);};
        ControlWorker worker(delivery,log,std::move(options));
        bool observed=false;
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(7);
        do {
            for(const auto& r:delivery->take()) {
                if(!fail && r.sensors) observed=r.sensors->cpu_rpm==1066 && r.sensors->gpu_rpm==1044;
                if(fail && r.text.find(L"stopped after five")!=std::wstring::npos) observed=true;
            }
            if(!observed) Sleep(10);
        }while(!observed && std::chrono::steady_clock::now()<deadline);
        delivery->close();worker.stop();
        require(observed,fail?"recurring worker reports stop after five failed real-path samples":"recurring worker publishes typed profile-decoded RPM via owned mailbox");
        require(s->live==0 && writes(*s).empty(),"recurring read-only worker releases transport without hardware writes");
        if(fail) require(s->reopens==5,"each failed sample has only one reconnect and polling stops");
    }
}
}
int wmain() {
    try {
        auto scratch=std::filesystem::path(L"C:/Temp/tandem/mechcontrol-native-phase3/check")/std::to_wstring(GetCurrentProcessId());
        std::filesystem::create_directories(scratch);
        require(std::filesystem::is_directory(config::executable_directory()/L"Profiles/MECHREVO"),"profiles staged beside executable rather than source fallback");
        abi_and_guard(); snapshots_and_startup(); worker_owner(scratch);shutdown_io(scratch);recurring_worker(scratch);fan_batches();
        return 0;
    }catch(const std::exception& ex){std::cerr<<"FAIL "<<ex.what()<<'\n';return 1;}
}
