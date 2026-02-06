#include "cIGZFrameWork.h"
#include "cRZCOMDllDirector.h"

#include "imgui.h"
#include "public/ImGuiPanelAdapter.h"
#include "public/ImGuiServiceIds.h"
#include "public/cIGZDrawService.h"
#include "public/cIGZImGuiService.h"
#include "utils/Logger.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <d3d.h>
#include <d3dtypes.h>
#include <vector>
#include <ddraw.h>
#include <windows.h>

namespace {
    constexpr auto kDrawSampleDirectorID = 0xC49D82A7;
    constexpr uint32_t kDrawSamplePanelId = 0x8A32F41C;

    enum class DrawPass : uint8_t {
        Draw = 0,
        PreStatic,
        Static,
        PostStatic,
        PreDynamic,
        Dynamic,
        PostDynamic,
        Count
    };

    enum class WorldDepthOverlayPass : int {
        Static = 0,
        PreDynamic = 1,
        Dynamic = 2,
        PostDynamic = 3
    };

    constexpr size_t kDrawPassCount = static_cast<size_t>(DrawPass::Count);
    constexpr size_t kHookByteCount = 5;
    constexpr size_t kCallSitePatchCount = 9;
    constexpr size_t kEventRingCapacity = 2048;

    struct HookEvent {
        uint64_t seq;
        DrawPass pass;
        bool begin;
        uint32_t tickMs;
    };

    struct InlineHook {
        const char* name;
        uintptr_t address;
        uintptr_t patchAddress = 0;
        void* hookFn;
        uint8_t original[kHookByteCount]{};
        void* trampoline = nullptr;
        bool installed = false;
    };

    struct CallSitePatch {
        const char* name;
        DrawPass pass;
        uintptr_t callSiteAddress;
        uintptr_t originalTarget = 0;
        int32_t originalRel = 0;
        void* hookFn;
        bool installed = false;
    };

    std::atomic<uint64_t> gEventSeq{0};
    std::array<HookEvent, kEventRingCapacity> gEventRing{};
    std::array<std::atomic<uint32_t>, kDrawPassCount> gBeginCounts{};
    std::array<std::atomic<uint32_t>, kDrawPassCount> gEndCounts{};

    uint32_t(__thiscall* gOrigDraw)(void*) = nullptr;
    void(__thiscall* gOrigPreStatic)(void*) = nullptr;
    void(__thiscall* gOrigStatic)(void*) = nullptr;
    void(__thiscall* gOrigPostStatic)(void*) = nullptr;
    void(__thiscall* gOrigPreDynamic)(void*) = nullptr;
    void(__thiscall* gOrigDynamic)(void*) = nullptr;
    void(__thiscall* gOrigPostDynamic)(void*) = nullptr;
    std::atomic<bool> gEnablePreDynamicDepthLayeredOverlay{false};
    std::atomic<int> gPreDynamicDepthOffset{-8};
    std::atomic<bool> gEnablePostDynamicDebugBox{false};
    std::atomic<bool> gEnablePostDynamicD3D7Overlay{false};
    std::atomic<bool> gEnableStaticD3D7DepthOverlay{false};
    std::atomic<int> gStaticD3D7DepthOverlayPass{static_cast<int>(WorldDepthOverlayPass::Dynamic)};
    std::atomic<int> gStaticD3D7ZBias{1};
    std::atomic<float> gStaticOverlayWorldX{1024.0f};
    std::atomic<float> gStaticOverlayWorldY{270.0f};
    std::atomic<float> gStaticOverlayWorldZ{1024.0f};
    std::atomic<uint32_t> gLastD3D7OverlayErrorLogTick{0};
    std::atomic<cIGZImGuiService*> gImGuiServiceForD3DOverlay{nullptr};

    void* (__thiscall* gGetDrawContext)(void*) =
        reinterpret_cast<void* (__thiscall*)(void*)>(0x004E82A0);
    void (__thiscall* gDrawBoundingBox)(void*, float*, const void*) =
        reinterpret_cast<void (__thiscall*)(void*, float*, const void*)>(0x007D5030);
    void (__fastcall* gSetDefaultRenderStateUnilaterally)(void*) =
        reinterpret_cast<void (__fastcall*)(void*)>(0x007D5230);
    void (__thiscall* gEnableDepthTestFlag)(void*, char) =
        reinterpret_cast<void (__thiscall*)(void*, char)>(0x007D27B0);
    void (__thiscall* gEnableDepthMaskFlag)(void*, bool) =
        reinterpret_cast<void (__thiscall*)(void*, bool)>(0x007D2800);
    void (__thiscall* gEnableBlendStateFlag)(void*, char) =
        reinterpret_cast<void (__thiscall*)(void*, char)>(0x007D4010);
    void (__thiscall* gSetBlendFunc)(void*, uint32_t, uint32_t) =
        reinterpret_cast<void (__thiscall*)(void*, uint32_t, uint32_t)>(0x007D28F0);
    void (__thiscall* gSetDepthFunc)(void*, uint32_t) =
        reinterpret_cast<void (__thiscall*)(void*, uint32_t)>(0x007D28A0);
    void (__thiscall* gSetDepthOffset)(void*, int) =
        reinterpret_cast<void (__thiscall*)(void*, int)>(0x007D4480);

