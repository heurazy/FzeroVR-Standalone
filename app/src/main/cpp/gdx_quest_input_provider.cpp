#include "gdx_quest_input_provider.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace {
std::mutex gInputMutex;
QuestGameInput gLatestInput{};
std::atomic_bool gRumbleActive{false};

int8_t ToN64Axis(float value) {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    const int rounded = static_cast<int>(std::lround(clamped * 80.0f));
    return static_cast<int8_t>(std::clamp(rounded, -80, 80));
}
}

void GdxQuestPublishInput(const QuestGameInput& input) {
    std::lock_guard<std::mutex> lock(gInputMutex);
    gLatestInput = input;
}

void GdxQuestSetRumble(bool active) {
    gRumbleActive.store(active, std::memory_order_relaxed);
}

bool GdxQuestRumbleActive() {
    return gRumbleActive.load(std::memory_order_relaxed);
}

extern "C" void gdx_quest_set_rumble(int active) {
    GdxQuestSetRumble(active != 0);
}

// Exact host ABI consumed by G-Diffuser/port/input_bridge.c. Keeping the symbol here means the
// upstream game-side bridge can be linked unchanged on Quest: it sees one connected N64 pad in
// port 1, with the OpenXR Touch mapping already translated to the standard OSContPad bitmask.
extern "C" int gdx_lus_read_pads(int capacity, uint16_t* outButtons, int8_t* outStickX,
                                  int8_t* outStickY, uint8_t* outConnected) {
    if (capacity <= 0 || outButtons == nullptr || outStickX == nullptr || outStickY == nullptr ||
        outConnected == nullptr) {
        return 0;
    }

    for (int i = 0; i < capacity; ++i) {
        outButtons[i] = 0;
        outStickX[i] = 0;
        outStickY[i] = 0;
        outConnected[i] = 0;
    }

    QuestGameInput snapshot{};
    {
        std::lock_guard<std::mutex> lock(gInputMutex);
        snapshot = gLatestInput;
    }

    outButtons[0] = static_cast<uint16_t>(snapshot.n64Buttons & 0xFFFFu);
    outStickX[0] = ToN64Axis(snapshot.stickX);
    outStickY[0] = ToN64Axis(snapshot.stickY);
    outConnected[0] = 1;
    return 1;
}
