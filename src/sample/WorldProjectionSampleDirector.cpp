// ReSharper disable CppDFAConstantConditions
// ReSharper disable CppDFAConstantFunctionResult
// ReSharper disable CppDFAUnreachableCode
#include "cIGZFrameWork.h"
#include "cISC4App.h"
#include "cISC4City.h"
#include "cISC43DRender.h"
#include "cISC4View3DWin.h"
#include "cRZCOMDllDirector.h"
#include "cISTETerrain.h"
#include "cS3DCamera.h"
#include "GZServPtrs.h"
#include "imgui.h"
#include "public/ImGuiTexture.h"
#include "public/cIGZImGuiService.h"
#include "public/ImGuiServiceIds.h"
#include "SC4UI.h"
#include "utils/Logger.h"
#include <windows.h>
#include <gdiplus.h>
#include <cmath>
#include <vector>

namespace {
    constexpr auto kWorldProjectionSampleDirectorID = 0xB7E4F2A9;
    constexpr uint32_t kWorldProjectionPanelId = 0x3D9C8B1F;
    constexpr const wchar_t* kBillboardImagePath =
        L"C:\\Users\\caspe\\CLionProjects\\sc4-imgui-service\\assets\\nam49.jpg";
    bool gGdiplusStarted = false;
    ULONG_PTR gGdiplusToken = 0;

    // Function pointer type for SC4's camera Project function
    typedef bool (__thiscall *ProjectFunc)(void* camera, float* worldPos, float* screenPos);
    const auto ProjectFn = reinterpret_cast<ProjectFunc>(0x007fff10);

    struct GridConfig {
        bool enabled = true;
        int gridSpacing = 64;    // World units between grid lines
        int gridExtent = 512;    // How far from center to draw
        float centerX = 512.0f;       // Center world position X
        float centerY = 281.0f;         // Center world position Y (height)
        float centerZ = 512.0f;       // Center world position Z
        ImVec4 gridColor = ImVec4(0.0f, 1.0f, 0.0f, 0.8f);  // RGBA
        float lineThickness = 2.0f;
        bool drawCenterMarker = true;
        float markerSize = 10.0f;
        bool conformToTerrain = false;
        bool terrainSnapToGrid = true;
        int terrainSampleStep = 16;
        bool drawText = true;
        bool textBillboard = true;
        float textDepthScale = 0.002f;
        float textOffsetX = 0.0f;
        float textOffsetY = -28.0f;
        bool textLeaderLine = true;
        bool textBackground = true;
        bool textOutline = true;
        bool textShadow = true;
        ImVec4 textColor = ImVec4(1.0f, 0.92f, 0.2f, 1.0f);
        char text[64] = "World label";
        bool drawImage = true;
        bool imageBillboard = true;
        float imageSize = 64.0f;
        float imageOffsetX = 0.0f;
        float imageOffsetY = 0.0f;
        ImGuiTexture imageTexture;
        std::vector<uint8_t> imagePixels;
        uint32_t imageWidth = 0;
        uint32_t imageHeight = 0;
        bool imageLoaded = false;
        cIGZImGuiService* imguiService = nullptr;
    };

    bool IsCityView() {
        const cISC4AppPtr app;
        if (!app) {
            return false;
        }
        return app->GetCity() != nullptr;
    }

    bool WorldToScreen(cS3DCamera* camera, float worldX, float worldY, float worldZ,
                       float& screenX, float& screenY, float* depth = nullptr) {
        if (!camera) {
            return false;
        }

        float worldPos[3] = {worldX, worldY, worldZ};
        float screenPos[3] = {0.0f, 0.0f, 0.0f};

        if (ProjectFn(camera, worldPos, screenPos)) {
            screenX = screenPos[0];
            screenY = screenPos[1];
            if (depth) {
                *depth = screenPos[2];
            }
            return true;
        }

        return false;
    }

    float ClampFloat(float value, float minValue, float maxValue) {
        if (value < minValue) {
            return minValue;
        }
        if (value > maxValue) {
            return maxValue;
        }
        return value;
    }

