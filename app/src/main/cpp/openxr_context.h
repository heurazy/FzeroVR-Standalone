#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android_native_app_glue.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <vector>

class QuestInput;
class RendererGLES;
class GDiffuserBridge;

class OpenXRContext {
public:
    bool Initialize(android_app* app);
    void Shutdown();
    void PollEvents(bool& shouldExit);
    bool RenderFrame(QuestInput& input, RendererGLES& renderer, GDiffuserBridge& game, double frameSeconds);

    bool SessionRunning() const { return sessionRunning_; }
    bool SessionEverBegan() const { return sessionEverBegan_; }
    XrInstance Instance() const { return instance_; }
    XrSession Session() const { return session_; }
    XrSpace AppSpace() const { return appSpace_; }
    const char* InternalDataPath() const;

private:
    struct EyeSwapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<XrSwapchainImageOpenGLESKHR> images;
    };

    bool InitializeLoader();
    bool CreateInstance();
    bool CreateEgl();
    bool CreateSession();
    bool CreateReferenceSpace();
    bool CreateSwapchains();
    bool ConfigureQuestPerformance();
    void RequestQuestRefreshRate();
    bool ApplyFoveation(XrSwapchain swapchain);
    bool HasInstanceExtension(const char* extension) const;
    int64_t ChooseColorFormat() const;

    android_app* app_ = nullptr;

    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLConfig eglConfig_ = nullptr;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
    EGLSurface eglSurface_ = EGL_NO_SURFACE;

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace appSpace_ = XR_NULL_HANDLE;
    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning_ = false;
    bool sessionEverBegan_ = false;

    bool perfSettingsEnabled_ = false;
    bool refreshRateEnabled_ = false;
    bool foveationEnabled_ = false;
    float targetRefreshHz_ = 72.0f;
    PFN_xrPerfSettingsSetPerformanceLevelEXT perfSettingsSetPerformanceLevel_ = nullptr;
    PFN_xrEnumerateDisplayRefreshRatesFB enumerateDisplayRefreshRates_ = nullptr;
    PFN_xrGetDisplayRefreshRateFB getDisplayRefreshRate_ = nullptr;
    PFN_xrRequestDisplayRefreshRateFB requestDisplayRefreshRate_ = nullptr;
    PFN_xrCreateFoveationProfileFB createFoveationProfile_ = nullptr;
    PFN_xrDestroyFoveationProfileFB destroyFoveationProfile_ = nullptr;
    PFN_xrUpdateSwapchainFB updateSwapchain_ = nullptr;
    XrFoveationProfileFB foveationProfile_ = XR_NULL_HANDLE;

    bool menuWasFlat_ = false;
    bool menuAnchorValid_ = false;
    XrPosef menuAnchorPose_{{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
    EyeSwapchain quadSwapchain_;
    bool hudAnchorValid_ = false;
    bool hudWasDiorama_ = false;
    XrPosef hudAnchorPose_{{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};

    std::vector<XrExtensionProperties> availableExtensions_;
    std::vector<XrViewConfigurationView> viewConfigs_;
    std::vector<EyeSwapchain> swapchains_;
    std::vector<XrView> views_;
};
