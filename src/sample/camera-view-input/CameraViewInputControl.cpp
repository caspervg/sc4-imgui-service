#include "sample/camera-view-input/CameraViewInputControl.hpp"
#include "sample/camera-view-input/SC4CameraControlLayout.hpp"

#include "cISC43DRender.h"
#include "SC4UI.h"
#include "utils/Logger.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <utility>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace
{
    constexpr uint32_t kModifierAlt = 0x40000;
    constexpr float kMagnificationWheelScalePerNotch = 1.04f;

    constexpr float kPi = SC4CameraControl::kPi;
    constexpr float kPitchMinRadians = SC4CameraControl::kPitchMinRadians;
    constexpr float kPitchMaxRadians = SC4CameraControl::kPitchMaxRadians;

    float Clip(const float value, const float minValue, const float maxValue)
    {
        return std::max(minValue, std::min(value, maxValue));
    }

    bool IsAltActive(const uint32_t modifiers)
    {
        if ((modifiers & kModifierAlt) != 0) {
            return true;
        }
        return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    }

}

CameraViewInputControl::CameraViewInputControl()
    : cSC4BaseViewInputControl(kCameraViewInputControlID)
    , cameraService_(nullptr)
    , onCancel_()
    , active_(false)
    , leftDragging_(false)
    , rightDragging_(false)
    , lastMouseX_(0)
    , lastMouseZ_(0)
    , rotateSensitivity_(0.0045f)
    , pitchSensitivity_(0.0035f)
    , wheelZoomStep_(24.0f)
    , keyboardPanStep_(10.0f)
    , invertPitch_(false)
{
}

CameraViewInputControl::~CameraViewInputControl()
{
    LOG_INFO("CameraViewInputControl destroyed");
}

bool CameraViewInputControl::Init()
{
    if (!cSC4BaseViewInputControl::Init()) {
        return false;
    }
    LOG_INFO("CameraViewInputControl initialized");
    return true;
}

bool CameraViewInputControl::Shutdown()
{
    leftDragging_ = false;
    rightDragging_ = false;
    active_ = false;
    return cSC4BaseViewInputControl::Shutdown();
}

void CameraViewInputControl::Activate()
{
    cSC4BaseViewInputControl::Activate();
    if (!Init()) {
        LOG_WARN("CameraViewInputControl: Init failed during Activate");
        return;
    }
    active_ = true;
}

void CameraViewInputControl::Deactivate()
{
    leftDragging_ = false;
    rightDragging_ = false;
    active_ = false;
    ReleaseCapture();
    cSC4BaseViewInputControl::Deactivate();
}

bool CameraViewInputControl::IsSelfScrollingView()
{
    return true;
}

bool CameraViewInputControl::ShouldStack()
{
    return true;
}

bool CameraViewInputControl::OnMouseDownL(const int32_t x, const int32_t z, const uint32_t)
{
    (void)x;
    (void)z;
    return false;
}

bool CameraViewInputControl::OnMouseUpL(const int32_t, const int32_t, const uint32_t)
{
    return false;
}

bool CameraViewInputControl::OnMouseDownR(const int32_t x, const int32_t z, const uint32_t modifiers)
{
    if (!IsUsable_() || !IsOnTop() || !IsAltActive(modifiers)) {
        return false;
    }

    view3D->ScrollStop();
    rightDragging_ = true;
    lastMouseX_ = x;
    lastMouseZ_ = z;
    SetCapture();
    return true;
}

bool CameraViewInputControl::OnMouseUpR(const int32_t, const int32_t, const uint32_t)
{
    if (!active_ || !rightDragging_) {
        return false;
    }

    rightDragging_ = false;
    ReleaseCapture();
    return true;
}

bool CameraViewInputControl::OnMouseMove(const int32_t x, const int32_t z, const uint32_t)
{
    if (!IsUsable_() || !IsOnTop()) {
        return false;
    }

    bool handled = rightDragging_ && RotateFromMouse_(x, z);

    if (rightDragging_) {
        lastMouseX_ = x;
        lastMouseZ_ = z;
    }

    return handled;
}

bool CameraViewInputControl::OnMouseWheel(const int32_t, const int32_t, const uint32_t modifiers, const int32_t wheelDelta)
{
    if (!IsUsable_() || !IsOnTop() || wheelDelta == 0) {
        return false;
    }

    if (IsAltActive(modifiers)) {
        auto* cameraControl = SC4CameraControl::GetActiveCameraControl();
        if (!cameraControl) {
            return false;
        }

        const float notches = static_cast<float>(wheelDelta) / 120.0f;
        const float scale = std::pow(kMagnificationWheelScalePerNotch, notches);
        return SC4CameraControl::SetCustomMagnification(cameraControl->customMagnification * scale);
    }

    return false;
}

bool CameraViewInputControl::OnMouseExit()
{
    leftDragging_ = false;
    rightDragging_ = false;
    ReleaseCapture();
    return false;
}

bool CameraViewInputControl::OnKeyDown(const int32_t vkCode, const uint32_t modifiers)
{
    (void)vkCode;
    (void)modifiers;
    return false;
}

void CameraViewInputControl::SetCameraService(cIGZS3DCameraService* cameraService)
{
    cameraService_ = cameraService;
}

void CameraViewInputControl::SetOnCancel(std::function<void()> onCancel)
{
    onCancel_ = std::move(onCancel);
}

void CameraViewInputControl::SetRotateSensitivity(const float radiansPerPixel)
{
    rotateSensitivity_ = std::max(0.0005f, radiansPerPixel);
}

void CameraViewInputControl::SetPitchSensitivity(const float radiansPerPixel)
{
    pitchSensitivity_ = std::max(0.0005f, radiansPerPixel);
}

