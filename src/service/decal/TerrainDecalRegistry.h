#pragma once

#include <map>
#include <optional>
#include <vector>

#include "public/cIGZTerrainDecalService.h"

struct TerrainDecalRuntimeAttachment {
    std::optional<uint32_t> overlayId;
};

struct TerrainDecalRecord {
    TerrainDecalId id{};
    TerrainDecalState state{};
    TerrainDecalRuntimeAttachment runtime{};
};

class TerrainDecalRegistry {
public:
    void Clear() noexcept;

    [[nodiscard]] TerrainDecalId AllocateId() noexcept;
    bool Insert(TerrainDecalRecord&& record);
    bool Remove(TerrainDecalId id);

    [[nodiscard]] TerrainDecalRecord* Find(TerrainDecalId id) noexcept;
    [[nodiscard]] const TerrainDecalRecord* Find(TerrainDecalId id) const noexcept;

    [[nodiscard]] uint32_t GetCount() const noexcept;

    void UpdateNextIdFromLoaded(TerrainDecalId loadedId) noexcept;

    [[nodiscard]] const std::map<uint32_t, TerrainDecalRecord>& Records() const noexcept;

private:
    std::map<uint32_t, TerrainDecalRecord> records_{};
    uint32_t nextId_ = 1;
};
