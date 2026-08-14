#include "openxr_context.h"

#include "gdiffuser_bridge.h"
#include "quest_input.h"
#include "renderer_gles.h"

#include <android/log.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
constexpr const char* kTag = "FZeroXVR/OpenXR";

bool XrOk(XrResult result, const char* what) {
    if (XR_FAILED(result)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "%s failed: %d", what, result);
        return false;
    }
    return true;
}

bool EglOk(const char* what) {
    const EGLint error = eglGetError();
    if (error != EGL_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "%s EGL error: 0x%x", what, error);
        return false;
    }
    return true;
}

float LoadTargetRefreshHz(const char* filesDir) {
    if (filesDir == nullptr || filesDir[0] == '\0') return 72.0f;
    char path[1024]{};
    std::snprintf(path, sizeof(path), "%s/vr_settings.cfg", filesDir);
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return 72.0f;
    float target = 72.0f;
    char line[128]{};
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        float parsed = 0.0f;
        if (std::sscanf(line, "refresh_hz=%f", &parsed) == 1 && parsed >= 60.0f && parsed <= 144.0f) {
            target = parsed;
        }
    }
    std::fclose(file);
    return target;
}

XrVector3f RotateVector(const XrQuaternionf& q, const XrVector3f& v) {
    // q * (v,0) * conjugate(q), expanded to avoid temporary quaternion objects.
    const XrVector3f u{q.x, q.y, q.z};
    const float dotUV = u.x*v.x + u.y*v.y + u.z*v.z;
    const float dotUU = u.x*u.x + u.y*u.y + u.z*u.z;
    const XrVector3f crossUV{
        u.y*v.z - u.z*v.y,
        u.z*v.x - u.x*v.z,
        u.x*v.y - u.y*v.x};
    return {
        2.f*dotUV*u.x + (q.w*q.w-dotUU)*v.x + 2.f*q.w*crossUV.x,
        2.f*dotUV*u.y + (q.w*q.w-dotUU)*v.y + 2.f*q.w*crossUV.y,
        2.f*dotUV*u.z + (q.w*q.w-dotUU)*v.z + 2.f*q.w*crossUV.z};
}

XrQuaternionf QuaternionFromBasis(const XrVector3f& xAxis, const XrVector3f& yAxis, const XrVector3f& zAxis) {
    // Columns are the world-space directions of local +X/+Y/+Z.
    const float m00=xAxis.x, m01=yAxis.x, m02=zAxis.x;
    const float m10=xAxis.y, m11=yAxis.y, m12=zAxis.y;
    const float m20=xAxis.z, m21=yAxis.z, m22=zAxis.z;
    XrQuaternionf q{};
    const float trace=m00+m11+m22;
    if (trace>0.f) {
        const float s=std::sqrt(trace+1.f)*2.f;
        q.w=0.25f*s;
        q.x=(m21-m12)/s;
        q.y=(m02-m20)/s;
        q.z=(m10-m01)/s;
    } else if (m00>m11 && m00>m22) {
        const float s=std::sqrt(1.f+m00-m11-m22)*2.f;
        q.w=(m21-m12)/s;
        q.x=0.25f*s;
        q.y=(m01+m10)/s;
        q.z=(m02+m20)/s;
    } else if (m11>m22) {
        const float s=std::sqrt(1.f+m11-m00-m22)*2.f;
        q.w=(m02-m20)/s;
        q.x=(m01+m10)/s;
        q.y=0.25f*s;
        q.z=(m12+m21)/s;
    } else {
        const float s=std::sqrt(1.f+m22-m00-m11)*2.f;
        q.w=(m10-m01)/s;
        q.x=(m02+m20)/s;
        q.y=(m12+m21)/s;
        q.z=0.25f*s;
    }
    const float n=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);
    if (n<1e-6f) return {0.f,0.f,0.f,1.f};
    return {q.x/n,q.y/n,q.z/n,q.w/n};
}

XrQuaternionf OrientationFacingPoint(const XrVector3f& from, const XrVector3f& target) {
    XrVector3f z{target.x-from.x,target.y-from.y,target.z-from.z};
    float zn=std::sqrt(z.x*z.x+z.y*z.y+z.z*z.z);
    if (zn<1e-5f) return {0.f,0.f,0.f,1.f};
    z.x/=zn; z.y/=zn; z.z/=zn;
    XrVector3f up{0.f,1.f,0.f};
    XrVector3f x{up.y*z.z-up.z*z.y,up.z*z.x-up.x*z.z,up.x*z.y-up.y*z.x};
    float xn=std::sqrt(x.x*x.x+x.y*x.y+x.z*x.z);
    if (xn<1e-5f) {
        up={0.f,0.f,1.f};
        x={up.y*z.z-up.z*z.y,up.z*z.x-up.x*z.z,up.x*z.y-up.y*z.x};
        xn=std::sqrt(x.x*x.x+x.y*x.y+x.z*x.z);
    }
    x.x/=xn; x.y/=xn; x.z/=xn;
    XrVector3f y{z.y*x.z-z.z*x.y,z.z*x.x-z.x*x.z,z.x*x.y-z.y*x.x};
    return QuaternionFromBasis(x,y,z);
}

XrQuaternionf YawOnlyOrientationFromForward(XrVector3f forward) {
    // Menus should be level with the horizon. Derive a yaw-only pose from the headset's
    // horizontal look direction so pitch/roll at the instant the menu opens cannot push the quad
    // above, below or diagonally away from the player's visual centre.
    const float horizontalLength = std::sqrt(forward.x * forward.x + forward.z * forward.z);
    if (horizontalLength < 1e-5f) {
        return {0.f, 0.f, 0.f, 1.f};
    }
    forward.x /= horizontalLength;
    forward.z /= horizontalLength;
    const float yaw = std::atan2(-forward.x, -forward.z);
    const float halfYaw = 0.5f * yaw;
    return {0.f, std::sin(halfYaw), 0.f, std::cos(halfYaw)};
}