    bool StartGdiplus() {
        if (gGdiplusStarted) {
            return true;
        }
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&gGdiplusToken, &input, nullptr) != Gdiplus::Ok) {
            gGdiplusToken = 0;
            return false;
        }
        gGdiplusStarted = true;
        return true;
    }

    void StopGdiplus() {
        if (!gGdiplusStarted) {
            return;
        }
        Gdiplus::GdiplusShutdown(gGdiplusToken);
        gGdiplusToken = 0;
        gGdiplusStarted = false;
    }

    bool LoadJpegToBgra(const wchar_t* path, std::vector<uint8_t>& outPixels,
                        uint32_t& outWidth, uint32_t& outHeight) {
        if (!gGdiplusStarted) {
            return false;
        }

        Gdiplus::Bitmap bitmap(path);
        if (bitmap.GetLastStatus() != Gdiplus::Ok) {
            return false;
        }

        outWidth = bitmap.GetWidth();
        outHeight = bitmap.GetHeight();
        if (outWidth == 0 || outHeight == 0) {
            return false;
        }

        Gdiplus::Rect rect(0, 0, static_cast<INT>(outWidth), static_cast<INT>(outHeight));
        Gdiplus::BitmapData data{};
        if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok) {
            return false;
        }

        outPixels.resize(static_cast<size_t>(outWidth) * outHeight * 4);
        for (uint32_t y = 0; y < outHeight; ++y) {
            const uint8_t* srcRow = static_cast<const uint8_t*>(data.Scan0) + y * data.Stride;
            uint8_t* dstRow = outPixels.data() + (static_cast<size_t>(y) * outWidth * 4);
            for (uint32_t x = 0; x < outWidth; ++x) {
                const uint8_t b = srcRow[x * 4 + 0];
                const uint8_t g = srcRow[x * 4 + 1];
                const uint8_t r = srcRow[x * 4 + 2];
                const uint8_t a = srcRow[x * 4 + 3];
                dstRow[x * 4 + 0] = b;
                dstRow[x * 4 + 1] = g;
                dstRow[x * 4 + 2] = r;
                dstRow[x * 4 + 3] = a;
            }
        }

        bitmap.UnlockBits(&data);
        return true;
    }

    void DrawWorldGrid(cS3DCamera* camera, cISTETerrain* terrain, const GridConfig& config) {
        if (!camera || !config.enabled) {
            return;
        }

        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        const ImU32 color = ImGui::ColorConvertFloat4ToU32(config.gridColor);
        const int stepValue = config.terrainSampleStep > 0 ? config.terrainSampleStep : 16;
        const float sampleStep = static_cast<float>(stepValue);

        // Draw grid lines parallel to X axis (running along X direction)
        for (float z = -config.gridExtent; z <= config.gridExtent; z += config.gridSpacing) {
            float worldZ = config.centerZ + z;

            // Start and end points along X axis
            float startX = config.centerX - config.gridExtent;
            float endX = config.centerX + config.gridExtent;

            if (!config.conformToTerrain || !terrain) {
                float screenX1, screenY1, screenX2, screenY2;
                bool visible1 = WorldToScreen(camera, startX, config.centerY, worldZ, screenX1, screenY1);
                bool visible2 = WorldToScreen(camera, endX, config.centerY, worldZ, screenX2, screenY2);

                if (visible1 && visible2) {
                    drawList->AddLine(
                        ImVec2(screenX1, screenY1),
                        ImVec2(screenX2, screenY2),
                        color,
                        config.lineThickness
                    );
                }
                continue;
            }

            bool hasPrev = false;
            ImVec2 prevPos{};
            for (float x = startX; x <= endX; x += sampleStep) {
                if (!terrain->LocationIsInBounds(x, worldZ)) {
                    hasPrev = false;
                    continue;
                }

                const float y = config.terrainSnapToGrid
                                    ? terrain->GetAltitudeAtNearestGrid(x, worldZ)
                                    : terrain->GetAltitude(x, worldZ);
                float sx, sy;
                if (!WorldToScreen(camera, x, y, worldZ, sx, sy)) {
                    hasPrev = false;
                    continue;
                }

                const ImVec2 curPos(sx, sy);
                if (hasPrev) {
                    drawList->AddLine(prevPos, curPos, color, config.lineThickness);
                }
                prevPos = curPos;
                hasPrev = true;
            }
        }

        // Draw grid lines parallel to Z axis (running along Z direction)
        for (float x = -config.gridExtent; x <= config.gridExtent; x += config.gridSpacing) {
            float worldX = config.centerX + x;

            // Start and end points along Z axis
            float startZ = config.centerZ - config.gridExtent;
            float endZ = config.centerZ + config.gridExtent;

            if (!config.conformToTerrain || !terrain) {
                float screenX1, screenY1, screenX2, screenY2;
                bool visible1 = WorldToScreen(camera, worldX, config.centerY, startZ, screenX1, screenY1);
                bool visible2 = WorldToScreen(camera, worldX, config.centerY, endZ, screenX2, screenY2);

                if (visible1 && visible2) {
                    drawList->AddLine(
                        ImVec2(screenX1, screenY1),
                        ImVec2(screenX2, screenY2),
                        color,
                        config.lineThickness
                    );
                }
                continue;
            }

            bool hasPrev = false;
            ImVec2 prevPos{};
            for (float z = startZ; z <= endZ; z += sampleStep) {
                if (!terrain->LocationIsInBounds(worldX, z)) {
                    hasPrev = false;
                    continue;
                }

                const float y = config.terrainSnapToGrid
                                    ? terrain->GetAltitudeAtNearestGrid(worldX, z)
                                    : terrain->GetAltitude(worldX, z);
                float sx, sy;
                if (!WorldToScreen(camera, worldX, y, z, sx, sy)) {
                    hasPrev = false;
                    continue;
                }

                const ImVec2 curPos(sx, sy);
                if (hasPrev) {
                    drawList->AddLine(prevPos, curPos, color, config.lineThickness);
                }
                prevPos = curPos;
                hasPrev = true;
            }
        }

        // Draw center marker
        if (config.drawCenterMarker) {
            float screenX, screenY;
            if (WorldToScreen(camera, config.centerX, config.centerY, config.centerZ, screenX, screenY)) {
                // Draw crosshair at center
                ImU32 markerColor = IM_COL32(255, 0, 0, 255);  // Red marker
                drawList->AddCircleFilled(
                    ImVec2(screenX, screenY),
                    config.markerSize * 0.5f,
                    markerColor
                );

                // Draw crosshair lines
                drawList->AddLine(
                    ImVec2(screenX - config.markerSize, screenY),
                    ImVec2(screenX + config.markerSize, screenY),
                    markerColor,
                    config.lineThickness
                );
                drawList->AddLine(
                    ImVec2(screenX, screenY - config.markerSize),
                    ImVec2(screenX, screenY + config.markerSize),
                    markerColor,
                    config.lineThickness
                );
            }
        }
    }

    void DrawWorldText(cS3DCamera* camera, const GridConfig& config) {
        if (!camera || !config.drawText) {
            return;
        }

        float screenX = 0.0f;
        float screenY = 0.0f;
        float depth = 0.0f;
        if (!WorldToScreen(camera, config.centerX, config.centerY, config.centerZ, screenX, screenY, &depth)) {
            return;
        }

        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        float alpha = config.textColor.w;
        if (!config.textBillboard) {
            const float fade = 1.0f / (1.0f + depth * config.textDepthScale);
            alpha *= ClampFloat(fade, 0.2f, 1.0f);
        }

        ImVec4 textColorF = config.textColor;
        textColorF.w = ClampFloat(alpha, 0.0f, 1.0f);
        const ImU32 textColor = ImGui::ColorConvertFloat4ToU32(textColorF);
        const ImU32 outlineColor = IM_COL32(0, 0, 0, 210);
        const ImU32 shadowColor = IM_COL32(0, 0, 0, 160);
        const ImU32 leaderColor = IM_COL32(0, 0, 0, 180);

        const char* label = config.text;
        const ImVec2 labelSize = ImGui::CalcTextSize(label);

        const ImVec2 anchor(screenX, screenY);
        ImVec2 textPos = ImVec2(
            screenX + config.textOffsetX - labelSize.x * 0.5f,
            screenY + config.textOffsetY - labelSize.y
        );

        if (config.textLeaderLine && (config.textOffsetX != 0.0f || config.textOffsetY != 0.0f)) {
            drawList->AddLine(anchor,
                              ImVec2(textPos.x + labelSize.x * 0.5f, textPos.y + labelSize.y),
                              leaderColor,
                              1.5f);
        }

        if (config.textBackground) {
            const ImVec2 pad(4.0f, 2.0f);
            drawList->AddRectFilled(
                ImVec2(textPos.x - pad.x, textPos.y - pad.y),
                ImVec2(textPos.x + labelSize.x + pad.x, textPos.y + labelSize.y + pad.y),
                IM_COL32(0, 0, 0, 140),
                4.0f
            );
        }

        if (config.textShadow) {
            drawList->AddText(ImVec2(textPos.x + 2.0f, textPos.y + 2.0f), shadowColor, label);
        }
        if (config.textOutline) {
            drawList->AddText(ImVec2(textPos.x + 1.0f, textPos.y), outlineColor, label);
            drawList->AddText(ImVec2(textPos.x - 1.0f, textPos.y), outlineColor, label);
            drawList->AddText(ImVec2(textPos.x, textPos.y + 1.0f), outlineColor, label);
            drawList->AddText(ImVec2(textPos.x, textPos.y - 1.0f), outlineColor, label);
        }

        drawList->AddText(textPos, textColor, label);
    }

    void DrawWorldImage(cS3DCamera* camera, cISTETerrain* terrain, GridConfig& config) {
        if (!camera || !config.drawImage || !config.imguiService) {
            return;
        }

        float screenX = 0.0f;
        float screenY = 0.0f;
        float depth = 0.0f;
        if (!WorldToScreen(camera, config.centerX, config.centerY, config.centerZ, screenX, screenY, &depth)) {
            return;
        }

        if (!config.imageLoaded) {
            config.imageLoaded = LoadJpegToBgra(kBillboardImagePath, config.imagePixels,
                                                config.imageWidth, config.imageHeight);
            if (config.imageLoaded) {
                config.imageTexture.Create(config.imguiService, config.imageWidth,
                                           config.imageHeight, config.imagePixels.data());
            }
        }

        void* texId = config.imageTexture.GetID();
        if (!texId && config.imageLoaded) {
            config.imageTexture.Create(config.imguiService, config.imageWidth,
                                       config.imageHeight, config.imagePixels.data());
            texId = config.imageTexture.GetID();
        }

        if (!texId) {
            return;
        }

        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        if (config.imageBillboard) {
            const float size = config.imageSize;
            const ImVec2 center(screenX + config.imageOffsetX, screenY + config.imageOffsetY);
            const ImVec2 half(size * 0.5f, size * 0.5f);
            const ImVec2 minPos(center.x - half.x, center.y - half.y);
            const ImVec2 maxPos(center.x + half.x, center.y + half.y);
            drawList->AddImage(texId, minPos, maxPos);
            return;
        }

        const float worldX = config.centerX + config.imageOffsetX;
        const float worldZ = config.centerZ + config.imageOffsetY;
        const float halfSize = config.imageSize * 0.5f;

        auto sampleHeight = [&](float x, float z) -> float {
            if (!config.conformToTerrain || !terrain || !terrain->LocationIsInBounds(x, z)) {
                return config.centerY;
            }
            return config.terrainSnapToGrid ? terrain->GetAltitudeAtNearestGrid(x, z)
                                            : terrain->GetAltitude(x, z);
        };

        const float y1 = sampleHeight(worldX - halfSize, worldZ - halfSize);
        const float y2 = sampleHeight(worldX + halfSize, worldZ - halfSize);
        const float y3 = sampleHeight(worldX + halfSize, worldZ + halfSize);
        const float y4 = sampleHeight(worldX - halfSize, worldZ + halfSize);

        float p1x, p1y, p2x, p2y, p3x, p3y, p4x, p4y;
        if (!WorldToScreen(camera, worldX - halfSize, y1, worldZ - halfSize, p1x, p1y) ||
            !WorldToScreen(camera, worldX + halfSize, y2, worldZ - halfSize, p2x, p2y) ||
            !WorldToScreen(camera, worldX + halfSize, y3, worldZ + halfSize, p3x, p3y) ||
            !WorldToScreen(camera, worldX - halfSize, y4, worldZ + halfSize, p4x, p4y)) {
            return;
        }

        drawList->AddImageQuad(
            texId,
            ImVec2(p1x, p1y),
            ImVec2(p2x, p2y),
            ImVec2(p3x, p3y),
            ImVec2(p4x, p4y)
        );
    }

    void RenderWorldProjectionPanel(void* userData) {
        if (!IsCityView()) {
            return;
        }

        auto* config = static_cast<GridConfig*>(userData);
        if (!config) {
            return;
        }

        // Get camera for rendering
        auto view3DWin = SC4UI::GetView3DWin();
        if (view3DWin) {
            cISC43DRender* renderer = view3DWin->GetRenderer();
            if (renderer) {
                cS3DCamera* camera = renderer->GetCamera();
                if (camera) {
                    cISC4AppPtr app;
                    cISTETerrain* terrain = nullptr;
                    if (app) {
                        cISC4City* city = app->GetCity();
                        if (city) {
                            terrain = city->GetTerrain();
                        }
                    }
                    DrawWorldGrid(camera, terrain, *config);
                    DrawWorldText(camera, *config);
                    DrawWorldImage(camera, terrain, *config);
                }
            }
        }

        // Control panel
        ImGui::Begin("World space", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Separator();

        ImGui::Checkbox("Enable grid", &config->enabled);

        if (config->enabled) {
            ImGui::Spacing();
            ImGui::Text("Grid Configuration");
            ImGui::Separator();

            ImGui::SliderInt("Spacing", &config->gridSpacing, 8, 256);
            ImGui::SliderInt("Extent", &config->gridExtent, 64, 2048);
            ImGui::SliderFloat("Line thickness", &config->lineThickness, 1.0f, 5.0f, "%.1f");

            ImGui::Spacing();
            ImGui::Text("Grid center");
            ImGui::Separator();

            ImGui::DragFloat("Center X", &config->centerX, 1.0f, 0.0f, 4096.0f, "%.1f");
            ImGui::DragFloat("Center Y (Height)", &config->centerY, 0.5f, -100.0f, 500.0f, "%.1f");
            ImGui::DragFloat("Center Z", &config->centerZ, 1.0f, 0.0f, 4096.0f, "%.1f");

            ImGui::Spacing();
            ImGui::Text("Appearance");
            ImGui::Separator();

            ImGui::ColorEdit4("Color", reinterpret_cast<float*>(&config->gridColor));

            ImGui::Spacing();
            ImGui::Checkbox("Draw marker", &config->drawCenterMarker);
            if (config->drawCenterMarker) {
                ImGui::SliderFloat("Marker size", &config->markerSize, 5.0f, 30.0f, "%.1f");
            }

            ImGui::Spacing();
            ImGui::Text("Terrain conform");
            ImGui::Separator();
            ImGui::Checkbox("Conform to terrain", &config->conformToTerrain);
            if (config->conformToTerrain) {
                ImGui::Checkbox("Snap to grid", &config->terrainSnapToGrid);
                ImGui::SliderInt("Sample step (m)", &config->terrainSampleStep, 4, 64);
            }

            ImGui::Spacing();
            ImGui::Text("World text");
            ImGui::Separator();

            ImGui::Checkbox("Draw text", &config->drawText);
            if (config->drawText) {
                ImGui::InputText("Text", config->text, IM_ARRAYSIZE(config->text));
                ImGui::Checkbox("Billboard", &config->textBillboard);
                if (!config->textBillboard) {
                    ImGui::SliderFloat("Depth scale", &config->textDepthScale, 0.0005f, 0.01f, "%.4f");
                }
                ImGui::DragFloat2("Text offset", &config->textOffsetX, 1.0f, -200.0f, 200.0f, "%.1f");
                ImGui::ColorEdit4("Text color", reinterpret_cast<float*>(&config->textColor));
                ImGui::Checkbox("Leader line", &config->textLeaderLine);
                ImGui::Checkbox("Background plate", &config->textBackground);
                ImGui::Checkbox("Outline", &config->textOutline);
                ImGui::Checkbox("Shadow", &config->textShadow);
            }

            ImGui::Spacing();
            ImGui::Text("Billboard image");
            ImGui::Separator();

            ImGui::Checkbox("Draw image", &config->drawImage);
            if (config->drawImage) {
                ImGui::Text("Image: nam49.jpg");
                ImGui::Text("Mode");
                if (ImGui::RadioButton("Billboard (pixels)", config->imageBillboard)) {
                    config->imageBillboard = true;
                }
                if (ImGui::RadioButton("Planar (world units)", !config->imageBillboard)) {
                    config->imageBillboard = false;
                }
                ImGui::SliderFloat("Image size", &config->imageSize, 16.0f, 256.0f, "%.1f");
                if (config->imageBillboard) {
                    ImGui::DragFloat2("Image offset (px)", &config->imageOffsetX, 1.0f, -200.0f, 200.0f, "%.1f");
                }
                else {
                    ImGui::DragFloat2("Image offset (world X/Z)", &config->imageOffsetX, 1.0f, -512.0f, 512.0f, "%.1f");
                }
            }
        }

        ImGui::End();
    }

    void ShutdownWorldProjection(void* userData) {
        auto* config = static_cast<GridConfig*>(userData);
        if (config) {
            config->imageTexture.Release();
        }
        delete config;
    }
}

