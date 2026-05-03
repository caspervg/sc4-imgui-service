#include "TerrainDecalHook.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "GZServPtrs.h"
#include "cISC4App.h"
#include "cISC4City.h"
#include "cISTETerrain.h"
#include "cISTETerrainView.h"
#include "utils/Logger.h"
#include "utils/VersionDetection.h"

namespace TerrainDecal
{
    TerrainDecalHook* TerrainDecalHook::sActiveHook_ = nullptr;

    namespace
    {
        [[nodiscard]] bool IsRingDecalSlot(const std::byte* const slotBase) noexcept
        {
            if (!slotBase) {
                return false;
            }

            uint32_t flags = 0;
            std::memcpy(&flags, slotBase + 4, sizeof(flags));
            return (flags & 0x80u) == 0u;
        }
    }

    TerrainDecalHook::TerrainDecalHook(const Options options)
        : options_(options)
        , renderer_(RendererOptions{
              .enableClippedRendering = options.enableExperimentalRenderer,
          })
    {
    }

    TerrainDecalHook::~TerrainDecalHook()
    {
        Uninstall();
    }

    bool TerrainDecalHook::Install()
    {
        if (callSitePatch_.IsInstalled() && setTexTransformCallSitePatch_.IsInstalled()) {
            return true;
        }

        if (!options_.installEnabled) {
            SetLastError_("terrain decal hook disabled by configuration");
            return false;
        }

        const auto gameVersion = VersionDetection::GetInstance().GetGameVersion();
        addresses_ = ResolveHookAddresses(gameVersion);
        if (!addresses_) {
            SetLastError_(std::string("unsupported game version: ") + std::to_string(gameVersion));
            LOG_INFO("TerrainDecalHook: {}. No patch installed.", lastError_);
            return false;
        }

        if (sActiveHook_ && sActiveHook_ != this) {
            SetLastError_("another terrain decal hook instance is already active");
            LOG_WARN("TerrainDecalHook: {}", lastError_);
            return false;
        }

        sActiveHook_ = this;
        callSitePatch_.Configure("cSTEOverlayManager::DrawDecals->DrawRect call site",
                                 addresses_->drawRectCallSite,
                                 reinterpret_cast<void*>(&DrawRectCallThunk));
        setTexTransformCallSitePatch_.Configure("cSTEOverlayManager::DrawDecals->SetTexTransform4 call site",
                                                addresses_->setTexTransform4CallSite,
                                                reinterpret_cast<void*>(&SetTexTransform4CallThunk));

        if (!callSitePatch_.Install()) {
            sActiveHook_ = nullptr;
            SetLastError_("failed to install draw-rect call-site patch");
            return false;
        }
        if (!setTexTransformCallSitePatch_.Install()) {
            callSitePatch_.Uninstall();
            sActiveHook_ = nullptr;
            SetLastError_("failed to install set-tex-transform call-site patch");
            return false;
        }

        lastError_.clear();
        LOG_INFO("TerrainDecalHook: installed at 0x{:08X} / 0x{:08X} for {}",
                 static_cast<uint32_t>(addresses_->drawRectCallSite),
                 static_cast<uint32_t>(addresses_->setTexTransform4CallSite),
                 DescribeKnownAddressSet(addresses_->gameVersion));
        return true;
    }

    void TerrainDecalHook::Uninstall()
    {
        drawShadowsRoughCallSitePatch_.Uninstall();
        drawShadowsCallSitePatch_.Uninstall();
        setTexTransformCallSitePatch_.Uninstall();
        callSitePatch_.Uninstall();
        currentTexTransformValid_ = false;
        currentTexTransformStage_ = -1;
        inShadowReRenderPhase_ = false;
        activeDecalSlotIndices_.clear();
        renderer_.ClearOverlayUvWindows();

        if (sActiveHook_ == this) {
            sActiveHook_ = nullptr;
        }
    }

    bool TerrainDecalHook::IsInstalled() const noexcept
    {
        return callSitePatch_.IsInstalled();
    }