    struct D3D7StateGuard {
        explicit D3D7StateGuard(IDirect3DDevice7* deviceIn)
            : device(deviceIn) {
            if (!device) {
                return;
            }
            okZEnable = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ZENABLE, &zEnable));
            okZWrite = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ZWRITEENABLE, &zWrite));
            okLighting = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_LIGHTING, &lighting));
            okAlphaBlend = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, &alphaBlend));
            okSrcBlend = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_SRCBLEND, &srcBlend));
            okDstBlend = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_DESTBLEND, &dstBlend));
            okCullMode = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_CULLMODE, &cullMode));
            okZBias = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ZBIAS, &zBias));
            okTexture0 = SUCCEEDED(device->GetTexture(0, &texture0));
            okTss0ColorOp = SUCCEEDED(device->GetTextureStageState(0, D3DTSS_COLOROP, &tss0ColorOp));
            okTss0ColorArg1 = SUCCEEDED(device->GetTextureStageState(0, D3DTSS_COLORARG1, &tss0ColorArg1));
            okTss0AlphaOp = SUCCEEDED(device->GetTextureStageState(0, D3DTSS_ALPHAOP, &tss0AlphaOp));
            okTss0AlphaArg1 = SUCCEEDED(device->GetTextureStageState(0, D3DTSS_ALPHAARG1, &tss0AlphaArg1));
            okTss1ColorOp = SUCCEEDED(device->GetTextureStageState(1, D3DTSS_COLOROP, &tss1ColorOp));
            okTss1AlphaOp = SUCCEEDED(device->GetTextureStageState(1, D3DTSS_ALPHAOP, &tss1AlphaOp));
        }

        ~D3D7StateGuard() {
            if (!device) {
                return;
            }
            if (okZEnable) {
                device->SetRenderState(D3DRENDERSTATE_ZENABLE, zEnable);
            }
            if (okZWrite) {
                device->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, zWrite);
            }
            if (okLighting) {
                device->SetRenderState(D3DRENDERSTATE_LIGHTING, lighting);
            }
            if (okAlphaBlend) {
                device->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, alphaBlend);
            }
            if (okSrcBlend) {
                device->SetRenderState(D3DRENDERSTATE_SRCBLEND, srcBlend);
            }
            if (okDstBlend) {
                device->SetRenderState(D3DRENDERSTATE_DESTBLEND, dstBlend);
            }
            if (okCullMode) {
                device->SetRenderState(D3DRENDERSTATE_CULLMODE, cullMode);
            }
            if (okZBias) {
                device->SetRenderState(D3DRENDERSTATE_ZBIAS, zBias);
            }
            if (okTss0ColorOp) {
                device->SetTextureStageState(0, D3DTSS_COLOROP, tss0ColorOp);
            }
            if (okTss0ColorArg1) {
                device->SetTextureStageState(0, D3DTSS_COLORARG1, tss0ColorArg1);
            }
            if (okTss0AlphaOp) {
                device->SetTextureStageState(0, D3DTSS_ALPHAOP, tss0AlphaOp);
            }
            if (okTss0AlphaArg1) {
                device->SetTextureStageState(0, D3DTSS_ALPHAARG1, tss0AlphaArg1);
            }
            if (okTss1ColorOp) {
                device->SetTextureStageState(1, D3DTSS_COLOROP, tss1ColorOp);
            }
            if (okTss1AlphaOp) {
                device->SetTextureStageState(1, D3DTSS_ALPHAOP, tss1AlphaOp);
            }
            device->SetTexture(0, texture0);
            if (texture0) {
                texture0->Release();
            }
        }

        IDirect3DDevice7* device = nullptr;
        bool okZEnable = false;
        bool okZWrite = false;
        bool okLighting = false;
        bool okAlphaBlend = false;
        bool okSrcBlend = false;
        bool okDstBlend = false;
        bool okCullMode = false;
        bool okZBias = false;
        bool okTexture0 = false;
        bool okTss0ColorOp = false;
        bool okTss0ColorArg1 = false;
        bool okTss0AlphaOp = false;
        bool okTss0AlphaArg1 = false;
        bool okTss1ColorOp = false;
        bool okTss1AlphaOp = false;
        DWORD zEnable = 0;
        DWORD zWrite = 0;
        DWORD lighting = 0;
        DWORD alphaBlend = 0;
        DWORD srcBlend = 0;
        DWORD dstBlend = 0;
        DWORD cullMode = 0;
        DWORD zBias = 0;
        DWORD tss0ColorOp = 0;
        DWORD tss0ColorArg1 = 0;
        DWORD tss0AlphaOp = 0;
        DWORD tss0AlphaArg1 = 0;
        DWORD tss1ColorOp = 0;
        DWORD tss1AlphaOp = 0;
        IDirectDrawSurface7* texture0 = nullptr;
    };

    struct Dx7DebugVertex {
        float x;
        float y;
        float z;
        float rhw;
        DWORD diffuse;
    };

    struct Dx7WorldOverlayVertex {
        float x;
        float y;
        float z;
        DWORD diffuse;
    };

    void DrawStaticD3D7DepthOverlay() {
        auto* imguiService = gImGuiServiceForD3DOverlay.load(std::memory_order_acquire);
        if (!imguiService) {
            return;
        }

        IDirect3DDevice7* device = nullptr;
        IDirectDraw7* dd = nullptr;
        if (!imguiService->AcquireD3DInterfaces(&device, &dd)) {
            return;
        }
        if (dd) {
            dd->Release();
            dd = nullptr;
        }
        if (!device) {
            return;
        }

        {
            D3D7StateGuard state(device);

            device->SetTexture(0, nullptr);
            device->SetRenderState(D3DRENDERSTATE_ZENABLE, TRUE);
            device->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
            device->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
            device->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
            device->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
            device->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
            device->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
            device->SetRenderState(D3DRENDERSTATE_ZBIAS, gStaticD3D7ZBias.load(std::memory_order_relaxed));
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

            const float pulse = static_cast<float>((GetTickCount() / 120) % 8) / 7.0f;
            const DWORD color = D3DRGBA(1.0f, 0.15f + 0.70f * pulse, 0.10f, 0.65f);

            const float centerX = gStaticOverlayWorldX.load(std::memory_order_relaxed);
            const float centerY = gStaticOverlayWorldY.load(std::memory_order_relaxed);
            const float centerZ = gStaticOverlayWorldZ.load(std::memory_order_relaxed);
            constexpr float half = 120.0f;
            Dx7WorldOverlayVertex verts[] = {
                {centerX - half, centerY, centerZ - half, color},
                {centerX + half, centerY, centerZ - half, color},
                {centerX + half, centerY, centerZ + half, color},
                {centerX - half, centerY, centerZ - half, color},
                {centerX + half, centerY, centerZ + half, color},
                {centerX - half, centerY, centerZ + half, color},
            };

            const HRESULT hr = device->DrawPrimitive(D3DPT_TRIANGLELIST,
                                                     D3DFVF_XYZ | D3DFVF_DIFFUSE,
                                                     verts,
                                                     6,
                                                     D3DDP_WAIT);
            if (FAILED(hr)) {
                const uint32_t now = GetTickCount();
                const uint32_t last = gLastD3D7OverlayErrorLogTick.load(std::memory_order_relaxed);
                if (now - last > 1000) {
                    gLastD3D7OverlayErrorLogTick.store(now, std::memory_order_relaxed);
                    LOG_WARN("DrawServiceSample: static depth overlay DrawPrimitive failed hr=0x{:08X}",
                             static_cast<uint32_t>(hr));
                }
            }
        }
        device->Release();
    }

    bool ShouldDrawWorldDepthOverlayInPass(const DrawPass pass) {
        const auto configuredPass = static_cast<WorldDepthOverlayPass>(
            gStaticD3D7DepthOverlayPass.load(std::memory_order_relaxed));
        switch (configuredPass) {
        case WorldDepthOverlayPass::Static:
            return pass == DrawPass::Static;
        case WorldDepthOverlayPass::PreDynamic:
            return pass == DrawPass::PreDynamic;
        case WorldDepthOverlayPass::Dynamic:
            return pass == DrawPass::Dynamic;
        case WorldDepthOverlayPass::PostDynamic:
            return pass == DrawPass::PostDynamic;
        default:
            return pass == DrawPass::Dynamic;
        }
    }

    void DrawD3D7OverlayLines() {
        auto* imguiService = gImGuiServiceForD3DOverlay.load(std::memory_order_acquire);
        if (!imguiService) {
            return;
        }

        IDirect3DDevice7* device = nullptr;
        IDirectDraw7* dd = nullptr;
        if (!imguiService->AcquireD3DInterfaces(&device, &dd)) {
            return;
        }
        if (dd) {
            dd->Release();
            dd = nullptr;
        }
        if (!device) {
            return;
        }

        D3DVIEWPORT7 vp{};
        if (FAILED(device->GetViewport(&vp)) || vp.dwWidth == 0 || vp.dwHeight == 0) {
            device->Release();
            return;
        }

        {
            D3D7StateGuard state(device);

            device->SetTexture(0, nullptr);
            device->SetRenderState(D3DRENDERSTATE_ZENABLE, FALSE);
            device->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
            device->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
            device->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
            device->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
            device->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
            device->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

            const float pulse = static_cast<float>((GetTickCount() / 120) % 8) / 7.0f;
            const int red = static_cast<int>(220.0f + 35.0f * pulse);
            const int green = static_cast<int>(80.0f + 150.0f * pulse);
            const DWORD color = D3DRGBA(static_cast<float>(red) / 255.0f,
                                        static_cast<float>(green) / 255.0f,
                                        0.10f,
                                        0.90f);

            const float left = static_cast<float>(vp.dwX) + 32.0f;
            const float top = static_cast<float>(vp.dwY) + 32.0f;
            const float right = static_cast<float>(vp.dwX + vp.dwWidth) - 32.0f;
            const float bottom = static_cast<float>(vp.dwY + vp.dwHeight) - 32.0f;

            Dx7DebugVertex verts[] = {
                {left, top, 0.0f, 1.0f, color}, {right, top, 0.0f, 1.0f, color},
                {right, top, 0.0f, 1.0f, color}, {right, bottom, 0.0f, 1.0f, color},
                {right, bottom, 0.0f, 1.0f, color}, {left, bottom, 0.0f, 1.0f, color},
                {left, bottom, 0.0f, 1.0f, color}, {left, top, 0.0f, 1.0f, color},
                // diagonal for easy confirmation this is not the bbox path
                {left, top, 0.0f, 1.0f, color}, {right, bottom, 0.0f, 1.0f, color},
            };

            const HRESULT hr = device->DrawPrimitive(D3DPT_LINELIST,
                                                     D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
                                                     verts,
                                                     10,
                                                     D3DDP_WAIT);
            if (FAILED(hr)) {
                const uint32_t now = GetTickCount();
                const uint32_t last = gLastD3D7OverlayErrorLogTick.load(std::memory_order_relaxed);
                if (now - last > 1000) {
                    gLastD3D7OverlayErrorLogTick.store(now, std::memory_order_relaxed);
                    LOG_WARN("DrawServiceSample: D3D7 DrawPrimitive failed hr=0x{:08X}", static_cast<uint32_t>(hr));
                }
            }
        }
        device->Release();
    }

    void DrawPreDynamicDepthLayeredOverlay(void* renderer) {
        if (!renderer || !gGetDrawContext || !gDrawBoundingBox) {
            return;
        }

        void* drawContext = gGetDrawContext(renderer);
        if (!drawContext) {
            return;
        }

        // Build a predictable debug state, then draw a terrain-spanning slab with depth test enabled.
        if (gSetDefaultRenderStateUnilaterally) {
            gSetDefaultRenderStateUnilaterally(drawContext);
        }
        if (gEnableDepthTestFlag) {
            gEnableDepthTestFlag(drawContext, 1);
        }
        if (gEnableDepthMaskFlag) {
            gEnableDepthMaskFlag(drawContext, false);
        }
        if (gEnableBlendStateFlag) {
            gEnableBlendStateFlag(drawContext, 1);
        }
        if (gSetBlendFunc) {
            gSetBlendFunc(drawContext, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA);
        }
        if (gSetDepthFunc) {
            gSetDepthFunc(drawContext, D3DCMP_LESSEQUAL);
        }
        if (gSetDepthOffset) {
            gSetDepthOffset(drawContext, gPreDynamicDepthOffset.load(std::memory_order_relaxed));
        }

        float bbox[6] = {-20000.0f, -20000.0f, -16.0f, 20000.0f, 20000.0f, 16.0f};
        float color[4] = {0.10f, 1.00f, 0.25f, 0.60f};
        gDrawBoundingBox(drawContext, bbox, color);
    }

    const char* PassName(const DrawPass pass) {
        switch (pass) {
        case DrawPass::Draw: return "Draw";
        case DrawPass::PreStatic: return "PreStatic";
        case DrawPass::Static: return "Static";
        case DrawPass::PostStatic: return "PostStatic";
        case DrawPass::PreDynamic: return "PreDynamic";
        case DrawPass::Dynamic: return "Dynamic";
        case DrawPass::PostDynamic: return "PostDynamic";
        default: return "Unknown";
        }
    }

    void RecordHookEvent(const DrawPass pass, const bool begin) {
        const uint64_t seq = gEventSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
        auto& slot = gEventRing[seq % kEventRingCapacity];
        slot = HookEvent{seq, pass, begin, GetTickCount()};

        const size_t passIndex = static_cast<size_t>(pass);
        if (begin) {
            gBeginCounts[passIndex].fetch_add(1, std::memory_order_relaxed);
        } else {
            gEndCounts[passIndex].fetch_add(1, std::memory_order_relaxed);
        }
    }

    uint32_t __fastcall HookDraw(void* self, void*) {
        RecordHookEvent(DrawPass::Draw, true);
        const uint32_t result = gOrigDraw ? gOrigDraw(self) : 0;
        RecordHookEvent(DrawPass::Draw, false);
        return result;
    }

    void __fastcall HookPreStatic(void* self, void*) {
        RecordHookEvent(DrawPass::PreStatic, true);
        if (gOrigPreStatic) {
            gOrigPreStatic(self);
        }
        RecordHookEvent(DrawPass::PreStatic, false);
    }

    void __fastcall HookStatic(void* self, void*) {
        RecordHookEvent(DrawPass::Static, true);
        if (gOrigStatic) {
            gOrigStatic(self);
        }
        if (gEnableStaticD3D7DepthOverlay.load(std::memory_order_relaxed) &&
            ShouldDrawWorldDepthOverlayInPass(DrawPass::Static)) {
            DrawStaticD3D7DepthOverlay();
        }
        RecordHookEvent(DrawPass::Static, false);
    }

    void __fastcall HookPostStatic(void* self, void*) {
        RecordHookEvent(DrawPass::PostStatic, true);
        if (gOrigPostStatic) {
            gOrigPostStatic(self);
        }
        RecordHookEvent(DrawPass::PostStatic, false);
    }

    void __fastcall HookPreDynamic(void* self, void*) {
        RecordHookEvent(DrawPass::PreDynamic, true);
        if (gEnablePreDynamicDepthLayeredOverlay.load(std::memory_order_relaxed)) {
            DrawPreDynamicDepthLayeredOverlay(self);
        }
        if (gOrigPreDynamic) {
            gOrigPreDynamic(self);
        }
        if (gEnableStaticD3D7DepthOverlay.load(std::memory_order_relaxed) &&
            ShouldDrawWorldDepthOverlayInPass(DrawPass::PreDynamic)) {
            DrawStaticD3D7DepthOverlay();
        }
        RecordHookEvent(DrawPass::PreDynamic, false);
    }

    void __fastcall HookDynamic(void* self, void*) {
        RecordHookEvent(DrawPass::Dynamic, true);
        if (gOrigDynamic) {
            gOrigDynamic(self);
        }
        if (gEnableStaticD3D7DepthOverlay.load(std::memory_order_relaxed) &&
            ShouldDrawWorldDepthOverlayInPass(DrawPass::Dynamic)) {
            DrawStaticD3D7DepthOverlay();
        }
        RecordHookEvent(DrawPass::Dynamic, false);
    }

    void __fastcall HookPostDynamic(void* self, void*) {
        RecordHookEvent(DrawPass::PostDynamic, true);
        if (gOrigPostDynamic) {
            gOrigPostDynamic(self);
        }
        if (gEnableStaticD3D7DepthOverlay.load(std::memory_order_relaxed) &&
            ShouldDrawWorldDepthOverlayInPass(DrawPass::PostDynamic)) {
            DrawStaticD3D7DepthOverlay();
        }
        if (gEnablePostDynamicDebugBox.load(std::memory_order_relaxed) &&
            gGetDrawContext && gDrawBoundingBox) {
            void* drawContext = gGetDrawContext(self);
            if (drawContext) {
                const float pulse = static_cast<float>((GetTickCount() / 150) % 8) / 7.0f;
                float bbox[6] = {-20000.0f, -20000.0f, -500.0f, 20000.0f, 20000.0f, 500.0f};
                float color[4] = {1.0f, 0.15f + 0.70f * pulse, 0.10f, 0.85f};
                gDrawBoundingBox(drawContext, bbox, color);
                if (gSetDefaultRenderStateUnilaterally) {
                    gSetDefaultRenderStateUnilaterally(drawContext);
                }
            }
        }
        if (gEnablePostDynamicD3D7Overlay.load(std::memory_order_relaxed)) {
            DrawD3D7OverlayLines();
        }
        RecordHookEvent(DrawPass::PostDynamic, false);
    }

    InlineHook gDrawHook{
        "cSC43DRender::Draw",
        0x007CB530,
        0,
        reinterpret_cast<void*>(&HookDraw)
    };

    std::array<CallSitePatch, kCallSitePatchCount> gCallSitePatches{{
        {"Draw::DrawPreStaticView_ [A]", DrawPass::PreStatic, 0x007CB770, 0, 0, reinterpret_cast<void*>(&HookPreStatic)},
        {"Draw::DrawStaticView_ [A]", DrawPass::Static, 0x007CB777, 0, 0, reinterpret_cast<void*>(&HookStatic)},
        {"Draw::DrawPostStaticView_ [A]", DrawPass::PostStatic, 0x007CB77E, 0, 0, reinterpret_cast<void*>(&HookPostStatic)},
        {"Draw::DrawPreStaticView_ [B]", DrawPass::PreStatic, 0x007CB82A, 0, 0, reinterpret_cast<void*>(&HookPreStatic)},
        {"Draw::DrawStaticView_ [B]", DrawPass::Static, 0x007CB831, 0, 0, reinterpret_cast<void*>(&HookStatic)},
        {"Draw::DrawPostStaticView_ [B]", DrawPass::PostStatic, 0x007CB838, 0, 0, reinterpret_cast<void*>(&HookPostStatic)},
        {"Draw::DrawPreDynamicView_", DrawPass::PreDynamic, 0x007CB84C, 0, 0, reinterpret_cast<void*>(&HookPreDynamic)},
        {"Draw::DrawDynamicView_", DrawPass::Dynamic, 0x007CB853, 0, 0, reinterpret_cast<void*>(&HookDynamic)},
        {"Draw::DrawPostDynamicView_", DrawPass::PostDynamic, 0x007CB85A, 0, 0, reinterpret_cast<void*>(&HookPostDynamic)}
    }};

    uintptr_t ResolvePatchAddress(uintptr_t address) {
        // Resolve common x86 jump stubs (jmp rel32, jmp rel8, jmp [imm32]).
        // We follow a short chain so hooks patch real function bodies.
        uintptr_t current = address;
        for (int i = 0; i < 6; ++i) {
            const auto* p = reinterpret_cast<const uint8_t*>(current);
            const uint8_t op0 = p[0];

            if (op0 == 0xE9) {
                const auto rel = *reinterpret_cast<const int32_t*>(p + 1);
                current = current + 5 + rel;
                continue;
            }
            if (op0 == 0xEB) {
                const auto rel8 = static_cast<int8_t>(p[1]);
                current = current + 2 + rel8;
                continue;
            }
            if (op0 == 0xFF && p[1] == 0x25) {
                const auto mem = *reinterpret_cast<const uintptr_t*>(p + 2);
                current = *reinterpret_cast<const uintptr_t*>(mem);
                continue;
            }
            break;
        }
        return current;
    }

    bool ComputeRelativeCallTarget(const uintptr_t callSiteAddress,
                                   const uintptr_t targetAddress,
                                   int32_t& relOut) {
        const auto delta = static_cast<intptr_t>(targetAddress) - static_cast<intptr_t>(callSiteAddress + kHookByteCount);
        if (delta < static_cast<intptr_t>(INT32_MIN) || delta > static_cast<intptr_t>(INT32_MAX)) {
            return false;
        }
        relOut = static_cast<int32_t>(delta);
        return true;
    }

    bool InstallInlineHook(InlineHook& hook) {
        if (hook.installed) {
            return true;
        }

        hook.patchAddress = ResolvePatchAddress(hook.address);
        auto* target = reinterpret_cast<uint8_t*>(hook.patchAddress);
        if (!target) {
            LOG_ERROR("DrawServiceSample: resolved null patch target for {}", hook.name);
            return false;
        }
        std::memcpy(hook.original, target, kHookByteCount);

        auto* trampoline = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kHookByteCount + kHookByteCount, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        if (!trampoline) {
            LOG_ERROR("DrawServiceSample: failed to allocate trampoline for {}", hook.name);
            return false;
        }

        std::memcpy(trampoline, target, kHookByteCount);
        trampoline[kHookByteCount] = 0xE9;

        const auto trampolineJmpSrc = reinterpret_cast<intptr_t>(trampoline + kHookByteCount);
        const auto trampolineJmpDst = reinterpret_cast<intptr_t>(target + kHookByteCount);
        const auto trampolineRel = static_cast<int32_t>(trampolineJmpDst - (trampolineJmpSrc + kHookByteCount));
        std::memcpy(trampoline + kHookByteCount + 1, &trampolineRel, sizeof(trampolineRel));

        DWORD oldProtect = 0;
        if (!VirtualProtect(target, kHookByteCount, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LOG_ERROR("DrawServiceSample: VirtualProtect failed while installing {} at 0x{:08X}",
                      hook.name, static_cast<uint32_t>(hook.patchAddress));
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return false;
        }

        target[0] = 0xE9;
        const auto hookJmpSrc = reinterpret_cast<intptr_t>(target);
        const auto hookJmpDst = reinterpret_cast<intptr_t>(hook.hookFn);
        const auto hookRel = static_cast<int32_t>(hookJmpDst - (hookJmpSrc + kHookByteCount));
        std::memcpy(target + 1, &hookRel, sizeof(hookRel));

        FlushInstructionCache(GetCurrentProcess(), target, kHookByteCount);
        VirtualProtect(target, kHookByteCount, oldProtect, &oldProtect);

        hook.trampoline = trampoline;
        hook.installed = true;
        return true;
    }

    void UninstallInlineHook(InlineHook& hook) {
        if (!hook.installed) {
            return;
        }

        auto* target = reinterpret_cast<uint8_t*>(hook.patchAddress);

        DWORD oldProtect = 0;
        if (VirtualProtect(target, kHookByteCount, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            std::memcpy(target, hook.original, kHookByteCount);
            FlushInstructionCache(GetCurrentProcess(), target, kHookByteCount);
            VirtualProtect(target, kHookByteCount, oldProtect, &oldProtect);
        }

        if (hook.trampoline) {
            VirtualFree(hook.trampoline, 0, MEM_RELEASE);
        }

        hook.trampoline = nullptr;
        hook.patchAddress = 0;
        hook.installed = false;
    }

    bool InstallCallSitePatch(CallSitePatch& patch) {
        if (patch.installed) {
            return true;
        }

        auto* site = reinterpret_cast<uint8_t*>(patch.callSiteAddress);
        if (site[0] != 0xE8) {
            LOG_ERROR("DrawServiceSample: expected CALL rel32 at 0x{:08X} for {}",
                      static_cast<uint32_t>(patch.callSiteAddress), patch.name);
            return false;
        }

        std::memcpy(&patch.originalRel, site + 1, sizeof(patch.originalRel));
        patch.originalTarget = patch.callSiteAddress + kHookByteCount + patch.originalRel;

        int32_t newRel = 0;
        if (!ComputeRelativeCallTarget(patch.callSiteAddress, reinterpret_cast<uintptr_t>(patch.hookFn), newRel)) {
            LOG_ERROR("DrawServiceSample: rel32 range failure for {}", patch.name);
            return false;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(site + 1, sizeof(newRel), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LOG_ERROR("DrawServiceSample: VirtualProtect failed for {}", patch.name);
            return false;
        }

        std::memcpy(site + 1, &newRel, sizeof(newRel));
        FlushInstructionCache(GetCurrentProcess(), site, kHookByteCount);
        VirtualProtect(site + 1, sizeof(newRel), oldProtect, &oldProtect);

        patch.installed = true;
        return true;
    }

    void UninstallCallSitePatch(CallSitePatch& patch) {
        if (!patch.installed) {
            return;
        }

        auto* site = reinterpret_cast<uint8_t*>(patch.callSiteAddress);
        DWORD oldProtect = 0;
        if (VirtualProtect(site + 1, sizeof(patch.originalRel), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            std::memcpy(site + 1, &patch.originalRel, sizeof(patch.originalRel));
            FlushInstructionCache(GetCurrentProcess(), site, kHookByteCount);
            VirtualProtect(site + 1, sizeof(patch.originalRel), oldProtect, &oldProtect);
        }

        patch.installed = false;
    }

    DrawPass PassFromHookIndex(const size_t index) {
        return static_cast<DrawPass>(index);
    }

    const char* HookNameForIndex(const size_t index) {
        switch (PassFromHookIndex(index)) {
        case DrawPass::Draw: return "cSC43DRender::Draw";
        case DrawPass::PreStatic: return "DrawPreStaticView_ call sites";
        case DrawPass::Static: return "DrawStaticView_ call sites";
        case DrawPass::PostStatic: return "DrawPostStaticView_ call sites";
        case DrawPass::PreDynamic: return "DrawPreDynamicView_ call site";
        case DrawPass::Dynamic: return "DrawDynamicView_ call site";
        case DrawPass::PostDynamic: return "DrawPostDynamicView_ call site";
        default: return "Unknown hook";
        }
    }

    bool AreAllPassCallSitesInstalled(const DrawPass pass) {
        bool found = false;
        for (const auto& patch : gCallSitePatches) {
            if (patch.pass == pass) {
                found = true;
                if (!patch.installed) {
                    return false;
                }
            }
        }
        return found;
    }

    bool AreAnyPassCallSitesInstalled(const DrawPass pass) {
        for (const auto& patch : gCallSitePatches) {
            if (patch.pass == pass && patch.installed) {
                return true;
            }
        }
        return false;
    }

    uintptr_t ResolveOriginalPassTarget(const DrawPass pass) {
        for (const auto& patch : gCallSitePatches) {
            if (patch.pass == pass && patch.installed) {
                return patch.originalTarget;
            }
        }
        return 0;
    }

    void RefreshOriginalHookTargets() {
        gOrigDraw = gDrawHook.trampoline
                        ? reinterpret_cast<uint32_t(__thiscall*)(void*)>(gDrawHook.trampoline)
                        : nullptr;
        gOrigPreStatic = reinterpret_cast<void(__thiscall*)(void*)>(
                             ResolveOriginalPassTarget(DrawPass::PreStatic));
        gOrigStatic = reinterpret_cast<void(__thiscall*)(void*)>(
                          ResolveOriginalPassTarget(DrawPass::Static));
        gOrigPostStatic = reinterpret_cast<void(__thiscall*)(void*)>(
                              ResolveOriginalPassTarget(DrawPass::PostStatic));
        gOrigPreDynamic = reinterpret_cast<void(__thiscall*)(void*)>(
                              ResolveOriginalPassTarget(DrawPass::PreDynamic));
        gOrigDynamic = reinterpret_cast<void(__thiscall*)(void*)>(
                           ResolveOriginalPassTarget(DrawPass::Dynamic));
        gOrigPostDynamic = reinterpret_cast<void(__thiscall*)(void*)>(
                               ResolveOriginalPassTarget(DrawPass::PostDynamic));
    }

    bool IsHookInstalled(const size_t index) {
        if (index >= kDrawPassCount) {
            return false;
        }
        if (index == static_cast<size_t>(DrawPass::Draw)) {
            return gDrawHook.installed;
        }
        return AreAllPassCallSitesInstalled(PassFromHookIndex(index));
    }

    bool InstallPassCallSitePatches(const DrawPass pass) {
        bool ok = true;
        for (auto& patch : gCallSitePatches) {
            if (patch.pass != pass) {
                continue;
            }
            if (!InstallCallSitePatch(patch)) {
                ok = false;
                break;
            }
        }

        if (!ok) {
            for (auto& patch : gCallSitePatches) {
                if (patch.pass == pass) {
                    UninstallCallSitePatch(patch);
                }
            }
        }
        return ok;
    }

    void UninstallPassCallSitePatches(const DrawPass pass) {
        for (auto& patch : gCallSitePatches) {
            if (patch.pass == pass) {
                UninstallCallSitePatch(patch);
            }
        }
    }

    void UninstallDrawSequenceHooks();

    bool InstallSingleDrawSequenceHook(const size_t index) {
        if (index >= kDrawPassCount) {
            return false;
        }

        bool ok = false;
        if (index == static_cast<size_t>(DrawPass::Draw)) {
            ok = InstallInlineHook(gDrawHook);
            if (ok) {
                LOG_INFO("DrawServiceSample: installed hook {} entry=0x{:08X} patch=0x{:08X}",
                         gDrawHook.name,
                         static_cast<uint32_t>(gDrawHook.address),
                         static_cast<uint32_t>(gDrawHook.patchAddress));
            }
        } else {
            const auto pass = PassFromHookIndex(index);
            ok = InstallPassCallSitePatches(pass);
            if (ok) {
                LOG_INFO("DrawServiceSample: installed hook {}", HookNameForIndex(index));
            }
        }

        if (ok) {
            RefreshOriginalHookTargets();
        }
        return ok;
    }

    void RemoveSingleDrawSequenceHook(const size_t index) {
        if (index >= kDrawPassCount) {
            return;
        }

        if (index == static_cast<size_t>(DrawPass::Draw)) {
            if (gDrawHook.installed) {
                LOG_INFO("DrawServiceSample: removed hook {}", gDrawHook.name);
            }
            UninstallInlineHook(gDrawHook);
        } else {
            const auto pass = PassFromHookIndex(index);
            if (AreAnyPassCallSitesInstalled(pass)) {
                LOG_INFO("DrawServiceSample: removed hook {}", HookNameForIndex(index));
            }
            UninstallPassCallSitePatches(pass);
        }

        RefreshOriginalHookTargets();
    }

    bool InstallDrawSequenceHooks(const bool drawOnly) {
        if (!InstallSingleDrawSequenceHook(static_cast<size_t>(DrawPass::Draw))) {
            return false;
        }

        if (drawOnly) {
            return true;
        }

        for (size_t i = static_cast<size_t>(DrawPass::PreStatic); i < kDrawPassCount; ++i) {
            if (!InstallSingleDrawSequenceHook(i)) {
                UninstallDrawSequenceHooks();
                return false;
            }
        }
        return true;
    }

    void UninstallDrawSequenceHooks() {
        if (gDrawHook.installed) {
            LOG_INFO("DrawServiceSample: removed hook {}", gDrawHook.name);
        }
        UninstallInlineHook(gDrawHook);

        for (size_t i = static_cast<size_t>(DrawPass::PreStatic); i < kDrawPassCount; ++i) {
            const auto pass = PassFromHookIndex(i);
            if (AreAnyPassCallSitesInstalled(pass)) {
                LOG_INFO("DrawServiceSample: removed hook {}", HookNameForIndex(i));
            }
            UninstallPassCallSitePatches(pass);
        }

        gOrigDraw = nullptr;
        gOrigPreStatic = nullptr;
        gOrigStatic = nullptr;
        gOrigPostStatic = nullptr;
        gOrigPreDynamic = nullptr;
        gOrigDynamic = nullptr;
        gOrigPostDynamic = nullptr;
    }

    bool AreDrawSequenceHooksInstalled() {
        if (!gDrawHook.installed) {
            return false;
        }
        for (size_t i = static_cast<size_t>(DrawPass::PreStatic); i < kDrawPassCount; ++i) {
            if (!AreAllPassCallSitesInstalled(PassFromHookIndex(i))) {
                return false;
            }
        }
        return true;
    }

    bool AreAnyDrawSequenceHooksInstalled() {
        if (gDrawHook.installed) {
            return true;
        }
        for (const auto& patch : gCallSitePatches) {
            if (patch.installed) {
                return true;
            }
        }
        return false;
    }

    class DrawServicePanel final : public ImGuiPanel {
    public:
        explicit DrawServicePanel(cIGZDrawService* drawService)
            : drawService_(drawService) {
            std::snprintf(status_, sizeof(status_), "Idle");
        }

        void OnInit() override {
            RefreshContext();
            LOG_INFO("DrawServiceSample: panel initialized");
        }

        void OnShutdown() override {
            LOG_INFO("DrawServiceSample: panel shutdown");
            delete this;
        }

        void OnRender() override {
            PullHookEvents();
            RenderHookOverlay();

            ImGui::Begin("Draw Service Sample", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

            ImGui::Text("Service: %p", drawService_);
            if (!drawService_) {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "Draw service unavailable.");
                ImGui::End();
                return;
            }

            ImGui::SeparatorText("Hooked Draw Sequence");
            ImGui::Text("Hooks installed: %s", AreDrawSequenceHooksInstalled() ? "yes" : "no");
            if (!AreAnyDrawSequenceHooksInstalled()) {
                if (ImGui::Button("Install Draw Hook Only")) {
                    if (InstallDrawSequenceHooks(true)) {
                        SetStatus("Draw-only hook installed");
                    } else {
                        SetStatus("Failed to install draw-only hook");
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Install All Hooks (Unsafe)")) {
                    if (InstallDrawSequenceHooks(false)) {
                        SetStatus("All draw sequence hooks installed");
                    } else {
                        SetStatus("Failed to install all draw sequence hooks");
                    }
                }
            } else {
                if (ImGui::Button("Remove Hooks")) {
                    UninstallDrawSequenceHooks();
                    SetStatus("Draw sequence hooks removed");
                }
            }
            ImGui::TextUnformatted("Private pass hook isolation:");
            const struct HookUiEntry {
                size_t index;
                const char* shortName;
            } hookUiEntries[] = {
                {1, "PreStatic"},
                {2, "Static"},
                {3, "PostStatic"},
                {4, "PreDynamic"},
                {5, "Dynamic"},
                {6, "PostDynamic"},
            };
            for (const auto& entry : hookUiEntries) {
                const bool installed = IsHookInstalled(entry.index);
                char label[64]{};
                std::snprintf(label, sizeof(label), "%s [%s]", entry.shortName, installed ? "ON" : "OFF");
                if (ImGui::Button(label)) {
                    if (installed) {
                        RemoveSingleDrawSequenceHook(entry.index);
                        SetStatus("Removed one private pass hook");
                    } else if (InstallSingleDrawSequenceHook(entry.index)) {
                        SetStatus("Installed one private pass hook");
                    } else {
                        SetStatus("Failed to install private pass hook");
                    }
                }
                ImGui::SameLine();
            }
            ImGui::NewLine();
            bool drawDebugBox = gEnablePostDynamicDebugBox.load(std::memory_order_relaxed);
            if (ImGui::Checkbox("PostDynamic debug world box", &drawDebugBox)) {
                gEnablePostDynamicDebugBox.store(drawDebugBox, std::memory_order_relaxed);
                SetStatus(drawDebugBox ? "Enabled PostDynamic debug box"
                                       : "Disabled PostDynamic debug box");
            }
            bool drawD3D7Overlay = gEnablePostDynamicD3D7Overlay.load(std::memory_order_relaxed);
            if (ImGui::Checkbox("PostDynamic raw D3D7 overlay", &drawD3D7Overlay)) {
                gEnablePostDynamicD3D7Overlay.store(drawD3D7Overlay, std::memory_order_relaxed);
                SetStatus(drawD3D7Overlay ? "Enabled PostDynamic raw D3D7 overlay"
                                          : "Disabled PostDynamic raw D3D7 overlay");
            }
            bool drawStaticDepthOverlay = gEnableStaticD3D7DepthOverlay.load(std::memory_order_relaxed);
            if (ImGui::Checkbox("Static world depth overlay (D3D7)", &drawStaticDepthOverlay)) {
                gEnableStaticD3D7DepthOverlay.store(drawStaticDepthOverlay, std::memory_order_relaxed);
                SetStatus(drawStaticDepthOverlay ? "Enabled Static world depth overlay"
                                                 : "Disabled Static world depth overlay");
            }
            int staticZBias = gStaticD3D7ZBias.load(std::memory_order_relaxed);
            if (ImGui::SliderInt("Static overlay ZBias", &staticZBias, -16, 16)) {
                gStaticD3D7ZBias.store(staticZBias, std::memory_order_relaxed);
            }
            static const char* worldDepthOverlayPassItems[] = {"Static", "PreDynamic", "Dynamic", "PostDynamic"};
            int worldDepthOverlayPass = gStaticD3D7DepthOverlayPass.load(std::memory_order_relaxed);
            if (ImGui::Combo("World depth overlay pass", &worldDepthOverlayPass,
                             worldDepthOverlayPassItems,
                             static_cast<int>(std::size(worldDepthOverlayPassItems)))) {
                gStaticD3D7DepthOverlayPass.store(worldDepthOverlayPass, std::memory_order_relaxed);
            }
            float staticOverlayX = gStaticOverlayWorldX.load(std::memory_order_relaxed);
            if (ImGui::SliderFloat("Static overlay world X", &staticOverlayX, 0.0f, 2048.0f, "%.1f")) {
                gStaticOverlayWorldX.store(staticOverlayX, std::memory_order_relaxed);
            }
            float staticOverlayY = gStaticOverlayWorldY.load(std::memory_order_relaxed);
            if (ImGui::SliderFloat("Static overlay world Y", &staticOverlayY, 0.0f, 512.0f, "%.1f")) {
                gStaticOverlayWorldY.store(staticOverlayY, std::memory_order_relaxed);
            }
            float staticOverlayZ = gStaticOverlayWorldZ.load(std::memory_order_relaxed);
            if (ImGui::SliderFloat("Static overlay world Z", &staticOverlayZ, 0.0f, 2048.0f, "%.1f")) {
                gStaticOverlayWorldZ.store(staticOverlayZ, std::memory_order_relaxed);
            }
            bool drawDepthLayered = gEnablePreDynamicDepthLayeredOverlay.load(std::memory_order_relaxed);
            if (ImGui::Checkbox("PreDynamic depth-layered overlay", &drawDepthLayered)) {
                gEnablePreDynamicDepthLayeredOverlay.store(drawDepthLayered, std::memory_order_relaxed);
                SetStatus(drawDepthLayered ? "Enabled PreDynamic depth-layered overlay"
                                           : "Disabled PreDynamic depth-layered overlay");
            }
            int depthOffset = gPreDynamicDepthOffset.load(std::memory_order_relaxed);
            if (ImGui::SliderInt("PreDynamic depth offset", &depthOffset, -64, 64)) {
                gPreDynamicDepthOffset.store(depthOffset, std::memory_order_relaxed);
            }
            ImGui::Checkbox("Overlay begin/end lines", &showHookOverlay_);
            ImGui::SameLine();
            ImGui::SliderInt("History", &overlayHistory_, 8, 128);

            for (size_t i = 0; i < kDrawPassCount; ++i) {
                const auto begin = gBeginCounts[i].load(std::memory_order_relaxed);
                const auto end = gEndCounts[i].load(std::memory_order_relaxed);
                ImGui::Text("%-12s begin=%u end=%u", PassName(static_cast<DrawPass>(i)), begin, end);
            }

            ImGui::SeparatorText("Recent Hook Events");
            const size_t beginIndex = recentEvents_.size() > static_cast<size_t>(overlayHistory_)
                                          ? recentEvents_.size() - static_cast<size_t>(overlayHistory_)
                                          : 0;
            for (size_t i = beginIndex; i < recentEvents_.size(); ++i) {
                const auto& ev = recentEvents_[i];
                ImGui::Text("%s %s (t=%u)", PassName(ev.pass), ev.begin ? "BEGIN" : "END", ev.tickMs);
            }

            ImGui::SeparatorText("Context");
            if (ImGui::Button("Wrap Active Renderer Context")) {
                RefreshContext();
            }
            ImGui::SameLine();
            ImGui::Checkbox("Auto-refresh", &autoRefresh_);
            if (autoRefresh_) {
                RefreshContext();
            }

            ImGui::Text("Handle ptr=%p ver=%u", drawContext_.ptr, drawContext_.version);
            const bool hasContext = drawContext_.ptr != nullptr;
            if (!hasContext) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "No draw context available.");
                ImGui::TextWrapped("%s", status_);
                ImGui::End();
                return;
            }

            ImGui::SeparatorText("Renderer Passes");
            if (ImGui::Button("Draw()")) {
                lastDrawResult_ = drawService_->RendererDraw();
                drawCallCount_++;
                SetStatus("RendererDraw called");
            }
            ImGui::SameLine();
            if (ImGui::Button("PreStatic")) {
                drawService_->RendererDrawPreStaticView();
                preStaticCount_++;
                SetStatus("RendererDrawPreStaticView called");
            }
            ImGui::SameLine();
            if (ImGui::Button("Static")) {
                drawService_->RendererDrawStaticView();
                staticCount_++;
                SetStatus("RendererDrawStaticView called");
            }
            ImGui::SameLine();
            if (ImGui::Button("PostStatic")) {
                drawService_->RendererDrawPostStaticView();
                postStaticCount_++;
                SetStatus("RendererDrawPostStaticView called");
            }
            if (ImGui::Button("PreDynamic")) {
                drawService_->RendererDrawPreDynamicView();
                preDynamicCount_++;
                SetStatus("RendererDrawPreDynamicView called");
            }
            ImGui::SameLine();
            if (ImGui::Button("Dynamic")) {
                drawService_->RendererDrawDynamicView();
                dynamicCount_++;
                SetStatus("RendererDrawDynamicView called");
            }
            ImGui::SameLine();
            if (ImGui::Button("PostDynamic")) {
                drawService_->RendererDrawPostDynamicView();
                postDynamicCount_++;
                SetStatus("RendererDrawPostDynamicView called");
            }

            ImGui::Checkbox("Auto Draw", &autoDraw_);
            ImGui::SameLine();
            ImGui::Checkbox("Auto Dynamic Trio", &autoDynamicTrio_);
            if (autoDraw_) {
                lastDrawResult_ = drawService_->RendererDraw();
                drawCallCount_++;
            }
            if (autoDynamicTrio_) {
                drawService_->RendererDrawPreDynamicView();
                drawService_->RendererDrawDynamicView();
                drawService_->RendererDrawPostDynamicView();
                preDynamicCount_++;
                dynamicCount_++;
                postDynamicCount_++;
            }
            ImGui::Text("Result=0x%08X | Draw=%u PreS=%u S=%u PostS=%u PreD=%u D=%u PostD=%u",
                        lastDrawResult_, drawCallCount_, preStaticCount_, staticCount_, postStaticCount_,
                        preDynamicCount_, dynamicCount_, postDynamicCount_);

            ImGui::SeparatorText("Highlight");
            ImGui::SliderInt("Highlight Type", &highlightType_, 0, 15);
            ImGui::ColorEdit4("Highlight RGBA", highlightColor_);
            if (ImGui::Button("Set Highlight Color")) {
                drawService_->SetHighlightColor(drawContext_, highlightType_,
                                                highlightColor_[0], highlightColor_[1],
                                                highlightColor_[2], highlightColor_[3]);
                SetStatus("SetHighlightColor called");
            }
            ImGui::SameLine();
            if (ImGui::Button("Set Highlight State")) {
                drawService_->SetRenderStateHighlight(drawContext_, highlightType_);
                SetStatus("SetRenderStateHighlight(type) called");
            }

            ImGui::SeparatorText("Render State");
            if (ImGui::Button("Default Render State")) {
                drawService_->SetDefaultRenderState(drawContext_);
                SetStatus("SetDefaultRenderState called");
            }
            ImGui::SameLine();
            if (ImGui::Button("Default Unilateral")) {
                drawService_->SetDefaultRenderStateUnilaterally(drawContext_);
                SetStatus("SetDefaultRenderStateUnilaterally called");
            }

            ImGui::Checkbox("Blend", &blend_);
            ImGui::SameLine();
            ImGui::Checkbox("Alpha Test", &alphaTest_);
            ImGui::SameLine();
            ImGui::Checkbox("Depth Test", &depthTest_);
            ImGui::SameLine();
            ImGui::Checkbox("Depth Mask", &depthMask_);
            ImGui::SameLine();
            ImGui::Checkbox("Cull", &cullFace_);
            ImGui::SameLine();
            ImGui::Checkbox("Color Mask", &colorMask_);
            if (ImGui::Button("Apply State Flags")) {
                drawService_->EnableBlendStateFlag(drawContext_, blend_);
                drawService_->EnableAlphaTestFlag(drawContext_, alphaTest_);
                drawService_->EnableDepthTestFlag(drawContext_, depthTest_);
                drawService_->EnableDepthMaskFlag(drawContext_, depthMask_);
                drawService_->EnableCullFaceFlag(drawContext_, cullFace_);
                drawService_->EnableColorMaskFlag(drawContext_, colorMask_);
                SetStatus("Applied blend/alpha/depth/cull/color-mask flags");
            }

            ImGui::SeparatorText("Texture / Lighting / Fog");
            ImGui::SliderInt("Texture Stage", &textureStage_, 0, 3);
            ImGui::Checkbox("Texture State Enabled", &textureStateEnabled_);
            if (ImGui::Button("Apply Texture Stage Flag")) {
                drawService_->EnableTextureStateFlag(drawContext_, textureStateEnabled_, textureStage_);
                SetStatus("EnableTextureStateFlag called");
            }

            ImGui::ColorEdit4("Texture Color", texColor_);
            if (ImGui::Button("Set Tex Color")) {
                drawService_->SetTexColor(drawContext_, texColor_[0], texColor_[1], texColor_[2], texColor_[3]);
                SetStatus("SetTexColor called");
            }

            ImGui::Checkbox("Lighting Enabled", &lightingEnabled_);
            ImGui::SameLine();
            if (ImGui::Button("Apply Lighting")) {
                drawService_->SetLighting(drawContext_, lightingEnabled_);
                SetStatus("SetLighting called");
            }
            const bool lightingNow = drawService_->GetLighting(drawContext_);
            ImGui::Text("GetLighting: %s", lightingNow ? "true" : "false");

            ImGui::Checkbox("Fog Enabled", &fogEnabled_);
            ImGui::ColorEdit3("Fog Color", fogColor_);
            ImGui::InputFloat("Fog Start", &fogStart_, 1.0f, 10.0f, "%.2f");
            ImGui::InputFloat("Fog End", &fogEnd_, 1.0f, 10.0f, "%.2f");
            if (ImGui::Button("Apply Fog")) {
                drawService_->SetFog(drawContext_, fogEnabled_, fogColor_, fogStart_, fogEnd_);
                SetStatus("SetFog called");
            }

            ImGui::Separator();
            ImGui::TextWrapped("%s", status_);
            ImGui::End();
        }

    private:
        static ImU32 PassColor(const DrawPass pass, const bool begin) {
            switch (pass) {
            case DrawPass::Draw: return begin ? IM_COL32(250, 220, 120, 255) : IM_COL32(160, 140, 80, 255);
            case DrawPass::PreStatic: return begin ? IM_COL32(120, 220, 255, 255) : IM_COL32(60, 140, 190, 255);
            case DrawPass::Static: return begin ? IM_COL32(80, 255, 180, 255) : IM_COL32(60, 170, 120, 255);
            case DrawPass::PostStatic: return begin ? IM_COL32(180, 220, 255, 255) : IM_COL32(95, 120, 150, 255);
            case DrawPass::PreDynamic: return begin ? IM_COL32(255, 180, 120, 255) : IM_COL32(200, 110, 70, 255);
            case DrawPass::Dynamic: return begin ? IM_COL32(255, 120, 120, 255) : IM_COL32(180, 70, 70, 255);
            case DrawPass::PostDynamic: return begin ? IM_COL32(220, 160, 255, 255) : IM_COL32(135, 90, 180, 255);
            default: return begin ? IM_COL32(180, 255, 180, 255) : IM_COL32(120, 120, 120, 255);
            }
        }

        void PullHookEvents() {
            const uint64_t latestSeq = gEventSeq.load(std::memory_order_acquire);
            if (latestSeq == lastSeenSeq_) {
                return;
            }

            const uint64_t oldestAvailable = latestSeq > kEventRingCapacity ? latestSeq - kEventRingCapacity + 1 : 1;
            uint64_t nextSeq = lastSeenSeq_ + 1;
            if (nextSeq < oldestAvailable) {
                nextSeq = oldestAvailable;
            }

            for (uint64_t seq = nextSeq; seq <= latestSeq; ++seq) {
                const HookEvent ev = gEventRing[seq % kEventRingCapacity];
                if (ev.seq == seq) {
                    recentEvents_.push_back(ev);
                }
            }
            lastSeenSeq_ = latestSeq;

            constexpr size_t kMaxRetainedEvents = 512;
            if (recentEvents_.size() > kMaxRetainedEvents) {
                recentEvents_.erase(recentEvents_.begin(),
                                    recentEvents_.begin() + (recentEvents_.size() - kMaxRetainedEvents));
            }
        }

        void RenderHookOverlay() const {
            if (!showHookOverlay_ || recentEvents_.empty()) {
                return;
            }

            ImDrawList* drawList = ImGui::GetForegroundDrawList();
            const ImVec2 origin(20.0f, 110.0f);
            const float width = 180.0f;
            const float rowHeight = 8.0f;

            const size_t count = recentEvents_.size() > static_cast<size_t>(overlayHistory_)
                                     ? static_cast<size_t>(overlayHistory_)
                                     : recentEvents_.size();
            const size_t start = recentEvents_.size() - count;

            for (size_t i = 0; i < count; ++i) {
                const auto& ev = recentEvents_[start + i];
                const float y = origin.y + rowHeight * static_cast<float>(i);
                const ImU32 color = PassColor(ev.pass, ev.begin);
                drawList->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + width, y), color, 2.0f);

                char label[64]{};
                std::snprintf(label, sizeof(label), "%s %s", PassName(ev.pass), ev.begin ? "BEGIN" : "END");
                drawList->AddText(ImVec2(origin.x + width + 6.0f, y - 6.0f), IM_COL32(255, 255, 255, 230), label);
            }
        }

        void RefreshContext() {
            drawContext_ = drawService_->WrapActiveRendererDrawContext();
            if (drawContext_.ptr) {
                SetStatus("Wrapped active renderer draw context");
            } else {
                SetStatus("No active renderer draw context");
            }
        }

        void SetStatus(const char* text) {
            std::snprintf(status_, sizeof(status_), "%s", text);
            LOG_INFO("DrawServiceSample: {}", text);
        }

    private:
        cIGZDrawService* drawService_;
        SC4DrawContextHandle drawContext_{nullptr, 0};
        bool autoRefresh_ = false;

        int highlightType_ = 0;
        float highlightColor_[4] = {1.0f, 0.2f, 0.2f, 1.0f};

        bool blend_ = true;
        bool alphaTest_ = false;
        bool depthTest_ = true;
        bool depthMask_ = true;
        bool cullFace_ = true;
        bool colorMask_ = true;

        int textureStage_ = 0;
        bool textureStateEnabled_ = true;
        float texColor_[4] = {1.0f, 1.0f, 1.0f, 1.0f};

        bool lightingEnabled_ = true;
        bool fogEnabled_ = false;
        float fogColor_[3] = {0.65f, 0.75f, 0.90f};
        float fogStart_ = 400.0f;
        float fogEnd_ = 1800.0f;
        bool autoDraw_ = false;
        bool autoDynamicTrio_ = false;
        uint32_t lastDrawResult_ = 0;
        uint32_t drawCallCount_ = 0;
        uint32_t preStaticCount_ = 0;
        uint32_t staticCount_ = 0;
        uint32_t postStaticCount_ = 0;
        uint32_t preDynamicCount_ = 0;
        uint32_t dynamicCount_ = 0;
        uint32_t postDynamicCount_ = 0;

        bool showHookOverlay_ = true;
        int overlayHistory_ = 32;
        uint64_t lastSeenSeq_ = 0;
        std::vector<HookEvent> recentEvents_{};

        char status_[160]{};
    };
}