class WorldProjectionSampleDirector final : public cRZCOMDllDirector {
public:
    WorldProjectionSampleDirector()
        : service(nullptr),
          panelRegistered(false) {
    }

    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kWorldProjectionSampleDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZCOMDllDirector::OnStart(pCOM);
        Logger::Initialize("SC4WorldProjectionSample", "");
        LOG_INFO("WorldProjectionSample: OnStart");

        if (mpFrameWork) {
            mpFrameWork->AddHook(this);
            LOG_INFO("WorldProjectionSample: framework hook added");
        }
        else {
            LOG_WARN("WorldProjectionSample: mpFrameWork not available on start");
        }

        return true;
    }

    bool PostAppInit() override {
        LOG_INFO("WorldProjectionSample: PostAppInit");

        if (!mpFrameWork || panelRegistered) {
            return true;
        }

        if (!mpFrameWork->GetSystemService(kImGuiServiceID, GZIID_cIGZImGuiService,
                                           reinterpret_cast<void**>(&service))) {
            LOG_WARN("WorldProjectionSample: ImGui service not available");
            return true;
        }

        LOG_INFO("WorldProjectionSample: obtained ImGui service (api={})", service->GetApiVersion());

        auto* config = new GridConfig();
        config->imguiService = service;
        ImGuiPanelDesc desc{};
        desc.id = kWorldProjectionPanelId;
        desc.order = 200;  // Render after other panels
        desc.visible = true;
        desc.on_render = &RenderWorldProjectionPanel;
        desc.on_shutdown = &ShutdownWorldProjection;
        desc.data = config;

        if (!service->RegisterPanel(desc)) {
            LOG_WARN("WorldProjectionSample: failed to register panel");
            ShutdownWorldProjection(config);
            service->Release();
            service = nullptr;
            return true;
        }

        LOG_INFO("WorldProjectionSample: registered panel {}", kWorldProjectionPanelId);
        panelRegistered = true;
        if (!StartGdiplus()) {
            LOG_WARN("WorldProjectionSample: failed to start GDI+");
        }
        return true;
    }

    bool PostAppShutdown() override {
        if (service) {
            service->UnregisterPanel(kWorldProjectionPanelId);
            service->Release();
            service = nullptr;
        }
        StopGdiplus();
        panelRegistered = false;
        return true;
    }

private:
    cIGZImGuiService* service;
    bool panelRegistered;
};

static WorldProjectionSampleDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static auto sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
