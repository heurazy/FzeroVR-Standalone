#pragma once

#include <array>
#include <cstdint>
#include <openxr/openxr.h>

struct VrMat4 {
    std::array<float, 16> m{};
};

struct VrEyeFrame {
    int eye = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t colorTexture = 0;
    XrPosef pose{{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
    XrFovf fov{};
    VrMat4 view{};
    VrMat4 projection{};
};

struct QuestHandState {
    bool active = false;
    XrPosef gripPose{{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
};

struct QuestGameInput {
    float stickX = 0.f;
    float stickY = 0.f;
    float rightStickX = 0.f;
    float rightStickY = 0.f;
    float leftTrigger = 0.f;
    float rightTrigger = 0.f;
    float leftSqueeze = 0.f;
    float rightSqueeze = 0.f;
    uint32_t n64Buttons = 0;
    QuestHandState hands[2];
};

enum N64ButtonBits : uint32_t {
    N64_A = 0x8000,
    N64_B = 0x4000,
    N64_Z = 0x2000,
    N64_START = 0x1000,
    N64_DPAD_UP = 0x0800,
    N64_DPAD_DOWN = 0x0400,
    N64_DPAD_LEFT = 0x0200,
    N64_DPAD_RIGHT = 0x0100,
    N64_L = 0x0020,
    N64_R = 0x0010,
    N64_C_UP = 0x0008,
    N64_C_DOWN = 0x0004,
    N64_C_LEFT = 0x0002,
    N64_C_RIGHT = 0x0001,
};
