#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cRZBaseSystemService.h"
#include "public/cIGZTerrainDecalService.h"
#include "public/TerrainDecalServiceIds.h"
#include "utils/VersionDetection.h"
#include "TerrainDecalHook.h"
#include "TerrainDecalRegistry.h"

class cIGZMessage2;
class cIGZMessage2Standard;
class cISC4DBSegment;
class cISC4City;
class cISTEOverlayManager;

class TerrainDecalService final : public cRZBaseSystemService, public cIGZTerrainDecalService {
public:
    TerrainDecalService();
    ~TerrainDecalService() = default;

    uint32_t AddRef() override;
    uint32_t Release() override;
    bool QueryInterface(uint32_t riid, void** ppvObj) override;

    [[nodiscard]] uint32_t GetServiceID() const override;
    bool CreateDecal(const TerrainDecalState& initialState, TerrainDecalId* outId) override;
    bool RemoveDecal(TerrainDecalId id) override;
    bool GetDecal(TerrainDecalId id, TerrainDecalSnapshot* outSnapshot) const override;
    bool ReplaceDecal(TerrainDecalId id, const TerrainDecalState& newState) override;
    uint32_t GetDecalCount() const override;
    uint32_t CopyDecals(TerrainDecalSnapshot* buffer, uint32_t capacity) const override;
    bool OnTick(uint32_t unknown1) override;

    void SetEnableExperimentalRenderer(bool enableExperimentalRenderer) noexcept;
    void SetDefaultDepthOffset(int defaultDepthOffset) noexcept;
    bool Init() override;
    bool Shutdown();
    bool HandleMessage(cIGZMessage2* message);

private:
    bool CreateRuntimeDecal_(TerrainDecalId id, const TerrainDecalState& state, bool updateNextId);
    bool ApplyStateToRuntime_(TerrainDecalRecord& record, const TerrainDecalState& state);
    bool RemoveRuntimeDecal_(TerrainDecalId id, bool removeRuntimeOverlay);
    void ClearRuntimeState_(bool removeRuntimeOverlays);
    void OnPostCityInit_(cIGZMessage2Standard* msg);
    void OnPreCityShutdown_(cIGZMessage2Standard* msg);
    void OnLoad_(cIGZMessage2Standard* msg);
    void OnSave_(cIGZMessage2Standard* msg);
    bool TryGetCurrentCity_(cISC4City*& city) const;
    cISTEOverlayManager* ResolveOverlayManager_(cISC4City* city,
                                               cISTETerrainView::tOverlayManagerType overlayType) const;
    cISC4DBSegment* QuerySegmentFromMessage_(const cIGZMessage2Standard* msg) const;
    bool CaptureLiveState_(const TerrainDecalRecord& record, TerrainDecalState& state) const;
    bool ValidateState_(TerrainDecalState& state, std::string& error) const;
    void RebindLoadedDecals_();
    static bool ResolveOverlayOverrides_(void* overlayManager, uint32_t overlayId,
                                         TerrainDecal::TerrainDecalOverlayOverrides& overrides, void* userData);
    bool ResolveOverlayOverridesImpl_(cISTEOverlayManager* overlayManager, uint32_t overlayId,
                                      TerrainDecal::TerrainDecalOverlayOverrides& overrides);
    TerrainDecalRecord* FindRecordByOverlayId_(cISTEOverlayManager* overlayManager, uint32_t overlayId) noexcept;

private:
    uint16_t versionTag_{};
    TerrainDecalRegistry registry_{};
    std::unique_ptr<TerrainDecal::TerrainDecalHook> renderHook_{};
    std::vector<TerrainDecalSnapshot> pendingLoadedDecals_{};
    bool enableExperimentalRenderer_ = true;
    int defaultDepthOffset_ = 4;
    bool cityLoaded_ = false;
};
