#include "quest_fast3d_window.h"

#include <fast/backends/gfx_opengl.h>
#include <fast/backends/gfx_window_manager_api.h>
#include <fast/debug/GfxDebugger.h>
#include <fast/interpreter.h>

#include <algorithm>
#include <chrono>
#include <memory>

namespace Fast {
extern void GfxSetInstance(std::shared_ptr<Interpreter> gfx);
}

namespace {
class QuestWindowBackend final : public Fast::GfxWindowBackend {
public:
    void Init(const char*, const char*, bool startFullscreen, uint32_t width, uint32_t height,
              int32_t posX, int32_t posY) override {
        width_ = width;
        height_ = height;
        posX_ = posX;
        posY_ = posY;
        mFullScreen = startFullscreen;
        mIsRunning = true;
    }
    void Close() override { mIsRunning = false; }
    void SetKeyboardCallbacks(bool (*onDown)(int), bool (*onUp)(int), void (*onAllUp)()) override {
        mOnKeyDown = onDown; mOnKeyUp = onUp; onAllUp_ = onAllUp;
    }
    void SetMouseCallbacks(bool (*onDown)(int), bool (*onUp)(int)) override {
        mOnMouseButtonDown = onDown; mOnMouseButtonUp = onUp;
    }
    void SetFullscreenChangedCallback(void (*cb)(bool)) override { mOnFullscreenChanged = cb; }
    void SetFullscreen(bool fullscreen) override {
        mFullScreen = fullscreen;
        if (mOnFullscreenChanged) mOnFullscreenChanged(fullscreen);
    }
    void GetActiveWindowRefreshRate(uint32_t* refreshRate) override { if (refreshRate) *refreshRate = refreshRate_; }
    void SetCursorVisibility(bool) override {}
    void SetMousePos(int32_t x, int32_t y) override { mouseX_ = x; mouseY_ = y; }
    void GetMousePos(int32_t* x, int32_t* y) override { if (x) *x = mouseX_; if (y) *y = mouseY_; }
    void GetMouseDelta(int32_t* x, int32_t* y) override { if (x) *x = 0; if (y) *y = 0; }
    void GetMouseWheel(float* x, float* y) override { if (x) *x = 0.f; if (y) *y = 0.f; }
    bool GetMouseState(uint32_t) override { return false; }
    void SetMouseCapture(bool capture) override { captured_ = capture; }
    bool IsMouseCaptured() override { return captured_; }
    void GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) override {
        if (width) *width = width_; if (height) *height = height_;
        if (posX) *posX = posX_; if (posY) *posY = posY_;
    }
    void SetDimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) override {
        width_ = width; height_ = height; posX_ = posX; posY_ = posY;
    }
    Ship::WindowRect GetPrimaryMonitorRect() override {
        return {0, 0, static_cast<int32_t>(width_), static_cast<int32_t>(height_)};
    }
    void HandleEvents() override {}
    bool IsFrameReady() override { return true; }
    void SwapBuffersBegin() override {}
    void SwapBuffersEnd() override {}
    double GetTime() override {
        using Clock = std::chrono::steady_clock;
        static const auto start = Clock::now();
        return std::chrono::duration<double>(Clock::now() - start).count();
    }
    int GetTargetFps() override { return static_cast<int>(mTargetFps); }
    void SetTargetFps(int fps) override { mTargetFps = std::max(fps, 1); }
    void SetMaxFrameLatency(int) override {}
    const char* GetKeyName(int) override { return ""; }
    bool CanDisableVsync() override { return false; }
    bool IsRunning() override { return mIsRunning; }
    void Destroy() override { mIsRunning = false; }
    bool IsFullscreen() override { return true; }

    void SetRefreshRate(uint32_t hz) { refreshRate_ = std::max(hz, 1u); }

private:
    uint32_t width_ = 2048;
    uint32_t height_ = 2048;
    uint32_t refreshRate_ = 90;
    int32_t posX_ = 0;
    int32_t posY_ = 0;
    int32_t mouseX_ = 0;
    int32_t mouseY_ = 0;
    bool captured_ = false;
    void (*onAllUp_)() = nullptr;
};

// These live for the process lifetime. Fast3D's Interpreter keeps raw backend pointers and Quest
// owns one EGL/OpenXR graphics context for the entire NativeActivity, so process-lifetime storage
// is both simpler and safer than handing ownership through Fast3dWindow's desktop-private fields.
QuestWindowBackend gQuestWindowBackend;
Fast::GfxRenderingAPIOGL gQuestRenderingApiLeft;
}

QuestFast3dWindow::QuestFast3dWindow(uint32_t width, uint32_t height)
    : Fast::Fast3dWindow(), width_(width), height_(height) {}

QuestFast3dWindow::~QuestFast3dWindow() = default;