class DrawServiceSampleDirector final : public cRZCOMDllDirector {
public:
    DrawServiceSampleDirector() = default;

    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kDrawSampleDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZCOMDllDirector::OnStart(pCOM);
        Logger::Initialize("SC4DrawServiceSample", "");
        LOG_INFO("DrawServiceSample: OnStart");
        if (mpFrameWork) {
            mpFrameWork->AddHook(this);
        } else {
            LOG_WARN("DrawServiceSample: framework unavailable in OnStart");
        }
        return true;
    }

    bool PostAppInit() override {
        if (!mpFrameWork || panelRegistered_) {
            return true;
        }

        if (!mpFrameWork->GetSystemService(kImGuiServiceID, GZIID_cIGZImGuiService,
                                           reinterpret_cast<void**>(&imguiService_))) {
            LOG_WARN("DrawServiceSample: ImGui service not available");
            return true;
        }

        if (!mpFrameWork->GetSystemService(kDrawServiceID, GZIID_cIGZDrawService,
                                           reinterpret_cast<void**>(&drawService_))) {
            LOG_WARN("DrawServiceSample: Draw service not available");
            imguiService_->Release();
            imguiService_ = nullptr;
            return true;
        }

        auto* panel = new DrawServicePanel(drawService_);
        const ImGuiPanelDesc desc =
            ImGuiPanelAdapter<DrawServicePanel>::MakeDesc(panel, kDrawSamplePanelId, 150, true);

        if (!imguiService_->RegisterPanel(desc)) {
            LOG_WARN("DrawServiceSample: failed to register panel");
            delete panel;
            UninstallDrawSequenceHooks();
            drawService_->Release();
            imguiService_->Release();
            drawService_ = nullptr;
            imguiService_ = nullptr;
            return true;
        }

        panelRegistered_ = true;
        gImGuiServiceForD3DOverlay.store(imguiService_, std::memory_order_release);
        LOG_INFO("DrawServiceSample: panel registered");
        return true;
    }

    bool PostAppShutdown() override {
        UninstallDrawSequenceHooks();
        gImGuiServiceForD3DOverlay.store(nullptr, std::memory_order_release);
        if (imguiService_) {
            imguiService_->UnregisterPanel(kDrawSamplePanelId);
            imguiService_->Release();
            imguiService_ = nullptr;
        }
        if (drawService_) {
            drawService_->Release();
            drawService_ = nullptr;
        }
        panelRegistered_ = false;
        return true;
    }

private:
    cIGZImGuiService* imguiService_ = nullptr;
    cIGZDrawService* drawService_ = nullptr;
    bool panelRegistered_ = false;
};

static DrawServiceSampleDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static auto sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
