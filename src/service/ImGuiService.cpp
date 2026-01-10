// ReSharper disable CppDFAUnreachableCode
// ReSharper disable CppDFAConstantConditions
#include "ImGuiService.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ddraw.h>
#include <imgui.h>
#include <ranges>
#include <windowsx.h>
#include <winerror.h>

#include "cIGZFrameWorkW32.h"
#include "cIGZGraphicSystem2.h"
#include "cRZAutoRefCount.h"
#include "cRZCOMDllDirector.h"
#include "DX7InterfaceHook.h"
#include "GZServPtrs.h"
#include "rlImGui.h"
#include "public/ImGuiServiceIds.h"
#include "utils/Logger.h"

struct RaylibOverlay {
    bool initialized = false;
    int width = 0;
    int height = 0;
    RenderTexture2D renderTarget{};
    HWND window = nullptr;
    IDirectDrawSurface7* overlaySurface = nullptr;
    uint32_t overlaySurfaceGeneration = 0;
    std::chrono::steady_clock::time_point lastFrameTime{};
    POINT origin{};
    bool hasWindowOrigin = false;
};

namespace {
    std::atomic<ImGuiService*> g_instance{nullptr};
    std::atomic<DWORD> g_renderThreadId{0};

    // Convert Windows virtual key to ImGui key
    ImGuiKey ImGui_ImplWin32_VirtualKeyToImGuiKey(int vk, bool isExtended) {
        switch (vk) {
            case VK_TAB: return ImGuiKey_Tab;
            case VK_LEFT: return ImGuiKey_LeftArrow;
            case VK_RIGHT: return ImGuiKey_RightArrow;
            case VK_UP: return ImGuiKey_UpArrow;
            case VK_DOWN: return ImGuiKey_DownArrow;
            case VK_PRIOR: return ImGuiKey_PageUp;
            case VK_NEXT: return ImGuiKey_PageDown;
            case VK_HOME: return ImGuiKey_Home;
            case VK_END: return ImGuiKey_End;
            case VK_INSERT: return ImGuiKey_Insert;
            case VK_DELETE: return ImGuiKey_Delete;
            case VK_BACK: return ImGuiKey_Backspace;
            case VK_SPACE: return ImGuiKey_Space;
            case VK_RETURN: return isExtended ? ImGuiKey_KeypadEnter : ImGuiKey_Enter;
            case VK_ESCAPE: return ImGuiKey_Escape;
            case VK_OEM_7: return ImGuiKey_Apostrophe;
            case VK_OEM_COMMA: return ImGuiKey_Comma;
            case VK_OEM_MINUS: return ImGuiKey_Minus;
            case VK_OEM_PERIOD: return ImGuiKey_Period;
            case VK_OEM_2: return ImGuiKey_Slash;
            case VK_OEM_1: return ImGuiKey_Semicolon;
            case VK_OEM_PLUS: return ImGuiKey_Equal;
            case VK_OEM_4: return ImGuiKey_LeftBracket;
            case VK_OEM_5: return ImGuiKey_Backslash;
            case VK_OEM_6: return ImGuiKey_RightBracket;
            case VK_OEM_3: return ImGuiKey_GraveAccent;
            case VK_CAPITAL: return ImGuiKey_CapsLock;
            case VK_SCROLL: return ImGuiKey_ScrollLock;
            case VK_NUMLOCK: return ImGuiKey_NumLock;
            case VK_SNAPSHOT: return ImGuiKey_PrintScreen;
            case VK_PAUSE: return ImGuiKey_Pause;
            case VK_NUMPAD0: return ImGuiKey_Keypad0;
            case VK_NUMPAD1: return ImGuiKey_Keypad1;
            case VK_NUMPAD2: return ImGuiKey_Keypad2;
            case VK_NUMPAD3: return ImGuiKey_Keypad3;
            case VK_NUMPAD4: return ImGuiKey_Keypad4;
            case VK_NUMPAD5: return ImGuiKey_Keypad5;
            case VK_NUMPAD6: return ImGuiKey_Keypad6;
            case VK_NUMPAD7: return ImGuiKey_Keypad7;
            case VK_NUMPAD8: return ImGuiKey_Keypad8;
            case VK_NUMPAD9: return ImGuiKey_Keypad9;
            case VK_DECIMAL: return ImGuiKey_KeypadDecimal;
            case VK_DIVIDE: return ImGuiKey_KeypadDivide;
            case VK_MULTIPLY: return ImGuiKey_KeypadMultiply;
            case VK_SUBTRACT: return ImGuiKey_KeypadSubtract;
            case VK_ADD: return ImGuiKey_KeypadAdd;
            case VK_LSHIFT: return ImGuiKey_LeftShift;
            case VK_LCONTROL: return ImGuiKey_LeftCtrl;
            case VK_LMENU: return ImGuiKey_LeftAlt;
            case VK_LWIN: return ImGuiKey_LeftSuper;
            case VK_RSHIFT: return ImGuiKey_RightShift;
            case VK_RCONTROL: return ImGuiKey_RightCtrl;
            case VK_RMENU: return ImGuiKey_RightAlt;
            case VK_RWIN: return ImGuiKey_RightSuper;
            case VK_APPS: return ImGuiKey_Menu;
            case '0': return ImGuiKey_0;
            case '1': return ImGuiKey_1;
            case '2': return ImGuiKey_2;
            case '3': return ImGuiKey_3;
            case '4': return ImGuiKey_4;
            case '5': return ImGuiKey_5;
            case '6': return ImGuiKey_6;
            case '7': return ImGuiKey_7;
            case '8': return ImGuiKey_8;
            case '9': return ImGuiKey_9;
            case 'A': return ImGuiKey_A;
            case 'B': return ImGuiKey_B;
            case 'C': return ImGuiKey_C;
            case 'D': return ImGuiKey_D;
            case 'E': return ImGuiKey_E;
            case 'F': return ImGuiKey_F;
            case 'G': return ImGuiKey_G;
            case 'H': return ImGuiKey_H;
            case 'I': return ImGuiKey_I;
            case 'J': return ImGuiKey_J;
            case 'K': return ImGuiKey_K;
            case 'L': return ImGuiKey_L;
            case 'M': return ImGuiKey_M;
            case 'N': return ImGuiKey_N;
            case 'O': return ImGuiKey_O;
            case 'P': return ImGuiKey_P;
            case 'Q': return ImGuiKey_Q;
            case 'R': return ImGuiKey_R;
            case 'S': return ImGuiKey_S;
            case 'T': return ImGuiKey_T;
            case 'U': return ImGuiKey_U;
            case 'V': return ImGuiKey_V;
            case 'W': return ImGuiKey_W;
            case 'X': return ImGuiKey_X;
            case 'Y': return ImGuiKey_Y;
            case 'Z': return ImGuiKey_Z;
            case VK_F1: return ImGuiKey_F1;
            case VK_F2: return ImGuiKey_F2;
            case VK_F3: return ImGuiKey_F3;
            case VK_F4: return ImGuiKey_F4;
            case VK_F5: return ImGuiKey_F5;
            case VK_F6: return ImGuiKey_F6;
            case VK_F7: return ImGuiKey_F7;
            case VK_F8: return ImGuiKey_F8;
            case VK_F9: return ImGuiKey_F9;
            case VK_F10: return ImGuiKey_F10;
            case VK_F11: return ImGuiKey_F11;
            case VK_F12: return ImGuiKey_F12;
            default: return ImGuiKey_None;
        }
    }

