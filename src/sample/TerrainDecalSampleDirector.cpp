#include "cIGZFrameWork.h"
#include "cRZCOMDllDirector.h"

#include "imgui.h"
#include "public/ImGuiPanelAdapter.h"
#include "public/ImGuiServiceIds.h"
#include "public/cIGZImGuiService.h"
#include "public/cIGZTerrainDecalService.h"
#include "public/TerrainDecalServiceIds.h"
#include "cISTETerrainView.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    constexpr auto kTerrainDecalSampleDirectorID = 0x7A84B3D2;
    constexpr uint32_t kTerrainDecalSamplePanelId = 0x7A84B3D3;
    constexpr uint32_t kDefaultTextureType = 0x7AB50E44;
    constexpr uint32_t kDefaultTextureGroup = 0x1ABE787D;
    constexpr uint32_t kDefaultTextureInstance = 0xAA40173A;
    static float SnapToTileCenter(const float value)
    {
        return std::floor(value / 16.0f) * 16.0f + 8.0f;
    }

    static TerrainDecalState MakeDefaultState()
    {
        TerrainDecalState state{};
        state.textureKey = cGZPersistResourceKey{kDefaultTextureType, kDefaultTextureGroup, kDefaultTextureInstance};
        state.overlayType = cISTETerrainView::tOverlayManagerType::DynamicLand;
        state.decalInfo.center = cS3DVector2{512.0f, 512.0f};
        state.decalInfo.baseSize = 16.0f;
        state.decalInfo.rotationTurns = 0.0f;
        state.decalInfo.aspectMultiplier = 1.0f;
        state.decalInfo.uvScaleU = 1.0f;
        state.decalInfo.uvScaleV = 1.0f;
        state.decalInfo.uvOffset = 0.0f;
        state.decalInfo.unknown8 = 0.0f;
        state.opacity = 1.0f;
        state.enabled = true;
        state.color = cS3DVector3(1.0f, 1.0f, 1.0f);
        state.drawMode = 0;
        state.flags = 0;
        state.hasUvWindow = false;
        state.uvWindow = TerrainDecalUvWindow{};
        return state;
    }

    static void CopySnapshotToState(const TerrainDecalSnapshot& snapshot, TerrainDecalState& state)
    {
        state = snapshot.state;
    }

    static char* FormatTextureKey(const cGZPersistResourceKey& key, char* buffer, const size_t bufferSize)
    {
        std::snprintf(buffer, bufferSize, "%08X:%08X:%08X", key.type, key.group, key.instance);
        return buffer;
    }

    static const char* OverlayTypeLabel(cISTETerrainView::tOverlayManagerType type)
    {
        switch (type) {
        case cISTETerrainView::tOverlayManagerType::StaticLand:
            return "Static Land";
        case cISTETerrainView::tOverlayManagerType::StaticWater:
            return "Static Water";
        case cISTETerrainView::tOverlayManagerType::DynamicLand:
            return "Dynamic Land";
        case cISTETerrainView::tOverlayManagerType::DynamicWater:
            return "Dynamic Water";
        default:
            return "Unknown";
        }
    }

    static char* FormatDecalSummary(const TerrainDecalSnapshot& snapshot, char* buffer, const size_t bufferSize)
    {
        char textureBuffer[64]{};
        FormatTextureKey(snapshot.state.textureKey, textureBuffer, sizeof(textureBuffer));
        std::snprintf(
            buffer,
            bufferSize,
            "#%u | %s | tex %s | pos %.1f, %.1f | size %.2f | rot %.3f | op %.2f | mode %u | flags %08X | %s%s",
            snapshot.id.value,
            OverlayTypeLabel(snapshot.state.overlayType),
            textureBuffer,
            snapshot.state.decalInfo.center.fX,
            snapshot.state.decalInfo.center.fY,
            snapshot.state.decalInfo.baseSize,
            snapshot.state.decalInfo.rotationTurns,
            snapshot.state.opacity,
            static_cast<unsigned>(snapshot.state.drawMode),
            snapshot.state.flags,
            snapshot.state.enabled ? "enabled" : "disabled",
            snapshot.state.hasUvWindow ? " | uv" : "");
        return buffer;
    }

    static const char* UvModeLabel(const TerrainDecalUvMode mode)
    {
        switch (mode) {
        case TerrainDecalUvMode::StretchSubrect:
            return "Stretch";
        case TerrainDecalUvMode::ClipSubrect:
            return "Clip";
        default:
            return "Unknown";
        }
    }

    class TerrainDecalPanel final : public ImGuiPanel
    {
    public:
        explicit TerrainDecalPanel(cIGZTerrainDecalService* service)
            : service_(service)
            , selectedId_{}
            , editor_(MakeDefaultState())
        {
        }

        void OnInit() override
        {
            LOG_INFO("TerrainDecalSample: panel initialized");
            RefreshList();
        }

        void OnRender() override
        {
            if (!service_) {
                ImGui::Begin("TerrainDecals Sample", nullptr, 0);
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), "TerrainDecal service unavailable.");
                ImGui::End();
                return;
            }

            if (autoRefresh_ && listDirty_) {
                RefreshList();
            }

            ImGui::Begin("TerrainDecals Sample", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::Text("Decal count: %u", service_->GetDecalCount());
            ImGui::Text("Selected ID: %u", selectedId_.value);

            ImGui::Checkbox("Auto refresh list", &autoRefresh_);
            ImGui::SameLine();
            if (ImGui::Button("Refresh")) {
                RefreshList();
            }

            ImGui::SameLine();
            if (ImGui::Button("Reset")) {
                editor_ = MakeDefaultState();
                SetStatus("Editor reset");
            }

            ImGui::SeparatorText("Decals");
            RenderDecalList_();

            ImGui::SeparatorText("Editor");
            RenderEditor_();

            ImGui::SeparatorText("Actions");
            RenderActions_();

            ImGui::Separator();
            ImGui::TextWrapped("%s", status_);
            ImGui::End();
        }

        void OnShutdown() override
        {
            LOG_INFO("TerrainDecalSample: panel shutdown");
            delete this;
        }

        void OnUnregister() override
        {
            LOG_INFO("TerrainDecalSample: panel unregistered");
        }

    private:
        void RefreshList()
        {
            listDirty_ = false;
            decals_.clear();

            const uint32_t count = service_->GetDecalCount();
            if (count == 0) {
                return;
            }

            decals_.resize(count);
            const uint32_t copied = service_->CopyDecals(decals_.data(), count);
            decals_.resize(copied);

            if (selectedId_.value != 0) {
                const auto it = std::find_if(decals_.begin(), decals_.end(), [&](const TerrainDecalSnapshot& snapshot) {
                    return snapshot.id.value == selectedId_.value;
                });
                if (it == decals_.end()) {
                    selectedId_ = {};
                }
            }
        }

        void MarkListDirty()
        {
            listDirty_ = true;
        }

        void SetStatus(const char* text)
        {
            std::snprintf(status_, sizeof(status_), "%s", text ? text : "");
            LOG_INFO("TerrainDecalSample: {}", status_);
        }

        bool LoadSelectedIntoEditor()
        {
            if (selectedId_.value == 0) {
                SetStatus("No decal selected");
                return false;
            }

            TerrainDecalSnapshot snapshot{};
            if (!service_->GetDecal(selectedId_, &snapshot)) {
                SetStatus("Failed to load selected decal");
                return false;
            }

            CopySnapshotToState(snapshot, editor_);
            SetStatus("Selected decal loaded into editor");
            return true;
        }

        bool CreateFromEditor()
        {
            TerrainDecalId newId{};
            if (!service_->CreateDecal(editor_, &newId)) {
                SetStatus("CreateDecal failed");
                return false;
            }

            selectedId_ = newId;
            MarkListDirty();
            RefreshList();
            SetStatus("Decal created");
            return true;
        }

        bool ReplaceSelected()
        {
            if (selectedId_.value == 0) {
                SetStatus("No decal selected");
                return false;
            }

            if (!service_->ReplaceDecal(selectedId_, editor_)) {
                SetStatus("ReplaceDecal failed");
                return false;
            }

            MarkListDirty();
            RefreshList();
            SetStatus("Decal updated");
            return true;
        }

        bool RemoveSelected()
        {
            if (selectedId_.value == 0) {
                SetStatus("No decal selected");
                return false;
            }

            if (!service_->RemoveDecal(selectedId_)) {
                SetStatus("RemoveDecal failed");
                return false;
            }

            selectedId_ = {};
            MarkListDirty();
            RefreshList();
            SetStatus("Decal removed");
            return true;
        }

        void RenderDecalList_()
        {
            if (decals_.empty()) {
                ImGui::TextUnformatted("No decals in the current city.");
                return;
            }

            ImGui::BeginChild("##TerrainDecalList", ImVec2(0.0f, 220.0f), true);
            for (const auto& decal : decals_) {
                char label[256]{};
                FormatDecalSummary(decal, label, sizeof(label));

                const bool selected = selectedId_.value == decal.id.value;
                if (ImGui::Selectable(label, selected)) {
                    selectedId_ = decal.id;
                }

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    selectedId_ = decal.id;
                    LoadSelectedIntoEditor();
                }
            }
            ImGui::EndChild();
        }

        void RenderEditor_()
        {
            char textureBuffer[64]{};
            FormatTextureKey(editor_.textureKey, textureBuffer, sizeof(textureBuffer));
            ImGui::Text("Texture key");
            ImGui::Text("Type: %08X", editor_.textureKey.type);
            ImGui::InputScalar("Group##texture", ImGuiDataType_U32, &editor_.textureKey.group, nullptr, nullptr, "%08X");
            ImGui::InputScalar("Instance##texture", ImGuiDataType_U32, &editor_.textureKey.instance, nullptr, nullptr, "%08X");
            ImGui::TextDisabled("Current: %s", textureBuffer);

            ImGui::Separator();
            const char* overlayTypes[] = {"Static Land", "Static Water", "Dynamic Land", "Dynamic Water"};
            int overlayType = static_cast<int>(editor_.overlayType);
            if (ImGui::Combo("Overlay manager", &overlayType, overlayTypes, IM_ARRAYSIZE(overlayTypes))) {
                editor_.overlayType = static_cast<cISTETerrainView::tOverlayManagerType>(overlayType);
            }
            ImGui::InputFloat2("Center (x/z)", &editor_.decalInfo.center.fX);
            ImGui::SameLine();
            if (ImGui::Button("Snap##center")) {
                editor_.decalInfo.center.fX = SnapToTileCenter(editor_.decalInfo.center.fX);
                editor_.decalInfo.center.fY = SnapToTileCenter(editor_.decalInfo.center.fY);
            }

            ImGui::InputFloat("Opacity", &editor_.opacity, 0.01f, 0.1f, "%.2f");
            ImGui::Checkbox("Enabled", &editor_.enabled);
            ImGui::ColorEdit3("Color", &editor_.color.fX);
            ImGui::InputScalar("Draw mode", ImGuiDataType_U8, &editor_.drawMode, nullptr, nullptr, "%u");
            ImGui::InputScalar("Flags", ImGuiDataType_U32, &editor_.flags, nullptr, nullptr, "%08X");

            ImGui::InputFloat("Base size", &editor_.decalInfo.baseSize, 0.1f, 1.0f, "%.2f");
            ImGui::InputFloat("Rotation turns", &editor_.decalInfo.rotationTurns, 0.01f, 0.1f, "%.3f");
            ImGui::InputFloat("Aspect multiplier", &editor_.decalInfo.aspectMultiplier, 0.01f, 0.1f, "%.3f");
            ImGui::InputFloat("UV scale U", &editor_.decalInfo.uvScaleU, 0.01f, 0.1f, "%.3f");
            ImGui::InputFloat("UV scale V", &editor_.decalInfo.uvScaleV, 0.01f, 0.1f, "%.3f");
            ImGui::InputFloat("UV offset", &editor_.decalInfo.uvOffset, 0.01f, 0.1f, "%.3f");
            ImGui::InputFloat("Unknown8", &editor_.decalInfo.unknown8, 0.01f, 0.1f, "%.3f");

            ImGui::Separator();
            ImGui::Checkbox("Has UV window", &editor_.hasUvWindow);
            ImGui::BeginDisabled(!editor_.hasUvWindow);
            ImGui::InputFloat("u1", &editor_.uvWindow.u1, 0.01f, 0.1f, "%.3f");
            ImGui::InputFloat("v1", &editor_.uvWindow.v1, 0.01f, 0.1f, "%.3f");
            ImGui::InputFloat("u2", &editor_.uvWindow.u2, 0.01f, 0.1f, "%.3f");
            ImGui::InputFloat("v2", &editor_.uvWindow.v2, 0.01f, 0.1f, "%.3f");
            const char* modes[] = {"Stretch", "Clip"};
            int mode = static_cast<int>(editor_.uvWindow.mode);
            if (ImGui::Combo("Mode", &mode, modes, IM_ARRAYSIZE(modes))) {
                editor_.uvWindow.mode = static_cast<TerrainDecalUvMode>(mode);
            }
            ImGui::Text("Current: %s", UvModeLabel(editor_.uvWindow.mode));
            ImGui::EndDisabled();
            if (ImGui::Button("Clear UV Window")) {
                editor_.hasUvWindow = false;
                editor_.uvWindow = TerrainDecalUvWindow{};
            }
        }

        void RenderActions_()
        {
            if (ImGui::Button("Load")) {
                LoadSelectedIntoEditor();
            }

            ImGui::SameLine();
            if (ImGui::Button("Create")) {
                CreateFromEditor();
            }

            ImGui::SameLine();
            if (ImGui::Button("Apply")) {
                ReplaceSelected();
            }

            ImGui::SameLine();
            if (ImGui::Button("Remove")) {
                RemoveSelected();
            }

            ImGui::SameLine();
            if (ImGui::Button("Duplicate")) {
                if (LoadSelectedIntoEditor()) {
                    CreateFromEditor();
                }
            }
        }

    private:
        cIGZTerrainDecalService* service_ = nullptr;
        std::vector<TerrainDecalSnapshot> decals_{};
        TerrainDecalId selectedId_{};
        TerrainDecalState editor_{};
        bool autoRefresh_ = true;
        bool listDirty_ = true;
        char status_[256] = "Idle";
    };
}

