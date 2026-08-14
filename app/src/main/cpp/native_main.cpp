#include "gdiffuser_bridge.h"
#include "openxr_context.h"
#include "quest_input.h"
#include "renderer_gles.h"

#include <android/log.h>
#include <android_native_app_glue.h>
#include <chrono>

namespace {
constexpr const char* kTag = "FZeroXVR";

struct AppLifecycleState {
    bool exitRequested = false;
    bool immersiveSessionBegan = false;
};

void OnAppCommand(android_app* app, int32_t cmd) {
    auto* lifecycle = app ? static_cast<AppLifecycleState*>(app->userData) : nullptr;
    switch (cmd) {
        case APP_CMD_START:
        case APP_CMD_RESUME:
        case APP_CMD_PAUSE:
        case APP_CMD_INIT_WINDOW:
        case APP_CMD_TERM_WINDOW:
            __android_log_print(ANDROID_LOG_INFO, kTag, "Android app cmd=%d", cmd);
            break;
        case APP_CMD_STOP:
            // Horizon transiently STOPs the NativeActivity once during the 2D->immersive handoff.
            // Ignore only that pre-session STOP. After OpenXR has genuinely begun at least once,
            // STOP means the user left the immersive activity and we should tear it down instead of
            // keeping a headless/paused VR runtime alive behind the launcher or Horizon shell.
            if (lifecycle && lifecycle->immersiveSessionBegan) {
                __android_log_print(ANDROID_LOG_INFO, kTag, "Android app STOP after immersive session -> clean VR shutdown");
                lifecycle->exitRequested = true;
            } else {
                __android_log_print(ANDROID_LOG_INFO, kTag, "Android app STOP during immersive handoff (ignored once)");
            }
            break;
        case APP_CMD_DESTROY:
            __android_log_print(ANDROID_LOG_INFO, kTag, "Android app DESTROY -> clean VR shutdown");
            if (lifecycle) lifecycle->exitRequested = true;
            break;
        default:
            break;
    }
}
}

extern "C" void android_main(android_app* app) {
    app_dummy();
    AppLifecycleState lifecycle{};
    app->userData = &lifecycle;
    app->onAppCmd = OnAppCommand;

    OpenXRContext xr;
    if (!xr.Initialize(app)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "OpenXR initialization failed");
        ANativeActivity_finish(app->activity);
        return;
    }

    QuestInput input;
    if (!input.Initialize(xr.Instance(), xr.Session())) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Quest Touch input initialization failed");
        xr.Shutdown();
        ANativeActivity_finish(app->activity);
        return;
    }

    RendererGLES renderer;
    if (!renderer.Initialize()) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "GLES renderer initialization failed");
        input.Shutdown();
        xr.Shutdown();
        ANativeActivity_finish(app->activity);
        return;
    }

    GDiffuserBridge game;
    game.Bootstrap(xr.InternalDataPath());

    bool shouldExit = false;
    auto previous = std::chrono::steady_clock::now();
    auto perfWindowStart = previous;
    uint64_t perfFrames = 0;
    double perfRenderMs = 0.0;

    while (!shouldExit && !lifecycle.exitRequested && app->destroyRequested == 0) {
        int events = 0;
        android_poll_source* source = nullptr;
        // Keep this finite: OpenXR session events do not necessarily wake Android's app looper.
        while (ALooper_pollOnce(0, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
            if (source != nullptr) source->process(app, source);
            if (app->destroyRequested != 0 || lifecycle.exitRequested) {
                shouldExit = true;
                break;
            }
        }

        xr.PollEvents(shouldExit);
        if (xr.SessionEverBegan()) lifecycle.immersiveSessionBegan = true;
        if (shouldExit) break;

        if (!xr.SessionRunning()) {
            // Avoid a hot spin before READY/after STOPPING while still polling XR frequently.
            ALooper_pollOnce(10, nullptr, &events, reinterpret_cast<void**>(&source));
            if (source != nullptr) source->process(app, source);
            previous = std::chrono::steady_clock::now();
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - previous).count();
        previous = now;

        const auto renderStart = std::chrono::steady_clock::now();
        if (!xr.RenderFrame(input, renderer, game, dt)) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "OpenXR frame loop failed");
            shouldExit = true;
        } else {
            const auto renderEnd = std::chrono::steady_clock::now();
            perfRenderMs += std::chrono::duration<double, std::milli>(renderEnd - renderStart).count();
            ++perfFrames;
            const double perfSeconds = std::chrono::duration<double>(renderEnd - perfWindowStart).count();
            if (perfSeconds >= 2.0) {
                __android_log_print(ANDROID_LOG_INFO, "FZeroXVR/Perf",
                                    "OpenXR %.1f fps, RenderFrame avg %.2f ms (%llu frames)",
                                    static_cast<double>(perfFrames) / perfSeconds,
                                    perfFrames ? perfRenderMs / static_cast<double>(perfFrames) : 0.0,
                                    static_cast<unsigned long long>(perfFrames));
                perfWindowStart = renderEnd;
                perfFrames = 0;
                perfRenderMs = 0.0;
            }
        }
    }

    game.Shutdown();
    renderer.Shutdown();
    input.Shutdown();
    xr.Shutdown();

    // xr EXITING/LOSS_PENDING can end the native loop before Android has destroyed the Activity.
    // Explicitly finish it so Horizon cannot keep a stale immersive task/session around.
    app->userData = nullptr;
    if (app->destroyRequested == 0) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "Finishing NativeActivity after VR shutdown");
        ANativeActivity_finish(app->activity);
    }
}