XrPosef MakeLevelAnchor(const XrPosef& leftEye, const XrPosef& rightEye,
                        float distanceMeters, float verticalOffsetMeters = 0.f) {
    const XrVector3f center{
        0.5f * (leftEye.position.x + rightEye.position.x),
        0.5f * (leftEye.position.y + rightEye.position.y),
        0.5f * (leftEye.position.z + rightEye.position.z)};
    const XrVector3f leftForward = RotateVector(leftEye.orientation, {0.f, 0.f, -1.f});
    const XrVector3f rightForward = RotateVector(rightEye.orientation, {0.f, 0.f, -1.f});
    XrVector3f forward{
        0.5f * (leftForward.x + rightForward.x), 0.f,
        0.5f * (leftForward.z + rightForward.z)};
    const float length = std::sqrt(forward.x * forward.x + forward.z * forward.z);
    if (length >= 1e-5f) {
        forward.x /= length;
        forward.z /= length;
    } else {
        forward = {0.f, 0.f, -1.f};
    }
    XrPosef out{};
    out.orientation = YawOnlyOrientationFromForward(forward);
    out.position = {
        center.x + forward.x * distanceMeters,
        center.y + verticalOffsetMeters,
        center.z + forward.z * distanceMeters};
    return out;
}
}

const char* OpenXRContext::InternalDataPath() const {
    return (app_ && app_->activity && app_->activity->internalDataPath)
        ? app_->activity->internalDataPath : "";
}

bool OpenXRContext::InitializeLoader() {
    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    XrResult result = xrGetInstanceProcAddr(
        XR_NULL_HANDLE,
        "xrInitializeLoaderKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader));
    if (XR_FAILED(result) || initializeLoader == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "xrInitializeLoaderKHR unavailable: %d", result);
        return false;
    }

    XrLoaderInitInfoAndroidKHR loaderInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInfo.applicationVM = app_->activity->vm;
    loaderInfo.applicationContext = app_->activity->clazz;
    return XrOk(initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo)),
                "xrInitializeLoaderKHR");
}

bool OpenXRContext::HasInstanceExtension(const char* extension) const {
    return std::any_of(availableExtensions_.begin(), availableExtensions_.end(),
                       [extension](const XrExtensionProperties& p) {
                           return std::strcmp(p.extensionName, extension) == 0;
                       });
}

bool OpenXRContext::CreateInstance() {
    uint32_t extensionCount = 0;
    if (!XrOk(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr),
              "xrEnumerateInstanceExtensionProperties(count)")) {
        return false;
    }
    availableExtensions_.assign(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    if (!XrOk(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount,
                                                      availableExtensions_.data()),
              "xrEnumerateInstanceExtensionProperties")) {
        return false;
    }

    const char* required[] = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
    };
    for (const char* ext : required) {
        if (!HasInstanceExtension(ext)) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "Required OpenXR extension missing: %s", ext);
            return false;
        }
    }

    std::vector<const char*> enabledExtensions(std::begin(required), std::end(required));
    perfSettingsEnabled_ = HasInstanceExtension(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    refreshRateEnabled_ = HasInstanceExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    foveationEnabled_ = HasInstanceExtension(XR_FB_FOVEATION_EXTENSION_NAME) &&
                        HasInstanceExtension(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME) &&
                        HasInstanceExtension(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME) &&
                        HasInstanceExtension(XR_FB_SWAPCHAIN_UPDATE_STATE_OPENGL_ES_EXTENSION_NAME);
    if (perfSettingsEnabled_) enabledExtensions.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    if (refreshRateEnabled_) enabledExtensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (foveationEnabled_) {
        enabledExtensions.push_back(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME);
        enabledExtensions.push_back(XR_FB_SWAPCHAIN_UPDATE_STATE_OPENGL_ES_EXTENSION_NAME);
        enabledExtensions.push_back(XR_FB_FOVEATION_EXTENSION_NAME);
        enabledExtensions.push_back(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME);
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "Quest perf extensions: perf=%d refresh=%d foveation=%d",
                        perfSettingsEnabled_ ? 1 : 0,
                        refreshRateEnabled_ ? 1 : 0,
                        foveationEnabled_ ? 1 : 0);

    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = app_->activity->vm;
    androidInfo.applicationActivity = app_->activity->clazz;

    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.next = &androidInfo;
    std::strncpy(createInfo.applicationInfo.applicationName, "F-Zero X VR",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    std::strncpy(createInfo.applicationInfo.engineName, "G-Diffuser Quest Host",
                 XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    createInfo.enabledExtensionNames = enabledExtensions.data();

    // Quest 3 Touch Plus has a first-class interaction profile in OpenXR 1.1. Prefer 1.1 so the
    // runtime can expose it directly, but retain a 1.0 fallback for older Quest runtimes/loaders.
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_1;
    XrResult createResult = xrCreateInstance(&createInfo, &instance_);
    if (createResult == XR_ERROR_API_VERSION_UNSUPPORTED) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "OpenXR 1.1 unsupported by runtime; falling back to 1.0");
        createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        createResult = xrCreateInstance(&createInfo, &instance_);
    }
    if (!XrOk(createResult, "xrCreateInstance")) return false;
    __android_log_print(ANDROID_LOG_INFO, kTag, "OpenXR API requested: %u.%u.%u",
                        XR_VERSION_MAJOR(createInfo.applicationInfo.apiVersion),
                        XR_VERSION_MINOR(createInfo.applicationInfo.apiVersion),
                        XR_VERSION_PATCH(createInfo.applicationInfo.apiVersion));

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!XrOk(xrGetSystem(instance_, &systemInfo, &systemId_), "xrGetSystem")) return false;

    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(instance_, systemId_, &properties))) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "OpenXR system: %s", properties.systemName);
    }
    return true;
}

