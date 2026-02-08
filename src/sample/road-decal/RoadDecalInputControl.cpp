#include "RoadDecalInputControl.hpp"

#include "cISC43DRender.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cmath>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace
{
    constexpr uint32_t kControlModifierMask = 0x20000;
    constexpr uint32_t kShiftModifierMask = 0x10000;
    constexpr float kSnapSubgridMeters = 2.0f;
    constexpr float kDecalHeightOffset = 0.05f;
    constexpr float kMinSampleDist = 0.5f;
    constexpr float kRotationStep = 3.1415926f / 4.0f; // 45 deg

    float SnapToSubgrid(float value)
    {
        return std::round(value / kSnapSubgridMeters) * kSnapSubgridMeters;
    }

    bool IsHardCornerModifierActive(uint32_t modifiers)
    {
        if ((modifiers & kShiftModifierMask) != 0) {
            return true;
        }
        return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    }

    bool IsCtrlModifierActive(uint32_t modifiers)
    {
        if ((modifiers & kControlModifierMask) != 0) {
            return true;
        }
        return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    }

    bool IsShiftModifierActive(uint32_t modifiers)
    {
        if ((modifiers & kShiftModifierMask) != 0) {
            return true;
        }
        return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    }

    float NormalizeRadians(float angle)
    {
        constexpr float kPi = 3.1415926f;
        constexpr float kTwoPi = 6.2831852f;
        while (angle > kPi) {
            angle -= kTwoPi;
        }
        while (angle < -kPi) {
            angle += kTwoPi;
        }
        return angle;
    }
}

RoadDecalInputControl::RoadDecalInputControl()
    : cSC4BaseViewInputControl(kRoadDecalControlID)
    , isActive_(false)
    , isDrawing_(false)
    , hasGridPreviewPoint_(false)
    , hasMousePosition_(false)
    , lastMouseX_(0)
    , lastMouseZ_(0)
    , currentStroke_({})
    , lastSamplePoint_({})
    , lastGridPreviewPoint_({})
    , markupType_(RoadMarkupType::SolidWhiteLine)
    , placementMode_(PlacementMode::Freehand)
    , width_(0.15f)
    , length_(3.0f)
    , rotation_(0.0f)
    , dashed_(false)
    , dashLength_(3.0f)
    , gapLength_(9.0f)
    , autoAlign_(true)
    , color_(GetRoadMarkupProperties(RoadMarkupType::SolidWhiteLine).defaultColor)
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
    CancelStroke_();
    const RoadDecalPoint zero{0.0f, 0.0f, 0.0f};
    SetRoadDecalGridPreview(false, zero);
    hasGridPreviewPoint_ = false;
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
}

void RoadDecalInputControl::Deactivate()
{
    isActive_ = false;
    CancelStroke_();
    const RoadDecalPoint zero{0.0f, 0.0f, 0.0f};
    SetRoadDecalGridPreview(false, zero);
    hasGridPreviewPoint_ = false;
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
    if (!isActive_) {
        return false;
    }

    hasMousePosition_ = true;
    lastMouseX_ = x;
    lastMouseZ_ = z;

    UpdateGridPreviewFromScreen_(x, z);
    if (isDrawing_) {
        UpdatePreviewFromScreen_(x, z);
    } else {
        UpdateHoverPreviewFromScreen_(x, z);
    }
    return true;
}

bool RoadDecalInputControl::OnMouseUpL(int32_t x, int32_t z, uint32_t modifiers)
{
    (void)x;
    (void)z;
    (void)modifiers;
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
    const RoadDecalPoint zero{0.0f, 0.0f, 0.0f};
    SetRoadDecalGridPreview(false, zero);
    hasGridPreviewPoint_ = false;
    hasMousePosition_ = false;
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
        if (onCancel_) {
            onCancel_();
        }
        return true;
    }

    if (vkCode == 'Z' && IsCtrlModifierActive(modifiers)) {
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

    if (vkCode == 'R') {
        const bool reverse = IsShiftModifierActive(modifiers);
        rotation_ = NormalizeRadians(rotation_ + (reverse ? -kRotationStep : kRotationStep));
        if (onRotationChanged_) {
            onRotationChanged_(rotation_);
        }
        RefreshRotationPreview_();
        return true;
    }

    return false;
}

