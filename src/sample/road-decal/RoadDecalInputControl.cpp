#include "RoadDecalInputControl.hpp"

#include "cISC43DRender.h"
#include "utils/Logger.h"

#include <cmath>
#include <windows.h>

namespace
{
    constexpr uint32_t kControlModifierMask = 0x20000;
    constexpr uint32_t kShiftModifierMask = 0x10000;
    constexpr float kSnapSubgridMeters = 2.0f;
    constexpr float kDecalHeightOffset = 0.05f;

    float SnapToSubgrid(const float value)
    {
        return std::round(value / kSnapSubgridMeters) * kSnapSubgridMeters;
    }

    bool IsHardCornerModifierActive(const uint32_t modifiers)
    {
        if ((modifiers & kShiftModifierMask) != 0) {
            return true;
        }
        return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    }
}

RoadDecalInputControl::RoadDecalInputControl()
    : cSC4BaseViewInputControl(kRoadDecalControlID)
    , isActive_(false)
    , isDrawing_(false)
    , currentStroke_({})
    , lastSamplePoint_({})
    , styleId_(0)
    , width_(1.0f)
    , dashed_(false)
    , onCancel_()
{
}

RoadDecalInputControl::~RoadDecalInputControl()
{
    LOG_INFO("RoadDecalInputControl destroyed");
}

bool RoadDecalInputControl::Init()
{
    if (!cSC4BaseViewInputControl::Init()) {
        return false;
    }
    LOG_INFO("RoadDecalInputControl initialized");
    return true;
}

bool RoadDecalInputControl::Shutdown()
{
    LOG_INFO("RoadDecalInputControl shutting down");
    CancelStroke_();
    RequestFullRedraw_();
    return cSC4BaseViewInputControl::Shutdown();
}

void RoadDecalInputControl::Activate()
{
    cSC4BaseViewInputControl::Activate();
    if (!Init()) {
        LOG_WARN("RoadDecalInputControl: Init failed during Activate");
        return;
    }
    isActive_ = true;
    LOG_INFO("RoadDecalInputControl activated");
}

void RoadDecalInputControl::Deactivate()
{
    LOG_INFO("RoadDecalInputControl deactivated");
    isActive_ = false;
    CancelStroke_();
    cSC4BaseViewInputControl::Deactivate();
}

bool RoadDecalInputControl::OnMouseDownL(int32_t x, int32_t z, uint32_t modifiers)
{
    if (!isActive_ || !IsOnTop()) {
        return false;
    }

    if (!isDrawing_) {
        return BeginStroke_(x, z, modifiers);
    }

    return AddSamplePoint_(x, z, modifiers);
}

bool RoadDecalInputControl::OnMouseMove(int32_t x, int32_t z, uint32_t)
{
    if (!isActive_ || !isDrawing_) {
        return false;
    }

    UpdatePreviewFromScreen_(x, z);
    return true;
}

bool RoadDecalInputControl::OnMouseUpL(int32_t x, int32_t z, uint32_t)
{
    (void)x;
    (void)z;
    return false;
}

bool RoadDecalInputControl::OnMouseDownR(int32_t, int32_t, uint32_t)
{
    if (!isActive_) {
        return false;
    }

    if (isDrawing_) {
        EndStroke_(true);
    } else {
        ClearAllStrokes_();
        RebuildRoadDecalGeometry();
        RequestFullRedraw_();
    }

    return true;
}

bool RoadDecalInputControl::OnMouseExit()
{
    ClearPreview_();
    RequestFullRedraw_();
    return false;
}

bool RoadDecalInputControl::OnKeyDown(int32_t vkCode, uint32_t modifiers)
{
    if (!isActive_) {
        return false;
    }

    if (vkCode == VK_ESCAPE) {
        CancelStroke_();
        LOG_INFO("RoadDecalInputControl: ESC pressed, canceling");
        if (onCancel_) {
            onCancel_();
        }
        return true;
    }

    if (vkCode == 'Z' && (modifiers & kControlModifierMask) != 0) {
        UndoLastStroke_();
        RebuildRoadDecalGeometry();
        RequestFullRedraw_();
        return true;
    }

    if (vkCode == VK_DELETE) {
        CancelStroke_();
        ClearAllStrokes_();
        RebuildRoadDecalGeometry();
        RequestFullRedraw_();
        return true;
    }

    if (vkCode == VK_RETURN && isDrawing_) {
        return EndStroke_(true);
    }

    return false;
}

void RoadDecalInputControl::SetStyle(const int styleId)
{
    styleId_ = styleId;
}

void RoadDecalInputControl::SetWidth(float width)
{
    if (width < 0.05f) {
        width = 0.05f;
    }
    width_ = width;
}

void RoadDecalInputControl::SetDashed(const bool dashed)
{
    dashed_ = dashed;
}

void RoadDecalInputControl::SetOnCancel(std::function<void()> onCancel)
{
    onCancel_ = std::move(onCancel);
}

