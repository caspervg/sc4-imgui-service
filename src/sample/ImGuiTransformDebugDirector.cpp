#include <d3d.h>
#include <ddraw.h>


#include "GZServPtrs.h"
#include "cIGZFrameWork.h"
#include "cRZCOMDllDirector.h"
#include "imgui.h"
#include "public/ImGuiPanel.h"
#include "public/ImGuiPanelAdapter.h"
#include "public/ImGuiServiceIds.h"
#include "public/TransformLogger.h"
#include "public/cIGZImGuiService.h"
#include "utils/Logger.h"

#include <map>
#include <vector>

namespace {
    constexpr uint32_t kTransformDebugDirectorID = 0xDEB00100;
    constexpr uint32_t kTransformDebugPanelId = 0xDEB00101;

    class TransformDebugPanel final : public ImGuiPanel {
    public:
        explicit TransformDebugPanel(cIGZImGuiService* service)
            : service_(service) {
            if (service_) {
                service_->AddRef();
            }
        }

        void OnInit() override {
            LOG_INFO("TransformDebugPanel: initialized");

            // Install transform logger if we can get the D3D device
            IDirect3DDevice7* d3d = nullptr;
            IDirectDraw7* dd = nullptr;
            if (service_ && service_->AcquireD3DInterfaces(&d3d, &dd)) {
                if (TransformLogger::Instance().Install(d3d)) {
                    LOG_INFO("TransformDebugPanel: transform logger installed");
                    loggerInstalled_ = true;
                }
                else {
                    LOG_WARN("TransformDebugPanel: failed to install transform logger");
                }
                d3d->Release();
                dd->Release();
            } else {
                LOG_WARN("TransformDebugPanel: failed to acquire D3D interfaces");
            }
        }

        void OnRender() override {
            ImGui::Begin("Transform Logger", nullptr, ImGuiWindowFlags_None);

            RenderControlBar();
            ImGui::Separator();

            if (ImGui::BeginTabBar("TransformTabs")) {
                if (ImGui::BeginTabItem("Transform Log")) {
                    RenderTransformLogTab();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Unique Matrices")) {
                    RenderUniqueMatricesTab();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("City Matrices")) {
                    RenderCityMatricesTab();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Projection Test")) {
                    RenderProjectionTestTab();
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            ImGui::End();
        }

        void OnShutdown() override {
            LOG_INFO("TransformDebugPanel: shutdown");

            if (loggerInstalled_) {
                TransformLogger::Instance().Uninstall();
                loggerInstalled_ = false;
            }

            if (service_) {
                service_->Release();
                service_ = nullptr;
            }

            delete this;
        }

    private:
        void RenderControlBar() {
            auto& logger = TransformLogger::Instance();

            // Installation status
            if (logger.IsInstalled()) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Hooks: ACTIVE");
            }
            else {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Hooks: NOT INSTALLED");
            }

            ImGui::SameLine();
            ImGui::Text("| Frame: %u", logger.GetFrameNumber());

            ImGui::SameLine();
            if (logger.HasValidCityMatrices()) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "| Matrices: VALID");
            }
            else {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "| Matrices: PENDING");
            }

            // Controls
            if (ImGui::Checkbox("Pause", &pauseCapture_)) {
                if (pauseCapture_) {
                    snapshotLog_ = logger.GetPreviousFrameLog();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Snapshot")) {
                snapshotLog_ = logger.GetPreviousFrameLog();
                pauseCapture_ = true;
            }
            ImGui::SameLine();
            bool captureEnabled = logger.IsCaptureEnabled();
            if (ImGui::Checkbox("Capture", &captureEnabled)) {
                logger.SetCaptureEnabled(captureEnabled);
            }
        }

