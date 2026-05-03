#pragma once

#include <cstdint>

#include "cGZPersistResourceKey.h"
#include "cIGZUnknown.h"
#include "cISTEOverlayManager.h"
#include "cISTETerrainView.h"
#include "cS3DVector3.h"

struct TerrainDecalId {
    uint32_t value = 0;
};

enum class TerrainDecalUvMode : uint32_t {
    StretchSubrect = 0,
    ClipSubrect = 1,
};

struct TerrainDecalUvWindow {
    float u1 = 0.0f;
    float v1 = 0.0f;
    float u2 = 1.0f;
    float v2 = 1.0f;
    TerrainDecalUvMode mode = TerrainDecalUvMode::StretchSubrect;
};

struct TerrainDecalState {
    cGZPersistResourceKey textureKey{};
    cISTETerrainView::tOverlayManagerType overlayType = cISTETerrainView::tOverlayManagerType::DynamicLand;
    cISTEOverlayManager::cDecalInfo decalInfo{};
    float opacity = 1.0f;
    bool enabled = true;
    cS3DVector3 color = cS3DVector3(1.0f, 1.0f, 1.0f);
    uint8_t drawMode = 0;
    uint32_t flags = 0;
    bool hasUvWindow = false;
    TerrainDecalUvWindow uvWindow{};
    // -1 falls back to the service-level default (INI: TerrainDecalDefaultDepthOffset).
    // Vanilla SC4 decals use 2; shadows use 3. Set to 4+ to render above shadows.
    int depthOffset = -1;
};

struct TerrainDecalSnapshot {
    TerrainDecalId id{};
    TerrainDecalState state{};
};

// ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
class cIGZTerrainDecalService : public cIGZUnknown {
public:
    [[nodiscard]] virtual uint32_t GetServiceID() const = 0;

    virtual bool CreateDecal(const TerrainDecalState& initialState, TerrainDecalId* outId) = 0;
    virtual bool RemoveDecal(TerrainDecalId id) = 0;

    virtual bool GetDecal(TerrainDecalId id, TerrainDecalSnapshot* outSnapshot) const = 0;
    virtual bool ReplaceDecal(TerrainDecalId id, const TerrainDecalState& newState) = 0;

    [[nodiscard]] virtual uint32_t GetDecalCount() const = 0;
    virtual uint32_t CopyDecals(TerrainDecalSnapshot* buffer, uint32_t capacity) const = 0;
};
