#pragma once

#include <cstddef>
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
    // -1 falls back to the service-level default (INI: TerrainDecalCustomDefaultDepthOffset).
    // Vanilla SC4 decals use 2; shadows use 3. Set to 4+ to render above shadows.
    int depthOffset = -1;
};

struct TerrainDecalSnapshot {
    TerrainDecalId id{};
    TerrainDecalState state{};
};

static constexpr size_t kTerrainDecalStateSizeV1 = offsetof(TerrainDecalState, depthOffset);
static constexpr size_t kTerrainDecalStateSizeV2 = sizeof(TerrainDecalState);
static constexpr size_t kTerrainDecalSnapshotSizeV1 = offsetof(TerrainDecalSnapshot, state) + kTerrainDecalStateSizeV1;
static constexpr size_t kTerrainDecalSnapshotSizeV2 = sizeof(TerrainDecalSnapshot);

// ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
class cIGZTerrainDecalService : public cIGZUnknown {
public:
    [[nodiscard]] virtual uint32_t GetServiceID() const = 0;

    // Legacy ABI. Struct reads/writes are limited to kTerrainDecalStateSizeV1 /
    // kTerrainDecalSnapshotSizeV1; depthOffset uses the service default.
    virtual bool CreateDecal(const TerrainDecalState& initialState, TerrainDecalId* outId) = 0;
    virtual bool RemoveDecal(TerrainDecalId id) = 0;

    virtual bool GetDecal(TerrainDecalId id, TerrainDecalSnapshot* outSnapshot) const = 0;
    virtual bool ReplaceDecal(TerrainDecalId id, const TerrainDecalState& newState) = 0;

    [[nodiscard]] virtual uint32_t GetDecalCount() const = 0;
    virtual uint32_t CopyDecals(TerrainDecalSnapshot* buffer, uint32_t capacity) const = 0;
};

// ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
class cIGZTerrainDecalService2 : public cIGZTerrainDecalService {
public:
    [[nodiscard]] virtual uint32_t GetStateSize() const = 0;
    [[nodiscard]] virtual uint32_t GetSnapshotSize() const = 0;

    virtual bool CreateDecal2(const TerrainDecalState* initialState, uint32_t stateSize, TerrainDecalId* outId) = 0;
    virtual bool GetDecal2(TerrainDecalId id, TerrainDecalSnapshot* outSnapshot, uint32_t snapshotSize) const = 0;
    virtual bool ReplaceDecal2(TerrainDecalId id, const TerrainDecalState* newState, uint32_t stateSize) = 0;
    virtual uint32_t CopyDecals2(TerrainDecalSnapshot* buffer, uint32_t capacity, uint32_t snapshotSize) const = 0;
};