        void RenderTransformLogTab() {
            auto& logger = TransformLogger::Instance();

            ImGui::Checkbox("WORLD", &showWorldTransforms_);
            ImGui::SameLine();
            ImGui::Checkbox("Identity", &showIdentityMatrices_);
            ImGui::SameLine();
            ImGui::Checkbox("Values", &showMatrixValues_);
            ImGui::SameLine();
            ImGui::Checkbox("Perspective only", &filterPerspectiveOnly_);

            const auto& log = pauseCapture_ ? snapshotLog_ : logger.GetPreviousFrameLog();
            ImGui::Text("Entries: %zu", log.size());

            ImGui::Separator();

            ImGui::BeginChild("TransformList", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);

            for (size_t i = 0; i < log.size(); ++i) {
                const auto& entry = log[i];
                auto analysis = TransformLogger::AnalyzeMatrix(entry.matrix, entry.state);

                if (!showWorldTransforms_ && entry.state == D3DTRANSFORMSTATE_WORLD) continue;
                if (!showIdentityMatrices_ && analysis.isIdentity) continue;
                if (filterPerspectiveOnly_ && !analysis.isPerspective) continue;

                const char* stateName = GetStateName(entry.state);
                ImVec4 color = GetStateColor(entry.state, analysis);

                ImGui::PushStyleColor(ImGuiCol_Text, color);

                char label[256];
                if (analysis.isPerspective) {
                    snprintf(label, sizeof(label), "[%3u] %s @ 0x%08X (PERSP FOV=%.1f)",
                        entry.callIndex, stateName, entry.callerAddress,
                        analysis.estimatedFovDegrees);
                }
                else if (analysis.isOrthographic) {
                    snprintf(label, sizeof(label), "[%3u] %s @ 0x%08X (ORTHO)",
                        entry.callIndex, stateName, entry.callerAddress);
                }
                else if (analysis.isIdentity) {
                    snprintf(label, sizeof(label), "[%3u] %s @ 0x%08X (IDENTITY)",
                        entry.callIndex, stateName, entry.callerAddress);
                }
                else {
                    snprintf(label, sizeof(label), "[%3u] %s @ 0x%08X",
                        entry.callIndex, stateName, entry.callerAddress);
                }

                bool nodeOpen = ImGui::TreeNode(reinterpret_cast<void*>(i), "%s", label);

                if (ImGui::BeginPopupContextItem()) {
                    char addrStr[32];
                    snprintf(addrStr, sizeof(addrStr), "0x%08X", entry.callerAddress);
                    if (ImGui::MenuItem("Copy address")) {
                        ImGui::SetClipboardText(addrStr);
                    }
                    if (ImGui::MenuItem("Filter to this caller")) {
                        logger.SetTargetCallerAddress(entry.callerAddress);
                    }
                    if (ImGui::MenuItem("Clear filter")) {
                        logger.SetTargetCallerAddress(0);
                    }
                    ImGui::EndPopup();
                }

                ImGui::PopStyleColor();

                if (nodeOpen) {
                    if (showMatrixValues_) {
                        RenderMatrixDetails(entry.matrix, analysis);
                    }
                    ImGui::TreePop();
                }
            }

            ImGui::EndChild();
        }

        void RenderUniqueMatricesTab() {
            auto& logger = TransformLogger::Instance();

            bool trackUnique = logger.IsTrackingUniqueMatrices();
            if (ImGui::Checkbox("Track unique matrices", &trackUnique)) {
                logger.SetTrackUniqueMatrices(trackUnique);
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear History")) {
                logger.ClearUniqueMatrixHistory();
            }

            float threshold = logger.GetMatrixSimilarityThreshold();
            if (ImGui::SliderFloat("Threshold", &threshold, 0.00001f, 0.01f, "%.5f",
                ImGuiSliderFlags_Logarithmic)) {
                logger.SetMatrixSimilarityThreshold(threshold);
            }

            ImGui::Separator();

            auto uniqueProj = logger.GetUniqueProjections();
            auto uniqueView = logger.GetUniqueViews();

            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                "Unique PROJECTION: %zu", uniqueProj.size());

