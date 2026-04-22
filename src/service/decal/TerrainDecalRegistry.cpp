#include "TerrainDecalRegistry.h"

void TerrainDecalRegistry::Clear() noexcept
{
    records_.clear();
    nextId_ = 1;
}

TerrainDecalId TerrainDecalRegistry::AllocateId() noexcept
{
    return TerrainDecalId{nextId_++};
}

bool TerrainDecalRegistry::Insert(TerrainDecalRecord&& record)
{
    if (record.id.value == 0) {
        return false;
    }

    auto [it, inserted] = records_.emplace(record.id.value, std::move(record));
    return inserted && it != records_.end();
}

bool TerrainDecalRegistry::Remove(const TerrainDecalId id)
{
    return records_.erase(id.value) > 0;
}

TerrainDecalRecord* TerrainDecalRegistry::Find(const TerrainDecalId id) noexcept
{
    const auto it = records_.find(id.value);
    return it != records_.end() ? &it->second : nullptr;
}

const TerrainDecalRecord* TerrainDecalRegistry::Find(const TerrainDecalId id) const noexcept
{
    const auto it = records_.find(id.value);
    return it != records_.end() ? &it->second : nullptr;
}

uint32_t TerrainDecalRegistry::GetCount() const noexcept
{
    return static_cast<uint32_t>(records_.size());
}

void TerrainDecalRegistry::UpdateNextIdFromLoaded(const TerrainDecalId loadedId) noexcept
{
    if (loadedId.value >= nextId_) {
        nextId_ = loadedId.value + 1;
    }
}

const std::map<uint32_t, TerrainDecalRecord>& TerrainDecalRegistry::Records() const noexcept
{
    return records_;
}
