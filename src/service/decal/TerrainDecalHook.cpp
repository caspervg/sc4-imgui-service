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
        constexpr std::ptrdiff_t kOverlayManagerDecalDrawCountOffset = 0xD8;

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
              .enableClippedRendering = options.enableCustomRenderer,
              .customDefaultDepthOffset = options.customDefaultDepthOffset,
              .shadowRecoveryOpacityScale = options.shadowRecoveryOpacityScale,
          })
    {
    }

    TerrainDecalHook::~TerrainDecalHook()
    {
        Uninstall();
    }

    bool TerrainDecalHook::Install()
    {
        if (callSitePatch_.IsInstalled() &&
            setTexTransformCallSitePatch_.IsInstalled() &&
            drawShadowsCallSitePatch_.IsInstalled() &&
            drawShadowsRoughCallSitePatch_.IsInstalled()) {
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
        drawShadowsCallSitePatch_.Configure("cSTEOverlayManager::DrawOverlays->DrawShadows call site",
                                            addresses_->drawShadowsCallSite,
                                            reinterpret_cast<void*>(&DrawShadowsCallThunk));
        drawShadowsRoughCallSitePatch_.Configure("cSTEOverlayManager::DrawOverlays->DrawShadowsRough call site",
                                                 addresses_->drawShadowsRoughCallSite,
                                                 reinterpret_cast<void*>(&DrawShadowsRoughCallThunk));

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
        if (!drawShadowsCallSitePatch_.Install()) {
            setTexTransformCallSitePatch_.Uninstall();
            callSitePatch_.Uninstall();
            sActiveHook_ = nullptr;
            SetLastError_("failed to install draw-shadows call-site patch");
            return false;
        }
        if (!drawShadowsRoughCallSitePatch_.Install()) {
            drawShadowsCallSitePatch_.Uninstall();
            setTexTransformCallSitePatch_.Uninstall();
            callSitePatch_.Uninstall();
            sActiveHook_ = nullptr;
            SetLastError_("failed to install draw-shadows-rough call-site patch");
            return false;
        }

        lastError_.clear();
        LOG_INFO("TerrainDecalHook: installed at 0x{:08X} / 0x{:08X}; shadow replay at 0x{:08X} / 0x{:08X} for {}",
                 static_cast<uint32_t>(addresses_->drawRectCallSite),
                 static_cast<uint32_t>(addresses_->setTexTransform4CallSite),
                 static_cast<uint32_t>(addresses_->drawShadowsCallSite),
                 static_cast<uint32_t>(addresses_->drawShadowsRoughCallSite),
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
        shadowRecoveryActive_ = false;
        renderer_.ClearOverlayUvWindows();

        if (sActiveHook_ == this) {
            sActiveHook_ = nullptr;
        }
    }

    bool TerrainDecalHook::IsInstalled() const noexcept
    {
        return callSitePatch_.IsInstalled() &&
               setTexTransformCallSitePatch_.IsInstalled() &&
               drawShadowsCallSitePatch_.IsInstalled() &&
               drawShadowsRoughCallSitePatch_.IsInstalled();
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

    void __fastcall TerrainDecalHook::DrawShadowsCallThunk(void* overlayManager,
                                                           void*,
                                                           const float* worldToScreenMatrix,
                                                           SC4DrawContext* drawContext,
                                                           int* decalIds)
    {
        if (!sActiveHook_) {
            return;
        }

        sActiveHook_->HandleDrawShadowsCall_(sActiveHook_->drawShadowsCallSitePatch_,
                                             overlayManager,
                                             worldToScreenMatrix,
                                             drawContext,
                                             decalIds);
    }

    void __fastcall TerrainDecalHook::DrawShadowsRoughCallThunk(void* overlayManager,
                                                                void*,
                                                                const float* worldToScreenMatrix,
                                                                SC4DrawContext* drawContext,
                                                                int* decalIds)
    {
        if (!sActiveHook_) {
            return;
        }

        sActiveHook_->HandleDrawShadowsCall_(sActiveHook_->drawShadowsRoughCallSitePatch_,
                                             overlayManager,
                                             worldToScreenMatrix,
                                             drawContext,
                                             decalIds);
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
            .mode = shadowRecoveryActive_ ? DrawMode::ShadowRecovery : DrawMode::Normal,
        };

        if (addresses_ && rect && addresses_->overlayRectOffset > 0) {
            request.overlayRectOffset = addresses_->overlayRectOffset;
            request.overlaySlotBase = reinterpret_cast<const std::byte*>(rect) - addresses_->overlayRectOffset;
        }

        if (request.overlaySlotBase && IsRingDecalSlot(request.overlaySlotBase)) {
            currentTexTransformValid_ = false;
            currentTexTransformStage_ = -1;
            if (!shadowRecoveryActive_) {
                CallOriginalDrawRect_(overlayManager, drawContext, rect);
            }
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
            return;
        }

        if (shadowRecoveryActive_) {
            return;
        }

        CallOriginalDrawRect_(overlayManager, drawContext, rect);
    }

    void TerrainDecalHook::HandleDrawShadowsCall_(const RelativeCallPatch& patch,
                                                  void* overlayManager,
                                                  const float* worldToScreenMatrix,
                                                  SC4DrawContext* drawContext,
                                                  int* decalIds)
    {
        CallOriginalOverlayPass_(patch, overlayManager, worldToScreenMatrix, drawContext, decalIds);
        ReplayManagedDecalsAfterShadows_(overlayManager, worldToScreenMatrix, drawContext, decalIds);
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

    void TerrainDecalHook::CallOriginalOverlayPass_(const RelativeCallPatch& patch,
                                                    void* overlayManager,
                                                    const float* worldToScreenMatrix,
                                                    SC4DrawContext* drawContext,
                                                    int* decalIds) const
    {
        const auto originalTarget = patch.GetOriginalTarget();
        if (!originalTarget) {
            return;
        }

        const auto original = reinterpret_cast<DrawOverlayPassFn>(originalTarget);
        original(overlayManager, worldToScreenMatrix, drawContext, decalIds);
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

    void TerrainDecalHook::ReplayManagedDecalsAfterShadows_(void* overlayManager,
                                                            const float* worldToScreenMatrix,
                                                            SC4DrawContext* drawContext,
                                                            int* decalIds)
    {
        if (!options_.enableCustomRenderer ||
            shadowRecoveryActive_ ||
            !addresses_ ||
            !addresses_->drawDecals ||
            !overlayManager ||
            !worldToScreenMatrix ||
            !drawContext ||
            !decalIds) {
            return;
        }

        currentTexTransformValid_ = false;
        currentTexTransformStage_ = -1;

        auto* const overlayManagerBytes = reinterpret_cast<std::byte*>(overlayManager);
        int savedDecalDrawCount = 0;
        std::memcpy(&savedDecalDrawCount,
                    overlayManagerBytes + kOverlayManagerDecalDrawCountOffset,
                    sizeof(savedDecalDrawCount));

        const bool previousShadowRecoveryActive = shadowRecoveryActive_;
        shadowRecoveryActive_ = true;
        const auto drawDecals = reinterpret_cast<DrawOverlayPassFn>(addresses_->drawDecals);
        drawDecals(overlayManager, worldToScreenMatrix, drawContext, decalIds);
        shadowRecoveryActive_ = previousShadowRecoveryActive;

        std::memcpy(overlayManagerBytes + kOverlayManagerDecalDrawCountOffset,
                    &savedDecalDrawCount,
                    sizeof(savedDecalDrawCount));

        currentTexTransformValid_ = false;
        currentTexTransformStage_ = -1;
    }

    void TerrainDecalHook::SetLastError_(std::string message)
    {
        lastError_ = std::move(message);
    }
}
