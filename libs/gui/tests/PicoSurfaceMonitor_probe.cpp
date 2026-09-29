// Copyright 2026 The Picomisu Project
// SPDX-License-Identifier: Apache-2.0
// Actual source/factory methods with only local services, properties and clocks.
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <gui/SurfaceMonitor.h>
#include <cutils/properties.h>
#include <dlfcn.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <vector>

using namespace android;
static_assert(sizeof(MonitorItem) == 48);
static_assert(sizeof(SurfaceMonitor) == (sizeof(void*) == 8 ? 6680 : 6640));
static int boot = 1, monitorProperty = 1;
static std::string buildType = "user", displayType = "72", driver = "/dev/binder";
static std::string configText, processName = "vr.test\n", threadName = "Worker\n";
static int callerPid = 7777, callerTid = 8888;
static nsecs_t mono = 1000000000, wall = 1700000000000000000;
static int clockCalls = 0, pidCalls = 0, tidCalls = 0;
static bool transferAvailable = true, sysAvailable = true, hasConfig = false;
static std::vector<int32_t> parameters = {85,70,55,0,0,1,1,10,10,60,3};
static std::vector<std::string> transactions, lookups, files;

static void require(bool good, const char* label) {
    if (!good) { std::fprintf(stderr, "monitor probe: %s\n", label); std::exit(1); }
}
static std::string hex(const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::string out;
    for (size_t i=0;i<size;++i) { char b[3]; std::snprintf(b,3,"%02x",bytes[i]); out+=b; }
    return out;
}
static uint64_t hash(const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    uint64_t h=14695981039346656037ULL;
    for (size_t i=0;i<size;++i) { h^=bytes[i]; h*=1099511628211ULL; }
    return h;
}

extern "C" __attribute__((visibility("default"))) int property_get(
        const char* key, char* value, const char* fallback) {
    const char* selected = nullptr;
    if (!std::strcmp(key,"ro.build.type")) selected=buildType.c_str();
    if (!std::strcmp(key,"sys.pvr.display.type")) selected=displayType.c_str();
    if (selected) { std::snprintf(value,PROPERTY_VALUE_MAX,"%s",selected); return std::strlen(value); }
    using Fn=int(*)(const char*,char*,const char*);
    static auto real=reinterpret_cast<Fn>(dlsym(RTLD_NEXT,"property_get"));
    require(real != nullptr,"real property getter"); return real(key,value,fallback);
}
extern "C" __attribute__((visibility("default"))) int32_t property_get_int32(
        const char* key, int32_t fallback) {
    if (!std::strcmp(key,"sys.boot_completed")) return boot;
    if (!std::strcmp(key,"persist.sys.monitor")) return monitorProperty;
    using Fn=int32_t(*)(const char*,int32_t);
    static auto real=reinterpret_cast<Fn>(dlsym(RTLD_NEXT,"property_get_int32"));
    require(real != nullptr,"real int property getter"); return real(key,fallback);
}
extern "C" __attribute__((visibility("default"))) nsecs_t systemTime(int clock) {
    ++clockCalls;
    require(clock==SYSTEM_TIME_MONOTONIC || clock==SYSTEM_TIME_REALTIME,"clock ID");
    return clock==SYSTEM_TIME_MONOTONIC ? mono : wall;
}
extern "C" __attribute__((visibility("default"))) pid_t getpid() { return 100; }
__attribute__((visibility("default"))) pid_t fakePid(void*) asm("_ZNK7android14IPCThreadState13getCallingPidEv");
pid_t fakePid(void*) { ++pidCalls; return callerPid; }
__attribute__((visibility("default"))) pid_t fakeTid(void*) asm("_ZN7android14IPCThreadState13getCallingTidEv");
pid_t fakeTid(void*) { ++tidCalls; return callerTid; }
__attribute__((visibility("default"))) String8 fakeDriver(void*) asm("_ZN7android12ProcessState13getDriverNameEv");
String8 fakeDriver(void*) { return String8(driver.c_str()); }