class TerrainDecalSampleDirector final : public cRZCOMDllDirector
{
public:
    TerrainDecalSampleDirector()
        : imguiService_(nullptr)
        , terrainDecalService_(nullptr)
        , panelRegistered_(false)
    {
    }

    [[nodiscard]] uint32_t GetDirectorID() const override
    {
        return kTerrainDecalSampleDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override
    {
        cRZCOMDllDirector::OnStart(pCOM);
        Logger::Initialize("SC4TerrainDecalSample", "");
        LOG_INFO("TerrainDecalSample: OnStart");

        if (mpFrameWork) {
            mpFrameWork->AddHook(this);
        }
        return true;
    }

    bool PostAppInit() override
    {
        LOG_INFO("TerrainDecalSample: PostAppInit");

        if (!mpFrameWork || panelRegistered_) {
            return true;
        }

        if (!mpFrameWork->GetSystemService(kImGuiServiceID, GZIID_cIGZImGuiService,
                                           reinterpret_cast<void**>(&imguiService_))) {
            LOG_WARN("TerrainDecalSample: ImGui service not available");
            return true;
        }

        if (!mpFrameWork->GetSystemService(kTerrainDecalServiceID, GZIID_cIGZTerrainDecalService,
                                           reinterpret_cast<void**>(&terrainDecalService_))) {
            LOG_WARN("TerrainDecalSample: TerrainDecal service not available");
            imguiService_->Release();
            imguiService_ = nullptr;
            return true;
        }

        auto* panel = new TerrainDecalPanel(terrainDecalService_);
        ImGuiPanelDesc desc = ImGuiPanelAdapter<TerrainDecalPanel>::MakeDesc(panel, kTerrainDecalSamplePanelId, 150, true);

        if (!imguiService_->RegisterPanel(desc)) {
            LOG_WARN("TerrainDecalSample: failed to register panel");
            delete panel;
            terrainDecalService_->Release();
            terrainDecalService_ = nullptr;
            imguiService_->Release();
            imguiService_ = nullptr;
            return true;
        }

        panelRegistered_ = true;
        LOG_INFO("TerrainDecalSample: panel registered");
        return true;
    }

    bool PostAppShutdown() override
    {
        if (imguiService_) {
            imguiService_->UnregisterPanel(kTerrainDecalSamplePanelId);
            imguiService_->Release();
            imguiService_ = nullptr;
        }

        if (terrainDecalService_) {
            terrainDecalService_->Release();
            terrainDecalService_ = nullptr;
        }

        panelRegistered_ = false;
        return true;
    }

private:
    cIGZImGuiService* imguiService_;
    cIGZTerrainDecalService* terrainDecalService_;
    bool panelRegistered_;
};

static TerrainDecalSampleDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector()
{
    static bool sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