void CameraViewInputControl::SetWheelZoomStep(const float unitsPerWheelStep)
{
    wheelZoomStep_ = std::max(1.0f, unitsPerWheelStep);
}

void CameraViewInputControl::SetKeyboardPanStep(const float unitsPerPress)
{
    keyboardPanStep_ = std::max(0.5f, unitsPerPress);
}

void CameraViewInputControl::SetInvertPitch(const bool invertPitch)
{
    invertPitch_ = invertPitch;
}

bool CameraViewInputControl::IsUsable_() const
{
    return active_ && view3D && cameraService_;
}

bool CameraViewInputControl::PickTerrainAt_(const int32_t screenX, const int32_t screenZ, float& x, float& y, float& z) const
{
    float world[3] = {0.0f, 0.0f, 0.0f};
    if (!view3D->PickTerrain(screenX, screenZ, world, false)) {
        return false;
    }

    x = world[0];
    y = world[1];
    z = world[2];
    return true;
}

bool CameraViewInputControl::PanFromMouse_(const int32_t x, const int32_t z)
{
    float oldX = 0.0f;
    float oldZ = 0.0f;

    float newX = 0.0f;
    float newZ = 0.0f;

    float ignoredY = 0.0f;
    if (!PickTerrainAt_(lastMouseX_, lastMouseZ_, oldX, ignoredY, oldZ) ||
        !PickTerrainAt_(x, z, newX, ignoredY, newZ)) {
        return false;
    }

    return ApplyCameraPositionDelta_(oldX - newX, 0.0f, oldZ - newZ);
}

bool CameraViewInputControl::RotateFromMouse_(const int32_t x, const int32_t z)
{
    float yaw = 0.0f;
    float pitch = 0.0f;
    if (!GetCameraAngles_(yaw, pitch)) {
        return false;
    }

    const int32_t dx = x - lastMouseX_;
    const int32_t dz = z - lastMouseZ_;

    yaw -= static_cast<float>(dx) * rotateSensitivity_;

    const float pitchScale = invertPitch_ ? 1.0f : -1.0f;
    pitch += static_cast<float>(dz) * pitchSensitivity_ * pitchScale;
    pitch = Clip(pitch, kPitchMinRadians, kPitchMaxRadians);

    return ApplyCameraAngles_(yaw, pitch, false);
}

bool CameraViewInputControl::ZoomByWheel_(const int32_t wheelDelta)
{
    const float notches = static_cast<float>(wheelDelta) / 120.0f;
    return MoveAlongLookVector_(notches * wheelZoomStep_);
}

bool CameraViewInputControl::AdjustPitchByWheel_(const int32_t wheelDelta)
{
    const float delta = (static_cast<float>(wheelDelta) / 120.0f) * (3.0f * (kPi / 180.0f));
    return NudgePitch_(delta);
}

bool CameraViewInputControl::NudgePan_(const float forward, const float right, const float step)
{
    float yaw = 0.0f;
    float pitch = 0.0f;
    if (!GetCameraAngles_(yaw, pitch)) {
        return false;
    }
    (void)pitch;

    const float dirX = std::cos(yaw);
    const float dirZ = std::sin(yaw);

    const float rightX = dirZ;
    const float rightZ = -dirX;

    const float moveX = (dirX * forward + rightX * right) * step;
    const float moveZ = (dirZ * forward + rightZ * right) * step;

    return ApplyCameraPositionDelta_(moveX, 0.0f, moveZ);
}

bool CameraViewInputControl::NudgeYaw_(const float deltaRadians)
{
    float yaw = 0.0f;
    float pitch = 0.0f;
    if (!GetCameraAngles_(yaw, pitch)) {
        return false;
    }

    return ApplyCameraAngles_(yaw + deltaRadians, pitch, true);
}

bool CameraViewInputControl::NudgePitch_(const float deltaRadians)
{
    float yaw = 0.0f;
    float pitch = 0.0f;
    if (!GetCameraAngles_(yaw, pitch)) {
        return false;
    }

    pitch = Clip(pitch + deltaRadians, kPitchMinRadians, kPitchMaxRadians);
    return ApplyCameraAngles_(yaw, pitch, true);
}

bool CameraViewInputControl::ApplyCameraPositionDelta_(const float deltaX, const float deltaY, const float deltaZ)
{
    auto* cameraControl = SC4CameraControl::GetActiveCameraControl();
    if (!cameraControl) {
        return false;
    }

    const cS3DVector3 target{
        cameraControl->viewTargetPosition.fX + deltaX,
        cameraControl->viewTargetPosition.fY + deltaY,
        cameraControl->viewTargetPosition.fZ + deltaZ
    };
    return SC4CameraControl::SetViewTargetPosition(target);
}

bool CameraViewInputControl::MoveAlongLookVector_(const float distance)
{
    auto* cameraControl = SC4CameraControl::GetActiveCameraControl();
    if (!cameraControl) {
        return false;
    }

    return SC4CameraControl::SetCameraDistance(cameraControl->cameraDistance - distance);
}

bool CameraViewInputControl::GetCameraAngles_(float& yawOut, float& pitchOut) const
{
    auto* cameraControl = SC4CameraControl::GetActiveCameraControl();
    if (!cameraControl) {
        return false;
    }

    yawOut = cameraControl->yaw;
    pitchOut = cameraControl->pitch;
    return true;
}

bool CameraViewInputControl::ApplyCameraAngles_(const float yaw, const float pitch, const bool updatePitchTables) const
{
    if (updatePitchTables) {
        return SC4CameraControl::SetYawPitch(yaw, pitch);
    }

    SC4CameraControl::QueueYawPitch(yaw, pitch);
    return true;
}