extern "C" __attribute__((visibility("default"))) FILE* fopen(const char* path, const char* mode) {
    if (!std::strcmp(path,"/system/etc/fps_config")) {
        files.push_back("config");
        return hasConfig ? fmemopen(configText.data(),configText.size(),"r") : nullptr;
    }
    int pid=0; char tail=0;
    if (std::sscanf(path,"/proc/%d/comm%c",&pid,&tail)==1) {
        files.push_back("comm:"+std::to_string(pid));
        std::string* content=pid==callerPid ? &processName : &threadName;
        return fmemopen(content->data(),content->size(),"r");
    }
    using Fn=FILE*(*)(const char*,const char*);
    static auto real=reinterpret_cast<Fn>(dlsym(RTLD_NEXT,"fopen"));
    require(real != nullptr,"real fopen"); return real(path,mode);
}

class Service : public BBinder {
public:
    bool sys;
    explicit Service(bool value) : sys(value) {}
    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override {
        transactions.push_back(std::string(sys?"sys":"transfer")+":"+std::to_string(code)+":"+
                               std::to_string(flags)+":"+hex(data.data(),data.dataSize()));
        if (sys && code==4) { reply->writeNoException(); reply->writeInt32Vector(parameters); }
        return NO_ERROR;
    }
};
class Manager : public BnInterface<IServiceManager> {
public:
    sp<IBinder> transfer=new Service(false), sys=new Service(true);
    sp<IBinder> select(const String16& name, const char* kind) const {
        std::string value=String8(name).string(); lookups.push_back(std::string(kind)+":"+value);
        if (value=="transferserver") return transferAvailable ? transfer : nullptr;
        if (value=="systransserver") return sysAvailable ? sys : nullptr;
        std::fprintf(stderr,"unexpected service: %s\n",value.c_str()); std::abort();
    }
    sp<IBinder> getService(const String16& n) const override { return select(n,"get"); }
    sp<IBinder> checkService(const String16& n) const override { return select(n,"check"); }
    status_t addService(const String16&,const sp<IBinder>&,bool,int) override { return INVALID_OPERATION; }
    Vector<String16> listServices(int) override { return {}; }
};
static sp<Manager> manager;
namespace android {
__attribute__((visibility("default"))) sp<IServiceManager> defaultServiceManager() { return manager; }
}

struct Storage {
    alignas(SurfaceMonitor) std::array<unsigned char,sizeof(SurfaceMonitor)+64> bytes;
    SurfaceMonitor* value;
    Storage() { bytes.fill(0x5a); value=new(bytes.data()) SurfaceMonitor(); }
    ~Storage() { value->~SurfaceMonitor(); guard(); }
    void guard() const {
        for(size_t i=sizeof(SurfaceMonitor);i<bytes.size();++i) require(bytes[i]==0x5a,"object guard");
        for(size_t i=8;i<48;++i) require(bytes[i]==0x5a,"unresolved prefix bytes changed");
    }
};

