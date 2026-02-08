#include "cIGZFrameWork.h"
#include "cRZCOMDllDirector.h"

#include "imgui.h"
#include "public/ImGuiPanelAdapter.h"
#include "public/ImGuiServiceIds.h"
#include "public/cIGZDrawService.h"
#include "public/cIGZImGuiService.h"
#include "sample/road-decal/RoadDecalData.hpp"
#include "sample/road-decal/RoadDecalInputControl.hpp"
#include "utils/Logger.h"
#include "SC4UI.h"

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace
{
    constexpr uint32_t kRoadDecalDirectorID = 0xE59A5D21;
    constexpr uint32_t kRoadDecalPanelId = 0x9B4A7A11;

    RoadDecalInputControl* gRoadDecalTool = nullptr;
    std::atomic<bool> gRoadDecalToolEnabled{false};

    RoadMarkupType gSelectedType = RoadMarkupType::SolidWhiteLine;
    PlacementMode gPlacementMode = PlacementMode::Freehand;
    float gWidth = 0.15f;
    float gLength = 3.0f;
    float gRotationDeg = 0.0f;
    bool gDashed = false;
    float gDashLength = 3.0f;
    float gGapLength = 9.0f;
    bool gAutoAlign = true;
    bool gUseTypeDefaultColor = true;
    uint32_t gCustomColor = 0xE0FFFFFF;
    char gSavePath[260] = "road_markups.dat";

    ImVec4 ColorToImVec4(uint32_t argb)
    {
        const float a = static_cast<float>((argb >> 24U) & 0xFFU) / 255.0f;
        const float r = static_cast<float>((argb >> 16U) & 0xFFU) / 255.0f;
        const float g = static_cast<float>((argb >> 8U) & 0xFFU) / 255.0f;
        const float b = static_cast<float>(argb & 0xFFU) / 255.0f;
        return {r, g, b, a};
    }

    uint32_t ImVec4ToColor(const ImVec4& c)
    {
        const uint32_t a = static_cast<uint32_t>(std::clamp(c.w, 0.0f, 1.0f) * 255.0f);
        const uint32_t r = static_cast<uint32_t>(std::clamp(c.x, 0.0f, 1.0f) * 255.0f);
        const uint32_t g = static_cast<uint32_t>(std::clamp(c.y, 0.0f, 1.0f) * 255.0f);
        const uint32_t b = static_cast<uint32_t>(std::clamp(c.z, 0.0f, 1.0f) * 255.0f);
        return (a << 24U) | (r << 16U) | (g << 8U) | b;
    }

    void SyncToolSettings()
    {
        if (!gRoadDecalTool) {
            return;
        }
        gRoadDecalTool->SetMarkupType(gSelectedType);
        gRoadDecalTool->SetPlacementMode(gPlacementMode);
        gRoadDecalTool->SetWidth(gWidth);
        gRoadDecalTool->SetLength(gLength);
        gRoadDecalTool->SetRotation(gRotationDeg * 3.1415926f / 180.0f);
        gRoadDecalTool->SetDashed(gDashed);
        gRoadDecalTool->SetDashPattern(gDashLength, gGapLength);
        gRoadDecalTool->SetAutoAlign(gAutoAlign);
        const uint32_t resolvedColor = gUseTypeDefaultColor
            ? GetRoadMarkupProperties(gSelectedType).defaultColor
            : gCustomColor;
        gRoadDecalTool->SetColor(resolvedColor);
    }

    bool EnableRoadDecalTool()
    {
        if (gRoadDecalToolEnabled.load(std::memory_order_relaxed)) {
            return true;
        }

        auto view3D = SC4UI::GetView3DWin();
        if (!view3D) {
            LOG_WARN("RoadMarkup: View3D not available");
            return false;
        }

        if (!gRoadDecalTool) {
            gRoadDecalTool = new RoadDecalInputControl();
            gRoadDecalTool->AddRef();
            gRoadDecalTool->SetOnRotationChanged([](float radians) {
                gRotationDeg = radians * 180.0f / 3.1415926f;
            });
            gRoadDecalTool->SetOnCancel([]() {});
            gRoadDecalTool->Activate();
        }
        SyncToolSettings();

        if (!view3D->SetCurrentViewInputControl(gRoadDecalTool,
                                                cISC4View3DWin::ViewInputControlStackOperation_RemoveCurrentControl)) {
            LOG_WARN("RoadMarkup: failed to set current view input control");
            return false;
        }

        gRoadDecalToolEnabled.store(true, std::memory_order_relaxed);
        return true;
    }

    void DisableRoadDecalTool()
    {
        if (!gRoadDecalToolEnabled.load(std::memory_order_relaxed)) {
            return;
        }

        auto view3D = SC4UI::GetView3DWin();
        if (view3D) {
            auto* currentControl = view3D->GetCurrentViewInputControl();
            if (currentControl == gRoadDecalTool) {
                view3D->RemoveCurrentViewInputControl(false);
            }
        }

        gRoadDecalToolEnabled.store(false, std::memory_order_relaxed);
    }

    void DestroyRoadDecalTool()
    {
        DisableRoadDecalTool();
        if (gRoadDecalTool) {
            gRoadDecalTool->Release();
            gRoadDecalTool = nullptr;
        }
    }

    void DrawPassRoadDecalCallback(DrawServicePass pass, bool begin, void*)
    {
        if (pass != DrawServicePass::PreDynamic || begin) {
            return;
        }
        DrawRoadDecals();
    }

    void DrawTypeButtons(RoadMarkupCategory category)
    {
        const auto& types = GetRoadMarkupTypesForCategory(category);
        for (size_t i = 0; i < types.size(); ++i) {
            const auto type = types[i];
            const auto& props = GetRoadMarkupProperties(type);
            if (ImGui::Selectable(props.displayName, gSelectedType == type)) {
                gSelectedType = type;
                gWidth = props.defaultWidth > 0.0f ? props.defaultWidth : gWidth;
                gLength = props.defaultLength > 0.0f ? props.defaultLength : gLength;
                gDashed = props.supportsDashing;
                if (gUseTypeDefaultColor) {
                    gCustomColor = props.defaultColor;
                }

                if (category == RoadMarkupCategory::DirectionalArrow) {
                    gPlacementMode = PlacementMode::SingleClick;
                } else if (category == RoadMarkupCategory::Crossing) {
                    gPlacementMode = PlacementMode::TwoPoint;
                } else {
                    gPlacementMode = PlacementMode::Freehand;
                }
            }
            if ((i + 1) % 2 != 0 && (i + 1) < types.size()) {
                ImGui::SameLine();
            }
        }
    }

    const char* PlacementModeName(PlacementMode mode)
    {
        switch (mode) {
            case PlacementMode::Freehand:
                return "Freehand";
            case PlacementMode::TwoPoint:
                return "Two Point";
            case PlacementMode::SingleClick:
                return "Single Click";
            case PlacementMode::Rectangle:
                return "Rectangle";
            case PlacementMode::Snapping:
                return "Snapping";
        }
        return "Unknown";
    }

    void HandlePanelRotationHotkeys()
    {
        if (!gRoadDecalToolEnabled.load(std::memory_order_relaxed)) {
            return;
        }
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput) {
            return;
        }
        if (!ImGui::IsKeyPressed(ImGuiKey_R, false)) {
            return;
        }

        gRotationDeg += io.KeyShift ? -45.0f : 45.0f;
        while (gRotationDeg > 180.0f) {
            gRotationDeg -= 360.0f;
        }
        while (gRotationDeg < -180.0f) {
            gRotationDeg += 360.0f;
        }
        SyncToolSettings();
    }

    class RoadDecalPanel final : public ImGuiPanel
    {
    public:
        void OnRender() override
        {
            EnsureDefaultRoadMarkupLayer();
            ImGui::Begin("Road Markings");
            HandlePanelRotationHotkeys();

            bool toolEnabled = gRoadDecalToolEnabled.load(std::memory_order_relaxed);
            if (ImGui::Checkbox("Enable Tool", &toolEnabled)) {
                if (toolEnabled) {
                    toolEnabled = EnableRoadDecalTool();
                } else {
                    DisableRoadDecalTool();
                }
                gRoadDecalToolEnabled.store(toolEnabled, std::memory_order_relaxed);
            }

            if (ImGui::BeginTabBar("##RoadMarkupTabs")) {
                if (ImGui::BeginTabItem("Lane Lines")) {
                    DrawTypeButtons(RoadMarkupCategory::LaneDivider);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Arrows")) {
                    DrawTypeButtons(RoadMarkupCategory::DirectionalArrow);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Crossings")) {
                    DrawTypeButtons(RoadMarkupCategory::Crossing);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Zones")) {
                    DrawTypeButtons(RoadMarkupCategory::ZoneMarking);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }

            const auto& props = GetRoadMarkupProperties(gSelectedType);
            const auto category = GetMarkupCategory(gSelectedType);
            const bool showDashControls = props.supportsDashing && category == RoadMarkupCategory::LaneDivider;
            const bool showLengthControl = (category == RoadMarkupCategory::DirectionalArrow ||
                                            category == RoadMarkupCategory::ZoneMarking ||
                                            category == RoadMarkupCategory::TextLabel ||
                                            (category == RoadMarkupCategory::Crossing &&
                                             gSelectedType != RoadMarkupType::StopBar));
            const bool showRotationControls = (category == RoadMarkupCategory::DirectionalArrow ||
                                               category == RoadMarkupCategory::ZoneMarking ||
                                               category == RoadMarkupCategory::TextLabel ||
                                               category == RoadMarkupCategory::Crossing);

            ImGui::Text("Selected: %s", props.displayName);
            if (category == RoadMarkupCategory::Crossing && gSelectedType == RoadMarkupType::StopBar) {
                ImGui::SliderFloat("Thickness", &gWidth, 0.05f, 4.0f, "%.2f m");
            } else {
                ImGui::SliderFloat("Width", &gWidth, 0.05f, 4.0f, "%.2f m");
            }
            if (showLengthControl) {
                const char* label = category == RoadMarkupCategory::Crossing ? "Depth" : "Length";
                ImGui::SliderFloat(label, &gLength, 0.5f, 12.0f, "%.2f m");
            }
            if (showDashControls) {
                ImGui::Checkbox("Dashed", &gDashed);
            } else {
                gDashed = false;
            }
            if (showDashControls && gDashed) {
                ImGui::SliderFloat("Dash Length", &gDashLength, 0.2f, 12.0f, "%.2f m");
                ImGui::SliderFloat("Gap Length", &gGapLength, 0.1f, 12.0f, "%.2f m");
            }
            if (showRotationControls) {
                ImGui::Checkbox("Auto Align", &gAutoAlign);
                ImGui::SliderFloat("Rotation", &gRotationDeg, -180.0f, 180.0f, "%.0f deg");
                if (ImGui::Button("-90")) {
                    gRotationDeg -= 90.0f;
                }
                ImGui::SameLine();
                if (ImGui::Button("+90")) {
                    gRotationDeg += 90.0f;
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset")) {
                    gRotationDeg = 0.0f;
                }
            }

            // Normalize to [-180, 180] to keep the slider stable.
            while (gRotationDeg > 180.0f) {
                gRotationDeg -= 360.0f;
            }
            while (gRotationDeg < -180.0f) {
                gRotationDeg += 360.0f;
            }

            ImGui::Checkbox("Use Type Color", &gUseTypeDefaultColor);
            ImVec4 color = ColorToImVec4(gUseTypeDefaultColor ? props.defaultColor : gCustomColor);
            if (ImGui::ColorEdit4("Color", &color.x)) {
                gCustomColor = ImVec4ToColor(color);
                gUseTypeDefaultColor = false;
            }
            ImGui::Text("Mode: %s", PlacementModeName(gPlacementMode));

            ImGui::Separator();
            if (ImGui::CollapsingHeader("Layers", ImGuiTreeNodeFlags_DefaultOpen)) {
                for (size_t i = 0; i < gRoadMarkupLayers.size(); ++i) {
                    auto& layer = gRoadMarkupLayers[i];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::Checkbox("##vis", &layer.visible);
                    ImGui::SameLine();
                    if (ImGui::Selectable(layer.name.c_str(), gActiveLayerIndex == static_cast<int>(i))) {
                        gActiveLayerIndex = static_cast<int>(i);
                    }
                    ImGui::PopID();
                }
                if (ImGui::Button("New")) {
                    AddRoadMarkupLayer("Layer");
                }
                ImGui::SameLine();
                if (ImGui::Button("Delete")) {
                    DeleteActiveRoadMarkupLayer();
                }
                if (gActiveLayerIndex >= 0 && gActiveLayerIndex < static_cast<int>(gRoadMarkupLayers.size())) {
                    ImGui::SameLine();
                    if (ImGui::Button("Up") && gActiveLayerIndex > 0) {
                        std::swap(gRoadMarkupLayers[gActiveLayerIndex], gRoadMarkupLayers[gActiveLayerIndex - 1]);
                        --gActiveLayerIndex;
                        for (size_t i = 0; i < gRoadMarkupLayers.size(); ++i) {
                            gRoadMarkupLayers[i].renderOrder = static_cast<int>(i);
                        }
                        RebuildRoadDecalGeometry();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Down") && gActiveLayerIndex + 1 < static_cast<int>(gRoadMarkupLayers.size())) {
                        std::swap(gRoadMarkupLayers[gActiveLayerIndex], gRoadMarkupLayers[gActiveLayerIndex + 1]);
                        ++gActiveLayerIndex;
                        for (size_t i = 0; i < gRoadMarkupLayers.size(); ++i) {
                            gRoadMarkupLayers[i].renderOrder = static_cast<int>(i);
                        }
                        RebuildRoadDecalGeometry();
                    }
                }
            }

            ImGui::Separator();
            if (ImGui::Button("Undo")) {
                UndoLastRoadMarkupStroke();
                RebuildRoadDecalGeometry();
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear All")) {
                ClearAllRoadMarkupStrokes();
                RebuildRoadDecalGeometry();
            }

            if (ImGui::CollapsingHeader("Persistence")) {
                ImGui::InputText("File", gSavePath, sizeof(gSavePath));
                if (ImGui::Button("Save")) {
                    SaveMarkupsToFile(gSavePath);
                }
                ImGui::SameLine();
                if (ImGui::Button("Load")) {
                    LoadMarkupsFromFile(gSavePath);
                }
            }

            ImGui::Text("Markings: %u", static_cast<uint32_t>(GetTotalRoadMarkupStrokeCount()));
            ImGui::TextUnformatted("LMB: place/draw  RMB: finish/clear  ESC: cancel  Ctrl+Z: undo");

            SyncToolSettings();
            ImGui::End();
        }
    };
}

