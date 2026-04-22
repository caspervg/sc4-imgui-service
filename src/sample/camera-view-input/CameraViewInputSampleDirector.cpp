#include "cIGZFrameWork.h"
#include "cIGZMessage2Standard.h"
#include "cIGZMessageServer2.h"
#include "cRZMessage2COMDirector.h"

#include "imgui.h"
#include "public/ImGuiPanelAdapter.h"
#include "public/ImGuiServiceIds.h"
#include "public/S3DCameraServiceIds.h"
#include "public/cIGZImGuiService.h"
#include "public/cIGZS3DCameraService.h"
#include "sample/camera-view-input/CameraViewInputControl.hpp"
#include "sample/camera-view-input/SC4CameraControlLayout.hpp"
#include "utils/Logger.h"
#include "SC4UI.h"
#include "GZServPtrs.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace
{
    constexpr uint32_t kCameraViewInputDirectorID = 0xA2D16E7B;
    constexpr uint32_t kCameraViewInputPanelId = 0x7D5C0192;
    constexpr uint32_t kSC4MessagePreSave = 0x26C63343;

    CameraViewInputControl* gCameraInputTool = nullptr;
    cIGZS3DCameraService* gCameraService = nullptr;
    std::atomic<bool> gCameraInputEnabled{false};

    float gRotateDegPerPixel = 0.26f;
    float gPitchDegPerPixel = 0.20f;
    bool gInvertPitch = false;

    void DisableCameraInputTool();
    void SyncToolSettings();

    void ResetAllDefaults()
    {
        gRotateDegPerPixel = 0.26f;
        gPitchDegPerPixel = 0.20f;
        gInvertPitch = false;
        SyncToolSettings();
        SC4CameraControl::ResetToDefaults();
        SC4CameraControl::RequestViewRedraw();
    }

    float DegToRad(const float degrees)
    {
        return degrees * (3.14159265358979323846f / 180.0f);
    }

    void SyncToolSettings()
    {
        if (!gCameraInputTool) {
            return;
        }

        gCameraInputTool->SetRotateSensitivity(DegToRad(gRotateDegPerPixel));
        gCameraInputTool->SetPitchSensitivity(DegToRad(gPitchDegPerPixel));
        gCameraInputTool->SetInvertPitch(gInvertPitch);
    }

    int GetZoomMin(const SC4CameraControlLayout& cameraControl)
    {
        return static_cast<int>(std::floor(cameraControl.minZoom));
    }

    int GetZoomMax(const SC4CameraControlLayout& cameraControl)
    {
        return std::max(GetZoomMin(cameraControl), static_cast<int>(std::ceil(cameraControl.maxZoom)));
    }

    bool EnableCameraInputTool()
    {
        if (gCameraInputEnabled.load(std::memory_order_relaxed)) {
            return true;
        }

        auto view3D = SC4UI::GetView3DWin();
        if (!view3D) {
            LOG_WARN("CameraViewInput: View3D not available");
            return false;
        }

        if (!gCameraService) {
            LOG_WARN("CameraViewInput: camera service not available");
            return false;
        }

        if (!gCameraInputTool) {
            gCameraInputTool = new CameraViewInputControl();
            gCameraInputTool->AddRef();
            gCameraInputTool->SetCameraService(gCameraService);
            gCameraInputTool->SetOnCancel([]() { DisableCameraInputTool(); });
            gCameraInputTool->Activate();
        }

        SyncToolSettings();

        if (!view3D->SetCurrentViewInputControl(gCameraInputTool,
                                                cISC4View3DWin::ViewInputControlStackOperation_RemoveCurrentControl)) {
            LOG_WARN("CameraViewInput: failed to set current view input control");
            return false;
        }

        gCameraInputEnabled.store(true, std::memory_order_relaxed);
        return true;
    }

    void DisableCameraInputTool()
    {
        if (!gCameraInputEnabled.load(std::memory_order_relaxed)) {
            return;
        }

        auto view3D = SC4UI::GetView3DWin();
        if (view3D) {
            auto* currentControl = view3D->GetCurrentViewInputControl();
            if (currentControl == gCameraInputTool) {
                view3D->RemoveCurrentViewInputControl(false);
            }
        }

        gCameraInputEnabled.store(false, std::memory_order_relaxed);
    }

    void DestroyCameraInputTool()
    {
        DisableCameraInputTool();
        if (gCameraInputTool) {
            gCameraInputTool->Release();
            gCameraInputTool = nullptr;
        }
    }

    class CameraViewInputPanel final : public ImGuiPanel
    {
    public:
        void OnRender() override
        {
            if (SC4CameraControl::FlushQueuedYawPitch()) {
                SC4CameraControl::RequestViewRedraw();
            }

            ImGui::Begin("Camera View Input", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

            bool enabled = gCameraInputEnabled.load(std::memory_order_relaxed);
            if (ImGui::Checkbox("Enable Alt Camera Control", &enabled)) {
                if (enabled) {
                    enabled = EnableCameraInputTool();
                }
                else {
                    DisableCameraInputTool();
                }
                gCameraInputEnabled.store(enabled, std::memory_order_relaxed);
            }

            ImGui::Separator();
            ImGui::SliderFloat("Rotate Sensitivity (deg/px)", &gRotateDegPerPixel, 0.05f, 1.0f, "%.2f");
            ImGui::SliderFloat("Pitch Sensitivity (deg/px)", &gPitchDegPerPixel, 0.05f, 1.0f, "%.2f");
            ImGui::Checkbox("Invert Pitch", &gInvertPitch);

            if (ImGui::Button("Reset Defaults")) {
                ResetAllDefaults();
            }

            SyncToolSettings();

            ImGui::Separator();
            ImGui::TextUnformatted("Alt + RMB: pitch/yaw");
            ImGui::TextUnformatted("Alt + Wheel: magnification");

            ImGui::Separator();

            if (auto* cameraControl = SC4CameraControl::GetActiveCameraControl()) {
                const int zoomMin = GetZoomMin(*cameraControl);
                const int zoomMax = GetZoomMax(*cameraControl);

                int zoom = cameraControl->zoom;
                if (ImGui::SliderInt("Zoom", &zoom, zoomMin, zoomMax)) {
                    SC4CameraControl::SetZoom(zoom);
                }

                int rotation = cameraControl->rotation;
                if (ImGui::SliderInt("Rotation", &rotation, 0, 3)) {
                    SC4CameraControl::SetRotation(rotation);
                }

                float customMagnification = cameraControl->customMagnification;
                if (ImGui::DragFloat("Magnification", &customMagnification, 0.01f, 0.001f, 10.0f, "%.3f")) {
                    SC4CameraControl::SetCustomMagnification(customMagnification);
                }

                float yaw = cameraControl->yaw;
                if (ImGui::DragFloat("Yaw", &yaw, 0.01f, -100.0f, 100.0f, "%.4f")) {
                    if (SC4CameraControl::SetYawPitch(yaw, cameraControl->pitch)) {
                        SC4CameraControl::RequestViewRedraw();
                    }
                }

                float pitch = cameraControl->pitch;
                if (ImGui::DragFloat("Pitch", &pitch, 0.005f,
                                     SC4CameraControl::kPitchMinRadians,
                                     SC4CameraControl::kPitchMaxRadians,
                                     "%.4f")) {
                    if (SC4CameraControl::SetYawPitch(cameraControl->yaw, pitch)) {
                        SC4CameraControl::RequestViewRedraw();
                    }
                }
            }
            else {
                ImGui::TextUnformatted("Camera control unavailable.");
            }

            ImGui::End();
        }
    };
} // namespace

class CameraViewInputSampleDirector final : public cRZMessage2COMDirector
{
public:
    CameraViewInputSampleDirector()
        : imguiService_(nullptr)
        , cameraService_(nullptr)
        , subscribedToPreSave_(false)
        , panelRegistered_(false)
    {
    }

    [[nodiscard]] uint32_t GetDirectorID() const override
    {
        return kCameraViewInputDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override
    {
        cRZCOMDllDirector::OnStart(pCOM);
        Logger::Initialize("SC4CameraViewInputSample", "");
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

        cIGZMessageServer2Ptr messageServer;
        if (messageServer && messageServer->AddNotification(this, kSC4MessagePreSave)) {
            subscribedToPreSave_ = true;
        }
        else {
            LOG_WARN("CameraViewInput: failed to subscribe to kSC4MessagePreSave");
        }

        if (!mpFrameWork->GetSystemService(kImGuiServiceID,
                                           GZIID_cIGZImGuiService,
                                           reinterpret_cast<void**>(&imguiService_))) {
            LOG_WARN("CameraViewInput: ImGui service not available");
            return true;
        }

        if (!mpFrameWork->GetSystemService(kS3DCameraServiceID,
                                           GZIID_cIGZS3DCameraService,
                                           reinterpret_cast<void**>(&cameraService_))) {
            LOG_WARN("CameraViewInput: camera service not available");
            imguiService_->Release();
            imguiService_ = nullptr;
            return true;
        }

        gCameraService = cameraService_;

        auto* panel = new CameraViewInputPanel();
        const ImGuiPanelDesc desc = ImGuiPanelAdapter<CameraViewInputPanel>::MakeDesc(panel, kCameraViewInputPanelId, 140, true);

        if (!imguiService_->RegisterPanel(desc)) {
            delete panel;
            gCameraService = nullptr;
            cameraService_->Release();
            cameraService_ = nullptr;
            imguiService_->Release();
            imguiService_ = nullptr;
            return true;
        }

        panelRegistered_ = true;
        LOG_INFO("CameraViewInput: panel registered");
        return true;
    }

    bool DoMessage(cIGZMessage2* message) override
    {
        auto* standardMessage = static_cast<cIGZMessage2Standard*>(message);
        if (standardMessage && standardMessage->GetType() == kSC4MessagePreSave) {
            ResetAllDefaults();
        }

        return true;
    }

    bool PostAppShutdown() override
    {
        DestroyCameraInputTool();

        if (subscribedToPreSave_) {
            cIGZMessageServer2Ptr messageServer;
            if (messageServer) {
                messageServer->RemoveNotification(this, kSC4MessagePreSave);
            }
            subscribedToPreSave_ = false;
        }

        if (imguiService_) {
            imguiService_->UnregisterPanel(kCameraViewInputPanelId);
            imguiService_->Release();
            imguiService_ = nullptr;
        }

        if (cameraService_) {
            cameraService_->Release();
            cameraService_ = nullptr;
        }

        gCameraService = nullptr;
        panelRegistered_ = false;
        return true;
    }

private:
    cIGZImGuiService* imguiService_;
    cIGZS3DCameraService* cameraService_;
    bool subscribedToPreSave_;
    bool panelRegistered_;
};

static CameraViewInputSampleDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector()
{
    static bool sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