void QuestFast3dWindow::Init() {
    if (initialized_) return;
    auto left = InterpreterShared();
    if (!left) return;
    gQuestWindowBackend.SetDimensions(width_, height_, 0, 0);

    left->SetGfxDebugger(std::make_shared<Fast::GfxDebugger>());
    left->Init(&gQuestWindowBackend, &gQuestRenderingApiLeft, "F-Zero X VR Left", true, width_, height_, 0, 0);

    // Quest stereo deliberately uses ONE Fast3D interpreter for both eyes. G-Diffuser prepares
    // task-local segment pointers, microcode variant, texture invalidations and cache state only on
    // the primary interpreter before handing the converted list to the VR host. A second independent
    // interpreter therefore starts from incomplete state and eventually diverges (most visibly in
    // the right eye). The VR host replays this one prepared interpreter deterministically twice,
    // restoring the task-start RSP/RDP state between eyes and caching each eye before the next run.

    // Fast3D's command handlers resolve the active interpreter through a process-global weak ptr.
    // Keep the primary interpreter selected outside the explicit stereo replay loop.
    Fast::GfxSetInstance(left);
    initialized_ = true;
}

void QuestFast3dWindow::Close() { running_ = false; gQuestWindowBackend.Close(); }
void QuestFast3dWindow::RunGuiOnly() { if (auto i = InterpreterShared()) i->RunGuiOnly(); }
void QuestFast3dWindow::StartFrame() { if (auto i = InterpreterShared()) i->StartFrame(); }
void QuestFast3dWindow::EndFrame() { if (auto i = InterpreterShared()) i->EndFrame(); }
bool QuestFast3dWindow::IsFrameReady() { return true; }
void QuestFast3dWindow::HandleEvents() {}
void QuestFast3dWindow::SetCursorVisibility(bool) {}
uint32_t QuestFast3dWindow::GetWidth() { return width_; }
uint32_t QuestFast3dWindow::GetHeight() { return height_; }
int32_t QuestFast3dWindow::GetPosX() { return 0; }
int32_t QuestFast3dWindow::GetPosY() { return 0; }
float QuestFast3dWindow::GetAspectRatio() { return height_ ? static_cast<float>(width_) / height_ : 1.f; }
void QuestFast3dWindow::SetMousePos(Ship::Coords) {}
Ship::Coords QuestFast3dWindow::GetMousePos() { return {0, 0}; }
Ship::Coords QuestFast3dWindow::GetMouseDelta() { return {0, 0}; }
Ship::CoordsF QuestFast3dWindow::GetMouseWheel() { return {0.f, 0.f}; }
bool QuestFast3dWindow::GetMouseState(Ship::MouseBtn) { return false; }
void QuestFast3dWindow::SetMouseCapture(bool) {}
bool QuestFast3dWindow::IsMouseCaptured() { return false; }
uint32_t QuestFast3dWindow::GetCurrentRefreshRate() { uint32_t hz = 90; gQuestWindowBackend.GetActiveWindowRefreshRate(&hz); return hz; }
bool QuestFast3dWindow::SupportsWindowedFullscreen() { return false; }
bool QuestFast3dWindow::CanDisableVerticalSync() { return false; }
void QuestFast3dWindow::SetResolutionMultiplier(float multiplier) {
    if (auto i = InterpreterShared()) i->SetResolutionMultiplier(multiplier);
}
void QuestFast3dWindow::SetMsaaLevel(uint32_t value) {
    if (auto i = InterpreterShared()) i->SetMsaaLevel(value);
}
void QuestFast3dWindow::SetFullscreen(bool) {}
bool QuestFast3dWindow::IsFullscreen() { return true; }
bool QuestFast3dWindow::IsRunning() { return running_; }
uintptr_t QuestFast3dWindow::GetGfxFrameBuffer() { auto i = InterpreterShared(); return i ? i->mGfxFrameBuffer : 0; }
const char* QuestFast3dWindow::GetKeyName(int32_t) { return ""; }
std::string QuestFast3dWindow::GetWindowBackendName() { return "Quest OpenXR OpenGL ES"; }
void QuestFast3dWindow::SetCurrentDimensions(uint32_t width, uint32_t height) { SetEyeDimensions(width, height); }
void QuestFast3dWindow::SetCurrentDimensions(uint32_t width, uint32_t height, int32_t, int32_t) { SetEyeDimensions(width, height); }
void QuestFast3dWindow::SetCurrentDimensions(bool, uint32_t width, uint32_t height) { SetEyeDimensions(width, height); }
void QuestFast3dWindow::SetCurrentDimensions(bool, uint32_t width, uint32_t height, int32_t, int32_t) { SetEyeDimensions(width, height); }
Ship::WindowRect QuestFast3dWindow::GetPrimaryMonitorRect() { return {0, 0, static_cast<int32_t>(width_), static_cast<int32_t>(height_)}; }

void QuestFast3dWindow::SetEyeDimensions(uint32_t width, uint32_t height) {
    width_ = std::max(width, 1u);
    height_ = std::max(height, 1u);
    gQuestWindowBackend.SetDimensions(width_, height_, 0, 0);
    if (auto i = InterpreterShared()) {
        i->mCurDimensions.width = width_;
        i->mCurDimensions.height = height_;
    }
}

std::shared_ptr<Fast::Interpreter> QuestFast3dWindow::InterpreterShared() const {
    return const_cast<QuestFast3dWindow*>(this)->GetInterpreterWeak().lock();
}

std::shared_ptr<Fast::Interpreter> QuestFast3dWindow::InterpreterForEye(int) const {
    return InterpreterShared();
}