bool RoadDecalInputControl::PickWorld_(int32_t screenX, int32_t screenZ, RoadDecalPoint& outPoint)
{
    if (!view3D) {
        LOG_WARN("RoadDecalInputControl: view3D not available");
        return false;
    }

    float worldCoords[3] = {0.0f, 0.0f, 0.0f};
    if (!view3D->PickTerrain(screenX, screenZ, worldCoords, false)) {
        return false;
    }

    outPoint.x = worldCoords[0];
    outPoint.y = worldCoords[1] + kDecalHeightOffset;
    outPoint.z = worldCoords[2];

    // Keep points on a 2m subgrid for cleaner, more road-like alignment.
    outPoint.x = SnapToSubgrid(outPoint.x);
    outPoint.z = SnapToSubgrid(outPoint.z);
    return true;
}

bool RoadDecalInputControl::BeginStroke_(int32_t screenX, int32_t screenZ, uint32_t modifiers)
{
    RoadDecalPoint p{};
    if (!PickWorld_(screenX, screenZ, p)) {
        return false;
    }
    p.hardCorner = IsHardCornerModifierActive(modifiers);

    if (!SetCapture()) {
        LOG_WARN("RoadDecalInputControl: failed to SetCapture");
        return false;
    }

    currentStroke_.points.clear();
    currentStroke_.styleId = styleId_;
    currentStroke_.width = width_;
    currentStroke_.dashed = dashed_;
    currentStroke_.points.push_back(p);
    lastSamplePoint_ = p;
    isDrawing_ = true;
    RefreshActiveStroke_();
    ClearPreview_();
    RequestFullRedraw_();
    return true;
}

bool RoadDecalInputControl::AddSamplePoint_(int32_t screenX, int32_t screenZ, uint32_t modifiers)
{
    RoadDecalPoint p{};
    if (!PickWorld_(screenX, screenZ, p)) {
        return false;
    }
    p.hardCorner = IsHardCornerModifierActive(modifiers);

    const float dx = p.x - lastSamplePoint_.x;
    const float dy = p.y - lastSamplePoint_.y;
    const float dz = p.z - lastSamplePoint_.z;
    const float dist2 = dx * dx + dy * dy + dz * dz;
    constexpr float kMinSampleDist = 0.5f;

    if (dist2 < kMinSampleDist * kMinSampleDist) {
        return false;
    }

    currentStroke_.points.push_back(p);
    lastSamplePoint_ = p;
    RefreshActiveStroke_();
    ClearPreview_();
    RequestFullRedraw_();
    return true;
}

bool RoadDecalInputControl::EndStroke_(bool commit)
{
    if (commit && currentStroke_.points.size() >= 2) {
        gRoadDecalStrokes.push_back(currentStroke_);
        RebuildRoadDecalGeometry();
    }

    currentStroke_.points.clear();
    isDrawing_ = false;
    SetRoadDecalActiveStroke(nullptr);
    ClearPreview_();
    ReleaseCapture();
    RequestFullRedraw_();
    return true;
}

void RoadDecalInputControl::CancelStroke_()
{
    if (!isDrawing_) {
        return;
    }
    currentStroke_.points.clear();
    isDrawing_ = false;
    SetRoadDecalActiveStroke(nullptr);
    ClearPreview_();
    ReleaseCapture();
    RequestFullRedraw_();
}

void RoadDecalInputControl::UpdatePreviewFromScreen_(int32_t screenX, int32_t screenZ)
{
    RoadDecalPoint p{};
    if (!PickWorld_(screenX, screenZ, p)) {
        ClearPreview_();
        RequestFullRedraw_();
        return;
    }

    SetRoadDecalPreviewSegment(true, lastSamplePoint_, p, currentStroke_.styleId, currentStroke_.width, currentStroke_.dashed);
    RequestFullRedraw_();
}

void RoadDecalInputControl::ClearPreview_()
{
    const RoadDecalPoint zero{0.0f, 0.0f, 0.0f};
    SetRoadDecalPreviewSegment(false, zero, zero, 0, 0.0f, false);
}

void RoadDecalInputControl::RefreshActiveStroke_()
{
    SetRoadDecalActiveStroke(&currentStroke_);
}

void RoadDecalInputControl::RequestFullRedraw_()
{
    return;
    //
    // if (!view3D) {
    //     return;
    // }
    //
    // cISC43DRender* renderer = view3D->GetRenderer();
    // if (!renderer) {
    //     return;
    // }
    //
    // renderer->ForceFullRedraw();
}

void RoadDecalInputControl::UndoLastStroke_()
{
    if (gRoadDecalStrokes.empty()) {
        return;
    }
    gRoadDecalStrokes.pop_back();
}

void RoadDecalInputControl::ClearAllStrokes_()
{
    if (gRoadDecalStrokes.empty()) {
        return;
    }
    gRoadDecalStrokes.clear();
}
