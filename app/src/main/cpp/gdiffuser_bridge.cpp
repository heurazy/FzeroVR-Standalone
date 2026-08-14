#include "gdiffuser_bridge.h"
#include "gdx_quest_input_provider.h"

#include <android/log.h>
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace {
constexpr const char* kTag = "FZeroXVR/GDiffuser";

struct GdxVrHostInput {
    float stickX;
    float stickY;
    float rightStickX;
    float rightStickY;
    float leftTrigger;
    float rightTrigger;
    float leftSqueeze;
    float rightSqueeze;
    uint32_t n64Buttons;
    XrPosef leftGrip;
    XrPosef rightGrip;
    uint32_t handActiveMask;
};

struct GdxVrHostEye {
    int eye;
    uint32_t width;
    uint32_t height;
    uint32_t colorTexture;
    float view[16];
    float projection[16];
    XrPosef xrPose;
    XrFovf xrFov;
};

#if defined(__clang__) || defined(__GNUC__)
#define GDX_WEAK __attribute__((weak))
#else
#define GDX_WEAK
#endif

extern "C" {
GDX_WEAK int gdx_vr_host_bootstrap(const char* filesDir);
GDX_WEAK void gdx_vr_host_set_stereo_views(const GdxVrHostEye* eyes, int count);
GDX_WEAK void gdx_vr_host_tick(const GdxVrHostInput* input);
GDX_WEAK int gdx_vr_host_render_eye(const GdxVrHostEye* eye);
GDX_WEAK int gdx_vr_host_render_hud(const GdxVrHostEye* eye);
GDX_WEAK int gdx_vr_host_get_cached_eye_view(int eye, XrPosef* pose, XrFovf* fov);
GDX_WEAK int gdx_vr_host_is_flat_ui(void);
GDX_WEAK int gdx_vr_host_is_race_hud(void);
GDX_WEAK int gdx_vr_host_is_diorama(void);
GDX_WEAK void gdx_vr_host_shutdown(void);
}

GdxVrHostInput ConvertInput(const QuestGameInput& in) {
    GdxVrHostInput out{};
    out.stickX = in.stickX;
    out.stickY = in.stickY;
    out.rightStickX = in.rightStickX;
    out.rightStickY = in.rightStickY;
    out.leftTrigger = in.leftTrigger;
    out.rightTrigger = in.rightTrigger;
    out.leftSqueeze = in.leftSqueeze;
    out.rightSqueeze = in.rightSqueeze;
    out.n64Buttons = in.n64Buttons;
    out.leftGrip = in.hands[0].gripPose;
    out.rightGrip = in.hands[1].gripPose;
    if (in.hands[0].active) out.handActiveMask |= 1u;
    if (in.hands[1].active) out.handActiveMask |= 2u;
    return out;
}
}

bool GDiffuserBridge::Bootstrap(const char* filesDir) {
    connected_ = false;
    tickAccumulator_ = 0.0;

    if (gdx_vr_host_bootstrap == nullptr || gdx_vr_host_tick == nullptr || gdx_vr_host_render_eye == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "G-Diffuser VR host hooks are not linked yet; using native stereo verification scene.");
        return false;
    }

    connected_ = gdx_vr_host_bootstrap(filesDir) != 0;
    __android_log_print(connected_ ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
                        "G-Diffuser VR host %s", connected_ ? "connected" : "bootstrap failed");
    return connected_;
}

void GDiffuserBridge::SetStereoViews(const VrEyeFrame* eyes, int count) {
    if (!connected_ || gdx_vr_host_set_stereo_views == nullptr || eyes == nullptr || count < 2) return;
    GdxVrHostEye hostEyes[2]{};
    for (int i = 0; i < 2; ++i) {
        hostEyes[i].eye = eyes[i].eye;
        hostEyes[i].width = eyes[i].width;
        hostEyes[i].height = eyes[i].height;
        hostEyes[i].colorTexture = eyes[i].colorTexture;
        std::memcpy(hostEyes[i].view, eyes[i].view.m.data(), sizeof(hostEyes[i].view));
        std::memcpy(hostEyes[i].projection, eyes[i].projection.m.data(), sizeof(hostEyes[i].projection));
        hostEyes[i].xrPose = eyes[i].pose;
        hostEyes[i].xrFov = eyes[i].fov;
    }
    gdx_vr_host_set_stereo_views(hostEyes, 2);
}

