#pragma once

#include "vr_types.h"

class GDiffuserBridge {
public:
    bool Bootstrap(const char* filesDir);
    void Shutdown();
    void SetStereoViews(const VrEyeFrame* eyes, int count);
    void Tick60Hz(const QuestGameInput& input, double frameSeconds);
    bool RenderEye(const VrEyeFrame& eye, const QuestGameInput& input);
    bool RenderHud(const VrEyeFrame& eye);
    bool GetCachedEyeView(int eye, XrPosef& pose, XrFovf& fov) const;
    bool FlatUiActive() const;
    bool RaceHudActive() const;
    bool DioramaActive() const;
    bool Connected() const { return connected_; }

private:
    bool connected_ = false;
    double tickAccumulator_ = 0.0;
};