    std::string_view TerrainDecalHook::GetLastError() const noexcept
    {
        return lastError_;
    }

    void TerrainDecalHook::SetOverlayUvWindow(const uint32_t overlayId, const TerrainDecalUvWindow& uvWindow)
    {
        renderer_.SetOverlayUvWindow(overlayId, uvWindow);
    }

    bool TerrainDecalHook::RemoveOverlayUvWindow(const uint32_t overlayId) noexcept
    {
        return renderer_.RemoveOverlayUvWindow(overlayId);
    }

    void TerrainDecalHook::ClearOverlayUvWindows() noexcept
    {
        renderer_.ClearOverlayUvWindows();
    }

    bool TerrainDecalHook::TryGetOverlayUvWindow(const uint32_t overlayId, TerrainDecalUvWindow& uvWindow) const noexcept
    {
        return renderer_.TryGetOverlayUvWindow(overlayId, uvWindow);
    }

    void TerrainDecalHook::SetOverlayOverridesResolver(const OverlayOverridesResolver resolver, void* const userData) noexcept
    {
        renderer_.SetOverlayOverridesResolver(resolver, userData);
    }

    void __fastcall TerrainDecalHook::DrawRectCallThunk(void* overlayManager,
                                                        void*,
                                                        SC4DrawContext* drawContext,
                                                        const cRZRect* rect)
    {
        if (!sActiveHook_) {
            return;
        }

        sActiveHook_->HandleDrawRectCall_(overlayManager, drawContext, rect);
    }

    void __fastcall TerrainDecalHook::SetTexTransform4CallThunk(SC4DrawContext* drawContext,
                                                                void*,
                                                                const float* matrix,
                                                                int stage)
    {
        if (!sActiveHook_) {
            return;
        }

        sActiveHook_->HandleSetTexTransform4Call_(drawContext, matrix, stage);
    }

    void __fastcall TerrainDecalHook::DrawShadowsWrapThunk(void* overlayManager,
                                                           void*,
                                                           float* worldMatrix,
                                                           SC4DrawContext* drawCtx,
                                                           int* decalIds)
    {
        if (!sActiveHook_) {
            return;
        }

        sActiveHook_->HandleDrawShadowsCall_(overlayManager, worldMatrix, drawCtx, decalIds,
                                             sActiveHook_->drawShadowsCallSitePatch_);
    }

    void __fastcall TerrainDecalHook::DrawShadowsRoughWrapThunk(void* overlayManager,
                                                                void*,
                                                                float* worldMatrix,
                                                                SC4DrawContext* drawCtx,
                                                                int* decalIds)
    {
        if (!sActiveHook_) {
            return;
        }

        sActiveHook_->HandleDrawShadowsCall_(overlayManager, worldMatrix, drawCtx, decalIds,
                                             sActiveHook_->drawShadowsRoughCallSitePatch_);
    }

    void TerrainDecalHook::HandleDrawShadowsCall_(void* overlayManager,
                                                  float* worldMatrix,
                                                  SC4DrawContext* drawCtx,
                                                  int* decalIds,
                                                  const RelativeCallPatch& patch)
    {
        const auto originalTarget = patch.GetOriginalTarget();
        if (originalTarget) {
            const auto original = reinterpret_cast<DrawShadowsFn>(originalTarget);
            original(overlayManager, worldMatrix, drawCtx, decalIds);
        }

        if (!activeDecalSlotIndices_.empty() && addresses_) {
            inShadowReRenderPhase_ = true;
            const auto drawDecals = reinterpret_cast<DrawDecalsFn>(addresses_->drawDecals);
            struct { const uint32_t* begin; const uint32_t* end; } range = {
                activeDecalSlotIndices_.data(),
                activeDecalSlotIndices_.data() + activeDecalSlotIndices_.size(),
            };
            drawDecals(overlayManager, worldMatrix, drawCtx, reinterpret_cast<int*>(&range));
            inShadowReRenderPhase_ = false;
        }

        activeDecalSlotIndices_.clear();
    }

