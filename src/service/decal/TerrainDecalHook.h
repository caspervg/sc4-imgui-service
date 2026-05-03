#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ClippedTerrainDecalRenderer.h"
#include "RelativeCallPatch.h"
#include "TerrainDecalSymbols.h"

class SC4DrawContext;

namespace TerrainDecal
{
    class TerrainDecalHook final
    {
    public:
        struct Options
        {
            bool installEnabled = true;
            bool enableExperimentalRenderer = true;
        };

        explicit TerrainDecalHook(Options options = {});
        ~TerrainDecalHook();

        [[nodiscard]] bool Install();
        void Uninstall();

        [[nodiscard]] bool IsInstalled() const noexcept;
        [[nodiscard]] std::string_view GetLastError() const noexcept;

        void SetOverlayUvWindow(uint32_t overlayId, const TerrainDecalUvWindow& uvWindow);
        [[nodiscard]] bool RemoveOverlayUvWindow(uint32_t overlayId) noexcept;
        void ClearOverlayUvWindows() noexcept;
        [[nodiscard]] bool TryGetOverlayUvWindow(uint32_t overlayId, TerrainDecalUvWindow& uvWindow) const noexcept;
        void SetOverlayOverridesResolver(OverlayOverridesResolver resolver, void* userData) noexcept;

    private:
        using DrawRectFn = void(__thiscall*)(void*, SC4DrawContext*, const cRZRect*);
        using SetTexTransform4Fn = void(__thiscall*)(SC4DrawContext*, const float*, int);
        using DrawShadowsFn = void(__thiscall*)(void*, float*, SC4DrawContext*, int*);
        using DrawDecalsFn = void(__thiscall*)(void*, float*, SC4DrawContext*, int*);

        static void __fastcall DrawRectCallThunk(void* overlayManager, void*, SC4DrawContext* drawContext, const cRZRect* rect);
        static void __fastcall SetTexTransform4CallThunk(SC4DrawContext* drawContext, void*, const float* matrix, int stage);
        static void __fastcall DrawShadowsWrapThunk(void* overlayManager, void*, float* worldMatrix, SC4DrawContext* drawCtx, int* decalIds);
        static void __fastcall DrawShadowsRoughWrapThunk(void* overlayManager, void*, float* worldMatrix, SC4DrawContext* drawCtx, int* decalIds);

        void HandleDrawRectCall_(void* overlayManager, SC4DrawContext* drawContext, const cRZRect* rect);
        void HandleSetTexTransform4Call_(SC4DrawContext* drawContext, const float* matrix, int stage);
        void HandleDrawShadowsCall_(void* overlayManager, float* worldMatrix, SC4DrawContext* drawCtx, int* decalIds, const RelativeCallPatch& patch);
        void CallOriginalDrawRect_(void* overlayManager, SC4DrawContext* drawContext, const cRZRect* rect) const;
        void CallOriginalSetTexTransform4_(SC4DrawContext* drawContext, const float* matrix, int stage) const;
        void SetLastError_(std::string message);

    private:
        Options options_;
        std::optional<HookAddresses> addresses_;
        RelativeCallPatch callSitePatch_;
        RelativeCallPatch setTexTransformCallSitePatch_;
        RelativeCallPatch drawShadowsCallSitePatch_;
        RelativeCallPatch drawShadowsRoughCallSitePatch_;
        ClippedTerrainDecalRenderer renderer_;
        std::string lastError_{};
        std::array<float, 16> currentTexTransform_{};
        std::vector<uint32_t> activeDecalSlotIndices_{};
        int currentTexTransformStage_ = -1;
        bool currentTexTransformValid_ = false;
        bool inShadowReRenderPhase_ = false;

        static TerrainDecalHook* sActiveHook_;
    };
}
