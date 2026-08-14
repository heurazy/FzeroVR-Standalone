#include "quest_input.h"
#include "gdx_quest_input_provider.h"

#include <android/log.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
constexpr const char* kTag = "FZeroXVR/Input";

void LogXrError(const char* what, XrResult result) {
    if (XR_FAILED(result)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "%s failed: %d", what, result);
    }
}
}

XrPath QuestInput::Path(const char* path) const {
    XrPath result = XR_NULL_PATH;
    LogXrError(path, xrStringToPath(instance_, path, &result));
    return result;
}

XrAction QuestInput::CreateAction(const char* name, const char* localizedName, XrActionType type) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(info.localizedActionName, localizedName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    info.actionType = type;
    info.countSubactionPaths = 0;
    info.subactionPaths = nullptr;
    XrAction action = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateAction(actionSet_, &info, &action))) {
        return XR_NULL_HANDLE;
    }
    return action;
}

bool QuestInput::Initialize(XrInstance instance, XrSession session) {
    instance_ = instance;
    session_ = session;

    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(setInfo.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(setInfo.localizedActionSetName, "F-Zero X gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    setInfo.priority = 0;
    if (XR_FAILED(xrCreateActionSet(instance_, &setInfo, &actionSet_))) {
        return false;
    }

    leftStick_ = CreateAction("left_stick", "Steering", XR_ACTION_TYPE_VECTOR2F_INPUT);
    rightStick_ = CreateAction("right_stick", "Camera buttons", XR_ACTION_TYPE_VECTOR2F_INPUT);
    leftTrigger_ = CreateAction("left_trigger", "Brake", XR_ACTION_TYPE_FLOAT_INPUT);
    rightTrigger_ = CreateAction("right_trigger", "Accelerate", XR_ACTION_TYPE_FLOAT_INPUT);
    leftSqueeze_ = CreateAction("left_squeeze", "Left slide", XR_ACTION_TYPE_FLOAT_INPUT);
    rightSqueeze_ = CreateAction("right_squeeze", "Right slide", XR_ACTION_TYPE_FLOAT_INPUT);
    buttonA_ = CreateAction("button_a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT);
    buttonB_ = CreateAction("button_b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT);
    buttonX_ = CreateAction("button_x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT);
    buttonY_ = CreateAction("button_y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT);
    buttonMenu_ = CreateAction("button_menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
    leftStickClick_ = CreateAction("left_stick_click", "Left stick click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    rightStickClick_ = CreateAction("right_stick_click", "Right stick click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    leftGripPose_ = CreateAction("left_grip_pose", "Left controller pose", XR_ACTION_TYPE_POSE_INPUT);
    rightGripPose_ = CreateAction("right_grip_pose", "Right controller pose", XR_ACTION_TYPE_POSE_INPUT);
    leftHaptic_ = CreateAction("left_haptic", "Left haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT);
    rightHaptic_ = CreateAction("right_haptic", "Right haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT);

    if (leftStick_ == XR_NULL_HANDLE || rightStick_ == XR_NULL_HANDLE ||
        leftGripPose_ == XR_NULL_HANDLE || rightGripPose_ == XR_NULL_HANDLE) {
        return false;
    }

    struct BindingDef { XrAction action; const char* path; };
    const BindingDef defs[] = {
        {leftStick_, "/user/hand/left/input/thumbstick"},
        {rightStick_, "/user/hand/right/input/thumbstick"},
        {leftTrigger_, "/user/hand/left/input/trigger/value"},
        {rightTrigger_, "/user/hand/right/input/trigger/value"},
        {leftSqueeze_, "/user/hand/left/input/squeeze/value"},
        {rightSqueeze_, "/user/hand/right/input/squeeze/value"},
        {buttonA_, "/user/hand/right/input/a/click"},
        {buttonB_, "/user/hand/right/input/b/click"},
        {buttonX_, "/user/hand/left/input/x/click"},
        {buttonY_, "/user/hand/left/input/y/click"},
        {buttonMenu_, "/user/hand/left/input/menu/click"},
        {leftStickClick_, "/user/hand/left/input/thumbstick/click"},
        {rightStickClick_, "/user/hand/right/input/thumbstick/click"},
        {leftGripPose_, "/user/hand/left/input/grip/pose"},
        {rightGripPose_, "/user/hand/right/input/grip/pose"},
        {leftHaptic_, "/user/hand/left/output/haptic"},
        {rightHaptic_, "/user/hand/right/output/haptic"},
    };

    std::vector<XrActionSuggestedBinding> bindings;
    bindings.reserve(std::size(defs));
    for (const auto& def : defs) {
        bindings.push_back({def.action, Path(def.path)});
    }

    // Prefer the OpenXR 1.1 Meta Touch Plus profile used by Quest 3/3S, then register the
    // older Quest Touch profiles as fallbacks. xrSuggestInteractionProfileBindings is allowed
    // once per profile, so a single action set can remain device-agnostic across Quest models.
    const char* interactionProfiles[] = {
        "/interaction_profiles/meta/touch_plus_controller",
        "/interaction_profiles/meta/touch_controller_quest_2",
        "/interaction_profiles/meta/touch_controller_quest_1_rift_s",
        "/interaction_profiles/oculus/touch_controller",
    };
    bool acceptedAnyProfile = false;
    for (const char* profilePath : interactionProfiles) {
        XrInteractionProfileSuggestedBinding profile{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        profile.interactionProfile = Path(profilePath);
        profile.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        profile.suggestedBindings = bindings.data();
        const XrResult suggestResult = xrSuggestInteractionProfileBindings(instance_, &profile);
        if (XR_SUCCEEDED(suggestResult)) {
            acceptedAnyProfile = true;
            __android_log_print(ANDROID_LOG_INFO, kTag, "registered interaction profile: %s", profilePath);
        } else {
            __android_log_print(ANDROID_LOG_DEBUG, kTag, "interaction profile unavailable: %s (%d)",
                                profilePath, suggestResult);
        }
    }
    if (!acceptedAnyProfile) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "No Quest Touch interaction profile accepted");
        return false;
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &actionSet_;
    if (XR_FAILED(xrAttachSessionActionSets(session_, &attach))) {
        return false;
    }

    const XrPosef identity{{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
    XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    spaceInfo.poseInActionSpace = identity;
    spaceInfo.subactionPath = XR_NULL_PATH;
    spaceInfo.action = leftGripPose_;
    if (XR_FAILED(xrCreateActionSpace(session_, &spaceInfo, &leftGripSpace_))) {
        return false;
    }
    spaceInfo.action = rightGripPose_;
    if (XR_FAILED(xrCreateActionSpace(session_, &spaceInfo, &rightGripSpace_))) {
        return false;
    }

    return true;
}

bool QuestInput::ReadBool(XrAction action) const {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = action;
    XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(session_, &get, &value))) {
        return false;
    }
    return value.isActive == XR_TRUE && value.currentState == XR_TRUE;
}

float QuestInput::ReadFloat(XrAction action) const {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = action;
    XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_FAILED(xrGetActionStateFloat(session_, &get, &value)) || value.isActive != XR_TRUE) {
        return 0.f;
    }
    return value.currentState;
}

XrVector2f QuestInput::ReadVec2(XrAction action) const {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = action;
    XrActionStateVector2f value{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_FAILED(xrGetActionStateVector2f(session_, &get, &value)) || value.isActive != XR_TRUE) {
        return {0.f, 0.f};
    }
    return value.currentState;
}

bool QuestInput::Sync(XrSession session, XrSpace baseSpace, XrTime time) {
    XrActiveActionSet active{actionSet_, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(session, &sync))) {
        return false;
    }

    const XrVector2f left = ReadVec2(leftStick_);
    const XrVector2f right = ReadVec2(rightStick_);
    state_.stickX = std::clamp(left.x, -1.f, 1.f);
    state_.stickY = std::clamp(left.y, -1.f, 1.f);
    state_.rightStickX = std::clamp(right.x, -1.f, 1.f);
    state_.rightStickY = std::clamp(right.y, -1.f, 1.f);
    state_.leftTrigger = ReadFloat(leftTrigger_);
    state_.rightTrigger = ReadFloat(rightTrigger_);
    state_.leftSqueeze = ReadFloat(leftSqueeze_);
    state_.rightSqueeze = ReadFloat(rightSqueeze_);

    uint32_t buttons = 0;
    // F-Zero X racing mapping, matched to Racer_UpdateFromControls():
    // N64 A accelerates, N64 B triggers boost, C-Down brakes, and Z/R are the
    // left/right side-attack / drift inputs. Keep Quest face-button duplicates
    // where useful so menus still feel natural.
    if (state_.rightTrigger > 0.15f || ReadBool(buttonA_)) buttons |= N64_A;
    if (ReadBool(buttonB_)) buttons |= N64_B;
    if (state_.leftTrigger > 0.15f) buttons |= N64_C_DOWN;
    if (state_.leftSqueeze > 0.45f) buttons |= N64_Z;
    if (state_.rightSqueeze > 0.45f) buttons |= N64_R;
    if (ReadBool(buttonX_)) buttons |= N64_L;
    if (ReadBool(buttonY_)) buttons |= N64_C_UP;
    if (ReadBool(buttonMenu_) || ReadBool(leftStickClick_)) buttons |= N64_START;

    constexpr float kCThreshold = 0.55f;
    if (state_.rightStickY > kCThreshold) buttons |= N64_C_UP;
    if (state_.rightStickY < -kCThreshold) buttons |= N64_C_DOWN;
    if (state_.rightStickX < -kCThreshold) buttons |= N64_C_LEFT;
    if (state_.rightStickX > kCThreshold) buttons |= N64_C_RIGHT;
    if (ReadBool(rightStickClick_)) buttons |= N64_DPAD_UP;
    state_.n64Buttons = buttons;

    const XrSpace spaces[2] = {leftGripSpace_, rightGripSpace_};
    for (int hand = 0; hand < 2; ++hand) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(spaces[hand], baseSpace, time, &location))) {
            const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            state_.hands[hand].active = (location.locationFlags & needed) == needed;
            if (state_.hands[hand].active) {
                state_.hands[hand].gripPose = location.pose;
            }
        } else {
            state_.hands[hand].active = false;
        }
    }

    // F-Zero X drives the N64 Rumble Pak as a binary motor. Re-issue a short OpenXR vibration
    // while that motor is active so the effect remains continuous, and stop explicitly on the
    // falling edge. Using both hands feels closest to a single controller-mounted Rumble Pak.
    const bool rumbleActive = GdxQuestRumbleActive();
    if (rumbleActive) {
        Pulse(session, 0, 0.55f, 0.12f);
        Pulse(session, 1, 0.55f, 0.12f);
    } else if (rumbleWasActive_) {
        const XrAction actions[2] = {leftHaptic_, rightHaptic_};
        for (XrAction action : actions) {
            if (action == XR_NULL_HANDLE) continue;
            XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
            info.action = action;
            xrStopHapticFeedback(session, &info);
        }
    }
    rumbleWasActive_ = rumbleActive;

    return true;
}

void QuestInput::Pulse(XrSession session, int hand, float amplitude, float seconds, float frequency) {
    if (hand < 0 || hand > 1) return;
    XrAction action = hand == 0 ? leftHaptic_ : rightHaptic_;
    if (action == XR_NULL_HANDLE) return;

    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = action;
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = std::clamp(amplitude, 0.f, 1.f);
    vibration.duration = static_cast<XrDuration>(std::max(seconds, 0.f) * 1'000'000'000.0);
    vibration.frequency = frequency;
    xrApplyHapticFeedback(session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

void QuestInput::Shutdown() {
    if (leftGripSpace_ != XR_NULL_HANDLE) xrDestroySpace(leftGripSpace_);
    if (rightGripSpace_ != XR_NULL_HANDLE) xrDestroySpace(rightGripSpace_);
    leftGripSpace_ = rightGripSpace_ = XR_NULL_HANDLE;
    if (actionSet_ != XR_NULL_HANDLE) xrDestroyActionSet(actionSet_);
    actionSet_ = XR_NULL_HANDLE;
    rumbleWasActive_ = false;
    GdxQuestSetRumble(false);
    instance_ = XR_NULL_HANDLE;
    session_ = XR_NULL_HANDLE;
}