void RoadDecalInputControl::SetMarkupType(RoadMarkupType type)
{
    markupType_ = type;
}

void RoadDecalInputControl::SetPlacementMode(PlacementMode mode)
{
    placementMode_ = mode;
}

void RoadDecalInputControl::SetWidth(float width)
{
    width_ = std::max(0.05f, width);
}

void RoadDecalInputControl::SetLength(float length)
{
    length_ = std::max(0.0f, length);
}

void RoadDecalInputControl::SetRotation(float radians)
{
    rotation_ = NormalizeRadians(radians);
    RefreshRotationPreview_();
}

void RoadDecalInputControl::SetDashed(bool dashed)
{
    dashed_ = dashed;
}

void RoadDecalInputControl::SetDashPattern(float dashLength, float gapLength)
{
    dashLength_ = std::max(0.05f, dashLength);
    gapLength_ = std::max(0.0f, gapLength);
}

void RoadDecalInputControl::SetAutoAlign(bool enabled)
{
    autoAlign_ = enabled;
}

void RoadDecalInputControl::SetColor(uint32_t color)
{
    color_ = color;
}

void RoadDecalInputControl::SetOnRotationChanged(std::function<void(float)> onRotationChanged)
{
    onRotationChanged_ = std::move(onRotationChanged);
}

void RoadDecalInputControl::SetOnCancel(std::function<void()> onCancel)
{
    onCancel_ = std::move(onCancel);
}

bool RoadDecalInputControl::PickWorld_(int32_t screenX, int32_t screenZ, RoadDecalPoint& outPoint)
{
    if (!view3D) {
        return false;
    }
    float worldCoords[3] = {0.0f, 0.0f, 0.0f};
    if (!view3D->PickTerrain(screenX, screenZ, worldCoords, false)) {
        return false;
    }
    outPoint.x = SnapToSubgrid(worldCoords[0]);
    outPoint.y = worldCoords[1] + kDecalHeightOffset;
    outPoint.z = SnapToSubgrid(worldCoords[2]);
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
        return false;
    }

    currentStroke_ = {};
    currentStroke_.type = markupType_;
    currentStroke_.width = width_;
    currentStroke_.length = length_;
    currentStroke_.rotation = rotation_;
    currentStroke_.dashed = dashed_;
    currentStroke_.dashLength = dashLength_;
    currentStroke_.gapLength = gapLength_;
    currentStroke_.visible = true;
    currentStroke_.opacity = 1.0f;
    currentStroke_.color = color_;
    currentStroke_.points.push_back(p);
    lastSamplePoint_ = p;
    isDrawing_ = true;

    if (placementMode_ == PlacementMode::SingleClick) {
        return EndStroke_(true);
    }

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

    if (placementMode_ == PlacementMode::TwoPoint || placementMode_ == PlacementMode::Rectangle) {
        if (currentStroke_.points.size() == 1) {
            currentStroke_.points.push_back(p);
        } else {
            currentStroke_.points[1] = p;
        }

        if (autoAlign_) {
            const auto& a = currentStroke_.points[0];
            const float dx = p.x - a.x;
            const float dz = p.z - a.z;
            if (std::fabs(dx) > kMinSampleDist || std::fabs(dz) > kMinSampleDist) {
                currentStroke_.rotation = std::atan2(dz, dx);
            }
        }

        RefreshActiveStroke_();
        ClearPreview_();
        RequestFullRedraw_();
        return EndStroke_(true);
    }

    const float dx = p.x - lastSamplePoint_.x;
    const float dy = p.y - lastSamplePoint_.y;
    const float dz = p.z - lastSamplePoint_.z;
    const float dist2 = dx * dx + dy * dy + dz * dz;
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
    if (commit) {
        const auto category = GetMarkupCategory(currentStroke_.type);
        const bool singlePoint = (category == RoadMarkupCategory::DirectionalArrow ||
                                  placementMode_ == PlacementMode::SingleClick);
        const bool valid = singlePoint ? !currentStroke_.points.empty() : currentStroke_.points.size() >= 2;
        if (valid) {
            AddRoadMarkupStrokeToActiveLayer(currentStroke_);
            RebuildRoadDecalGeometry();
        }
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

    RoadMarkupStroke preview = currentStroke_;
    if (preview.points.empty()) {
        preview.points.push_back(p);
    } else if (placementMode_ == PlacementMode::SingleClick) {
        preview.points[0] = p;
    } else if (placementMode_ == PlacementMode::TwoPoint || placementMode_ == PlacementMode::Rectangle) {
        if (preview.points.size() == 1) {
            preview.points.push_back(p);
        } else {
            preview.points[1] = p;
        }
    } else {
        // Freehand preview should extend from the last committed click to cursor.
        // Never overwrite the last committed point, or the path appears to "invert".
        if (preview.points.size() >= 1) {
            preview.points.push_back(p);
        }
    }

    SetRoadDecalPreviewSegment(true, preview);
    RequestFullRedraw_();
}