void GDiffuserBridge::Tick60Hz(const QuestGameInput& input, double frameSeconds) {
    // Publish every OpenXR frame. Upstream input_bridge.c reads this through gdx_lus_read_pads()
    // when the decomp host is linked; keeping it independent of connected_ also makes bring-up
    // deterministic while the renderer/bootstrap hooks are being integrated.
    GdxQuestPublishInput(input);

    if (!connected_ || gdx_vr_host_tick == nullptr) return;

    constexpr double kTick = 1.001 / 60.0;
    tickAccumulator_ += std::clamp(frameSeconds, 0.0, 0.1);
    const GdxVrHostInput converted = ConvertInput(input);

    // Keep F-Zero X simulation at its original NTSC logic rate, but NEVER execute multiple
    // simulation slices in one OpenXR frame. G-Diffuser's cooperative scheduler can yield back to
    // the host expecting VI/presentation to advance; desktop-style catch-up therefore both breaks
    // that assumption and creates a VR death spiral when one heavy tick overruns its 16.7 ms
    // budget. OpenXR already paces the host loop, so run at most one tick and discard stale debt.
    if (tickAccumulator_ >= kTick) {
        gdx_vr_host_tick(&converted);
        tickAccumulator_ -= kTick;
        if (tickAccumulator_ > kTick) {
            tickAccumulator_ = 0.0;
        }
    }
}

bool GDiffuserBridge::GetCachedEyeView(int eye, XrPosef& pose, XrFovf& fov) const {
    return connected_ && gdx_vr_host_get_cached_eye_view != nullptr &&
           gdx_vr_host_get_cached_eye_view(eye, &pose, &fov) != 0;
}

bool GDiffuserBridge::FlatUiActive() const {
    return connected_ && gdx_vr_host_is_flat_ui != nullptr && gdx_vr_host_is_flat_ui() != 0;
}

bool GDiffuserBridge::RaceHudActive() const {
    return connected_ && gdx_vr_host_is_race_hud != nullptr && gdx_vr_host_is_race_hud() != 0;
}

bool GDiffuserBridge::DioramaActive() const {
    return connected_ && gdx_vr_host_is_diorama != nullptr && gdx_vr_host_is_diorama() != 0;
}

bool GDiffuserBridge::RenderHud(const VrEyeFrame& eye) {
    if (!connected_ || gdx_vr_host_render_hud == nullptr) return false;
    GdxVrHostEye hostEye{};
    hostEye.eye = eye.eye;
    hostEye.width = eye.width;
    hostEye.height = eye.height;
    hostEye.colorTexture = eye.colorTexture;
    std::memcpy(hostEye.view, eye.view.m.data(), sizeof(hostEye.view));
    std::memcpy(hostEye.projection, eye.projection.m.data(), sizeof(hostEye.projection));
    hostEye.xrPose = eye.pose;
    hostEye.xrFov = eye.fov;
    return gdx_vr_host_render_hud(&hostEye) != 0;
}

bool GDiffuserBridge::RenderEye(const VrEyeFrame& eye, const QuestGameInput& input) {
    if (!connected_ || gdx_vr_host_render_eye == nullptr) return false;

    GdxVrHostEye hostEye{};
    hostEye.eye = eye.eye;
    hostEye.width = eye.width;
    hostEye.height = eye.height;
    hostEye.colorTexture = eye.colorTexture;
    std::memcpy(hostEye.view, eye.view.m.data(), sizeof(hostEye.view));
    std::memcpy(hostEye.projection, eye.projection.m.data(), sizeof(hostEye.projection));
    hostEye.xrPose = eye.pose;
    hostEye.xrFov = eye.fov;
    return gdx_vr_host_render_eye(&hostEye) != 0;
}

void GDiffuserBridge::Shutdown() {
    if (connected_ && gdx_vr_host_shutdown != nullptr) {
        gdx_vr_host_shutdown();
    }
    connected_ = false;
    tickAccumulator_ = 0.0;
}
