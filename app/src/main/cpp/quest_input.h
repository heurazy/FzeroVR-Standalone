#pragma once

#include "vr_types.h"

#include <openxr/openxr.h>

class QuestInput {
public:
    bool Initialize(XrInstance instance, XrSession session);
    void Shutdown();
    bool Sync(XrSession session, XrSpace baseSpace, XrTime time);

    const QuestGameInput& State() const { return state_; }
    void Pulse(XrSession session, int hand, float amplitude, float seconds, float frequency = XR_FREQUENCY_UNSPECIFIED);

private:
    XrAction CreateAction(const char* name, const char* localizedName, XrActionType type);
    XrPath Path(const char* path) const;
    bool ReadBool(XrAction action) const;
    float ReadFloat(XrAction action) const;
    XrVector2f ReadVec2(XrAction action) const;

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSession session_ = XR_NULL_HANDLE;
    XrActionSet actionSet_ = XR_NULL_HANDLE;

    XrAction leftStick_ = XR_NULL_HANDLE;
    XrAction rightStick_ = XR_NULL_HANDLE;
    XrAction leftTrigger_ = XR_NULL_HANDLE;
    XrAction rightTrigger_ = XR_NULL_HANDLE;
    XrAction leftSqueeze_ = XR_NULL_HANDLE;
    XrAction rightSqueeze_ = XR_NULL_HANDLE;
    XrAction buttonA_ = XR_NULL_HANDLE;
    XrAction buttonB_ = XR_NULL_HANDLE;
    XrAction buttonX_ = XR_NULL_HANDLE;
    XrAction buttonY_ = XR_NULL_HANDLE;
    XrAction buttonMenu_ = XR_NULL_HANDLE;
    XrAction leftStickClick_ = XR_NULL_HANDLE;
    XrAction rightStickClick_ = XR_NULL_HANDLE;
    XrAction leftGripPose_ = XR_NULL_HANDLE;
    XrAction rightGripPose_ = XR_NULL_HANDLE;
    XrAction leftHaptic_ = XR_NULL_HANDLE;
    XrAction rightHaptic_ = XR_NULL_HANDLE;

    XrSpace leftGripSpace_ = XR_NULL_HANDLE;
    XrSpace rightGripSpace_ = XR_NULL_HANDLE;
    QuestGameInput state_{};
    bool rumbleWasActive_ = false;
};