static int snapshots=0;
static void dump(const char* label, const Storage& s) {
    s.guard();
    const size_t frame=56+2*sizeof(void*), frameIndex=frame+5760;
    const size_t avg=frame+5776, duration=avg+96, pending=duration+496;
    const size_t param=sizeof(void*)==8 ? 6632 : 6588;
    uintptr_t transfer=0,sys=0;
    std::memcpy(&transfer,s.bytes.data()+56,sizeof(transfer));
    std::memcpy(&sys,s.bytes.data()+56+sizeof(void*),sizeof(sys));
    std::printf("surface-monitor %s prefix=%s transfer=%d sys=%d frame-hash=%016llx histories=%s/%s/%s settings=%s parameters=%s updating=%u clocks=%d pid=%d tid=%d\n",
        label,hex(s.bytes.data(),56).c_str(),transfer!=0,sys!=0,
        static_cast<unsigned long long>(hash(s.bytes.data()+frame,5760)),
        hex(s.bytes.data()+frameIndex,8+sizeof(size_t)).c_str(),
        hex(s.bytes.data()+avg+80,8+sizeof(size_t)).c_str(),
        hex(s.bytes.data()+duration+480,8+sizeof(size_t)).c_str(),
        hex(s.bytes.data()+pending,152).c_str(),hex(s.bytes.data()+param,44).c_str(),
        s.bytes[param+44],clockCalls,pidCalls,tidCalls);
    for(const auto& t:transactions) std::printf("surface-monitor packet %s\n",t.c_str());
    for(const auto& t:lookups) std::printf("surface-monitor lookup %s\n",t.c_str());
    for(const auto& t:files) std::printf("surface-monitor file %s\n",t.c_str());
    transactions.clear();lookups.clear();files.clear();++snapshots;
}
static void frame(SurfaceMonitor* m, int i, nsecs_t step, nsecs_t work) {
    const nsecs_t begin=1000000000LL+i*step;
    for(int stage=0;stage<4;++stage) {
        mono=begin+(stage==3?work:stage*1000);
        wall=1700000000000000000LL+i*step;
        m->setFrameItem(static_cast<MonitorIndex>(stage));
    }
    m->addFrame(String8("vr.test"));
}

int main(int argc,char** argv) {
    if(argc!=2) return 2;
    const std::string mode=argv[1];
    if(mode=="boot-off") boot=0;
    if(mode=="release") monitorProperty=0;
    if(mode=="userdebug") { monitorProperty=0;buildType="userdebug"; }
    if(mode=="vnd") driver="/dev/vndbinder";
    if(mode=="no-transfer") transferAvailable=false;
    if(mode=="no-sys") sysAvailable=false;
    if(mode=="custom") { hasConfig=true;configText="80 100 0\n";displayType="100"; }
    if(mode=="parameters") parameters={90,60,50,-1,1,2,2,20,15,65,4};
    if(mode=="short-parameters") parameters.resize(10);
    if(mode=="self") callerPid=100;
    if(mode=="system") processName="system_server\n";
    if(mode=="render-thread") threadName="RenderThread\n";
    if(mode=="quick" || mode=="mixed" || mode=="downgrade") displayType="120";
    manager=new Manager();
    {
        Storage s;
        dump("constructor",s);
        if(mode=="boot-off" || mode=="release" || mode=="vnd" || mode=="no-transfer") {
            for(int i=0;i<4;++i) s.value->setFrameItem(static_cast<MonitorIndex>(i));
            s.value->addFrame(String8("ignored"));
            s.value->updateCurrentDisplayFps(0,String8("ignored"));
            dump("disabled",s);
        } else if(mode=="self" || mode=="system" || mode=="render-thread") {
            frame(s.value,0,16000000,1000000);
            frame(s.value,1,16000000,1000000);
            dump("excluded",s);
        } else {
            frame(s.value,0,16000000,1000000);
            dump("one-frame",s);
            s.value->scanOperationArea(String8("single"));
            dump("one-analysis",s);
            nsecs_t step=mode=="quick" ? 30000000 : mode=="downgrade" ? 16000000 : 10000000;
            const int batches=mode=="upgrade" || mode=="downgrade" ? 10 : 1;
            for(int i=0;i<120*batches;++i) {
                nsecs_t actualStep=step;
                if(mode=="mixed" && i%20==0) actualStep=100000000;
                frame(s.value,i,actualStep,1000000);
            }
            dump("batches",s);
            for(int i=0;i<15;++i) frame(s.value,i,25000000,i==5 ? 400000000 : 1000000);
            s.value->updateCurrentDisplayFps(1,String8("update"));
            dump("flush-update",s);
            s.value->clear();
            dump("clear",s);
        }
    }
    std::printf("surface-monitor-probe passed=%d\n",snapshots);
    return 0;
}
