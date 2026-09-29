// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
// Same program with Source/factory GUI; local objects and Parcel only.
#include <binder/Parcel.h>
#include <gui/LayerState.h>
#include <gui/SurfaceComposerClient.h>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
using namespace android;
DisplayState& peekDisplayState(void*, const sp<IBinder>&)
        asm("_ZN7android21SurfaceComposerClient11Transaction15getDisplayStateERKNS_2spINS_7IBinderEEE");
static void require(bool ok) { if (!ok) std::exit(1); }
int main() {
    static_assert(offsetof(DisplayState, flags) == (sizeof(void*) == 8 ? 72 : 60));
    static_assert(sizeof(DisplayState) == (sizeof(void*) == 8 ? 80 : 64));
    int fixtures = 0;
    DisplayState initial;
    require(initial.flags == 0);
    std::puts("display-flags constructor=zero");
    ++fixtures;
    for (uint32_t flags : {0u, 0x100000u, 0xffffffffu, 0xa555aaaau}) {
        DisplayState state;
        state.what = DisplayState::eDisplayFlagsChanged;
        state.flags = flags;
        state.layerStack = 123;
        state.orientation = 2;
        state.viewport = Rect(1, 2, 4, 8);
        state.frame = Rect(10, 20, 40, 80);
        state.width = 1920;
        state.height = 1080;
        Parcel wire;
        require(state.write(wire) == NO_ERROR);
        DisplayState decoded;
        wire.setDataPosition(0);
        require(decoded.read(wire) == NO_ERROR && wire.dataAvail() == 0 &&
                decoded.flags == flags && decoded.what == state.what && decoded.width == state.width &&
                decoded.height == state.height && decoded.viewport == state.viewport);
        std::printf("display-flags wire=%08x bytes=", flags);
        for (size_t i = 0; i < wire.dataSize(); ++i) std::printf("%02x", wire.data()[i]);
        std::puts("");
        ++fixtures;
        DisplayState target;
        target.width = 37;
        target.merge(state);
        require(target.flags == flags && target.what == DisplayState::eDisplayFlagsChanged && target.width == 37);
        std::printf("display-flags merge=%08x\n", flags);
        ++fixtures;
        state.what = 0;
        target.flags = 0x11223344;
        target.merge(state);
        require(target.flags == 0x11223344);
        std::printf("display-flags unmasked=%08x retained=11223344\n", flags);
        ++fixtures;
        static_assert(sizeof(SurfaceComposerClient::Transaction) <= 8192);
        auto transaction = ::new (::operator new(8192)) SurfaceComposerClient::Transaction;
        sp<IBinder> token;
        auto& before = peekDisplayState(transaction, token);
        before.what = DisplayState::eDisplaySizeChanged;
        before.width = 37;
        transaction->setDisplayFlags(token, flags);
        const auto& after = peekDisplayState(transaction, token);
        require(after.flags == flags && after.what == 0x18 && after.width == 37);
        std::printf("display-flags setter=%08x what=18 width=37\n", flags);
        delete transaction;
        ++fixtures;
    }
    require(fixtures == 17);
    std::printf("display-flags-probe passed=%d\n", fixtures);
}