void RoadDecalInputControl::UpdateGridPreviewFromScreen_(int32_t screenX, int32_t screenZ)
{
    RoadDecalPoint p{};
    if (!PickWorld_(screenX, screenZ, p)) {
        const RoadDecalPoint zero{0.0f, 0.0f, 0.0f};
        SetRoadDecalGridPreview(false, zero);
        hasGridPreviewPoint_ = false;
        return;
    }

    if (hasGridPreviewPoint_ &&
        std::fabs(lastGridPreviewPoint_.x - p.x) < 0.01f &&
        std::fabs(lastGridPreviewPoint_.z - p.z) < 0.01f) {
        return;
    }

    lastGridPreviewPoint_ = p;
    hasGridPreviewPoint_ = true;
    SetRoadDecalGridPreview(true, p);
}

void RoadDecalInputControl::UpdateHoverPreviewFromScreen_(int32_t screenX, int32_t screenZ)
{
    if (placementMode_ == PlacementMode::Freehand) {
        ClearPreview_();
        return;
    }

    RoadDecalPoint p{};
    if (!PickWorld_(screenX, screenZ, p)) {
        ClearPreview_();
        return;
    }

    RoadMarkupStroke preview{};
    preview.type = markupType_;
    preview.width = width_;
    preview.length = length_;
    preview.rotation = rotation_;
    preview.dashed = dashed_;
    preview.dashLength = dashLength_;
    preview.gapLength = gapLength_;
    preview.visible = true;
    preview.opacity = 0.7f;
    preview.color = color_;

    const auto category = GetMarkupCategory(markupType_);
    if (placementMode_ == PlacementMode::SingleClick ||
        category == RoadMarkupCategory::DirectionalArrow ||
        category == RoadMarkupCategory::ZoneMarking ||
        category == RoadMarkupCategory::TextLabel) {
        preview.points.push_back(p);
        SetRoadDecalPreviewSegment(true, preview);
        return;
    }

    // For line/two-point modes, preview a tiny starter segment around cursor.
    RoadDecalPoint p2 = p;
    p2.x += kSnapSubgridMeters;
    preview.points.push_back(p);
    preview.points.push_back(p2);
    SetRoadDecalPreviewSegment(true, preview);
}

void RoadDecalInputControl::ClearPreview_()
{
    RoadMarkupStroke emptyStroke{};
    SetRoadDecalPreviewSegment(false, emptyStroke);
}

void RoadDecalInputControl::RefreshActiveStroke_()
{
    SetRoadDecalActiveStroke(&currentStroke_);
}

void RoadDecalInputControl::RefreshRotationPreview_()
{
    if (isDrawing_) {
        currentStroke_.rotation = rotation_;
        RefreshActiveStroke_();
        if (hasMousePosition_) {
            UpdatePreviewFromScreen_(lastMouseX_, lastMouseZ_);
        } else {
            RequestFullRedraw_();
        }
        return;
    }

    if (hasMousePosition_) {
        UpdateHoverPreviewFromScreen_(lastMouseX_, lastMouseZ_);
    } else {
        RequestFullRedraw_();
    }
}

void RoadDecalInputControl::RequestFullRedraw_()
{
    return;
}

void RoadDecalInputControl::UndoLastStroke_()
{
    UndoLastRoadMarkupStroke();
}

void RoadDecalInputControl::ClearAllStrokes_()
{
    ClearAllRoadMarkupStrokes();
}
