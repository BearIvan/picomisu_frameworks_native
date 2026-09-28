// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
// Local Binder relay only: does not connect to the running SurfaceFlinger.
#include <binder/Binder.h>
#include <binder/Parcel.h>
#include <gui/IDisplayEventConnection.h>
#include <gui/ISurfaceComposer.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace android;

static void require(bool condition) {
    if (!condition) {
        std::fprintf(stderr, "display-config probe assertion failed\n");
        std::exit(1);
    }
}

class Recorder : public BBinder {
public:
    int calls = 0, expectedSource = 0, expectedConfig = 0;
    std::vector<uint8_t> payload;
protected:
    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override {
        require(code == 4 && flags == 0 && reply != nullptr);
        payload.assign(data.data(), data.data() + data.dataSize());
        require(data.enforceInterface(ISurfaceComposer::descriptor));
        require(data.dataAvail() == 8);
        require(data.readInt32() == expectedSource);
        require(data.readInt32() == expectedConfig);
        require(data.dataAvail() == 0);
        ++calls;
        return reply->writeStrongBinder(nullptr);
    }
};

int main() {
    sp<Recorder> recorder = new Recorder;
    sp<ISurfaceComposer> composer = interface_cast<ISurfaceComposer>(recorder);
    require(composer != nullptr);
    int fixtures = 0;
    for (int source = 0; source != 2; ++source) {
        for (int config = 0; config != 2; ++config) {
            recorder->expectedSource = source;
            recorder->expectedConfig = config;
            require(composer->createDisplayEventConnection(
                    static_cast<ISurfaceComposer::VsyncSource>(source),
                    static_cast<ISurfaceComposer::ConfigChanged>(config)) == nullptr);
            require(recorder->calls == ++fixtures);
            std::printf("display-config source=%d config=%d code=4 flags=0 bytes=", source, config);
            for (uint8_t byte : recorder->payload) std::printf("%02x", byte);
            std::puts("");
        }
    }
    recorder->expectedSource = 0;
    recorder->expectedConfig = 0;
    require(composer->createDisplayEventConnection() == nullptr);
    require(recorder->calls == 5);
    std::puts("display-config default=app,suppress code=4 flags=0");
    std::puts("display-config-probe passed=5");
}