    void TerrainDecalHook::HandleDrawRectCall_(void* overlayManager, SC4DrawContext* drawContext, const cRZRect* rect)
    {
        DrawRequest request{
            .overlayManager = overlayManager,
            .drawContext = drawContext,
            .rect = rect,
            .overlaySlotBase = nullptr,
            .activeTexTransform = currentTexTransformValid_ ? currentTexTransform_.data() : nullptr,
            .activeTexTransformStage = currentTexTransformValid_ ? currentTexTransformStage_ : -1,
            .overlayRectOffset = 0,
            .addresses = addresses_ ? &*addresses_ : nullptr,
            .terrain = nullptr,
            .terrainView = nullptr,
        };

        if (addresses_ && rect && addresses_->overlayRectOffset > 0) {
            request.overlayRectOffset = addresses_->overlayRectOffset;
            request.overlaySlotBase = reinterpret_cast<const std::byte*>(rect) - addresses_->overlayRectOffset;
        }

        if (request.overlaySlotBase && IsRingDecalSlot(request.overlaySlotBase)) {
            currentTexTransformValid_ = false;
            currentTexTransformStage_ = -1;
            CallOriginalDrawRect_(overlayManager, drawContext, rect);
            return;
        }

        const cISC4AppPtr app;
        cISC4City* city = app ? app->GetCity() : nullptr;
        request.terrain = city ? city->GetTerrain() : nullptr;
        request.terrainView = request.terrain ? request.terrain->GetView() : nullptr;

        const auto result = renderer_.Draw(request);
        currentTexTransformValid_ = false;
        currentTexTransformStage_ = -1;

        if (result == DrawResult::Handled) {
            if (!inShadowReRenderPhase_ && request.overlaySlotBase && addresses_) {
                const auto slotArrayBase = *reinterpret_cast<const uintptr_t*>(
                    static_cast<const char*>(overlayManager) + addresses_->overlaySlotsPtrOffset);
                const auto slotBase = reinterpret_cast<uintptr_t>(request.overlaySlotBase);
                const auto slotIndex = static_cast<uint32_t>(
                    (slotBase - slotArrayBase) / static_cast<uintptr_t>(addresses_->overlaySlotStride));
                activeDecalSlotIndices_.push_back(slotIndex);
            }
            return;
        }

        CallOriginalDrawRect_(overlayManager, drawContext, rect);
    }

    void TerrainDecalHook::HandleSetTexTransform4Call_(SC4DrawContext* drawContext, const float* matrix, const int stage)
    {
        if (matrix) {
            std::copy_n(matrix, currentTexTransform_.size(), currentTexTransform_.begin());
            currentTexTransformStage_ = stage;
            currentTexTransformValid_ = true;
        }
        else {
            currentTexTransformValid_ = false;
            currentTexTransformStage_ = -1;
        }

        CallOriginalSetTexTransform4_(drawContext, matrix, stage);
    }

    void TerrainDecalHook::CallOriginalDrawRect_(void* overlayManager,
                                                 SC4DrawContext* drawContext,
                                                 const cRZRect* rect) const
    {
        const auto originalTarget = callSitePatch_.GetOriginalTarget();
        if (!originalTarget) {
            return;
        }

        const auto original = reinterpret_cast<DrawRectFn>(originalTarget);
        original(overlayManager, drawContext, rect);
    }

    void TerrainDecalHook::CallOriginalSetTexTransform4_(SC4DrawContext* drawContext,
                                                         const float* matrix,
                                                         const int stage) const
    {
        const auto originalTarget = setTexTransformCallSitePatch_.GetOriginalTarget();
        if (!originalTarget) {
            return;
        }

        const auto original = reinterpret_cast<SetTexTransform4Fn>(originalTarget);
        original(drawContext, matrix, stage);
    }

    void TerrainDecalHook::SetLastError_(std::string message)
    {
        lastError_ = std::move(message);
    }
}
