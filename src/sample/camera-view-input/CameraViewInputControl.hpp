#pragma once

#include "cSC4BaseViewInputControl.h"
#include "public/cIGZS3DCameraService.h"

#include <cstdint>
#include <functional>

class CameraViewInputControl final : public cSC4BaseViewInputControl
{
public:
    static constexpr uint32_t kCameraViewInputControlID = 0x93A1D6C2;

    CameraViewInputControl();
    ~CameraViewInputControl() override;

    bool Init() override;
    bool Shutdown() override;

    void Activate() override;
    void Deactivate() override;
    bool IsSelfScrollingView() override;
    bool ShouldStack() override;

    bool OnMouseDownL(int32_t x, int32_t z, uint32_t modifiers) override;
    bool OnMouseUpL(int32_t x, int32_t z, uint32_t modifiers) override;
    bool OnMouseDownR(int32_t x, int32_t z, uint32_t modifiers) override;
    bool OnMouseUpR(int32_t x, int32_t z, uint32_t modifiers) override;
    bool OnMouseMove(int32_t x, int32_t z, uint32_t modifiers) override;
    bool OnMouseWheel(int32_t x, int32_t z, uint32_t modifiers, int32_t wheelDelta) override;
    bool OnMouseExit() override;
    bool OnKeyDown(int32_t vkCode, uint32_t modifiers) override;

    void SetCameraService(cIGZS3DCameraService* cameraService);
    void SetOnCancel(std::function<void()> onCancel);

    void SetRotateSensitivity(float radiansPerPixel);
    void SetPitchSensitivity(float radiansPerPixel);
    void SetWheelZoomStep(float unitsPerWheelStep);
    void SetKeyboardPanStep(float unitsPerPress);
    void SetInvertPitch(bool invertPitch);

private:
    bool IsUsable_() const;
    bool PickTerrainAt_(int32_t screenX, int32_t screenZ, float& x, float& y, float& z) const;

    bool PanFromMouse_(int32_t x, int32_t z);
    bool RotateFromMouse_(int32_t x, int32_t z);
    bool ZoomByWheel_(int32_t wheelDelta);
    bool AdjustPitchByWheel_(int32_t wheelDelta);

    bool NudgePan_(float forward, float right, float step);
    bool NudgeYaw_(float deltaRadians);
    bool NudgePitch_(float deltaRadians);

    bool ApplyCameraPositionDelta_(float deltaX, float deltaY, float deltaZ);
    bool MoveAlongLookVector_(float distance);

    bool GetCameraAngles_(float& yawOut, float& pitchOut) const;
    bool ApplyCameraAngles_(float yaw, float pitch, bool updatePitchTables) const;

private:
    cIGZS3DCameraService* cameraService_;
    std::function<void()> onCancel_;

    bool active_;
    bool leftDragging_;
    bool rightDragging_;
    int32_t lastMouseX_;
    int32_t lastMouseZ_;

    float rotateSensitivity_;
    float pitchSensitivity_;
    float wheelZoomStep_;
    float keyboardPanStep_;
    bool invertPitch_;
};
