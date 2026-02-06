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

#include <array>
#include <atomic>
#include <cstdint>

namespace
{
    constexpr uint32_t kRoadDecalDirectorID = 0xE59A5D21;
    constexpr uint32_t kRoadDecalPanelId = 0x9B4A7A11;
    constexpr float kRoadDecalWidth = 0.8f;

    RoadDecalInputControl* gRoadDecalTool = nullptr;
    std::atomic<bool> gRoadDecalToolEnabled{false};
    std::atomic<int> gRoadDecalStyle{0};
    std::atomic<bool> gRoadDecalDashed{false};

    bool EnableRoadDecalTool()
    {
        if (gRoadDecalToolEnabled.load(std::memory_order_relaxed)) {
            return true;
        }

        auto view3D = SC4UI::GetView3DWin();
        if (!view3D) {
            LOG_WARN("RoadDecalSample: View3D not available");
            return false;
        }

        if (!gRoadDecalTool) {
            gRoadDecalTool = new RoadDecalInputControl();
            gRoadDecalTool->AddRef();
            gRoadDecalTool->SetStyle(gRoadDecalStyle.load(std::memory_order_relaxed));
            gRoadDecalTool->SetWidth(kRoadDecalWidth);
            gRoadDecalTool->SetDashed(gRoadDecalDashed.load(std::memory_order_relaxed));
            gRoadDecalTool->SetOnCancel([]() {});
            gRoadDecalTool->Activate();
        }

        if (!view3D->SetCurrentViewInputControl(gRoadDecalTool,
                                                cISC4View3DWin::ViewInputControlStackOperation_RemoveCurrentControl)) {
            LOG_WARN("RoadDecalSample: failed to set current view input control");
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

    class RoadDecalPanel final : public ImGuiPanel
    {
    public:
        void OnRender() override
        {
            ImGui::Begin("Road Decals");

            bool toolEnabled = gRoadDecalToolEnabled.load(std::memory_order_relaxed);
            if (ImGui::Checkbox("Enable", &toolEnabled)) {
                if (toolEnabled) {
                    toolEnabled = EnableRoadDecalTool();
                } else {
                    DisableRoadDecalTool();
                }
                gRoadDecalToolEnabled.store(toolEnabled, std::memory_order_relaxed);
            }

            static const char* styleItems[] = {"White", "Yellow", "Red"};
            int style = gRoadDecalStyle.load(std::memory_order_relaxed);
            if (ImGui::Combo("Color", &style, styleItems, static_cast<int>(std::size(styleItems)))) {
                gRoadDecalStyle.store(style, std::memory_order_relaxed);
                if (gRoadDecalTool) {
                    gRoadDecalTool->SetStyle(style);
                }
            }

            bool dashed = gRoadDecalDashed.load(std::memory_order_relaxed);
            if (ImGui::Checkbox("Dashed", &dashed)) {
                gRoadDecalDashed.store(dashed, std::memory_order_relaxed);
                if (gRoadDecalTool) {
                    gRoadDecalTool->SetDashed(dashed);
                }
            }

            if (ImGui::Button("Undo")) {
                if (!gRoadDecalStrokes.empty()) {
                    gRoadDecalStrokes.pop_back();
                    RebuildRoadDecalGeometry();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear")) {
                if (!gRoadDecalStrokes.empty()) {
                    gRoadDecalStrokes.clear();
                    RebuildRoadDecalGeometry();
                }
            }

            //ImGui::Text("Strokes: %u", static_cast<uint32_t>(gRoadDecalStrokes.size()));
            //ImGui::TextUnformatted("LMB add points, Shift hard corner, RMB/Enter finish, Ctrl+Z undo, Delete clear.");
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
        LOG_INFO("RoadDecalSample: OnStart");
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
            LOG_WARN("RoadDecalSample: ImGui service not available");
            return true;
        }

        auto* panel = new RoadDecalPanel();
        const ImGuiPanelDesc desc =
            ImGuiPanelAdapter<RoadDecalPanel>::MakeDesc(panel, kRoadDecalPanelId, 120, true);

        if (!imguiService_->RegisterPanel(desc)) {
            LOG_WARN("RoadDecalSample: failed to register panel");
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
            LOG_WARN("RoadDecalSample: Draw service not available; decals will not be drawn");
            return true;
        }

        if (!drawService_->RegisterDrawPassCallback(DrawServicePass::PreDynamic,
                                                    &DrawPassRoadDecalCallback,
                                                    nullptr,
                                                    &drawPassCallbackToken_)) {
            LOG_WARN("RoadDecalSample: failed to subscribe to pre-dynamic draw pass");
        }
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