    POINT GetCursorHotspot_() {
        POINT hotspot{};
        CURSORINFO cursorInfo{};
        cursorInfo.cbSize = sizeof(cursorInfo);
        if (!GetCursorInfo(&cursorInfo) || !(cursorInfo.flags & CURSOR_SHOWING)) {
            return hotspot;
        }
        ICONINFO iconInfo{};
        if (!GetIconInfo(cursorInfo.hCursor, &iconInfo)) {
            return hotspot;
        }
        hotspot.x = iconInfo.xHotspot;
        hotspot.y = iconInfo.yHotspot;
        if (iconInfo.hbmMask) {
            DeleteObject(iconInfo.hbmMask);
        }
        if (iconInfo.hbmColor) {
            DeleteObject(iconInfo.hbmColor);
        }
        return hotspot;
    }

    struct Dx7OverlayStateRestore {
        IDirect3DDevice7* device;
        bool hasAlphaBlend;
        bool hasSrcBlend;
        bool hasDstBlend;
        bool hasZEnable;
        bool hasZWrite;
        bool hasCullMode;
        bool hasLighting;
        bool hasShade;
        bool hasFog;
        bool hasClipping;
        bool hasAlphaTest;
        DWORD rsAlphaBlend;
        DWORD rsSrcBlend;
        DWORD rsDstBlend;
        DWORD rsZEnable;
        DWORD rsZWrite;
        DWORD rsCullMode;
        DWORD rsLighting;
        DWORD rsShade;
        DWORD rsFog;
        DWORD rsClipping;
        DWORD rsAlphaTest;
        IDirectDrawSurface7* tex0;
        bool hasTss0ColorOp;
        bool hasTss0ColorArg1;
        bool hasTss0ColorArg2;
        bool hasTss0AlphaOp;
        bool hasTss0AlphaArg1;
        bool hasTss0AlphaArg2;
        bool hasTss1ColorOp;
        bool hasTss1AlphaOp;
        bool hasTss0MinFilter;
        bool hasTss0MagFilter;
        bool hasTss0MipFilter;
        bool hasTss0AddressU;
        bool hasTss0AddressV;
        DWORD tss0ColorOp;
        DWORD tss0ColorArg1;
        DWORD tss0ColorArg2;
        DWORD tss0AlphaOp;
        DWORD tss0AlphaArg1;
        DWORD tss0AlphaArg2;
        DWORD tss1ColorOp;
        DWORD tss1AlphaOp;
        DWORD tss0MinFilter;
        DWORD tss0MagFilter;
        DWORD tss0MipFilter;
        DWORD tss0AddressU;
        DWORD tss0AddressV;