            if (ImGui::BeginChild("ProjList", ImVec2(0, 180), true)) {
                for (size_t i = 0; i < uniqueProj.size(); ++i) {
                    const auto& fp = uniqueProj[i];

                    char label[256];
                    if (fp.analysis.isPerspective) {
                        snprintf(label, sizeof(label),
                            "[%zu] PERSP FOV=%.1f @ 0x%08X (%u hits)",
                            i, fp.analysis.estimatedFovDegrees, fp.callerAddress, fp.hitCount);
                    }
                    else {
                        snprintf(label, sizeof(label),
                            "[%zu] %s @ 0x%08X (%u hits)",
                            i, fp.analysis.isOrthographic ? "ORTHO" : "OTHER",
                            fp.callerAddress, fp.hitCount);
                    }

                    ImVec4 color = fp.analysis.isPerspective ?
                        ImVec4(0.5f, 1.0f, 0.5f, 1.0f) : ImVec4(0.7f, 0.7f, 1.0f, 1.0f);

                    ImGui::PushStyleColor(ImGuiCol_Text, color);
                    if (ImGui::TreeNode(reinterpret_cast<void*>(i), "%s", label)) {
                        RenderMatrixDetails(fp.matrix, fp.analysis);
                        ImGui::Text("Frames: %u - %u", fp.firstSeenFrame, fp.lastSeenFrame);
                        ImGui::TreePop();
                    }
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndChild();

            ImGui::Separator();

            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f),
                "Unique VIEW: %zu", uniqueView.size());

