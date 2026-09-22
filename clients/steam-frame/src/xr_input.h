// Frame VR client — OpenXR input (V4): controllers + hand tracking
// SPDX-License-Identifier: BSL-1.0
//
// Sets up an action set (poses, trigger, squeeze, thumbstick, A/B) bound for
// the KHR simple controller (universal) and Oculus Touch (the Frame's
// controllers emulate Touch), plus XR_EXT_hand_tracking joints. Each frame,
// fills the oxrsys Trackingpacket's controller + input + hand fields so the
// server (and the game) receive the player's hands.

#pragma once
#include <openxr/openxr.h>

namespace oxr { namespace protocol { struct TrackingPacket; } }

class XrInput {
public:
    bool Init(XrInstance instance, XrSession session, XrSpace baseSpace,
              bool handTracking, bool eyeGaze);
    // Sync + locate everything for displayTime; fills controller/input/hand
    // fields (and trackingFlags) of `pkt`. Head fields are left to the caller.
    void Update(XrTime displayTime, oxr::protocol::TrackingPacket& pkt);

    // Latest eye-gaze direction in the base space (unit vector). Returns false
    // until a valid gaze sample is available. This is the client-side half of
    // gaze foveation; oxrsys does not consume it yet (fixed-center FFE), but a
    // gaze-aware server/runtime would center the fovea here.
    bool GazeDirection(float outDir[3]) const;

    void Destroy();

private:
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace baseSpace_ = XR_NULL_HANDLE;

    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction poseAction_ = XR_NULL_HANDLE;     // grip pose, L+R
    XrAction triggerAction_ = XR_NULL_HANDLE;  // float, L+R
    XrAction squeezeAction_ = XR_NULL_HANDLE;  // float, L+R
    XrAction thumbAction_ = XR_NULL_HANDLE;    // vector2f, L+R
    XrAction primaryAction_ = XR_NULL_HANDLE;  // bool A/X, L+R
    XrAction secondaryAction_ = XR_NULL_HANDLE;// bool B/Y, L+R

    XrPath handPath_[2] = {XR_NULL_PATH, XR_NULL_PATH}; // left, right
    XrSpace poseSpace_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

    bool handTracking_ = false;
    XrHandTrackerEXT handTracker_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    PFN_xrLocateHandJointsEXT pfnLocateHandJoints_ = nullptr;
    PFN_xrDestroyHandTrackerEXT pfnDestroyHandTracker_ = nullptr;

    bool eyeGaze_ = false;
    XrAction gazeAction_ = XR_NULL_HANDLE;
    XrSpace gazeSpace_ = XR_NULL_HANDLE;
    mutable float gazeDir_[3] = {0, 0, -1};
    mutable bool gazeValid_ = false;
};
