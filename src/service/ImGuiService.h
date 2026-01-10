#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define SC4IMGUI_WIN32_CLOSEWINDOW_RENAME
#define CloseWindow CloseWindowWin32
#define Rectangle RectangleWin32
#define ShowCursor ShowCursorWin32
#define LoadImage LoadImageWin32
#define DrawText DrawTextWin32
#define DrawTextEx DrawTextExWin32
#include <cstdint>
#include <d3d.h>
#include <unordered_map>
#include <vector>
#include <Windows.h>
#include <atomic>
#include <mutex>
#include <memory>
#undef DrawTextEx
#undef DrawText
#undef LoadImage
#undef ShowCursor
#undef Rectangle
#undef CloseWindow
#undef SC4IMGUI_WIN32_CLOSEWINDOW_RENAME

#include "cRZBaseSystemService.h"
#include "public/cIGZImGuiService.h"

// Forward declaration
struct IDirectDrawSurface7;
struct RaylibOverlay;

// ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
class ImGuiService final : public cRZBaseSystemService, public cIGZImGuiService
{
public:
    ImGuiService();
    ~ImGuiService();

    uint32_t AddRef() override;
    uint32_t Release() override;

    bool QueryInterface(uint32_t riid, void** ppvObj) override;

    bool Init() override;
    bool Shutdown() override;
    bool OnTick(uint32_t unknown1) override;
    bool OnIdle(uint32_t unknown1) override;

    [[nodiscard]] uint32_t GetServiceID() const override;
    [[nodiscard]] uint32_t GetApiVersion() const override;
    [[nodiscard]] void* GetContext() const override;
    bool RegisterPanel(const ImGuiPanelDesc& desc) override;
    bool UnregisterPanel(uint32_t panelId) override;
    bool SetPanelVisible(uint32_t panelId, bool visible) override;
    bool AcquireD3DInterfaces(IDirect3DDevice7** outD3D, IDirectDraw7** outDD) override;
    [[nodiscard]] bool IsDeviceReady() const override;
    [[nodiscard]] uint32_t GetDeviceGeneration() const override;

    ImGuiTextureHandle CreateTexture(const ImGuiTextureDesc& desc) override;
    [[nodiscard]] Texture2D GetTexture(ImGuiTextureHandle handle) override;
    void ReleaseTexture(ImGuiTextureHandle handle) override;
    [[nodiscard]] bool IsTextureValid(ImGuiTextureHandle handle) const override;

private:
    struct PanelEntry
    {
        ImGuiPanelDesc desc;
        bool initialized;
    };

    struct ManagedTexture
    {
        uint32_t id;
        uint32_t width;
        uint32_t height;
        uint32_t creationGeneration;
        std::vector<uint8_t> sourceData;       // RGBA32 pixel data for recreation
        uint32_t textureId;                    // Raylib texture id
        bool hasTexture;
        bool needsRecreation;
        bool useSystemMemory;

        ManagedTexture()
            : id(0)
            , width(0)
            , height(0)
            , creationGeneration(0)
            , textureId(0)
            , hasTexture(false)
            , needsRecreation(false)
            , useSystemMemory(false) {}
    };

    static void RenderFrameThunk_(IDirect3DDevice7* device);
    void RenderFrame_(IDirect3DDevice7* device);
    bool EnsureInitialized_();
    void InitializePanels_();
    void SortPanels_();
    bool InstallWndProcHook_(HWND hwnd);
    void RemoveWndProcHook_();
    static LRESULT CALLBACK WndProcHook(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // Texture management helpers
    bool CreateRaylibTexture_(ManagedTexture& tex);
    void OnDeviceLost_();
    void OnDeviceRestored_();
    void InvalidateAllTextures_();
    bool InitializeRaylib_(HWND hwnd);
    void ShutdownRaylib_();
    bool EnsureRaylibTarget_(HWND hwnd);
    bool AlignRaylibWindow_(HWND hwnd, const RECT& clientRect);
    bool AnyPanelVisible_() const;
    bool UploadRaylibFrame_(IDirect3DDevice7* device, IDirectDraw7* dd);        
    void ReleaseOverlaySurface_();

private:
    std::vector<PanelEntry> panels_;
    mutable std::mutex panelsMutex_;

    std::unordered_map<uint32_t, ManagedTexture> textures_;  // Key: texture ID
    mutable std::mutex texturesMutex_;

    HWND gameWindow_;
    WNDPROC originalWndProc_;
    bool initialized_;
    bool imguiInitialized_;
    bool hookInstalled_;
    bool warnedNoDriver_;
    bool warnedMissingWindow_;
    bool deviceLost_;
    std::atomic<uint32_t> deviceGeneration_;
    uint32_t nextTextureId_;
    std::unique_ptr<RaylibOverlay> raylib_;
};