bool OpenXRContext::CreateEgl() {
    eglDisplay_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglDisplay_ == EGL_NO_DISPLAY) return false;

    EGLint major = 0, minor = 0;
    if (eglInitialize(eglDisplay_, &major, &minor) != EGL_TRUE) return EglOk("eglInitialize");
    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) return EglOk("eglBindAPI");

#ifdef EGL_OPENGL_ES3_BIT_KHR
    constexpr EGLint renderable = EGL_OPENGL_ES3_BIT_KHR;
#else
    constexpr EGLint renderable = EGL_OPENGL_ES2_BIT;
#endif

    const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, renderable,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE,
    };
    EGLint configCount = 0;
    if (eglChooseConfig(eglDisplay_, configAttribs, &eglConfig_, 1, &configCount) != EGL_TRUE || configCount < 1) {
        EglOk("eglChooseConfig");
        return false;
    }

    const EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE,
    };
    eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, contextAttribs);
    if (eglContext_ == EGL_NO_CONTEXT) return EglOk("eglCreateContext");

    // Fast3D can legitimately render to framebuffer 0 when it does not need an offscreen
    // game framebuffer. The EGL surface is therefore a real render target, not merely a tiny
    // context anchor. Size it to the OpenXR recommended stereo view extent so direct Fast3D
    // rendering is never quantized through the old 16x16 pbuffer and then magnified into giant
    // pixels in the headset.
    EGLint pbufferWidth = 2048;
    EGLint pbufferHeight = 2048;
    uint32_t viewCount = 0;
    if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(instance_, systemId_,
                                                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                        0, &viewCount, nullptr)) &&
        viewCount > 0) {
        std::vector<XrViewConfigurationView> configs(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(instance_, systemId_,
                                                            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                            viewCount, &viewCount, configs.data()))) {
            pbufferWidth = 1;
            pbufferHeight = 1;
            for (const auto& view : configs) {
                pbufferWidth = std::max<EGLint>(pbufferWidth,
                                                static_cast<EGLint>(view.recommendedImageRectWidth));
                pbufferHeight = std::max<EGLint>(pbufferHeight,
                                                 static_cast<EGLint>(view.recommendedImageRectHeight));
            }
        }
    }

    const EGLint pbufferAttribs[] = {
        EGL_WIDTH, pbufferWidth,
        EGL_HEIGHT, pbufferHeight,
        EGL_NONE,
    };
    eglSurface_ = eglCreatePbufferSurface(eglDisplay_, eglConfig_, pbufferAttribs);
    if (eglSurface_ == EGL_NO_SURFACE) return EglOk("eglCreatePbufferSurface");
    if (eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_) != EGL_TRUE) {
        return EglOk("eglMakeCurrent");
    }

    __android_log_print(ANDROID_LOG_INFO, kTag, "EGL %d.%d / GLES %s / pbuffer %dx%d", major, minor,
                        reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                        pbufferWidth, pbufferHeight);
    return true;
}

bool OpenXRContext::CreateSession() {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
    if (!XrOk(xrGetInstanceProcAddr(instance_, "xrGetOpenGLESGraphicsRequirementsKHR",
                                    reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)),
              "xrGetInstanceProcAddr(xrGetOpenGLESGraphicsRequirementsKHR)") || getRequirements == nullptr) {
        return false;
    }

    XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (!XrOk(getRequirements(instance_, systemId_, &requirements), "xrGetOpenGLESGraphicsRequirementsKHR")) {
        return false;
    }

    const XrVersion gles30 = XR_MAKE_VERSION(3, 0, 0);
    if (gles30 < requirements.minApiVersionSupported || gles30 > requirements.maxApiVersionSupported) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "Runtime GLES range does not contain 3.0 (min=%llu max=%llu)",
                            static_cast<unsigned long long>(requirements.minApiVersionSupported),
                            static_cast<unsigned long long>(requirements.maxApiVersionSupported));
    }

    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = eglDisplay_;
    binding.config = eglConfig_;
    binding.context = eglContext_;

    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = systemId_;
    return XrOk(xrCreateSession(instance_, &sessionInfo, &session_), "xrCreateSession");
}

bool OpenXRContext::CreateReferenceSpace() {
    XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    info.poseInReferenceSpace.orientation.w = 1.f;
    return XrOk(xrCreateReferenceSpace(session_, &info, &appSpace_), "xrCreateReferenceSpace(LOCAL)");
}