            std::map<uint32_t, uint32_t> viewCallerCounts;
            for (const auto& fp : uniqueView) {
                viewCallerCounts[fp.callerAddress]++;
            }
            for (const auto& [addr, count] : viewCallerCounts) {
                ImGui::BulletText("0x%08X: %u matrices", addr, count);
            }
        }

        void RenderCityMatricesTab() {
            auto& logger = TransformLogger::Instance();

            D3DMATRIX view, projection;
            if (!logger.GetCityViewMatrices(view, projection)) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
                    "City view matrices not captured yet.");
                ImGui::TextWrapped("Enter city view and wait for rendering.");
                return;
            }

            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Matrices captured!");

            auto projAnalysis = TransformLogger::AnalyzeMatrix(projection, D3DTRANSFORMSTATE_PROJECTION);

            ImGui::Separator();
            ImGui::Text("VIEW Matrix:");
            RenderMatrixDetails(view, TransformAnalysis{});

            ImGui::Separator();
            ImGui::Text("PROJECTION Matrix:");
            RenderMatrixDetails(projection, projAnalysis);

            ImGui::Separator();
            ImGui::Text("Translation: (%.2f, %.2f, %.2f)", view._41, view._42, view._43);

            if (projAnalysis.isPerspective) {
                ImGui::Text("FOV: %.1f°  Near: %.2f  Far: %.2f",
                    projAnalysis.estimatedFovDegrees,
                    projAnalysis.estimatedNear,
                    projAnalysis.estimatedFar);
            }
        }

        void RenderProjectionTestTab() {
            auto& logger = TransformLogger::Instance();

            if (!logger.HasValidCityMatrices()) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
                    "Waiting for city matrices...");
                return;
            }

            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Ready!");

            ImGui::Separator();

            ImGui::DragFloat3("World Pos", testWorldPos_, 10.0f);
            ImGui::Checkbox("Draw marker", &drawTestMarker_);

            ScreenPoint pt = logger.WorldToScreen(testWorldPos_[0], testWorldPos_[1], testWorldPos_[2]);

            if (pt.visible) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
                    "Screen: (%.1f, %.1f) depth=%.4f", pt.x, pt.y, pt.depth);
            }
            else {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
                    "Off-screen: (%.1f, %.1f)", pt.x, pt.y);
            }

            ImGui::Separator();

            ImGui::Checkbox("Draw grid", &drawGrid_);
            if (drawGrid_) {
                ImGui::DragFloat("Spacing", &gridSpacing_, 10.0f, 10.0f, 500.0f);
                ImGui::SliderInt("Size", &gridSize_, 1, 15);
            }

            ImGui::Separator();

            ImGui::Text("Presets:");
            if (ImGui::Button("Origin")) {
                testWorldPos_[0] = 0; testWorldPos_[1] = 0; testWorldPos_[2] = 0;
            }
            ImGui::SameLine();
            if (ImGui::Button("500,0,500")) {
                testWorldPos_[0] = 500; testWorldPos_[1] = 0; testWorldPos_[2] = 500;
            }
            ImGui::SameLine();
            if (ImGui::Button("1000,50,1000")) {
                testWorldPos_[0] = 1000; testWorldPos_[1] = 50; testWorldPos_[2] = 1000;
            }

            // Draw overlays
            DrawOverlays(logger);
        }

        void DrawOverlays(TransformLogger& logger) {
            ImDrawList* dl = ImGui::GetBackgroundDrawList();

            // Single marker
            if (drawTestMarker_) {
                ScreenPoint pt = logger.WorldToScreen(testWorldPos_[0], testWorldPos_[1], testWorldPos_[2]);
                if (pt.visible) {
                    ImVec2 p(pt.x, pt.y);
                    dl->AddCircleFilled(p, 8.0f, IM_COL32(255, 255, 0, 255));
                    dl->AddLine(ImVec2(p.x - 15, p.y), ImVec2(p.x + 15, p.y), IM_COL32(255, 255, 0, 200), 1.0f);
                    dl->AddLine(ImVec2(p.x, p.y - 15), ImVec2(p.x, p.y + 15), IM_COL32(255, 255, 0, 200), 1.0f);

                    char label[64];
                    snprintf(label, sizeof(label), "(%.0f,%.0f,%.0f)",
                        testWorldPos_[0], testWorldPos_[1], testWorldPos_[2]);
                    dl->AddText(ImVec2(p.x + 12, p.y - 8), IM_COL32(255, 255, 255, 255), label);
                }
            }

            // Grid
            if (drawGrid_) {
                int half = gridSize_ / 2;
                for (int gx = -half; gx <= half; ++gx) {
                    for (int gz = -half; gz <= half; ++gz) {
                        float wx = testWorldPos_[0] + gx * gridSpacing_;
                        float wy = testWorldPos_[1];
                        float wz = testWorldPos_[2] + gz * gridSpacing_;

                        ScreenPoint gpt = logger.WorldToScreen(wx, wy, wz);
                        if (gpt.visible) {
                            ImU32 color;
                            if (gx == 0 && gz == 0) {
                                color = IM_COL32(255, 0, 0, 255);
                            }
                            else if (gx == 0) {
                                color = IM_COL32(0, 255, 0, 200);
                            }
                            else if (gz == 0) {
                                color = IM_COL32(0, 0, 255, 200);
                            }
                            else {
                                color = IM_COL32(128, 128, 128, 150);
                            }

                            float size = (gx == 0 && gz == 0) ? 6.0f : 4.0f;
                            dl->AddCircleFilled(ImVec2(gpt.x, gpt.y), size, color);
                        }
                    }
                }
            }
        }

        void RenderMatrixDetails(const D3DMATRIX& m, const TransformAnalysis& analysis) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
            ImGui::Text("[%9.4f %9.4f %9.4f %9.4f]", m._11, m._12, m._13, m._14);
            ImGui::Text("[%9.4f %9.4f %9.4f %9.4f]", m._21, m._22, m._23, m._24);
            ImGui::Text("[%9.4f %9.4f %9.4f %9.4f]", m._31, m._32, m._33, m._34);
            ImGui::Text("[%9.4f %9.4f %9.4f %9.4f]", m._41, m._42, m._43, m._44);
            ImGui::PopStyleColor();

            if (analysis.isPerspective) {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                    "FOV=%.1f° Near=%.1f Far=%.1f",
                    analysis.estimatedFovDegrees, analysis.estimatedNear, analysis.estimatedFar);
            }
        }

        static const char* GetStateName(D3DTRANSFORMSTATETYPE state) {
            switch (state) {
            case D3DTRANSFORMSTATE_WORLD: return "WORLD";
            case D3DTRANSFORMSTATE_VIEW: return "VIEW";
            case D3DTRANSFORMSTATE_PROJECTION: return "PROJ";
            default: return "???";
            }
        }

        static ImVec4 GetStateColor(D3DTRANSFORMSTATETYPE state, const TransformAnalysis& analysis) {
            if (analysis.isPerspective) return ImVec4(0.5f, 1.0f, 0.5f, 1.0f);
            if (analysis.isOrthographic) return ImVec4(0.7f, 0.7f, 1.0f, 1.0f);
            if (analysis.isIdentity) return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);

            switch (state) {
            case D3DTRANSFORMSTATE_VIEW: return ImVec4(0.3f, 0.8f, 1.0f, 1.0f);
            case D3DTRANSFORMSTATE_PROJECTION: return ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
            default: return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
            }
        }

    private:
        cIGZImGuiService* service_ = nullptr;
        bool loggerInstalled_ = false;

        // UI state
        bool pauseCapture_ = false;
        bool showWorldTransforms_ = false;
        bool showIdentityMatrices_ = false;
        bool showMatrixValues_ = true;
        bool filterPerspectiveOnly_ = false;
        bool drawTestMarker_ = true;
        bool drawGrid_ = false;

        float testWorldPos_[3] = {500.0f, 0.0f, 500.0f};
        float gridSpacing_ = 100.0f;
        int gridSize_ = 5;

        std::vector<TransformLogEntry> snapshotLog_;
    };
}