        explicit Dx7OverlayStateRestore(IDirect3DDevice7* deviceIn)
            : device(deviceIn)
            , hasAlphaBlend(false)
            , hasSrcBlend(false)
            , hasDstBlend(false)
            , hasZEnable(false)
            , hasZWrite(false)
            , hasCullMode(false)
            , hasLighting(false)
            , hasShade(false)
            , hasFog(false)
            , hasClipping(false)
            , hasAlphaTest(false)
            , rsAlphaBlend(0)
            , rsSrcBlend(0)
            , rsDstBlend(0)
            , rsZEnable(0)
            , rsZWrite(0)
            , rsCullMode(0)
            , rsLighting(0)
            , rsShade(0)
            , rsFog(0)
            , rsClipping(0)
            , rsAlphaTest(0)
            , tex0(nullptr)
            , hasTss0ColorOp(false)
            , hasTss0ColorArg1(false)
            , hasTss0ColorArg2(false)
            , hasTss0AlphaOp(false)
            , hasTss0AlphaArg1(false)
            , hasTss0AlphaArg2(false)
            , hasTss1ColorOp(false)
            , hasTss1AlphaOp(false)
            , hasTss0MinFilter(false)
            , hasTss0MagFilter(false)
            , hasTss0MipFilter(false)
            , hasTss0AddressU(false)
            , hasTss0AddressV(false)
            , tss0ColorOp(0)
            , tss0ColorArg1(0)
            , tss0ColorArg2(0)
            , tss0AlphaOp(0)
            , tss0AlphaArg1(0)
            , tss0AlphaArg2(0)
            , tss1ColorOp(0)
            , tss1AlphaOp(0)
            , tss0MinFilter(0)
            , tss0MagFilter(0)
            , tss0MipFilter(0)
            , tss0AddressU(0)
            , tss0AddressV(0) {
            if (!device) {
                return;
            }
            hasAlphaBlend = SUCCEEDED(
                device->GetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, &rsAlphaBlend));
            hasSrcBlend = SUCCEEDED(
                device->GetRenderState(D3DRENDERSTATE_SRCBLEND, &rsSrcBlend));
            hasDstBlend = SUCCEEDED(
                device->GetRenderState(D3DRENDERSTATE_DESTBLEND, &rsDstBlend));
            hasZEnable = SUCCEEDED(
                device->GetRenderState(D3DRENDERSTATE_ZENABLE, &rsZEnable));
            hasZWrite = SUCCEEDED(
                device->GetRenderState(D3DRENDERSTATE_ZWRITEENABLE, &rsZWrite));
            hasCullMode = SUCCEEDED(
                device->GetRenderState(D3DRENDERSTATE_CULLMODE, &rsCullMode));
            hasLighting = SUCCEEDED(
                device->GetRenderState(D3DRENDERSTATE_LIGHTING, &rsLighting));
            hasShade = SUCCEEDED(
                device->GetRenderState(D3DRENDERSTATE_SHADEMODE, &rsShade));
            hasFog = SUCCEEDED(
                device->GetRenderState(D3DRENDERSTATE_FOGENABLE, &rsFog));
            hasClipping = SUCCEEDED(
                device->GetRenderState(D3DRENDERSTATE_CLIPPING, &rsClipping));
            hasAlphaTest = SUCCEEDED(
                device->GetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, &rsAlphaTest));
            if (SUCCEEDED(device->GetTexture(0, &tex0))) {
                if (!tex0) {
                    tex0 = nullptr;
                }
            }
            hasTss0ColorOp = SUCCEEDED(
                device->GetTextureStageState(0, D3DTSS_COLOROP, &tss0ColorOp));
            hasTss0ColorArg1 = SUCCEEDED(
                device->GetTextureStageState(0, D3DTSS_COLORARG1, &tss0ColorArg1));
            hasTss0ColorArg2 = SUCCEEDED(
                device->GetTextureStageState(0, D3DTSS_COLORARG2, &tss0ColorArg2));
            hasTss0AlphaOp = SUCCEEDED(
                device->GetTextureStageState(0, D3DTSS_ALPHAOP, &tss0AlphaOp));
            hasTss0AlphaArg1 = SUCCEEDED(
                device->GetTextureStageState(0, D3DTSS_ALPHAARG1, &tss0AlphaArg1));
            hasTss0AlphaArg2 = SUCCEEDED(
                device->GetTextureStageState(0, D3DTSS_ALPHAARG2, &tss0AlphaArg2));
            hasTss1ColorOp = SUCCEEDED(
                device->GetTextureStageState(1, D3DTSS_COLOROP, &tss1ColorOp));
            hasTss1AlphaOp = SUCCEEDED(
                device->GetTextureStageState(1, D3DTSS_ALPHAOP, &tss1AlphaOp));
            hasTss0MinFilter = SUCCEEDED(
                device->GetTextureStageState(0, D3DTSS_MINFILTER, &tss0MinFilter));
            hasTss0MagFilter = SUCCEEDED(
                device->GetTextureStageState(0, D3DTSS_MAGFILTER, &tss0MagFilter));
            hasTss0MipFilter = SUCCEEDED(
                device->GetTextureStageState(0, D3DTSS_MIPFILTER, &tss0MipFilter));
            hasTss0AddressU = SUCCEEDED(
                device->GetTextureStageState(0, D3DTSS_ADDRESSU, &tss0AddressU));
            hasTss0AddressV = SUCCEEDED(
                device->GetTextureStageState(0, D3DTSS_ADDRESSV, &tss0AddressV));
        }

        ~Dx7OverlayStateRestore() {
            if (!device) {
                return;
            }
            if (hasAlphaBlend) {
                device->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, rsAlphaBlend);
            }
            if (hasSrcBlend) {
                device->SetRenderState(D3DRENDERSTATE_SRCBLEND, rsSrcBlend);
            }
            if (hasDstBlend) {
                device->SetRenderState(D3DRENDERSTATE_DESTBLEND, rsDstBlend);
            }
            if (hasZEnable) {
                device->SetRenderState(D3DRENDERSTATE_ZENABLE, rsZEnable);
            }
            if (hasZWrite) {
                device->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, rsZWrite);
            }
            if (hasCullMode) {
                device->SetRenderState(D3DRENDERSTATE_CULLMODE, rsCullMode);
            }
            if (hasLighting) {
                device->SetRenderState(D3DRENDERSTATE_LIGHTING, rsLighting);
            }
            if (hasShade) {
                device->SetRenderState(D3DRENDERSTATE_SHADEMODE, rsShade);
            }
            if (hasFog) {
                device->SetRenderState(D3DRENDERSTATE_FOGENABLE, rsFog);
            }
            if (hasClipping) {
                device->SetRenderState(D3DRENDERSTATE_CLIPPING, rsClipping);
            }
            if (hasAlphaTest) {
                device->SetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, rsAlphaTest);
            }
            if (tex0) {
                device->SetTexture(0, tex0);
                tex0->Release();
            } else {
                device->SetTexture(0, nullptr);
            }
            if (hasTss0ColorOp) {
                device->SetTextureStageState(0, D3DTSS_COLOROP, tss0ColorOp);
            }
            if (hasTss0ColorArg1) {
                device->SetTextureStageState(0, D3DTSS_COLORARG1, tss0ColorArg1);
            }
            if (hasTss0ColorArg2) {
                device->SetTextureStageState(0, D3DTSS_COLORARG2, tss0ColorArg2);
            }
            if (hasTss0AlphaOp) {
                device->SetTextureStageState(0, D3DTSS_ALPHAOP, tss0AlphaOp);
            }
            if (hasTss0AlphaArg1) {
                device->SetTextureStageState(0, D3DTSS_ALPHAARG1, tss0AlphaArg1);
            }
            if (hasTss0AlphaArg2) {
                device->SetTextureStageState(0, D3DTSS_ALPHAARG2, tss0AlphaArg2);
            }
            if (hasTss1ColorOp) {
                device->SetTextureStageState(1, D3DTSS_COLOROP, tss1ColorOp);
            }
            if (hasTss1AlphaOp) {
                device->SetTextureStageState(1, D3DTSS_ALPHAOP, tss1AlphaOp);
            }
            if (hasTss0MinFilter) {
                device->SetTextureStageState(0, D3DTSS_MINFILTER, tss0MinFilter);
            }
            if (hasTss0MagFilter) {
                device->SetTextureStageState(0, D3DTSS_MAGFILTER, tss0MagFilter);
            }
            if (hasTss0MipFilter) {
                device->SetTextureStageState(0, D3DTSS_MIPFILTER, tss0MipFilter);
            }
            if (hasTss0AddressU) {
                device->SetTextureStageState(0, D3DTSS_ADDRESSU, tss0AddressU);
            }
            if (hasTss0AddressV) {
                device->SetTextureStageState(0, D3DTSS_ADDRESSV, tss0AddressV);
            }
        }
    };

}

ImGuiService::ImGuiService()
    : cRZBaseSystemService(kImGuiServiceID, 0)
    , gameWindow_(nullptr)
    , originalWndProc_(nullptr)
    , initialized_(false)
    , imguiInitialized_(false)
    , hookInstalled_(false)
    , warnedNoDriver_(false)
    , warnedMissingWindow_(false)
    , deviceLost_(false)
    , deviceGeneration_(0)
    , nextTextureId_(1) {}

ImGuiService::~ImGuiService() {
    auto expected = this;
    g_instance.compare_exchange_strong(expected, nullptr, std::memory_order_release);
}

uint32_t ImGuiService::AddRef() {
    return cRZBaseSystemService::AddRef();
}

uint32_t ImGuiService::Release() {
    return cRZBaseSystemService::Release();
}

bool ImGuiService::QueryInterface(uint32_t riid, void** ppvObj) {
    if (riid == GZIID_cIGZImGuiService) {
        *ppvObj = static_cast<cIGZImGuiService*>(this);
        AddRef();
        return true;
    }

    return cRZBaseSystemService::QueryInterface(riid, ppvObj);
}

bool ImGuiService::Init() {
    if (initialized_) {
        return true;
    }

    Logger::Initialize("SC4ImGuiService", "");
    LOG_INFO("ImGuiService: initialized");
    SetServiceRunning(true);
    initialized_ = true;
    g_instance.store(this, std::memory_order_release);
    return true;
}

