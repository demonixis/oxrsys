// SPDX-License-Identifier: BSL-1.0
#include "xr_input.h"

#include <oxrsys/protocol/Protocol.h>

#include <cstdio>
#include <cstring>
#include <vector>

#define LOG_INF(fmt, ...) fprintf(stderr, "[INFO] XrInput: " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) fprintf(stderr, "[ERROR] XrInput: " fmt "\n", ##__VA_ARGS__)

using namespace oxr;

static XrPath path(XrInstance inst, const char* s)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath(inst, s, &p);
    return p;
}

template <typename T>
static bool proc(XrInstance inst, const char* name, T* out)
{
    return XR_SUCCEEDED(xrGetInstanceProcAddr(inst, name, (PFN_xrVoidFunction*)out));
}

bool XrInput::Init(XrInstance instance, XrSession session, XrSpace baseSpace,
                   bool handTracking, bool eyeGaze)
{
    instance_ = instance;
    session_ = session;
    baseSpace_ = baseSpace;
    handPath_[0] = path(instance, "/user/hand/left");
    handPath_[1] = path(instance, "/user/hand/right");

    XrActionSetCreateInfo asci = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(asci.actionSetName, "gameplay");
    strcpy(asci.localizedActionSetName, "Gameplay");
    if (XR_FAILED(xrCreateActionSet(instance, &asci, &actionSet_))) { LOG_ERR("action set"); return false; }

    auto mkAction = [&](const char* name, XrActionType type, XrAction* out) {
        XrActionCreateInfo ci = {XR_TYPE_ACTION_CREATE_INFO};
        strncpy(ci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
        strncpy(ci.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        ci.actionType = type;
        ci.countSubactionPaths = 2;
        ci.subactionPaths = handPath_;
        return XR_SUCCEEDED(xrCreateAction(actionSet_, &ci, out));
    };
    if (!mkAction("grip_pose", XR_ACTION_TYPE_POSE_INPUT, &poseAction_) ||
        !mkAction("trigger", XR_ACTION_TYPE_FLOAT_INPUT, &triggerAction_) ||
        !mkAction("squeeze", XR_ACTION_TYPE_FLOAT_INPUT, &squeezeAction_) ||
        !mkAction("thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, &thumbAction_) ||
        !mkAction("primary", XR_ACTION_TYPE_BOOLEAN_INPUT, &primaryAction_) ||
        !mkAction("secondary", XR_ACTION_TYPE_BOOLEAN_INPUT, &secondaryAction_)) {
        LOG_ERR("action create"); return false;
    }

    auto suggest = [&](const char* profile, std::vector<XrActionSuggestedBinding> b) {
        XrInteractionProfileSuggestedBinding s = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        s.interactionProfile = path(instance, profile);
        s.countSuggestedBindings = (uint32_t)b.size();
        s.suggestedBindings = b.data();
        XrResult r = xrSuggestInteractionProfileBindings(instance, &s);
        if (XR_FAILED(r)) LOG_INF("bindings for %s not accepted (%d) — ok if unsupported", profile, (int)r);
    };
    auto B = [&](XrAction a, const char* p) {
        return XrActionSuggestedBinding{a, path(instance, p)};
    };

    // KHR simple controller — universal; poses + select/menu only.
    suggest("/interaction_profiles/khr/simple_controller", {
        B(poseAction_, "/user/hand/left/input/grip/pose"),
        B(poseAction_, "/user/hand/right/input/grip/pose"),
        B(primaryAction_, "/user/hand/left/input/select/click"),
        B(primaryAction_, "/user/hand/right/input/select/click"),
        B(secondaryAction_, "/user/hand/left/input/menu/click"),
        B(secondaryAction_, "/user/hand/right/input/menu/click"),
    });

    // Eye gaze (XR_EXT_eye_gaze_interaction): a pose action on /user/eyes_ext.
    if (eyeGaze) {
        XrActionCreateInfo ci = {XR_TYPE_ACTION_CREATE_INFO};
        strcpy(ci.actionName, "gaze");
        strcpy(ci.localizedActionName, "Gaze");
        ci.actionType = XR_ACTION_TYPE_POSE_INPUT;
        if (XR_SUCCEEDED(xrCreateAction(actionSet_, &ci, &gazeAction_))) {
            suggest("/interaction_profiles/ext/eye_gaze_interaction", {
                B(gazeAction_, "/user/eyes_ext/input/gaze_ext/pose"),
            });
            eyeGaze_ = true;
        }
    }

    // Oculus Touch — the Frame's controllers emulate this. Full input surface.
    suggest("/interaction_profiles/oculus/touch_controller", {
        B(poseAction_, "/user/hand/left/input/grip/pose"),
        B(poseAction_, "/user/hand/right/input/grip/pose"),
        B(triggerAction_, "/user/hand/left/input/trigger/value"),
        B(triggerAction_, "/user/hand/right/input/trigger/value"),
        B(squeezeAction_, "/user/hand/left/input/squeeze/value"),
        B(squeezeAction_, "/user/hand/right/input/squeeze/value"),
        B(thumbAction_, "/user/hand/left/input/thumbstick"),
        B(thumbAction_, "/user/hand/right/input/thumbstick"),
        B(primaryAction_, "/user/hand/left/input/x/click"),
        B(primaryAction_, "/user/hand/right/input/a/click"),
        B(secondaryAction_, "/user/hand/left/input/y/click"),
        B(secondaryAction_, "/user/hand/right/input/b/click"),
    });

    XrSessionActionSetsAttachInfo ai = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &actionSet_;
    if (XR_FAILED(xrAttachSessionActionSets(session, &ai))) { LOG_ERR("attach"); return false; }

    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo sci = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sci.action = poseAction_;
        sci.subactionPath = handPath_[h];
        sci.poseInActionSpace.orientation.w = 1.0f;
        xrCreateActionSpace(session, &sci, &poseSpace_[h]);
    }
    if (eyeGaze_ && gazeAction_) {
        XrActionSpaceCreateInfo sci = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sci.action = gazeAction_;
        sci.poseInActionSpace.orientation.w = 1.0f;
        xrCreateActionSpace(session, &sci, &gazeSpace_);
    }

    handTracking_ = handTracking;
    if (handTracking_) {
        PFN_xrCreateHandTrackerEXT create = nullptr;
        if (proc(instance, "xrCreateHandTrackerEXT", &create) &&
            proc(instance, "xrLocateHandJointsEXT", &pfnLocateHandJoints_) &&
            proc(instance, "xrDestroyHandTrackerEXT", &pfnDestroyHandTracker_)) {
            for (int h = 0; h < 2; h++) {
                XrHandTrackerCreateInfoEXT ci = {XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
                ci.hand = (h == 0) ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
                ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
                if (XR_FAILED(create(session, &ci, &handTracker_[h]))) {
                    handTracker_[h] = XR_NULL_HANDLE;
                }
            }
        }
        // Downgrade if the system rejected both trackers (ext advertised but
        // unsupported by the device — e.g. DisplayXR's sim display).
        if (!handTracker_[0] && !handTracker_[1]) handTracking_ = false;
    }

    LOG_INF("input ready (hand tracking %s, eye gaze %s)",
            handTracking_ ? "on" : "off", eyeGaze_ ? "on" : "off");
    return true;
}

void XrInput::Update(XrTime displayTime, protocol::TrackingPacket& pkt)
{
    if (!actionSet_) return;

    XrActiveActionSet active = {actionSet_, XR_NULL_PATH};
    XrActionsSyncInfo si = {XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(session_, &si))) return;

    float* ctrlPos[2] = {pkt.leftControllerPos, pkt.rightControllerPos};
    float* ctrlRot[2] = {pkt.leftControllerRot, pkt.rightControllerRot};
    float* trig[2] = {&pkt.leftTrigger, &pkt.rightTrigger};
    float* grip[2] = {&pkt.leftGrip, &pkt.rightGrip};
    float* thumb[2] = {pkt.leftThumbstick, pkt.rightThumbstick};
    const uint32_t primaryBit[2] = {protocol::BUTTON_X, protocol::BUTTON_A};
    const uint32_t secondaryBit[2] = {protocol::BUTTON_Y, protocol::BUTTON_B};
    const uint32_t trigBit[2] = {protocol::BUTTON_LEFT_TRIGGER, protocol::BUTTON_RIGHT_TRIGGER};
    const uint32_t ctrlFlag[2] = {protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE,
                                  protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE};

    for (int h = 0; h < 2; h++) {
        XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
        gi.subactionPath = handPath_[h];

        // Pose
        gi.action = poseAction_;
        XrActionStatePose ps = {XR_TYPE_ACTION_STATE_POSE};
        if (XR_SUCCEEDED(xrGetActionStatePose(session_, &gi, &ps)) && ps.isActive && poseSpace_[h]) {
            XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(poseSpace_[h], baseSpace_, displayTime, &loc)) &&
                (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                ctrlPos[h][0] = loc.pose.position.x;
                ctrlPos[h][1] = loc.pose.position.y;
                ctrlPos[h][2] = loc.pose.position.z;
                ctrlRot[h][0] = loc.pose.orientation.x;
                ctrlRot[h][1] = loc.pose.orientation.y;
                ctrlRot[h][2] = loc.pose.orientation.z;
                ctrlRot[h][3] = loc.pose.orientation.w;
                pkt.trackingFlags |= ctrlFlag[h];
            }
        }

        auto getFloat = [&](XrAction a, float* out) {
            gi.action = a;
            XrActionStateFloat s = {XR_TYPE_ACTION_STATE_FLOAT};
            if (XR_SUCCEEDED(xrGetActionStateFloat(session_, &gi, &s)) && s.isActive) *out = s.currentState;
        };
        getFloat(triggerAction_, trig[h]);
        getFloat(squeezeAction_, grip[h]);

        gi.action = thumbAction_;
        XrActionStateVector2f v2 = {XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &gi, &v2)) && v2.isActive) {
            thumb[h][0] = v2.currentState.x;
            thumb[h][1] = v2.currentState.y;
        }

        auto getBool = [&](XrAction a) {
            gi.action = a;
            XrActionStateBoolean s = {XR_TYPE_ACTION_STATE_BOOLEAN};
            return XR_SUCCEEDED(xrGetActionStateBoolean(session_, &gi, &s)) && s.isActive && s.currentState;
        };
        if (getBool(primaryAction_)) pkt.buttonState |= primaryBit[h];
        if (getBool(secondaryAction_)) pkt.buttonState |= secondaryBit[h];
        if (*trig[h] > 0.6f) pkt.buttonState |= trigBit[h];
    }

    // Eye gaze: locate the gaze pose; forward direction = -Z rotated by the pose.
    if (eyeGaze_ && gazeSpace_) {
        XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = gazeAction_;
        XrActionStatePose ps = {XR_TYPE_ACTION_STATE_POSE};
        if (XR_SUCCEEDED(xrGetActionStatePose(session_, &gi, &ps)) && ps.isActive) {
            XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(gazeSpace_, baseSpace_, displayTime, &loc)) &&
                (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                const XrQuaternionf& q = loc.pose.orientation;
                // Rotate (0,0,-1) by q.
                gazeDir_[0] = -2.0f * (q.x * q.z + q.w * q.y);
                gazeDir_[1] = -2.0f * (q.y * q.z - q.w * q.x);
                gazeDir_[2] = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
                gazeValid_ = true;

                // Ship gaze upstream so the server can centre its foveated encode on it.
                // gazeDir_ is in base space; the server wants it relative to the view, so
                // rotate by the conjugate of the head orientation set just before this call.
                const float hx = -pkt.headOrientation[0];
                const float hy = -pkt.headOrientation[1];
                const float hz = -pkt.headOrientation[2];
                const float hw = pkt.headOrientation[3];
                const float tx = 2.0f * (hy * gazeDir_[2] - hz * gazeDir_[1]);
                const float ty = 2.0f * (hz * gazeDir_[0] - hx * gazeDir_[2]);
                const float tz = 2.0f * (hx * gazeDir_[1] - hy * gazeDir_[0]);
                pkt.gazeDirection[0] = gazeDir_[0] + hw * tx + (hy * tz - hz * ty);
                pkt.gazeDirection[1] = gazeDir_[1] + hw * ty + (hz * tx - hx * tz);
                pkt.gazeDirection[2] = gazeDir_[2] + hw * tz + (hx * ty - hy * tx);
                pkt.trackingFlags |= protocol::TRACKING_FLAG_EYE_GAZE_ACTIVE;
            }
        }
    }

    // Hand joints (26 per hand): x,y,z,radius, located in the base space.
    if (handTracking_ && pfnLocateHandJoints_) {
        float (*joints[2])[4] = {pkt.leftHandJoints, pkt.rightHandJoints};
        const uint32_t handFlag[2] = {protocol::TRACKING_FLAG_LEFT_HAND_ACTIVE,
                                      protocol::TRACKING_FLAG_RIGHT_HAND_ACTIVE};
        for (int h = 0; h < 2; h++) {
            if (!handTracker_[h]) continue;
            XrHandJointLocationEXT locs[XR_HAND_JOINT_COUNT_EXT];
            XrHandJointLocationsEXT jl = {XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
            jl.jointCount = XR_HAND_JOINT_COUNT_EXT;
            jl.jointLocations = locs;
            XrHandJointsLocateInfoEXT li = {XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
            li.baseSpace = baseSpace_;
            li.time = displayTime;
            if (XR_SUCCEEDED(pfnLocateHandJoints_(handTracker_[h], &li, &jl)) && jl.isActive) {
                uint32_t n = XR_HAND_JOINT_COUNT_EXT;
                if (n > protocol::HAND_JOINT_COUNT) n = protocol::HAND_JOINT_COUNT;
                for (uint32_t j = 0; j < n; j++) {
                    joints[h][j][0] = locs[j].pose.position.x;
                    joints[h][j][1] = locs[j].pose.position.y;
                    joints[h][j][2] = locs[j].pose.position.z;
                    joints[h][j][3] = locs[j].radius;
                }
                pkt.trackingFlags |= handFlag[h];
            }
        }
    }
}

bool XrInput::GazeDirection(float outDir[3]) const
{
    if (!gazeValid_) return false;
    outDir[0] = gazeDir_[0]; outDir[1] = gazeDir_[1]; outDir[2] = gazeDir_[2];
    return true;
}

void XrInput::Destroy()
{
    for (int h = 0; h < 2; h++) {
        if (handTracker_[h] && pfnDestroyHandTracker_) pfnDestroyHandTracker_(handTracker_[h]);
        if (poseSpace_[h]) xrDestroySpace(poseSpace_[h]);
    }
    if (gazeSpace_) xrDestroySpace(gazeSpace_);
    if (actionSet_) xrDestroyActionSet(actionSet_);
}