bool OpenXRContext::ConfigureQuestPerformance() {
    if (perfSettingsEnabled_) {
        xrGetInstanceProcAddr(instance_, "xrPerfSettingsSetPerformanceLevelEXT",
                              reinterpret_cast<PFN_xrVoidFunction*>(&perfSettingsSetPerformanceLevel_));
        if (perfSettingsSetPerformanceLevel_ != nullptr) {
            const XrResult cpu = perfSettingsSetPerformanceLevel_(
                session_, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
            const XrResult gpu = perfSettingsSetPerformanceLevel_(
                session_, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
            __android_log_print((XR_SUCCEEDED(cpu) && XR_SUCCEEDED(gpu)) ? ANDROID_LOG_INFO : ANDROID_LOG_WARN,
                                kTag, "Quest perf level sustained-high CPU=%d GPU=%d", cpu, gpu);
        }
    }

    if (refreshRateEnabled_) {
        xrGetInstanceProcAddr(instance_, "xrEnumerateDisplayRefreshRatesFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&enumerateDisplayRefreshRates_));
        xrGetInstanceProcAddr(instance_, "xrGetDisplayRefreshRateFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&getDisplayRefreshRate_));
        xrGetInstanceProcAddr(instance_, "xrRequestDisplayRefreshRateFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&requestDisplayRefreshRate_));
    }

    if (foveationEnabled_) {
        xrGetInstanceProcAddr(instance_, "xrCreateFoveationProfileFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&createFoveationProfile_));
        xrGetInstanceProcAddr(instance_, "xrDestroyFoveationProfileFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&destroyFoveationProfile_));
        xrGetInstanceProcAddr(instance_, "xrUpdateSwapchainFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&updateSwapchain_));
        if (createFoveationProfile_ != nullptr && updateSwapchain_ != nullptr) {
            XrFoveationLevelProfileCreateInfoFB level{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
            level.level = XR_FOVEATION_LEVEL_HIGH_FB;
            level.verticalOffset = 0.f;
            level.dynamic = XR_FOVEATION_DYNAMIC_LEVEL_ENABLED_FB;
            XrFoveationProfileCreateInfoFB profileInfo{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
            profileInfo.next = &level;
            const XrResult result = createFoveationProfile_(session_, &profileInfo, &foveationProfile_);
            if (XR_FAILED(result)) {
                foveationProfile_ = XR_NULL_HANDLE;
                __android_log_print(ANDROID_LOG_WARN, kTag, "xrCreateFoveationProfileFB failed: %d", result);
            } else {
                __android_log_print(ANDROID_LOG_INFO, kTag,
                                    "Quest FFR profile: HIGH + dynamic (projection swapchains only)");
            }
        }
    }
    return true;
}

void OpenXRContext::RequestQuestRefreshRate() {
    if (!refreshRateEnabled_ || enumerateDisplayRefreshRates_ == nullptr || requestDisplayRefreshRate_ == nullptr ||
        session_ == XR_NULL_HANDLE || !sessionRunning_) {
        return;
    }
    uint32_t count = 0;
    const XrResult countResult = enumerateDisplayRefreshRates_(session_, 0, &count, nullptr);
    if (XR_FAILED(countResult) || count == 0) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "Quest refresh enumeration unavailable while running: result=%d count=%u",
                            countResult, count);
        return;
    }
    std::vector<float> rates(count);
    const XrResult listResult = enumerateDisplayRefreshRates_(session_, count, &count, rates.data());
    if (XR_FAILED(listResult) || count == 0) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "Quest refresh list failed: %d", listResult);
        return;
    }
    const float targetHz = targetRefreshHz_;
    float chosen = rates.front();
    float bestDelta = std::fabs(chosen - targetHz);
    for (float rate : rates) {
        const float delta = std::fabs(rate - targetHz);
        if (delta < bestDelta) {
            chosen = rate;
            bestDelta = delta;
        }
    }
    float before = 0.f;
    if (getDisplayRefreshRate_ != nullptr) getDisplayRefreshRate_(session_, &before);
    const XrResult request = requestDisplayRefreshRate_(session_, chosen);
    __android_log_print(XR_SUCCEEDED(request) ? ANDROID_LOG_INFO : ANDROID_LOG_WARN,
                        kTag, "Quest refresh %.1f -> request %.1f Hz (target %.1f, %u modes) result=%d",
                        before, chosen, targetHz, count, request);
}

bool OpenXRContext::ApplyFoveation(XrSwapchain swapchain) {
    if (!foveationEnabled_ || foveationProfile_ == XR_NULL_HANDLE || updateSwapchain_ == nullptr ||
        swapchain == XR_NULL_HANDLE) {
        return false;
    }
    XrSwapchainStateFoveationFB state{XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB};
    state.profile = foveationProfile_;
    const XrResult result = updateSwapchain_(
        swapchain, reinterpret_cast<const XrSwapchainStateBaseHeaderFB*>(&state));
    if (XR_FAILED(result)) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "xrUpdateSwapchainFB(foveation) failed: %d", result);
        return false;
    }
    return true;
}

int64_t OpenXRContext::ChooseColorFormat() const {
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateSwapchainFormats(session_, 0, &count, nullptr)) || count == 0) return 0;
    std::vector<int64_t> formats(count);
    if (XR_FAILED(xrEnumerateSwapchainFormats(session_, count, &count, formats.data()))) return 0;

    constexpr int64_t preferred[] = {GL_SRGB8_ALPHA8, GL_RGBA8};
    for (int64_t want : preferred) {
        if (std::find(formats.begin(), formats.end(), want) != formats.end()) return want;
    }
    return formats.front();
}

bool OpenXRContext::CreateSwapchains() {
    uint32_t viewCount = 0;
    if (!XrOk(xrEnumerateViewConfigurationViews(instance_, systemId_,
                                                 XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                 0, &viewCount, nullptr),
              "xrEnumerateViewConfigurationViews(count)")) {
        return false;
    }
    if (viewCount != 2) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Expected stereo view count 2, got %u", viewCount);
        return false;
    }

    viewConfigs_.assign(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (!XrOk(xrEnumerateViewConfigurationViews(instance_, systemId_,
                                                 XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                 viewCount, &viewCount, viewConfigs_.data()),
              "xrEnumerateViewConfigurationViews")) {
        return false;
    }

    const int64_t colorFormat = ChooseColorFormat();
    if (colorFormat == 0) return false;

    swapchains_.resize(viewCount);
    views_.assign(viewCount, {XR_TYPE_VIEW});
    for (uint32_t i = 0; i < viewCount; ++i) {
        EyeSwapchain& eye = swapchains_[i];
        eye.width = viewConfigs_[i].recommendedImageRectWidth;
        eye.height = viewConfigs_[i].recommendedImageRectHeight;

        XrSwapchainCreateInfoFoveationFB foveationCreate{XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
        foveationCreate.flags = XR_SWAPCHAIN_CREATE_FOVEATION_SCALED_BIN_BIT_FB;
        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create.next = (foveationEnabled_ && foveationProfile_ != XR_NULL_HANDLE) ? &foveationCreate : nullptr;
        create.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        create.format = colorFormat;
        create.sampleCount = 1;
        create.width = eye.width;
        create.height = eye.height;
        create.faceCount = 1;
        create.arraySize = 1;
        create.mipCount = 1;
        if (!XrOk(xrCreateSwapchain(session_, &create, &eye.handle), "xrCreateSwapchain")) return false;
        if (foveationEnabled_ && foveationProfile_ != XR_NULL_HANDLE) {
            const bool applied = ApplyFoveation(eye.handle);
            __android_log_print(applied ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, kTag,
                                "eye %u FFR %s", i, applied ? "enabled" : "unavailable");
        }

        uint32_t imageCount = 0;
        if (!XrOk(xrEnumerateSwapchainImages(eye.handle, 0, &imageCount, nullptr),
                  "xrEnumerateSwapchainImages(count)")) return false;
        eye.images.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        if (!XrOk(xrEnumerateSwapchainImages(
                      eye.handle, imageCount, &imageCount,
                      reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data())),
                  "xrEnumerateSwapchainImages")) return false;

        __android_log_print(ANDROID_LOG_INFO, kTag, "eye %u swapchain %ux%u (%u images)",
                            i, eye.width, eye.height, imageCount);
    }

    // Dedicated 4:3 compositor surface shared by menus and the race HUD. It must be independent
    // from the projection swapchains because gameplay submits the two eye images and the HUD quad
    // in the same xrEndFrame call.
    quadSwapchain_.width = 2048;
    quadSwapchain_.height = 1536;
    XrSwapchainCreateInfo quadCreate{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    quadCreate.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    quadCreate.format = colorFormat;
    quadCreate.sampleCount = 1;
    quadCreate.width = quadSwapchain_.width;
    quadCreate.height = quadSwapchain_.height;
    quadCreate.faceCount = 1;
    quadCreate.arraySize = 1;
    quadCreate.mipCount = 1;
    if (!XrOk(xrCreateSwapchain(session_, &quadCreate, &quadSwapchain_.handle),
              "xrCreateSwapchain(quad)")) return false;
    uint32_t quadImageCount = 0;
    if (!XrOk(xrEnumerateSwapchainImages(quadSwapchain_.handle, 0, &quadImageCount, nullptr),
              "xrEnumerateSwapchainImages(quad count)")) return false;
    quadSwapchain_.images.assign(quadImageCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    if (!XrOk(xrEnumerateSwapchainImages(
                  quadSwapchain_.handle, quadImageCount, &quadImageCount,
                  reinterpret_cast<XrSwapchainImageBaseHeader*>(quadSwapchain_.images.data())),
              "xrEnumerateSwapchainImages(quad)")) return false;
    __android_log_print(ANDROID_LOG_INFO, kTag, "quad swapchain %ux%u (%u images)",
                        quadSwapchain_.width, quadSwapchain_.height, quadImageCount);
    return true;
}

bool OpenXRContext::Initialize(android_app* app) {
    app_ = app;
    if (!app_ || !app_->activity) return false;
    targetRefreshHz_ = LoadTargetRefreshHz(InternalDataPath());
    __android_log_print(ANDROID_LOG_INFO, kTag, "Launcher VR settings: target refresh %.1f Hz", targetRefreshHz_);
    if (!InitializeLoader()) return false;
    if (!CreateInstance()) return false;
    if (!CreateEgl()) return false;
    if (!CreateSession()) return false;
    if (!ConfigureQuestPerformance()) return false;
    if (!CreateReferenceSpace()) return false;
    if (!CreateSwapchains()) return false;
    return true;
}

void OpenXRContext::PollEvents(bool& shouldExit) {
    if (instance_ == XR_NULL_HANDLE) return;

    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                sessionState_ = changed->state;
                __android_log_print(ANDROID_LOG_INFO, kTag, "session state -> %d", sessionState_);

                if (sessionState_ == XR_SESSION_STATE_READY && !sessionRunning_) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (XR_SUCCEEDED(xrBeginSession(session_, &begin))) {
                        sessionRunning_ = true;
                        sessionEverBegan_ = true;
                        RequestQuestRefreshRate();
                    }
                } else if (sessionState_ == XR_SESSION_STATE_STOPPING && sessionRunning_) {
                    xrEndSession(session_);
                    sessionRunning_ = false;
                    // STOPPING is NOT a final quit on Quest. Horizon can cycle an immersive app
                    // through STOPPING -> IDLE -> READY while changing VR presentation state
                    // (for example when F-Zero transitions from menus into a GP). Keep the Android
                    // activity alive and let READY restart the same OpenXR session. Final teardown
                    // is driven only by EXITING/LOSS_PENDING or Android DESTROY/STOP.
                } else if (sessionState_ == XR_SESSION_STATE_EXITING ||
                           sessionState_ == XR_SESSION_STATE_LOSS_PENDING) {
                    shouldExit = true;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
                const auto* changed = reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(&event);
                __android_log_print(ANDROID_LOG_INFO, kTag, "Quest refresh changed %.1f -> %.1f Hz",
                                    changed->fromDisplayRefreshRate, changed->toDisplayRefreshRate);
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                shouldExit = true;
                break;
            default:
                break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

bool OpenXRContext::RenderFrame(QuestInput& input, RendererGLES& renderer,
                                GDiffuserBridge& game, double frameSeconds) {
    if (!sessionRunning_) return true;

    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    if (!XrOk(xrWaitFrame(session_, &waitInfo, &frameState), "xrWaitFrame")) return false;

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (!XrOk(xrBeginFrame(session_, &beginInfo), "xrBeginFrame")) return false;

    std::vector<XrCompositionLayerProjectionView> layerViews(swapchains_.size());
    XrCompositionLayerProjection projectionLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projectionLayer.space = appSpace_;
    XrCompositionLayerQuad menuLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad hudLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};

    bool viewsValid = false;
    if (frameState.shouldRender == XR_TRUE) {
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = frameState.predictedDisplayTime;
        locate.space = appSpace_;

        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t viewCount = 0;
        if (XrOk(xrLocateViews(session_, &locate, &viewState,
                               static_cast<uint32_t>(views_.size()), &viewCount, views_.data()),
                 "xrLocateViews")) {
            const XrViewStateFlags validFlags = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
            viewsValid = viewCount == views_.size() && (viewState.viewStateFlags & validFlags) == validFlags;
        }

        // Always publish the predicted eye poses before the game tick. Gameplay uses them for the
        // stereo camera matrices; flat menus ignore the per-eye matrices but still need the current
        // headset pose so their world-space anchor can be created at the correct place.
        if (viewsValid) {
            VrEyeFrame stereoViews[2]{};
            for (uint32_t eyeIndex = 0; eyeIndex < 2; ++eyeIndex) {
                stereoViews[eyeIndex].eye = static_cast<int>(eyeIndex);
                stereoViews[eyeIndex].width = swapchains_[eyeIndex].width;
                stereoViews[eyeIndex].height = swapchains_[eyeIndex].height;
                stereoViews[eyeIndex].pose = views_[eyeIndex].pose;
                stereoViews[eyeIndex].fov = views_[eyeIndex].fov;
            }
            game.SetStereoViews(stereoViews, 2);
        }
    }

    input.Sync(session_, appSpace_, frameState.predictedDisplayTime);
    game.Tick60Hz(input.State(), frameSeconds);

    bool submitProjection = false;
    bool submitMenuQuad = false;
    bool submitHudQuad = false;
    if (frameState.shouldRender == XR_TRUE && viewsValid) {
        const bool flatUi = game.FlatUiActive();
        if (flatUi) {
            // A flat menu must NOT be submitted through two projection views: the runtime applies
            // two different optical projections to those images, so even byte-identical eye
            // textures do not fuse perfectly. Submit one physical OpenXR quad visible to BOTH eyes.
            // Its pose is captured only when entering a flat UI mode, making it a true world-space
            // panel instead of a head-locked HUD.
            if (!menuWasFlat_ || !menuAnchorValid_) {
                const XrVector3f center{
                    0.5f * (views_[0].pose.position.x + views_[1].pose.position.x),
                    0.5f * (views_[0].pose.position.y + views_[1].pose.position.y),
                    0.5f * (views_[0].pose.position.z + views_[1].pose.position.z)};

                // Average both eye forward vectors, then flatten to the horizontal plane. This
                // makes the panel appear exactly in front of the player rather than inheriting a
                // tiny per-eye offset or the headset's current pitch/roll.
                const XrVector3f forwardLeft = RotateVector(views_[0].pose.orientation, {0.f, 0.f, -1.f});
                const XrVector3f forwardRight = RotateVector(views_[1].pose.orientation, {0.f, 0.f, -1.f});
                XrVector3f forward{
                    0.5f * (forwardLeft.x + forwardRight.x),
                    0.f,
                    0.5f * (forwardLeft.z + forwardRight.z)};
                const float horizontalLength = std::sqrt(forward.x * forward.x + forward.z * forward.z);
                if (horizontalLength >= 1e-5f) {
                    forward.x /= horizontalLength;
                    forward.z /= horizontalLength;
                } else {
                    forward = {0.f, 0.f, -1.f};
                }

                constexpr float kMenuDistanceMeters = 3.0f;
                menuAnchorPose_.orientation = YawOnlyOrientationFromForward(forward);
                menuAnchorPose_.position = {
                    center.x + forward.x * kMenuDistanceMeters,
                    center.y,
                    center.z + forward.z * kMenuDistanceMeters};
                menuAnchorValid_ = true;
                __android_log_print(ANDROID_LOG_INFO, kTag,
                                    "menu quad centered 3.0m ahead at %.2f %.2f %.2f",
                                    menuAnchorPose_.position.x,
                                    menuAnchorPose_.position.y,
                                    menuAnchorPose_.position.z);
            }

            EyeSwapchain& eye = quadSwapchain_;
            uint32_t imageIndex = 0;
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (!XrOk(xrAcquireSwapchainImage(eye.handle, &acquire, &imageIndex),
                      "xrAcquireSwapchainImage(menu)")) return false;

            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            if (!XrOk(xrWaitSwapchainImage(eye.handle, &wait), "xrWaitSwapchainImage(menu)")) return false;

            VrEyeFrame renderEye{};
            renderEye.eye = 0;
            renderEye.width = eye.width;
            renderEye.height = eye.height;
            renderEye.colorTexture = eye.images[imageIndex].image;
            renderEye.pose = views_[0].pose;
            renderEye.fov = views_[0].fov;
            if (!renderer.RenderEye(renderEye, game, input.State())) return false;

            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            if (!XrOk(xrReleaseSwapchainImage(eye.handle, &release),
                      "xrReleaseSwapchainImage(menu)")) return false;

            menuLayer.space = appSpace_;
            menuLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            menuLayer.pose = menuAnchorPose_;
            menuLayer.subImage.swapchain = eye.handle;
            menuLayer.subImage.imageRect.offset = {0, 0};
            menuLayer.subImage.imageRect.extent = {
                static_cast<int32_t>(eye.width), static_cast<int32_t>(eye.height)};
            menuLayer.subImage.imageArrayIndex = 0;
            // F-Zero X menus are authored for a 320x240 (4:3) presentation. The Quest eye
            // swapchain is nearly square (2800x2933); using that ratio made the quad far too tall
            // and visually cropped the UI. Keep the physical panel at the game's native 4:3 ratio.
            constexpr float kMenuWidthMeters = 2.2f;
            menuLayer.size.width = kMenuWidthMeters;
            menuLayer.size.height = kMenuWidthMeters * (3.0f / 4.0f);
            submitMenuQuad = true;
        } else {
            menuAnchorValid_ = false;
            for (uint32_t eyeIndex = 0; eyeIndex < swapchains_.size(); ++eyeIndex) {
                EyeSwapchain& eye = swapchains_[eyeIndex];
                uint32_t imageIndex = 0;
                XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                if (!XrOk(xrAcquireSwapchainImage(eye.handle, &acquire, &imageIndex),
                          "xrAcquireSwapchainImage")) return false;

                XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wait.timeout = XR_INFINITE_DURATION;
                if (!XrOk(xrWaitSwapchainImage(eye.handle, &wait), "xrWaitSwapchainImage")) return false;

                VrEyeFrame renderEye{};
                renderEye.eye = static_cast<int>(eyeIndex);
                renderEye.width = eye.width;
                renderEye.height = eye.height;
                renderEye.colorTexture = eye.images[imageIndex].image;
                renderEye.pose = views_[eyeIndex].pose;
                renderEye.fov = views_[eyeIndex].fov;
                if (!renderer.RenderEye(renderEye, game, input.State())) return false;

                XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                if (!XrOk(xrReleaseSwapchainImage(eye.handle, &release), "xrReleaseSwapchainImage")) return false;

                XrCompositionLayerProjectionView& projectionView = layerViews[eyeIndex];
                projectionView = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                // The Fast3D eye texture may be reused for one or more XR frames (game ~60 Hz,
                // compositor 72+ Hz). Submit the pose/FOV that actually produced this cached image;
                // otherwise OpenXR timewarps from the wrong origin and the terrain appears to
                // wobble/follow the head during turns.
                XrPosef renderedPose = views_[eyeIndex].pose;
                XrFovf renderedFov = views_[eyeIndex].fov;
                game.GetCachedEyeView(static_cast<int>(eyeIndex), renderedPose, renderedFov);
                projectionView.pose = renderedPose;
                projectionView.fov = renderedFov;
                projectionView.subImage.swapchain = eye.handle;
                projectionView.subImage.imageRect.offset = {0, 0};
                projectionView.subImage.imageRect.extent = {
                    static_cast<int32_t>(eye.width), static_cast<int32_t>(eye.height)};
                projectionView.subImage.imageArrayIndex = 0;
            }
            projectionLayer.viewCount = static_cast<uint32_t>(layerViews.size());
            projectionLayer.views = layerViews.data();
            submitProjection = true;

            const bool dioramaHud = game.DioramaActive();
            const QuestGameInput& liveInput = input.State();
            const bool wristHud = liveInput.hands[0].active;
            if (game.RaceHudActive() && quadSwapchain_.handle != XR_NULL_HANDLE) {
                if (wristHud) {
                    // Hand HUD: follow the LEFT grip every OpenXR frame and place the panel just
                    // above the back of the hand / controller, slightly toward the knuckles instead
                    // of back along the forearm. Keep it facing the cyclopean eye for readability.
                    const XrPosef& leftGrip = liveInput.hands[0].gripPose;
                    const XrVector3f handOffset = RotateVector(leftGrip.orientation,{0.f,0.060f,-0.035f});
                    hudAnchorPose_.position={
                        leftGrip.position.x+handOffset.x,
                        leftGrip.position.y+handOffset.y,
                        leftGrip.position.z+handOffset.z};
                    const XrVector3f headCenter{
                        0.5f*(views_[0].pose.position.x+views_[1].pose.position.x),
                        0.5f*(views_[0].pose.position.y+views_[1].pose.position.y),
                        0.5f*(views_[0].pose.position.z+views_[1].pose.position.z)};
                    hudAnchorPose_.orientation=OrientationFacingPoint(hudAnchorPose_.position,headCenter);
                    if (!hudAnchorValid_) {
                        __android_log_print(ANDROID_LOG_INFO,kTag,
                                            "race HUD attached to left hand");
                    }
                    hudAnchorValid_=true;
                    hudWasDiorama_=dioramaHud;
                } else if (!hudAnchorValid_ || hudWasDiorama_ != dioramaHud) {
                    // Tracking fallback: keep the previous comfortable world-space HUD behaviour.
                    if (dioramaHud) {
                        hudAnchorPose_=MakeLevelAnchor(views_[0].pose,views_[1].pose,0.65f,-0.35f);
                    } else {
                        hudAnchorPose_=MakeLevelAnchor(views_[0].pose,views_[1].pose,4.0f);
                    }
                    hudAnchorValid_=true;
                    hudWasDiorama_=dioramaHud;
                }

                uint32_t hudImageIndex = 0;
                XrSwapchainImageAcquireInfo hudAcquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                if (!XrOk(xrAcquireSwapchainImage(quadSwapchain_.handle, &hudAcquire, &hudImageIndex),
                          "xrAcquireSwapchainImage(hud)")) return false;
                XrSwapchainImageWaitInfo hudWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                hudWait.timeout = XR_INFINITE_DURATION;
                if (!XrOk(xrWaitSwapchainImage(quadSwapchain_.handle, &hudWait),
                          "xrWaitSwapchainImage(hud)")) return false;

                VrEyeFrame hudEye{};
                hudEye.eye = 0;
                hudEye.width = quadSwapchain_.width;
                hudEye.height = quadSwapchain_.height;
                hudEye.colorTexture = quadSwapchain_.images[hudImageIndex].image;
                hudEye.pose = views_[0].pose;
                hudEye.fov = views_[0].fov;
                if (!game.RenderHud(hudEye)) return false;
                glFlush();

                XrSwapchainImageReleaseInfo hudRelease{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                if (!XrOk(xrReleaseSwapchainImage(quadSwapchain_.handle, &hudRelease),
                          "xrReleaseSwapchainImage(hud)")) return false;

                // HUD matte reconstruction outputs premultiplied RGB (foreground * alpha), which
                // is OpenXR's default source-alpha convention. Do not request unpremultiplied
                // handling or the compositor would multiply the HUD by alpha a second time.
                hudLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                hudLayer.space = appSpace_;
                hudLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                hudLayer.pose = hudAnchorPose_;
                hudLayer.subImage.swapchain = quadSwapchain_.handle;
                hudLayer.subImage.imageRect.offset = {0, 0};
                hudLayer.subImage.imageRect.extent = {
                    static_cast<int32_t>(quadSwapchain_.width),
                    static_cast<int32_t>(quadSwapchain_.height)};
                hudLayer.subImage.imageArrayIndex = 0;
                // Preserve approximately the same angular HUD size when moving its physical plane
                // from 4 m to the 0.65 m diorama. This keeps the native 4:3 UI near the peripheral
                // edges while its binocular depth now matches the miniature world.
                const float hudWidthMeters = wristHud ? 0.24f : (dioramaHud ? 0.62f : 3.6f);
                hudLayer.size.width = hudWidthMeters;
                hudLayer.size.height = hudWidthMeters * (3.0f / 4.0f);
                submitHudQuad = true;
            } else {
                hudAnchorValid_ = false;
                hudWasDiorama_ = false;
            }
        }
        menuWasFlat_ = flatUi;
    }

    const XrCompositionLayerBaseHeader* submittedLayers[2]{};
    uint32_t submittedLayerCount = 0;
    if (submitMenuQuad) {
        submittedLayers[submittedLayerCount++] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menuLayer);
    } else if (submitProjection) {
        submittedLayers[submittedLayerCount++] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer);
        if (submitHudQuad) {
            // Later composition layers appear on top of earlier layers; keep the world projection
            // first and the alpha HUD panel second.
            submittedLayers[submittedLayerCount++] =
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudLayer);
        }
    }

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = submittedLayerCount;
    endInfo.layers = submittedLayerCount ? submittedLayers : nullptr;
    return XrOk(xrEndFrame(session_, &endInfo), "xrEndFrame");
}

void OpenXRContext::Shutdown() {
    if (sessionRunning_ && session_ != XR_NULL_HANDLE) {
        xrEndSession(session_);
        sessionRunning_ = false;
    }

    for (auto& eye : swapchains_) {
        if (eye.handle != XR_NULL_HANDLE) xrDestroySwapchain(eye.handle);
        eye.handle = XR_NULL_HANDLE;
    }
    if (quadSwapchain_.handle != XR_NULL_HANDLE) xrDestroySwapchain(quadSwapchain_.handle);
    quadSwapchain_ = {};
    swapchains_.clear();
    views_.clear();
    viewConfigs_.clear();

    if (appSpace_ != XR_NULL_HANDLE) xrDestroySpace(appSpace_);
    if (foveationProfile_ != XR_NULL_HANDLE && destroyFoveationProfile_ != nullptr) {
        destroyFoveationProfile_(foveationProfile_);
    }
    foveationProfile_ = XR_NULL_HANDLE;
    if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
    appSpace_ = XR_NULL_HANDLE;
    session_ = XR_NULL_HANDLE;
    sessionEverBegan_ = false;
    menuWasFlat_ = false;
    menuAnchorValid_ = false;
    hudAnchorValid_ = false;

    if (eglDisplay_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (eglSurface_ != EGL_NO_SURFACE) eglDestroySurface(eglDisplay_, eglSurface_);
        if (eglContext_ != EGL_NO_CONTEXT) eglDestroyContext(eglDisplay_, eglContext_);
        eglTerminate(eglDisplay_);
    }
    eglSurface_ = EGL_NO_SURFACE;
    eglContext_ = EGL_NO_CONTEXT;
    eglDisplay_ = EGL_NO_DISPLAY;
    eglConfig_ = nullptr;

    if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
    instance_ = XR_NULL_HANDLE;
    systemId_ = XR_NULL_SYSTEM_ID;
    availableExtensions_.clear();
    perfSettingsSetPerformanceLevel_ = nullptr;
    enumerateDisplayRefreshRates_ = nullptr;
    getDisplayRefreshRate_ = nullptr;
    requestDisplayRefreshRate_ = nullptr;
    createFoveationProfile_ = nullptr;
    destroyFoveationProfile_ = nullptr;
    updateSwapchain_ = nullptr;
    perfSettingsEnabled_ = false;
    refreshRateEnabled_ = false;
    foveationEnabled_ = false;
    app_ = nullptr;
}