class TransformDebugDirector final : public cRZCOMDllDirector {
public:
    TransformDebugDirector()
        : service_(nullptr)
        , panelRegistered_(false) {}

    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kTransformDebugDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZCOMDllDirector::OnStart(pCOM);
        Logger::Initialize("SC4TransformDebug", "");
        LOG_INFO("TransformDebug: OnStart");

        if (mpFrameWork) {
            mpFrameWork->AddHook(this);
            LOG_INFO("TransformDebug: framework hook added");
        }
        return true;
    }

    bool PostAppInit() override {
        LOG_INFO("TransformDebug: PostAppInit");

        if (!mpFrameWork || panelRegistered_) {
            return true;
        }

        if (!mpFrameWork->GetSystemService(kImGuiServiceID, GZIID_cIGZImGuiService,
            reinterpret_cast<void**>(&service_))) {
            LOG_WARN("TransformDebug: ImGui service not available");
            return true;
        }

        LOG_INFO("TransformDebug: got ImGui service (api={})", service_->GetApiVersion());

        auto* panel = new TransformDebugPanel(service_);
        auto desc = ImGuiPanelAdapter<TransformDebugPanel>::MakeDesc(
            panel,
            kTransformDebugPanelId,
            9000,  // High order to render on top
            true   // Visible by default
        );

        if (!service_->RegisterPanel(desc)) {
            LOG_WARN("TransformDebug: failed to register panel");
            delete panel;
            service_->Release();
            service_ = nullptr;
            return true;
        }

        LOG_INFO("TransformDebug: panel registered");
        panelRegistered_ = true;
        return true;
    }

    bool PostAppShutdown() override {
        if (service_) {
            service_->UnregisterPanel(kTransformDebugPanelId);
            service_->Release();
            service_ = nullptr;
        }
        panelRegistered_ = false;
        return true;
    }

private:
    cIGZImGuiService* service_;
    bool panelRegistered_;
};

static TransformDebugDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static bool sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}