bool ImGuiService::Shutdown() {
    if (!initialized_) {
        return true;
    }

    {
        std::lock_guard panelLock(panelsMutex_);
        for (const auto& panel : panels_) {
            if (panel.desc.on_shutdown) {
                panel.desc.on_shutdown(panel.desc.data);
            }
        }
        panels_.clear();
    }

    // Clean up all textures before shutting down ImGui
    {
        std::lock_guard textureLock(texturesMutex_);
        const bool canUnload = raylib_ && raylib_->initialized;
        for (auto& texture : textures_ | std::views::values) {
            if (canUnload && texture.hasTexture && texture.textureId != 0) {
                Texture2D tex{};
                tex.id = texture.textureId;
                tex.width = static_cast<int>(texture.width);
                tex.height = static_cast<int>(texture.height);
                tex.mipmaps = 1;
                tex.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                UnloadTexture(tex);
            }
            texture.textureId = 0;
            texture.hasTexture = false;
        }
        textures_.clear();
    }

    RemoveWndProcHook_();
    DX7InterfaceHook::SetFrameCallback(nullptr);
    DX7InterfaceHook::ShutdownImGui();
    ShutdownRaylib_();

    imguiInitialized_ = false;
    hookInstalled_ = false;
    deviceGeneration_.fetch_add(1, std::memory_order_release);
    SetServiceRunning(false);
    initialized_ = false;
    return true;
}

bool ImGuiService::OnTick(uint32_t) {
    if (!initialized_) {
        return true;
    }

    if (EnsureInitialized_()) {
        InitializePanels_();
    }
    return true;
}

bool ImGuiService::OnIdle(uint32_t) {
    return OnTick(0);
}

uint32_t ImGuiService::GetServiceID() const {
    return serviceID;
}

uint32_t ImGuiService::GetApiVersion() const {
    return kImGuiServiceApiVersion;
}

void* ImGuiService::GetContext() const {
    auto* self = const_cast<ImGuiService*>(this);
    if (!self->imguiInitialized_) {
        self->EnsureInitialized_();
    }
    return ImGui::GetCurrentContext();
}

bool ImGuiService::RegisterPanel(const ImGuiPanelDesc& desc) {
    if (!desc.on_render) {
        LOG_WARN("ImGuiService: rejected panel {} (null on_render)", desc.id);
        return false;
    }

    {
        std::lock_guard lock(panelsMutex_);
        const auto it = std::ranges::find_if(panels_, [&](const PanelEntry& entry) {
            return entry.desc.id == desc.id;
        });
        if (it != panels_.end()) {
            LOG_WARN("ImGuiService: rejected panel {} (duplicate id)", desc.id);
            return false;
        }

        panels_.push_back(PanelEntry{desc, false});
        SortPanels_();
    }

    if (imguiInitialized_) {
        InitializePanels_();
    }
    LOG_INFO("ImGuiService: registered panel {} (order={})", desc.id, desc.order);
    return true;
}

bool ImGuiService::UnregisterPanel(uint32_t panelId) {
    std::lock_guard lock(panelsMutex_);
    const auto it = std::ranges::find_if(panels_, [&](const PanelEntry& entry) {
        return entry.desc.id == panelId;
    });
    if (it == panels_.end()) {
        LOG_WARN("ImGuiService: unregister failed for panel {}", panelId);
        return false;
    }

    if (it->desc.on_unregister) {
        it->desc.on_unregister(it->desc.data);
    }

    panels_.erase(it);
    LOG_INFO("ImGuiService: unregistered panel {}", panelId);
    return true;
}

bool ImGuiService::SetPanelVisible(const uint32_t panelId, const bool visible) {
    std::lock_guard lock(panelsMutex_);
    const auto it = std::ranges::find_if(panels_, [&](const PanelEntry& entry) {
        return entry.desc.id == panelId;
    });
    if (it == panels_.end()) {
        return false;
    }

    if (it->desc.visible == visible) {
        return true;
    }

    it->desc.visible = visible;
    if (it->desc.on_visible_changed) {
        it->desc.on_visible_changed(it->desc.data, visible);
    }
    return true;
}

bool ImGuiService::AcquireD3DInterfaces(IDirect3DDevice7** outD3D, IDirectDraw7** outDD) {
    if (!outD3D || !outDD) {
        return false;
    }

    auto* d3dx = DX7InterfaceHook::GetD3DXInterface();
    if (!d3dx) {
        return false;
    }

    auto* d3d = d3dx->GetD3DDevice();
    auto* dd = d3dx->GetDD();
    if (!d3d || !dd) {
        return false;
    }

    d3d->AddRef();
    dd->AddRef();
    *outD3D = d3d;
    *outDD = dd;
    return true;
}

bool ImGuiService::IsDeviceReady() const {
    if (!imguiInitialized_) {
        return false;
    }
    auto* d3dx = DX7InterfaceHook::GetD3DXInterface();
    return d3dx && d3dx->GetD3DDevice() && d3dx->GetDD();
}

uint32_t ImGuiService::GetDeviceGeneration() const {
    return deviceGeneration_.load(std::memory_order_acquire);
}

void ImGuiService::RenderFrameThunk_(IDirect3DDevice7* device) {
    auto* instance = g_instance.load(std::memory_order_acquire);
    if (instance) {
        instance->RenderFrame_(device);
    }
}

void ImGuiService::RenderFrame_(IDirect3DDevice7* device) {
    static auto loggedFirstRender = false;
    const DWORD threadId = GetCurrentThreadId();
    const DWORD prevThreadId = g_renderThreadId.load(std::memory_order_acquire);
    if (prevThreadId == 0) {
        g_renderThreadId.store(threadId, std::memory_order_release);
        LOG_DEBUG("ImGuiService::RenderFrame_: render thread id set to {}", threadId);
    } else if (prevThreadId != threadId) {
        LOG_WARN("ImGuiService::RenderFrame_: render thread id changed ({} -> {})",
            prevThreadId, threadId);
        g_renderThreadId.store(threadId, std::memory_order_release);
    }
    if (!imguiInitialized_) {
        return;
    }

    {
        std::lock_guard lock(panelsMutex_);
        if (panels_.empty()) {
            return;
        }
    }

    auto* d3dx = DX7InterfaceHook::GetD3DXInterface();
    if (!d3dx || device != d3dx->GetD3DDevice()) {
        return;
    }
    if (!ImGui::GetCurrentContext()) {
        return;
    }

    InitializePanels_();

    auto* dd = d3dx->GetDD();
    if (!dd) {
        return;
    }

    // Check for device loss
    HRESULT hr = dd->TestCooperativeLevel();
    if (hr == DDERR_SURFACELOST || hr == DDERR_WRONGMODE) {
        // Treat only explicit device loss conditions as device lost
        if (!deviceLost_) {
            OnDeviceLost_();
        }
        return;  // Skip rendering when device is lost
    } else if (FAILED(hr)) {
        // Non-device-loss failure: skip rendering but do not change deviceLost_ state
        return;
    } else if (deviceLost_) {
        OnDeviceRestored_();
    }

    if (!AnyPanelVisible_()) {
        return;
    }

    if (!EnsureRaylibTarget_(gameWindow_)) {
        return;
    }
    if (!IsWindowReady() || GetWindowHandle() == nullptr) {
        LOG_WARN("ImGuiService::RenderFrame_: raylib window not ready");        
        return;
    }

    auto now = std::chrono::steady_clock::now();
    float deltaSeconds = 0.0f;
    if (raylib_) {
        if (raylib_->lastFrameTime.time_since_epoch().count() == 0) {
            deltaSeconds = 1.0f / 60.0f;
        } else {
            deltaSeconds = std::chrono::duration<float>(now - raylib_->lastFrameTime).count();
        }
        raylib_->lastFrameTime = now;
    }

    BeginDrawing();
    BeginTextureMode(raylib_->renderTarget);
    ClearBackground({0, 0, 0, 0});

    // Custom ImGui frame setup - skip rlImGui's input processing since we feed input directly from WndProc
    {
        ImGuiIO& io = ImGui::GetIO();
        // Set display size (raylib render target size)
        io.DisplaySize = ImVec2(static_cast<float>(raylib_->width), static_cast<float>(raylib_->height));
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        // Set delta time
        io.DeltaTime = deltaSeconds > 0.0f ? deltaSeconds : 1.0f / 60.0f;
        // Start new ImGui frame (input already fed from WndProc)
        ImGui::NewFrame();
    }

    {
        std::lock_guard lock(panelsMutex_);
        for (auto& panel : panels_) {
            if (panel.desc.visible && panel.desc.on_update) {
                panel.desc.on_update(panel.desc.data);
            }
        }

        for (auto& panel : panels_) {
            if (panel.desc.visible && panel.desc.on_render) {
                panel.desc.on_render(panel.desc.data);
            }
        }
    }

    rlImGuiEnd();
    EndTextureMode();
    EndDrawing();

    if (!UploadRaylibFrame_(device, dd)) {
        return;
    }

    if (!loggedFirstRender) {
        LOG_INFO("ImGuiService: rendered first frame with {} panel(s)", panels_.size());
        loggedFirstRender = true;
    }
}