extern std::atomic<cIGZImGuiService*> gImGuiServiceForD3DOverlay;

class RoadDecalSampleDirector final : public cRZCOMDllDirector
{
public:
    [[nodiscard]] uint32_t GetDirectorID() const override
    {
        return kRoadDecalDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override
    {
        cRZCOMDllDirector::OnStart(pCOM);
        Logger::Initialize("SC4RoadDecalSample", "");
        if (mpFrameWork) {
            mpFrameWork->AddHook(this);
        }
        return true;
    }

    bool PostAppInit() override
    {
        if (!mpFrameWork || panelRegistered_) {
            return true;
        }

        if (!mpFrameWork->GetSystemService(kImGuiServiceID,
                                           GZIID_cIGZImGuiService,
                                           reinterpret_cast<void**>(&imguiService_))) {
            LOG_WARN("RoadMarkup: ImGui service not available");
            return true;
        }

        auto* panel = new RoadDecalPanel();
        const ImGuiPanelDesc desc = ImGuiPanelAdapter<RoadDecalPanel>::MakeDesc(panel, kRoadDecalPanelId, 120, true);
        if (!imguiService_->RegisterPanel(desc)) {
            delete panel;
            imguiService_->Release();
            imguiService_ = nullptr;
            return true;
        }
        panelRegistered_ = true;
        gImGuiServiceForD3DOverlay.store(imguiService_, std::memory_order_release);

        if (!mpFrameWork->GetSystemService(kDrawServiceID,
                                           GZIID_cIGZDrawService,
                                           reinterpret_cast<void**>(&drawService_))) {
            LOG_WARN("RoadMarkup: Draw service not available");
            return true;
        }

        drawService_->RegisterDrawPassCallback(DrawServicePass::PreDynamic,
                                               &DrawPassRoadDecalCallback,
                                               nullptr,
                                               &drawPassCallbackToken_);
        return true;
    }

    bool PostAppShutdown() override
    {
        if (drawService_) {
            if (drawPassCallbackToken_ != 0) {
                drawService_->UnregisterDrawPassCallback(drawPassCallbackToken_);
                drawPassCallbackToken_ = 0;
            }
            drawService_->Release();
            drawService_ = nullptr;
        }

        DestroyRoadDecalTool();
        gImGuiServiceForD3DOverlay.store(nullptr, std::memory_order_release);

        if (imguiService_) {
            imguiService_->UnregisterPanel(kRoadDecalPanelId);
            imguiService_->Release();
            imguiService_ = nullptr;
        }
        panelRegistered_ = false;
        return true;
    }

private:
    cIGZImGuiService* imguiService_ = nullptr;
    cIGZDrawService* drawService_ = nullptr;
    uint32_t drawPassCallbackToken_ = 0;
    bool panelRegistered_ = false;
};

static RoadDecalSampleDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector()
{
    static bool sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