bool ImGuiService::EnsureInitialized_() {
    if (imguiInitialized_) {
        return true;
    }

    cIGZGraphicSystem2Ptr pGS2;
    if (!pGS2) {
        return false;
    }

    cIGZGDriver* pDriver = pGS2->GetGDriver();
    if (!pDriver) {
        if (!warnedNoDriver_) {
            LOG_WARN("ImGuiService: graphics driver not available yet");
            warnedNoDriver_ = true;
        }
        return false;
    }

    if (pDriver->GetGZCLSID() != kSCGDriverDirectX) {
        if (!warnedNoDriver_) {
            LOG_WARN("ImGuiService: not a DirectX driver, skipping initialization");
            warnedNoDriver_ = true;
        }
        return false;
    }

    if (!DX7InterfaceHook::CaptureInterface(pDriver)) {
        LOG_ERROR("ImGuiService: failed to capture D3DX interface");
        return false;
    }
    auto* d3dx = DX7InterfaceHook::GetD3DXInterface();
    if (!d3dx || !d3dx->GetD3DDevice() || !d3dx->GetDD()) {
        LOG_WARN("ImGuiService: D3DX interface not ready yet (d3dx={}, d3d={}, dd={})",
            static_cast<void*>(d3dx),
            static_cast<void*>(d3dx ? d3dx->GetD3DDevice() : nullptr),
            static_cast<void*>(d3dx ? d3dx->GetDD() : nullptr));
        return false;
    }

    cRZAutoRefCount<cIGZFrameWorkW32> pFrameworkW32;
    if (!RZGetFrameWork()->QueryInterface(GZIID_cIGZFrameWorkW32, pFrameworkW32.AsPPVoid())) {
        return false;
    }
    if (!pFrameworkW32) {
        return false;
    }

    HWND hwnd = pFrameworkW32->GetMainHWND();
    if (!hwnd || !IsWindow(hwnd)) {
        if (!warnedMissingWindow_) {
            LOG_WARN("ImGuiService: game window not ready yet");
            warnedMissingWindow_ = true;
        }
        return false;
    }

    if (!InitializeRaylib_(hwnd)) {
        LOG_ERROR("ImGuiService: failed to initialize raylib ImGui renderer");
        return false;
    }

    imguiInitialized_ = true;
    deviceGeneration_.fetch_add(1, std::memory_order_release);
    warnedNoDriver_ = false;
    warnedMissingWindow_ = false;

    if (!InstallWndProcHook_(hwnd)) {
        LOG_WARN("ImGuiService: failed to install WndProc hook");
    }
    DX7InterfaceHook::SetFrameCallback(&ImGuiService::RenderFrameThunk_);
    DX7InterfaceHook::InstallSceneHooks();
    LOG_INFO("ImGuiService: ImGui initialized and scene hooks installed");
    return true;
}

void ImGuiService::InitializePanels_() {
    if (!imguiInitialized_) {
        return;
    }

    for (auto& panel : panels_) {
        if (!panel.initialized) {
            if (panel.desc.on_init) {
                panel.desc.on_init(panel.desc.data);
            }
            panel.initialized = true;
        }
    }
}

void ImGuiService::SortPanels_() {
    std::sort(panels_.begin(), panels_.end(), [](const PanelEntry& a, const PanelEntry& b) {
        return a.desc.order < b.desc.order;
    });
}

bool ImGuiService::InstallWndProcHook_(HWND hwnd) {
    if (hookInstalled_) {
        return true;
    }

    originalWndProc_ = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    if (!originalWndProc_) {
        return false;
    }

    gameWindow_ = hwnd;
    if (!SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ImGuiService::WndProcHook))) {
        originalWndProc_ = nullptr;
        return false;
    }

    hookInstalled_ = true;
    return true;
}

void ImGuiService::RemoveWndProcHook_() {
    if (hookInstalled_ && gameWindow_ && originalWndProc_) {
        SetWindowLongPtrW(gameWindow_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalWndProc_));
    }

    hookInstalled_ = false;
    originalWndProc_ = nullptr;
    gameWindow_ = nullptr;
}

LRESULT CALLBACK ImGuiService::WndProcHook(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* instance = g_instance.load(std::memory_order_acquire);
    ImGuiContext* imguiCtx = ImGui::GetCurrentContext();

    // Feed input directly to ImGui's IO - bypass raylib entirely
    bool imguiHandled = false;
    if (imguiCtx && instance && instance->AnyPanelVisible_()) {
        ImGuiIO& io = ImGui::GetIO();

        switch (msg) {
            // Mouse position
            case WM_MOUSEMOVE: {
                POINT pt;
                pt.x = GET_X_LPARAM(lParam);
                pt.y = GET_Y_LPARAM(lParam);
                io.AddMousePosEvent(static_cast<float>(pt.x), static_cast<float>(pt.y));
                break;
            }

            // Mouse buttons
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                imguiHandled = io.WantCaptureMouse;
                break;
            case WM_LBUTTONUP:
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                imguiHandled = io.WantCaptureMouse;
                break;
            case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
                io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
                imguiHandled = io.WantCaptureMouse;
                break;
            case WM_RBUTTONUP:
                io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
                imguiHandled = io.WantCaptureMouse;
                break;
            case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
                io.AddMouseButtonEvent(ImGuiMouseButton_Middle, true);
                imguiHandled = io.WantCaptureMouse;
                break;
            case WM_MBUTTONUP:
                io.AddMouseButtonEvent(ImGuiMouseButton_Middle, false);
                imguiHandled = io.WantCaptureMouse;
                break;

            // Mouse wheel
            case WM_MOUSEWHEEL:
                io.AddMouseWheelEvent(0.0f, static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA);
                imguiHandled = io.WantCaptureMouse;
                break;
            case WM_MOUSEHWHEEL:
                io.AddMouseWheelEvent(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA, 0.0f);
                imguiHandled = io.WantCaptureMouse;
                break;

            // Keyboard
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN: {
                const bool isExtended = (HIWORD(lParam) & KF_EXTENDED) != 0;
                const int vk = static_cast<int>(wParam);
                ImGuiKey imguiKey = ImGui_ImplWin32_VirtualKeyToImGuiKey(vk, isExtended);
                if (imguiKey != ImGuiKey_None) {
                    io.AddKeyEvent(imguiKey, true);
                }
                // Handle modifier keys
                io.AddKeyEvent(ImGuiMod_Ctrl, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
                io.AddKeyEvent(ImGuiMod_Shift, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                io.AddKeyEvent(ImGuiMod_Alt, (GetKeyState(VK_MENU) & 0x8000) != 0);
                imguiHandled = io.WantCaptureKeyboard;
                break;
            }
            case WM_KEYUP:
            case WM_SYSKEYUP: {
                const bool isExtended = (HIWORD(lParam) & KF_EXTENDED) != 0;
                const int vk = static_cast<int>(wParam);
                ImGuiKey imguiKey = ImGui_ImplWin32_VirtualKeyToImGuiKey(vk, isExtended);
                if (imguiKey != ImGuiKey_None) {
                    io.AddKeyEvent(imguiKey, false);
                }
                io.AddKeyEvent(ImGuiMod_Ctrl, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
                io.AddKeyEvent(ImGuiMod_Shift, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                io.AddKeyEvent(ImGuiMod_Alt, (GetKeyState(VK_MENU) & 0x8000) != 0);
                imguiHandled = io.WantCaptureKeyboard;
                break;
            }

            // Character input
            case WM_CHAR:
                if (wParam > 0 && wParam < 0x10000) {
                    io.AddInputCharacterUTF16(static_cast<unsigned short>(wParam));
                }
                imguiHandled = io.WantCaptureKeyboard;
                break;

            default:
                break;
        }
    }

    if (imguiHandled) {
        return 0;
    }

    // Forward to game
    if (instance && instance->originalWndProc_) {
        return CallWindowProcW(instance->originalWndProc_, hWnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// Texture management implementation

ImGuiTextureHandle ImGuiService::CreateTexture(const ImGuiTextureDesc& desc) {  
    const DWORD threadId = GetCurrentThreadId();
    const DWORD renderThreadId = g_renderThreadId.load(std::memory_order_acquire);
    if (renderThreadId != 0 && renderThreadId != threadId) {
        LOG_WARN("ImGuiService::CreateTexture: called off render thread (tid={}, render_tid={})",
            threadId, renderThreadId);
    } else {
        LOG_DEBUG("ImGuiService::CreateTexture: thread id {}", threadId);
    }
    // Validate parameters
    if (desc.width == 0 || desc.height == 0 || !desc.pixels) {
        LOG_ERROR("ImGuiService::CreateTexture: invalid parameters (width={}, height={}, pixels={})",
            desc.width, desc.height, static_cast<const void*>(desc.pixels));
        return ImGuiTextureHandle{0, 0};
    }

    // Check for potential integer overflow in size calculation
    // Ensure width * height doesn't overflow when computing pixel count
    if (desc.height > SIZE_MAX / desc.width) {
        LOG_ERROR("ImGuiService::CreateTexture: dimensions would overflow (width={}, height={})",
            desc.width, desc.height);
        return ImGuiTextureHandle{0, 0};
    }

    const size_t pixelCount = static_cast<size_t>(desc.width) * desc.height;

    // Ensure pixelCount * 4 doesn't overflow when computing byte size
    if (pixelCount > SIZE_MAX / 4) {
        LOG_ERROR("ImGuiService::CreateTexture: texture too large ({} pixels)", pixelCount);
        return ImGuiTextureHandle{0, 0};
    }

    if (!raylib_ || !raylib_->initialized) {
        LOG_WARN("ImGuiService::CreateTexture: raylib not ready, texture will be created on-demand");
    }

    uint32_t currentGen = deviceGeneration_.load(std::memory_order_acquire);

    // Create managed texture entry
    ManagedTexture tex;
    tex.id = nextTextureId_++;
    tex.width = desc.width;
    tex.height = desc.height;
    tex.creationGeneration = currentGen;
    tex.useSystemMemory = desc.useSystemMemory;
    tex.textureId = 0;
    tex.hasTexture = false;
    tex.needsRecreation = false;

    // Store source pixel data for recreation after device loss
    const size_t dataSize = pixelCount * 4;  // RGBA32
    tex.sourceData.resize(dataSize);
    std::memcpy(tex.sourceData.data(), desc.pixels, dataSize);

    // Attempt initial texture creation
    if (raylib_ && raylib_->initialized && !deviceLost_) {
        if (!CreateRaylibTexture_(tex)) {
            LOG_WARN("ImGuiService::CreateTexture: texture creation failed, will retry later (id={})", tex.id);
            tex.needsRecreation = true;
        }
    } else {
        tex.needsRecreation = true;
    }

    const uint32_t textureId = tex.id;

    {
        std::lock_guard lock(texturesMutex_);
        textures_.emplace(textureId, std::move(tex));
    }

    LOG_INFO("ImGuiService::CreateTexture: created texture id={} ({}x{}, gen={})",
        textureId, desc.width, desc.height, currentGen);

    return ImGuiTextureHandle{textureId, currentGen};
}

bool ImGuiService::CreateRaylibTexture_(ManagedTexture& tex) {
    if (!raylib_ || !raylib_->initialized) {
        return false;
    }

    if (tex.width == 0 || tex.height == 0 || tex.sourceData.empty()) {
        return false;
    }

    if (tex.hasTexture && tex.textureId != 0) {
        Texture2D oldTex{};
        oldTex.id = tex.textureId;
        oldTex.width = static_cast<int>(tex.width);
        oldTex.height = static_cast<int>(tex.height);
        oldTex.mipmaps = 1;
        oldTex.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        UnloadTexture(oldTex);
        tex.textureId = 0;
        tex.hasTexture = false;
    }

    Image image{};
    image.data = tex.sourceData.data();
    image.width = static_cast<int>(tex.width);
    image.height = static_cast<int>(tex.height);
    image.mipmaps = 1;
    image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

    Texture2D newTex = LoadTextureFromImage(image);
    if (newTex.id == 0) {
        LOG_ERROR("ImGuiService::CreateRaylibTexture_: LoadTextureFromImage failed (id={})", tex.id);
        return false;
    }

    tex.textureId = newTex.id;
    tex.hasTexture = true;
    tex.needsRecreation = false;
    uint32_t currentGen = deviceGeneration_.load(std::memory_order_acquire);
    tex.creationGeneration = currentGen;

    LOG_INFO("ImGuiService::CreateRaylibTexture_: texture created successfully (id={}, gen={})",
        tex.id, currentGen);
    return true;
}

Texture2D ImGuiService::GetTexture(ImGuiTextureHandle handle) {
    Texture2D invalid{};
    invalid.id = 0;
    // Check device generation first - return nullptr if mismatch
    uint32_t currentGen = deviceGeneration_.load(std::memory_order_acquire);
    if (handle.generation != currentGen) {
        return invalid;
    }

    // Check device lost flag
    if (deviceLost_) {
        return invalid;
    }

    std::lock_guard lock(texturesMutex_);

    // Find texture by ID
    auto it = textures_.find(handle.id);

    if (it == textures_.end()) {
        return invalid;
    }

    ManagedTexture& tex = it->second;

    // Recreate texture if needed
    if (tex.needsRecreation || !tex.hasTexture || tex.textureId == 0) {
        if (!CreateRaylibTexture_(tex)) {
            LOG_WARN("ImGuiService::GetTexture: failed to recreate texture (id={})", tex.id);
            return invalid;
        }
    }

    Texture2D texture{};
    texture.id = tex.textureId;
    texture.width = static_cast<int>(tex.width);
    texture.height = static_cast<int>(tex.height);
    texture.mipmaps = 1;
    texture.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    return texture;
}

void ImGuiService::ReleaseTexture(ImGuiTextureHandle handle) {
    std::lock_guard lock(texturesMutex_);
    auto it = textures_.find(handle.id);

    if (it == textures_.end()) {
        return;
    }

    if (raylib_ && raylib_->initialized && it->second.hasTexture && it->second.textureId != 0) {
        Texture2D tex{};
        tex.id = it->second.textureId;
        tex.width = static_cast<int>(it->second.width);
        tex.height = static_cast<int>(it->second.height);
        tex.mipmaps = 1;
        tex.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        UnloadTexture(tex);
    }
    it->second.textureId = 0;
    it->second.hasTexture = false;

    LOG_INFO("ImGuiService::ReleaseTexture: released texture (id={})", handle.id);
    textures_.erase(it);
}

bool ImGuiService::IsTextureValid(ImGuiTextureHandle handle) const {
    uint32_t currentGen = deviceGeneration_.load(std::memory_order_acquire);
    if (handle.generation != currentGen) {
        return false;
    }

    if (deviceLost_) {
        return false;
    }

    std::lock_guard lock(texturesMutex_);
    return textures_.find(handle.id) != textures_.end();
}

void ImGuiService::OnDeviceLost_() {
    deviceLost_ = true;

    // Invalidate all textures
    {
        std::lock_guard lock(texturesMutex_);
        const bool canUnload = raylib_ && raylib_->initialized;
        for (auto& tex : textures_ | std::views::values) {
            if (canUnload && tex.hasTexture && tex.textureId != 0) {
                Texture2D texture{};
                texture.id = tex.textureId;
                texture.width = static_cast<int>(tex.width);
                texture.height = static_cast<int>(tex.height);
                texture.mipmaps = 1;
                texture.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                UnloadTexture(texture);
            }
            tex.textureId = 0;
            tex.hasTexture = false;
            tex.needsRecreation = true;
        }
    }
    ReleaseOverlaySurface_();

    {
        std::lock_guard lock(texturesMutex_);
        LOG_WARN("ImGuiService::OnDeviceLost_: device lost, invalidated {} texture(s)", textures_.size());
    }
}

void ImGuiService::OnDeviceRestored_() {
    deviceLost_ = false;

    // Increment device generation to invalidate old handles
    uint32_t newGen = deviceGeneration_.fetch_add(1, std::memory_order_release) + 1;

    LOG_INFO("ImGuiService::OnDeviceRestored_: device restored (new gen={}), textures will recreate on-demand", newGen);
}

void ImGuiService::InvalidateAllTextures_() {
    std::lock_guard lock(texturesMutex_);
    for (auto& tex : textures_ | std::views::values) {
        if (raylib_ && raylib_->initialized && tex.hasTexture && tex.textureId != 0) {
            Texture2D texture{};
            texture.id = tex.textureId;
            texture.width = static_cast<int>(tex.width);
            texture.height = static_cast<int>(tex.height);
            texture.mipmaps = 1;
            texture.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            UnloadTexture(texture);
        }
        tex.textureId = 0;
        tex.hasTexture = false;
        tex.needsRecreation = true;
    }
}

bool ImGuiService::InitializeRaylib_(HWND hwnd) {
    if (!raylib_) {
        raylib_ = std::make_unique<RaylibOverlay>();
    }
    if (raylib_->initialized) {
        return true;
    }

    RECT rect{};
    if (!GetClientRect(hwnd, &rect)) {
        return false;
    }
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return false;
    }

    SetTraceLogLevel(LOG_NONE);
    SetConfigFlags(FLAG_WINDOW_HIDDEN | FLAG_WINDOW_UNDECORATED);
    InitWindow(width, height, "SC4 ImGui Hidden");
    if (!IsWindowReady() || GetWindowHandle() == nullptr) {
        if (IsWindowReady()) {
            CloseWindow();
        }
        return false;
    }

    raylib_->window = static_cast<HWND>(GetWindowHandle());
    raylib_->renderTarget = LoadRenderTexture(width, height);
    if (raylib_->renderTarget.id == 0) {
        if (IsWindowReady()) {
            CloseWindow();
        }
        return false;
    }
    raylib_->width = width;
    raylib_->height = height;

    rlImGuiSetup(true);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    raylib_->lastFrameTime = std::chrono::steady_clock::now();
    raylib_->initialized = true;
    AlignRaylibWindow_(hwnd, rect);
    return true;
}

void ImGuiService::ShutdownRaylib_() {
    if (!raylib_) {
        return;
    }

    ReleaseOverlaySurface_();
    if (raylib_->initialized) {
        rlImGuiShutdown();
        if (raylib_->renderTarget.id != 0) {
            UnloadRenderTexture(raylib_->renderTarget);
        }
        if (IsWindowReady()) {
            CloseWindow();
        }
        raylib_->initialized = false;
    }
    raylib_.reset();
}

bool ImGuiService::EnsureRaylibTarget_(HWND hwnd) {
    if (!raylib_ || !raylib_->initialized) {
        return InitializeRaylib_(hwnd);
    }
    if (!IsWindowReady() || GetWindowHandle() == nullptr) {
        ShutdownRaylib_();
        return InitializeRaylib_(hwnd);
    }

    RECT rect{};
    if (!GetClientRect(hwnd, &rect)) {
        return false;
    }
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return false;
    }

    if (width == raylib_->width && height == raylib_->height) {
        AlignRaylibWindow_(hwnd, rect);
        return true;
    }

    if (!IsWindowReady()) {
        return false;
    }
    SetWindowSize(width, height);
    if (raylib_->renderTarget.id != 0) {
        UnloadRenderTexture(raylib_->renderTarget);
    }
    raylib_->renderTarget = LoadRenderTexture(width, height);
    if (raylib_->renderTarget.id == 0) {
        return false;
    }
    raylib_->width = width;
    raylib_->height = height;
    ReleaseOverlaySurface_();
    AlignRaylibWindow_(hwnd, rect);
    return true;
}

bool ImGuiService::AlignRaylibWindow_(HWND hwnd, const RECT& clientRect) {
    if (!raylib_ || !raylib_->initialized || !raylib_->window) {
        return false;
    }
    const int width = clientRect.right - clientRect.left;
    const int height = clientRect.bottom - clientRect.top;
    if (width <= 0 || height <= 0) {
        return false;
    }
    POINT origin{clientRect.left, clientRect.top};
    if (!ClientToScreen(hwnd, &origin)) {
        return false;
    }

    const bool moved = !raylib_->hasWindowOrigin || origin.x != raylib_->origin.x || origin.y != raylib_->origin.y;
    if (!moved) {
        return true;
    }

    SetWindowPos(raylib_->window, HWND_TOPMOST, origin.x, origin.y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    raylib_->origin = origin;
    raylib_->hasWindowOrigin = true;
    return true;
}

bool ImGuiService::AnyPanelVisible_() const {
    std::lock_guard lock(panelsMutex_);
    for (const auto& panel : panels_) {
        if (panel.desc.visible) {
            return true;
        }
    }
    return false;
}

bool ImGuiService::UploadRaylibFrame_(IDirect3DDevice7* device, IDirectDraw7* dd) {
    if (!raylib_ || !raylib_->initialized || !device || !dd) {
        return false;
    }

    const int width = raylib_->width;
    const int height = raylib_->height;
    if (width <= 0 || height <= 0) {
        return false;
    }

    Image image = LoadImageFromTexture(raylib_->renderTarget.texture);
    if (!image.data) {
        return false;
    }
    if (image.width != width || image.height != height) {
        UnloadImage(image);
        return false;
    }

    const uint32_t currentGen = deviceGeneration_.load(std::memory_order_acquire);
    if (raylib_->overlaySurface && raylib_->overlaySurface->IsLost() == DDERR_SURFACELOST) {
        ReleaseOverlaySurface_();
    }
    if (!raylib_->overlaySurface || raylib_->overlaySurfaceGeneration != currentGen) {
        ReleaseOverlaySurface_();

        DDSURFACEDESC2 ddsd{};
        ddsd.dwSize = sizeof(ddsd);
        ddsd.dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
        ddsd.dwWidth = static_cast<DWORD>(width);
        ddsd.dwHeight = static_cast<DWORD>(height);
        ddsd.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY;
        ddsd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        ddsd.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
        ddsd.ddpfPixelFormat.dwRGBBitCount = 32;
        ddsd.ddpfPixelFormat.dwRBitMask = 0x00FF0000;
        ddsd.ddpfPixelFormat.dwGBitMask = 0x0000FF00;
        ddsd.ddpfPixelFormat.dwBBitMask = 0x000000FF;
        ddsd.ddpfPixelFormat.dwRGBAlphaBitMask = 0xFF000000;

        IDirectDrawSurface7* surface = nullptr;
        HRESULT hr = dd->CreateSurface(&ddsd, &surface, nullptr);
        if (hr == DDERR_OUTOFVIDEOMEMORY) {
            ddsd.ddsCaps.dwCaps &= ~DDSCAPS_VIDEOMEMORY;
            ddsd.ddsCaps.dwCaps |= DDSCAPS_SYSTEMMEMORY;
            hr = dd->CreateSurface(&ddsd, &surface, nullptr);
        }
        if (FAILED(hr) || !surface) {
            LOG_ERROR("ImGuiService::UploadRaylibFrame_: CreateSurface failed (hr=0x{:08X})", hr);
            UnloadImage(image);
            return false;
        }
        raylib_->overlaySurface = surface;
        raylib_->overlaySurfaceGeneration = currentGen;
    }

    DDSURFACEDESC2 lockDesc{};
    lockDesc.dwSize = sizeof(lockDesc);
    HRESULT hr = raylib_->overlaySurface->Lock(nullptr, &lockDesc, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr);
    if (FAILED(hr)) {
        LOG_WARN("ImGuiService::UploadRaylibFrame_: Lock failed (hr=0x{:08X})", hr);
        ReleaseOverlaySurface_();
        UnloadImage(image);
        return false;
    }

    const auto* srcPixels = static_cast<const uint8_t*>(image.data);
    auto* dstPixels = static_cast<uint8_t*>(lockDesc.lpSurface);
    const uint32_t srcPitch = static_cast<uint32_t>(width) * 4;
    const uint32_t dstPitch = lockDesc.lPitch;

    for (int y = 0; y < height; ++y) {
        const int srcY = height - 1 - y;
        const uint8_t* srcRow = srcPixels + srcY * srcPitch;
        uint8_t* dstRow = dstPixels + y * dstPitch;
        for (int x = 0; x < width; ++x) {
            const uint8_t r = srcRow[x * 4 + 0];
            const uint8_t g = srcRow[x * 4 + 1];
            const uint8_t b = srcRow[x * 4 + 2];
            const uint8_t a = srcRow[x * 4 + 3];
            dstRow[x * 4 + 0] = b;
            dstRow[x * 4 + 1] = g;
            dstRow[x * 4 + 2] = r;
            dstRow[x * 4 + 3] = a;
        }
    }

    raylib_->overlaySurface->Unlock(nullptr);
    UnloadImage(image);

    Dx7OverlayStateRestore stateRestore(device);
    device->SetRenderState(D3DRENDERSTATE_ZENABLE, FALSE);
    device->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
    device->SetRenderState(D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
    device->SetRenderState(D3DRENDERSTATE_FOGENABLE, FALSE);
    device->SetRenderState(D3DRENDERSTATE_CLIPPING, TRUE);
    device->SetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);

    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTFN_LINEAR);
    device->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTFG_LINEAR);
    device->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTFP_POINT);
    device->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);

    device->SetTexture(0, raylib_->overlaySurface);

    struct ScreenVertex {
        float x, y, z, rhw;
        DWORD color;
        float u, v;
    };
    constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

    const float left = -0.5f;
    const float top = -0.5f;
    const float right = static_cast<float>(width) - 0.5f;
    const float bottom = static_cast<float>(height) - 0.5f;

    ScreenVertex verts[4] = {
        { left,  top,    0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 0.0f },
        { right, top,    0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 0.0f },
        { left,  bottom, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 1.0f },
        { right, bottom, 0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 1.0f }
    };

    device->DrawPrimitive(D3DPT_TRIANGLESTRIP, kFvf, verts, 4, 0);
    return true;
}

void ImGuiService::ReleaseOverlaySurface_() {
    if (!raylib_) {
        return;
    }
    if (raylib_->overlaySurface) {
        raylib_->overlaySurface->Release();
        raylib_->overlaySurface = nullptr;
    }
    raylib_->overlaySurfaceGeneration = 0;
}
