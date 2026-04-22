#include "cIGZFrameWork.h"
#include "cISC4BuildingOccupant.h"
#include "cISC4Lot.h"
#include "cISC4LotConfiguration.h"
#include "cISC4LotManager.h"
#include "cISC4Occupant.h"
#include "cISCPropertyHolder.h"
#include "cISC4TrafficSimulator.h"
#include "cRZCOMDllDirector.h"
#include "cRZBaseString.h"
#include "SC4List.h"
#include "SC4Rect.h"
#include "SC4Vector.h"
#include "utils/Logger.h"
#include "utils/VersionDetection.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>
#include <Windows.h>

namespace {
    constexpr uint32_t kTransitAccessDirectorID = 0x6A4F5142;

    constexpr uintptr_t kGetLotFacingStreetCountNetworkMaskPushAddress = 0x004BE6DC;
    constexpr uintptr_t kGetLotFacingStreetCountScoringMaskTestAddress = 0x004BE70B;
    constexpr uintptr_t kFerryTerminalRoadAccessNetworkMaskPushAddress = 0x006C1726;
    constexpr uintptr_t kCalculateRoadAccessAddress = 0x006C1A30;
    constexpr uintptr_t kCalculateRoadAccessNetworkMaskPushAddress = 0x006C1BD1;
    constexpr uintptr_t kRoadAccessMapLookupAddress = 0x006C0F30;
    constexpr uintptr_t kSpLotManagerAddress = 0x00B43D08;
    constexpr uintptr_t kSpTrafficSimulatorAddress = 0x00B43DA8;
    constexpr uintptr_t kSpTrafficNetworkMapAddress = 0x00B43D54;

    constexpr size_t kJumpByteCount = 5;
    constexpr size_t kPatchByteCount = 6;
    constexpr size_t kGetConnectedDestinationCountVTableIndex = 0x98 / sizeof(void*);
    constexpr size_t kGetSubnetworksForLotVTableIndex = 0x9C / sizeof(void*);
    constexpr uint32_t kTransitSwitchPointProperty = 0xE90E25A1;
    constexpr uint32_t kStockRoadLikeNetworkMask = 0x00000449;
    constexpr uint32_t kStockLowPriorityFacingNetworkMask = 0x00000408;
    constexpr uint32_t kDirtRoadNetworkMask = 1u << 11;
    constexpr uint32_t kTrafficNetworkExcludedFlag = 0x00200000;
    constexpr bool kPatchStockRoadAccessForDirtRoad = true;

    constexpr uint32_t GetEffectiveRoadLikeNetworkMask() {
        return kPatchStockRoadAccessForDirtRoad
            ? (kStockRoadLikeNetworkMask | kDirtRoadNetworkMask)
            : kStockRoadLikeNetworkMask;
    }

    constexpr uint32_t GetEffectiveLowPriorityFacingNetworkMask() {
        return kPatchStockRoadAccessForDirtRoad
            ? (kStockLowPriorityFacingNetworkMask | kDirtRoadNetworkMask)
            : kStockLowPriorityFacingNetworkMask;
    }

    struct InlineHook {
        const char* name;
        uintptr_t address;
        void* hookFn;
        uintptr_t patchAddress = 0;
        std::array<uint8_t, kPatchByteCount> original{};
        void* trampoline = nullptr;
        bool installed = false;
    };

    using CalculateRoadAccessFn = bool(__thiscall*)(cISC4Lot*);
    using RoadAccessMapLookupFn = uint8_t*(__thiscall*)(void*, int32_t*);
    using GetConnectedDestinationCountFn = uint32_t(__thiscall*)(cISC4TrafficSimulator*, cISC4Lot*, int);
    using GetSubnetworksForLotFn = bool(__thiscall*)(cISC4TrafficSimulator*, cISC4Lot*, SC4Vector<uint32_t>&);

    CalculateRoadAccessFn gOriginalCalculateRoadAccess = nullptr;
    GetConnectedDestinationCountFn gOriginalGetConnectedDestinationCount = nullptr;
    GetSubnetworksForLotFn gOriginalGetSubnetworksForLot = nullptr;
    std::atomic<uint32_t> gHookCallCount{0};
    std::atomic<uint32_t> gOriginalFailureCount{0};
    std::atomic<uint32_t> gExceptionSuccessCount{0};
    std::atomic<uint32_t> gTrafficSimulatorHookInstallAttemptCount{0};
    std::atomic<uint32_t> gDestinationHookCallCount{0};
    std::atomic<uint32_t> gDestinationExceptionCount{0};
    std::atomic<uint32_t> gSubnetworksHookCallCount{0};
    std::atomic<uint32_t> gSubnetworksExceptionCount{0};
    bool gDirtRoadMaskPatchInstalled = false;
    void** gTrafficSimulatorVTable = nullptr;
    void* gOriginalGetConnectedDestinationCountSlot = nullptr;
    void* gOriginalGetSubnetworksForLotSlot = nullptr;
    bool gTrafficSimulatorHookInstalled = false;

    bool InstallTrafficSimulatorHook();
    std::vector<cISC4Lot*> GetTransitAccessCandidateLots(cISC4Lot* lot);

    bool ShouldLogSample(uint32_t count) {
        return count <= 64 || (count % 1024) == 0;
    }

    std::string GetLotDebugLabel(cISC4Lot* lot) {
        if (!lot) {
            return "<null lot>";
        }

        int32_t lotX = 0;
        int32_t lotZ = 0;
        const bool hasLocation = lot->GetLocation(lotX, lotZ);

        std::string name;

        if (cISC4LotConfiguration* lotConfiguration = lot->GetLotConfiguration()) {
            cRZBaseString lotConfigurationName;
            if (lotConfiguration->GetName(lotConfigurationName)) {
                const char* text = lotConfigurationName.ToChar();
                if (text && text[0] != '\0') {
                    name = text;
                }
            }
        }

        if (name.empty()) {
            if (cISC4BuildingOccupant* building = lot->GetBuilding()) {
                cRZBaseString buildingName;
                if (building->GetName(buildingName)) {
                    const char* text = buildingName.ToChar();
                    if (text && text[0] != '\0') {
                        name = text;
                    }
                }

                if (name.empty()) {
                    cIGZString* buildingNameValue = building->GetBuildingName();
                    if (buildingNameValue) {
                        const char* text = buildingNameValue->ToChar();
                        if (text && text[0] != '\0') {
                            name = text;
                        }
                    }
                }

                if (name.empty()) {
                    cIGZString* exemplarName = building->GetExemplarName();
                    if (exemplarName) {
                        const char* text = exemplarName->ToChar();
                        if (text && text[0] != '\0') {
                            name = text;
                        }
                    }
                }
            }
        }

        if (name.empty()) {
            name = "lot";
        }

        if (hasLocation) {
            return fmt::format("{} at ({}, {})", name, lotX, lotZ);
        }

        return fmt::format("{} at (<unknown>, <unknown>)", name);
    }

    size_t GetLotOccupantCount(cISC4Lot* lot) {
        if (!lot) {
            return 0;
        }

        SC4List<cISC4Occupant*> occupants;
        if (!lot->GetLotOccupants(occupants)) {
            return 0;
        }

        size_t count = 0;
        for (cISC4Occupant* occupant : occupants) {
            (void)occupant;
            ++count;
        }
        return count;
    }

    cISC4LotManager* GetLotManager() {
        return *reinterpret_cast<cISC4LotManager**>(kSpLotManagerAddress);
    }

    cISC4TrafficSimulator* GetTrafficSimulator() {
        return *reinterpret_cast<cISC4TrafficSimulator**>(kSpTrafficSimulatorAddress);
    }

    void* GetTrafficNetworkMap() {
        return *reinterpret_cast<void**>(kSpTrafficNetworkMapAddress);
    }

    uintptr_t ResolvePatchAddress(uintptr_t address) {
        uintptr_t current = address;
        for (int i = 0; i < 6; ++i) {
            const auto* p = reinterpret_cast<const uint8_t*>(current);
            if (p[0] == 0xE9) {
                const auto rel = *reinterpret_cast<const int32_t*>(p + 1);
                current = current + 5 + rel;
                continue;
            }
            if (p[0] == 0xEB) {
                const auto rel = static_cast<int8_t>(p[1]);
                current = current + 2 + rel;
                continue;
            }
            if (p[0] == 0xFF && p[1] == 0x25) {
                const auto mem = *reinterpret_cast<const uintptr_t*>(p + 2);
                current = *reinterpret_cast<const uintptr_t*>(mem);
                continue;
            }
            break;
        }
        return current;
    }

    bool IsRel32InRange(uintptr_t from, uintptr_t to, int32_t& relOut) {
        const auto delta = static_cast<intptr_t>(to) - static_cast<intptr_t>(from + kJumpByteCount);
        if (delta < static_cast<intptr_t>(std::numeric_limits<int32_t>::min()) ||
            delta > static_cast<intptr_t>(std::numeric_limits<int32_t>::max())) {
            return false;
        }
        relOut = static_cast<int32_t>(delta);
        return true;
    }

    bool InstallInlineHook(InlineHook& hook) {
        if (hook.installed) {
            LOG_DEBUG("TransitAccess: {} hook already installed at 0x{:08X}",
                      hook.name,
                      static_cast<uint32_t>(hook.patchAddress));
            return true;
        }

        hook.patchAddress = ResolvePatchAddress(hook.address);
        auto* target = reinterpret_cast<uint8_t*>(hook.patchAddress);
        LOG_DEBUG("TransitAccess: resolved {} from 0x{:08X} to 0x{:08X}",
                  hook.name,
                  static_cast<uint32_t>(hook.address),
                  static_cast<uint32_t>(hook.patchAddress));

        constexpr std::array<uint8_t, kPatchByteCount> kExpectedRoadAccessPrologue{
            0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8
        };
        if (hook.address == kCalculateRoadAccessAddress &&
            std::memcmp(target, kExpectedRoadAccessPrologue.data(), kExpectedRoadAccessPrologue.size()) != 0) {
            LOG_ERROR("TransitAccess: unexpected CalculateRoadAccess prologue at 0x{:08X}",
                      static_cast<uint32_t>(hook.patchAddress));
            return false;
        }

        std::memcpy(hook.original.data(), target, hook.original.size());
        LOG_TRACE("TransitAccess: original {} prologue {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                  hook.name,
                  hook.original[0],
                  hook.original[1],
                  hook.original[2],
                  hook.original[3],
                  hook.original[4],
                  hook.original[5]);

        auto* trampoline = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kPatchByteCount + kJumpByteCount, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        if (!trampoline) {
            LOG_ERROR("TransitAccess: failed to allocate trampoline for {}", hook.name);
            return false;
        }

        std::memcpy(trampoline, target, kPatchByteCount);
        trampoline[kPatchByteCount] = 0xE9;

        int32_t trampolineRel = 0;
        if (!IsRel32InRange(reinterpret_cast<uintptr_t>(trampoline + kPatchByteCount),
                            reinterpret_cast<uintptr_t>(target + kPatchByteCount),
                            trampolineRel)) {
            LOG_ERROR("TransitAccess: trampoline rel32 out of range for {}", hook.name);
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return false;
        }
        std::memcpy(trampoline + kPatchByteCount + 1, &trampolineRel, sizeof(trampolineRel));

        int32_t hookRel = 0;
        if (!IsRel32InRange(reinterpret_cast<uintptr_t>(target),
                            reinterpret_cast<uintptr_t>(hook.hookFn),
                            hookRel)) {
            LOG_ERROR("TransitAccess: hook rel32 out of range for {}", hook.name);
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return false;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(target, kPatchByteCount, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LOG_ERROR("TransitAccess: VirtualProtect failed while installing {}", hook.name);
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return false;
        }

        target[0] = 0xE9;
        std::memcpy(target + 1, &hookRel, sizeof(hookRel));
        for (size_t i = kJumpByteCount; i < kPatchByteCount; ++i) {
            target[i] = 0x90;
        }

        FlushInstructionCache(GetCurrentProcess(), target, kPatchByteCount);
        VirtualProtect(target, kPatchByteCount, oldProtect, &oldProtect);

        hook.trampoline = trampoline;
        hook.installed = true;
        LOG_DEBUG("TransitAccess: installed {} trampoline at 0x{:08X}",
                  hook.name,
                  reinterpret_cast<uint32_t>(hook.trampoline));
        return true;
    }

    void UninstallInlineHook(InlineHook& hook) {
        if (!hook.installed) {
            return;
        }

        auto* target = reinterpret_cast<uint8_t*>(hook.patchAddress);
        DWORD oldProtect = 0;
        if (VirtualProtect(target, kPatchByteCount, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            std::memcpy(target, hook.original.data(), hook.original.size());
            FlushInstructionCache(GetCurrentProcess(), target, kPatchByteCount);
            VirtualProtect(target, kPatchByteCount, oldProtect, &oldProtect);
            LOG_DEBUG("TransitAccess: restored {} at 0x{:08X}",
                      hook.name,
                      static_cast<uint32_t>(hook.patchAddress));
        }

        if (hook.trampoline) {
            VirtualFree(hook.trampoline, 0, MEM_RELEASE);
        }

        hook.trampoline = nullptr;
        hook.patchAddress = 0;
        hook.installed = false;
    }

    bool PatchImmediate32(
        uintptr_t immediateAddress,
        uint32_t expectedCurrentValue,
        uint32_t newValue,
        const char* description) {

        auto* immediate = reinterpret_cast<uint32_t*>(immediateAddress);
        if (*immediate == newValue) {
            LOG_DEBUG("TransitAccess: {} immediate already patched at 0x{:08X}: 0x{:08X}",
                      description,
                      static_cast<uint32_t>(immediateAddress),
                      newValue);
            return true;
        }
        if (*immediate != expectedCurrentValue) {
            LOG_ERROR("TransitAccess: unexpected {} immediate at 0x{:08X}: current=0x{:08X} expected=0x{:08X}",
                      description,
                      static_cast<uint32_t>(immediateAddress),
                      *immediate,
                      expectedCurrentValue);
            return false;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(immediate, sizeof(newValue), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LOG_ERROR("TransitAccess: VirtualProtect failed while patching {}", description);
            return false;
        }

        std::memcpy(immediate, &newValue, sizeof(newValue));
        FlushInstructionCache(GetCurrentProcess(), immediate, sizeof(newValue));
        VirtualProtect(immediate, sizeof(newValue), oldProtect, &oldProtect);

        LOG_INFO("TransitAccess: patched {} at 0x{:08X}: 0x{:08X} -> 0x{:08X}",
                 description,
                 static_cast<uint32_t>(immediateAddress),
                 expectedCurrentValue,
                 newValue);
        return true;
    }

    bool PatchPushImm32(uintptr_t pushAddress, uint32_t expectedCurrentMask, uint32_t newMask, const char* description) {
        auto* pushInstruction = reinterpret_cast<uint8_t*>(pushAddress);
        if (pushInstruction[0] != 0x68) {
            LOG_ERROR("TransitAccess: expected PUSH imm32 for {} at 0x{:08X}",
                      description,
                      static_cast<uint32_t>(pushAddress));
            return false;
        }
        return PatchImmediate32(pushAddress + 1, expectedCurrentMask, newMask, description);
    }

    bool PatchTestEaxImm32(uintptr_t testAddress, uint32_t expectedCurrentMask, uint32_t newMask, const char* description) {
        auto* testInstruction = reinterpret_cast<uint8_t*>(testAddress);
        if (testInstruction[0] != 0xA9) {
            LOG_ERROR("TransitAccess: expected TEST EAX, imm32 for {} at 0x{:08X}",
                      description,
                      static_cast<uint32_t>(testAddress));
            return false;
        }
        return PatchImmediate32(testAddress + 1, expectedCurrentMask, newMask, description);
    }

    bool PatchDirtRoadMasks(bool enableDirtRoad) {
        const uint32_t expectedRoadMask = enableDirtRoad
            ? kStockRoadLikeNetworkMask
            : GetEffectiveRoadLikeNetworkMask();
        const uint32_t newRoadMask = enableDirtRoad
            ? GetEffectiveRoadLikeNetworkMask()
            : kStockRoadLikeNetworkMask;
        const uint32_t expectedFacingMask = enableDirtRoad
            ? kStockLowPriorityFacingNetworkMask
            : GetEffectiveLowPriorityFacingNetworkMask();
        const uint32_t newFacingMask = enableDirtRoad
            ? GetEffectiveLowPriorityFacingNetworkMask()
            : kStockLowPriorityFacingNetworkMask;

        bool success = true;
        success = PatchPushImm32(
            kCalculateRoadAccessNetworkMaskPushAddress,
            expectedRoadMask,
            newRoadMask,
            "CalculateRoadAccess road-like mask") && success;
        success = PatchPushImm32(
            kFerryTerminalRoadAccessNetworkMaskPushAddress,
            expectedRoadMask,
            newRoadMask,
            "CalculateFerryTerminalRoadAccess road-like mask") && success;
        success = PatchPushImm32(
            kGetLotFacingStreetCountNetworkMaskPushAddress,
            expectedRoadMask,
            newRoadMask,
            "GetLotFacingStreetCount lookup mask") && success;
        success = PatchTestEaxImm32(
            kGetLotFacingStreetCountScoringMaskTestAddress,
            expectedFacingMask,
            newFacingMask,
            "GetLotFacingStreetCount low-priority scoring mask") && success;

        LOG_INFO("TransitAccess: {} DirtRoad mask patches {}",
                 enableDirtRoad ? "installed" : "removed",
                 success ? "successfully" : "with errors");
        if (enableDirtRoad && !success) {
            LOG_WARN("TransitAccess: rolling back partial DirtRoad mask patch install");
            PatchDirtRoadMasks(false);
        }
        return success;
    }

    bool RectsTouchBySide(const SC4Rect<int32_t>& a, const SC4Rect<int32_t>& b) {
        const bool zRangesOverlap = a.topLeftY <= b.bottomRightY && b.topLeftY <= a.bottomRightY;
        const bool xRangesOverlap = a.topLeftX <= b.bottomRightX && b.topLeftX <= a.bottomRightX;

        if (zRangesOverlap && (a.bottomRightX + 1 == b.topLeftX || b.bottomRightX + 1 == a.topLeftX)) {
            return true;
        }

        return xRangesOverlap && (a.bottomRightY + 1 == b.topLeftY || b.bottomRightY + 1 == a.topLeftY);
    }

    bool RectsTouchOrNearlyTouchBySide(
        const SC4Rect<int32_t>& a,
        const SC4Rect<int32_t>& b,
        int32_t maxGapCells) {

        const bool zRangesOverlap = a.topLeftY <= b.bottomRightY && b.topLeftY <= a.bottomRightY;
        const bool xRangesOverlap = a.topLeftX <= b.bottomRightX && b.topLeftX <= a.bottomRightX;

        if (zRangesOverlap) {
            const int32_t gapAB = b.topLeftX - a.bottomRightX - 1;
            const int32_t gapBA = a.topLeftX - b.bottomRightX - 1;
            if ((gapAB >= 0 && gapAB <= maxGapCells) ||
                (gapBA >= 0 && gapBA <= maxGapCells)) {
                return true;
            }
        }

        if (xRangesOverlap) {
            const int32_t gapAB = b.topLeftY - a.bottomRightY - 1;
            const int32_t gapBA = a.topLeftY - b.bottomRightY - 1;
            if ((gapAB >= 0 && gapAB <= maxGapCells) ||
                (gapBA >= 0 && gapBA <= maxGapCells)) {
                return true;
            }
        }

        return false;
    }

    std::string FormatSubnetworks(const SC4Vector<uint32_t>& subnetworks) {
        if (subnetworks.empty()) {
            return "[]";
        }

        std::string result = "[";
        bool first = true;
        for (uint32_t subnetwork : subnetworks) {
            if (!first) {
                result += ",";
            }
            result += fmt::format("{}", subnetwork);
            first = false;
        }
        result += "]";
        return result;
    }

    std::vector<cISC4Lot*> GetNearbySideLots(cISC4Lot* lot, int32_t maxGapCells) {
        std::vector<cISC4Lot*> result;
        cISC4LotManager* lotManager = GetLotManager();
        if (!lot || !lotManager) {
            return result;
        }

        SC4Rect<int32_t> sourceBounds;
        if (!lot->GetBoundingRect(sourceBounds)) {
            return result;
        }

        const int32_t width = sourceBounds.bottomRightX - sourceBounds.topLeftX + 1;
        const int32_t height = sourceBounds.bottomRightY - sourceBounds.topLeftY + 1;
        std::unordered_set<cISC4Lot*> candidates;
        if (width > 0 && height > 0) {
            candidates.reserve(static_cast<size_t>((width * 2) + (height * 2)));
        }

        auto addCandidate = [&](cISC4Lot* candidate) {
            if (!candidate || candidate == lot) {
                return;
            }
            candidates.insert(candidate);
        };

        SC4List<cISC4Lot*> surroundingLots;
        if (lotManager->GetLotsSurroundingLot(lot, surroundingLots, 0)) {
            for (cISC4Lot* candidate : surroundingLots) {
                addCandidate(candidate);
            }
        }

        SC4List<cISC4Lot*> rectLots;
        const int32_t scanRadius = maxGapCells + 1;
        if (lotManager->GetLotsInCellRect(
                rectLots,
                std::max(0, sourceBounds.topLeftX - scanRadius),
                std::max(0, sourceBounds.topLeftY - scanRadius),
                sourceBounds.bottomRightX + scanRadius,
                sourceBounds.bottomRightY + scanRadius,
                true)) {
            for (cISC4Lot* candidate : rectLots) {
                addCandidate(candidate);
            }
        }

        for (int32_t x = sourceBounds.topLeftX - scanRadius; x <= sourceBounds.bottomRightX + scanRadius; ++x) {
            for (int32_t z = sourceBounds.topLeftY - scanRadius; z <= sourceBounds.bottomRightY + scanRadius; ++z) {
                if (x < 0 || z < 0) {
                    continue;
                }
                addCandidate(lotManager->GetLot(x, z, true));
                addCandidate(lotManager->GetLot(x, z, false));
            }
        }

        result.reserve(candidates.size());
        for (cISC4Lot* candidate : candidates) {
            SC4Rect<int32_t> candidateBounds;
            if (candidate->GetBoundingRect(candidateBounds) &&
                RectsTouchOrNearlyTouchBySide(sourceBounds, candidateBounds, maxGapCells)) {
                result.push_back(candidate);
            }
        }

        return result;
    }

    std::vector<cISC4Lot*> GetSideTouchingAdjacentLots(cISC4Lot* lot) {
        return GetNearbySideLots(lot, 0);
    }

    std::vector<cISC4Lot*> GetTransitAccessCandidateLots(cISC4Lot* lot) {
        return GetNearbySideLots(lot, 1);
    }

    bool IsTransitEnabledLot(cISC4Lot* lot) {
        if (!lot) {
            return false;
        }

        cISCPropertyHolder* lotPropertyHolder = lot->AsPropertyHolder();
        if (lotPropertyHolder && lotPropertyHolder->HasProperty(kTransitSwitchPointProperty)) {
            LOG_TRACE("TransitAccess: lot {} TE property present on lot property holder",
                      reinterpret_cast<void*>(lot));
            return true;
        }

        cISC4BuildingOccupant* building = lot->GetBuilding();
        if (!building) {
            LOG_TRACE("TransitAccess: lot {} has no building for TE property check (occupants={})",
                      GetLotDebugLabel(lot),
                      GetLotOccupantCount(lot));
            return false;
        }

        cISC4Occupant* buildingOccupant = building->AsOccupant();
        if (!buildingOccupant) {
            LOG_TRACE("TransitAccess: lot {} building has no occupant for TE property check",
                      GetLotDebugLabel(lot));
            return false;
        }

        cISCPropertyHolder* buildingPropertyHolder = buildingOccupant->AsPropertyHolder();
        const bool result = buildingPropertyHolder &&
                            buildingPropertyHolder->HasProperty(kTransitSwitchPointProperty);
        LOG_TRACE("TransitAccess: lot {} TE property {} on building property holder",
                  GetLotDebugLabel(lot),
                  result ? "present" : "absent");
        return result;
    }

    bool TrafficNetworkEntryHasFlag(void* entry, uint32_t flag) {
        if (!entry) {
            return false;
        }

        using HasFlagFn = bool(__thiscall*)(void*, uint32_t);
        auto** vtable = *reinterpret_cast<void***>(entry);
        if (!vtable || !vtable[8]) {
            LOG_WARN("TransitAccess: traffic network entry {} has invalid vtable", entry);
            return false;
        }
        auto hasFlag = reinterpret_cast<HasFlagFn>(vtable[8]); // Windows vtable +0x20.
        return hasFlag(entry, flag);
    }

    bool HasRoadLikeNetworkAtCell(int32_t x, int32_t z) {
        void* trafficNetworkMap = GetTrafficNetworkMap();
        if (!trafficNetworkMap) {
            return false;
        }

        using GetNetworkInfoFn = void*(__thiscall*)(void*, int32_t, int32_t, uint32_t, bool);
        auto** vtable = *reinterpret_cast<void***>(trafficNetworkMap);
        if (!vtable || !vtable[8]) {
            LOG_WARN("TransitAccess: traffic network map {} has invalid vtable", trafficNetworkMap);
            return false;
        }
        auto getNetworkInfo = reinterpret_cast<GetNetworkInfoFn>(vtable[8]); // Windows vtable +0x20.

        void* entry = getNetworkInfo(trafficNetworkMap, x, z, GetEffectiveRoadLikeNetworkMask(), true);
        const bool result = entry && !TrafficNetworkEntryHasFlag(entry, kTrafficNetworkExcludedFlag);
        if (result) {
            LOG_TRACE("TransitAccess: road-like network hit at cell {},{} entry {}",
                      x,
                      z,
                      entry);
        }
        return result;
    }

    bool HasRoadLikeNetworkOnOrAroundLot(cISC4Lot* lot) {
        SC4Rect<int32_t> bounds;
        if (!lot || !lot->GetBoundingRect(bounds)) {
            LOG_TRACE("TransitAccess: could not get TE lot bounds for {}", GetLotDebugLabel(lot));
            return false;
        }

        LOG_TRACE("TransitAccess: checking TE lot {} network contact bounds [{},{},{},{}]",
                  GetLotDebugLabel(lot),
                  bounds.topLeftX,
                  bounds.topLeftY,
                  bounds.bottomRightX,
                  bounds.bottomRightY);

        for (int32_t z = bounds.topLeftY - 1; z <= bounds.bottomRightY + 1; ++z) {
            for (int32_t x = bounds.topLeftX - 1; x <= bounds.bottomRightX + 1; ++x) {
                if (x < 0 || z < 0) {
                    continue;
                }

                // On-street TE lots may own the cell that also has the road network, so
                // check both the lot footprint and its immediate perimeter.
                if (HasRoadLikeNetworkAtCell(x, z)) {
                    return true;
                }
            }
        }

        return false;
    }

    void SetRoadAccessCache(cISC4Lot* lot, bool value) {
        auto lookup = reinterpret_cast<RoadAccessMapLookupFn>(kRoadAccessMapLookupAddress);
        int32_t key = 0;
        auto* cacheValue = lookup(reinterpret_cast<uint8_t*>(lot) + 0x40, &key);
        if (cacheValue) {
            *cacheValue = value ? 1 : 0;
            LOG_DEBUG("TransitAccess: wrote road-access cache for {} = {}",
                      GetLotDebugLabel(lot),
                      value);
        } else {
            LOG_WARN("TransitAccess: road-access cache lookup returned null for {}",
                     GetLotDebugLabel(lot));
        }
    }

    bool GetLotSubnetworks(cISC4TrafficSimulator* trafficSimulator, cISC4Lot* lot, SC4Vector<uint32_t>& subnetworks) {
        subnetworks.clear();
        if (!trafficSimulator || !lot) {
            return false;
        }

        if (gOriginalGetSubnetworksForLot) {
            return gOriginalGetSubnetworksForLot(trafficSimulator, lot, subnetworks);
        }

        return trafficSimulator->GetSubnetworksForLot(lot, subnetworks);
    }

    bool GatherAdjacentTransitSubnetworks(
        cISC4TrafficSimulator* trafficSimulator,
        cISC4Lot* lot,
        SC4Vector<uint32_t>& subnetworks,
        bool shouldLog) {

        subnetworks.clear();
        if (!trafficSimulator || !lot) {
            return false;
        }

        std::unordered_set<uint32_t> seen;
        const std::vector<cISC4Lot*> candidates = GetTransitAccessCandidateLots(lot);
        if (shouldLog) {
            LOG_TRACE("TransitAccess: subnetworks fallback source {} has {} candidate lots",
                      GetLotDebugLabel(lot),
                      candidates.size());
        }

        for (cISC4Lot* candidate : candidates) {
            if (!IsTransitEnabledLot(candidate)) {
                continue;
            }

            SC4Vector<uint32_t> candidateSubnetworks;
            if (!GetLotSubnetworks(trafficSimulator, candidate, candidateSubnetworks)) {
                continue;
            }

            for (uint32_t subnetwork : candidateSubnetworks) {
                if (seen.insert(subnetwork).second) {
                    subnetworks.push_back(subnetwork);
                }
            }

            if (shouldLog) {
                LOG_TRACE("TransitAccess: subnetworks fallback candidate {} subnetworks={}",
                          GetLotDebugLabel(candidate),
                          FormatSubnetworks(candidateSubnetworks));
            }
        }

        return !subnetworks.empty();
    }

    bool GetSubnetworksAroundLot(
        cISC4TrafficSimulator* trafficSimulator,
        cISC4Lot* lot,
        SC4Vector<uint32_t>& subnetworks) {

        subnetworks.clear();
        if (!trafficSimulator || !lot) {
            return false;
        }

        SC4Rect<int32_t> bounds;
        if (!lot->GetBoundingRect(bounds)) {
            return false;
        }

        SC4Rect<int> scanBounds;
        scanBounds.topLeftX = std::max(0, bounds.topLeftX - 1);
        scanBounds.topLeftY = std::max(0, bounds.topLeftY - 1);
        scanBounds.bottomRightX = bounds.bottomRightX + 1;
        scanBounds.bottomRightY = bounds.bottomRightY + 1;
        return trafficSimulator->GetSubnetworksInRectangle(scanBounds, subnetworks);
    }

    uint32_t GetAdjacentTransitEnabledDestinationCount(
        cISC4TrafficSimulator* trafficSimulator,
        cISC4Lot* lot,
        int purpose,
        bool shouldLog) {

        if (!trafficSimulator || !lot || !gOriginalGetConnectedDestinationCount) {
            return 0;
        }

        uint32_t bestCount = 0;
        const std::vector<cISC4Lot*> candidates = GetTransitAccessCandidateLots(lot);

        if (shouldLog) {
            SC4Vector<uint32_t> sourceSubnetworks;
            GetLotSubnetworks(trafficSimulator, lot, sourceSubnetworks);
            LOG_TRACE("TransitAccess: destination fallback source {} purpose={} stock subnetworks={}",
                      GetLotDebugLabel(lot),
                      purpose,
                      FormatSubnetworks(sourceSubnetworks));
            LOG_TRACE("TransitAccess: destination fallback source {} has {} side-touching candidates",
                      GetLotDebugLabel(lot),
                      candidates.size());
        }

        for (cISC4Lot* candidate : candidates) {
            if (shouldLog) {
                SC4Rect<int32_t> candidateBounds;
                if (candidate->GetBoundingRect(candidateBounds)) {
                    LOG_TRACE("TransitAccess: destination fallback inspecting candidate {} bounds [{},{},{},{}]",
                              GetLotDebugLabel(candidate),
                              candidateBounds.topLeftX,
                              candidateBounds.topLeftY,
                              candidateBounds.bottomRightX,
                              candidateBounds.bottomRightY);
                } else {
                    LOG_TRACE("TransitAccess: destination fallback inspecting candidate {} bounds unavailable",
                              GetLotDebugLabel(candidate));
                }
            }

            if (!IsTransitEnabledLot(candidate)) {
                if (shouldLog) {
                    LOG_TRACE("TransitAccess: destination fallback candidate {} is not TE",
                              GetLotDebugLabel(candidate));
                }
                continue;
            }

            const uint32_t candidateCount = gOriginalGetConnectedDestinationCount(
                trafficSimulator,
                candidate,
                purpose);
            bestCount = std::max(bestCount, candidateCount);

            if (shouldLog || candidateCount > 0) {
                SC4Vector<uint32_t> candidateSubnetworks;
                SC4Vector<uint32_t> perimeterSubnetworks;
                GetLotSubnetworks(trafficSimulator, candidate, candidateSubnetworks);
                GetSubnetworksAroundLot(trafficSimulator, candidate, perimeterSubnetworks);

                LOG_TRACE("TransitAccess: destination fallback candidate {} purpose={} count={} lotSubnetworks={} perimeterSubnetworks={}",
                          GetLotDebugLabel(candidate),
                          purpose,
                          candidateCount,
                          FormatSubnetworks(candidateSubnetworks),
                          FormatSubnetworks(perimeterSubnetworks));
            }
        }

        return bestCount;
    }

    bool __fastcall HookGetSubnetworksForLot(
        cISC4TrafficSimulator* trafficSimulator,
        void*,
        cISC4Lot* lot,
        SC4Vector<uint32_t>& subnetworks) {

        const uint32_t hookCallCount = ++gSubnetworksHookCallCount;
        const bool shouldLog = ShouldLogSample(hookCallCount);

        bool stockResult = false;
        if (gOriginalGetSubnetworksForLot) {
            stockResult = gOriginalGetSubnetworksForLot(trafficSimulator, lot, subnetworks);
        } else {
            subnetworks.clear();
        }

        if (stockResult && !subnetworks.empty()) {
            if (shouldLog) {
                LOG_TRACE("TransitAccess: GetSubnetworksForLot stock lot {} subnetworks={}",
                          GetLotDebugLabel(lot),
                          FormatSubnetworks(subnetworks));
            }
            return true;
        }

        SC4Vector<uint32_t> fallbackSubnetworks;
        if (GatherAdjacentTransitSubnetworks(trafficSimulator, lot, fallbackSubnetworks, shouldLog)) {
            subnetworks = fallbackSubnetworks;
            const uint32_t exceptionCount = ++gSubnetworksExceptionCount;
            LOG_INFO("TransitAccess: subnetworks TE fallback lot {} subnetworks={} totalSubnetworksFallbacks={}",
                     GetLotDebugLabel(lot),
                     FormatSubnetworks(subnetworks),
                     exceptionCount);
            return true;
        }

        if (shouldLog) {
            LOG_TRACE("TransitAccess: GetSubnetworksForLot no TE fallback lot {}", GetLotDebugLabel(lot));
        }
        return stockResult;
    }

    void LogDestinationAccessDiagnostics(cISC4Lot* lot) {
        cISC4TrafficSimulator* trafficSimulator = GetTrafficSimulator();
        if (!trafficSimulator || !lot) {
            LOG_DEBUG("TransitAccess: destination diagnostics skipped for {} trafficSimulator={}",
                      GetLotDebugLabel(lot),
                      reinterpret_cast<void*>(trafficSimulator));
            return;
        }

        SC4Vector<uint32_t> sourceSubnetworks;
        GetLotSubnetworks(trafficSimulator, lot, sourceSubnetworks);

        const uint32_t sourcePurpose0 = gOriginalGetConnectedDestinationCount
            ? gOriginalGetConnectedDestinationCount(trafficSimulator, lot, 0)
            : 0;
        const uint32_t sourcePurpose1 = gOriginalGetConnectedDestinationCount
            ? gOriginalGetConnectedDestinationCount(trafficSimulator, lot, 1)
            : 0;
        LOG_INFO("TransitAccess: destination diagnostics source {} hookInstalled={} subnetworks={} purpose0={} purpose1={}",
                 GetLotDebugLabel(lot),
                 gTrafficSimulatorHookInstalled,
                 FormatSubnetworks(sourceSubnetworks),
                 sourcePurpose0,
                 sourcePurpose1);

        for (cISC4Lot* candidate : GetTransitAccessCandidateLots(lot)) {
            if (!IsTransitEnabledLot(candidate)) {
                continue;
            }

            SC4Vector<uint32_t> candidateSubnetworks;
            SC4Vector<uint32_t> perimeterSubnetworks;
            GetLotSubnetworks(trafficSimulator, candidate, candidateSubnetworks);
            GetSubnetworksAroundLot(trafficSimulator, candidate, perimeterSubnetworks);

            const uint32_t candidatePurpose0 = gOriginalGetConnectedDestinationCount
                ? gOriginalGetConnectedDestinationCount(trafficSimulator, candidate, 0)
                : 0;
            const uint32_t candidatePurpose1 = gOriginalGetConnectedDestinationCount
                ? gOriginalGetConnectedDestinationCount(trafficSimulator, candidate, 1)
                : 0;
            LOG_INFO("TransitAccess: destination diagnostics TE candidate {} subnetworks={} perimeterSubnetworks={} purpose0={} purpose1={}",
                     GetLotDebugLabel(candidate),
                     FormatSubnetworks(candidateSubnetworks),
                     FormatSubnetworks(perimeterSubnetworks),
                     candidatePurpose0,
                     candidatePurpose1);
        }
    }

    bool HasAdjacentTransitEnabledRoadAccess(cISC4Lot* lot) {
        cISC4LotManager* lotManager = GetLotManager();
        if (!lot || !lotManager) {
            LOG_TRACE("TransitAccess: missing lot or lot manager lot={} lotManager={}",
                      GetLotDebugLabel(lot),
                      reinterpret_cast<void*>(lotManager));
            return false;
        }

        SC4Rect<int32_t> sourceBounds;
        if (!lot->GetBoundingRect(sourceBounds)) {
            LOG_TRACE("TransitAccess: could not get source lot bounds for {}", GetLotDebugLabel(lot));
            return false;
        }

        LOG_TRACE("TransitAccess: scanning adjacent lots for source {} bounds [{},{},{},{}]",
                  GetLotDebugLabel(lot),
                  sourceBounds.topLeftX,
                  sourceBounds.topLeftY,
                  sourceBounds.bottomRightX,
                  sourceBounds.bottomRightY);

        const std::vector<cISC4Lot*> candidates = GetSideTouchingAdjacentLots(lot);

        LOG_TRACE("TransitAccess: found {} unique adjacent lot candidates for source {}",
                  candidates.size(),
                  GetLotDebugLabel(lot));

        for (cISC4Lot* candidate : candidates) {
            SC4Rect<int32_t> candidateBounds;
            if (!candidate->GetBoundingRect(candidateBounds)) {
                LOG_TRACE("TransitAccess: skipping candidate with no bounds {}", GetLotDebugLabel(candidate));
                continue;
            }

            LOG_TRACE("TransitAccess: checking adjacent candidate {} bounds [{},{},{},{}]",
                      GetLotDebugLabel(candidate),
                      candidateBounds.topLeftX,
                      candidateBounds.topLeftY,
                      candidateBounds.bottomRightX,
                      candidateBounds.bottomRightY);

            if (IsTransitEnabledLot(candidate) && HasRoadLikeNetworkOnOrAroundLot(candidate)) {
                LOG_DEBUG("TransitAccess: source lot {} gets road access through TE lot {}",
                          GetLotDebugLabel(lot),
                          GetLotDebugLabel(candidate));
                return true;
            }
        }

        return false;
    }

    bool __fastcall HookCalculateRoadAccess(cISC4Lot* lot, void*) {
        InstallTrafficSimulatorHook();

        const uint32_t hookCallCount = ++gHookCallCount;
        if (ShouldLogSample(hookCallCount)) {
            LOG_TRACE("TransitAccess: CalculateRoadAccess hook call {} lot {}",
                      hookCallCount,
                      GetLotDebugLabel(lot));
        }

        if (gOriginalCalculateRoadAccess && gOriginalCalculateRoadAccess(lot)) {
            if (ShouldLogSample(hookCallCount)) {
                LOG_TRACE("TransitAccess: original road access succeeded for {}", GetLotDebugLabel(lot));
            }
            return true;
        }

        const uint32_t originalFailureCount = ++gOriginalFailureCount;
        if (ShouldLogSample(originalFailureCount)) {
            LOG_DEBUG("TransitAccess: original road access failed for lot {} failureCount={}",
                      GetLotDebugLabel(lot),
                      originalFailureCount);
        }

        if (HasAdjacentTransitEnabledRoadAccess(lot)) {
            SetRoadAccessCache(lot, true);
            const uint32_t exceptionSuccessCount = ++gExceptionSuccessCount;
            LOG_INFO("TransitAccess: TE road-access exception success for lot {} totalSuccess={}",
                     GetLotDebugLabel(lot),
                     exceptionSuccessCount);
            LogDestinationAccessDiagnostics(lot);
            return true;
        }

        if (ShouldLogSample(originalFailureCount)) {
            LOG_TRACE("TransitAccess: no TE road-access exception for {}", GetLotDebugLabel(lot));
        }
        return false;
    }

    uint32_t __fastcall HookGetConnectedDestinationCount(
        cISC4TrafficSimulator* trafficSimulator,
        void*,
        cISC4Lot* lot,
        int purpose) {

        const uint32_t stockCount = gOriginalGetConnectedDestinationCount
            ? gOriginalGetConnectedDestinationCount(trafficSimulator, lot, purpose)
            : 0;
        const uint32_t hookCallCount = ++gDestinationHookCallCount;
        const bool shouldLog = ShouldLogSample(hookCallCount);

        if (stockCount != 0 || purpose < 0 || purpose > 1) {
            if (shouldLog) {
                LOG_TRACE("TransitAccess: GetConnectedDestinationCount stock lot {} purpose={} count={}",
                          GetLotDebugLabel(lot),
                          purpose,
                          stockCount);
            }
            return stockCount;
        }

        const uint32_t fallbackCount = GetAdjacentTransitEnabledDestinationCount(
            trafficSimulator,
            lot,
            purpose,
            shouldLog);
        if (fallbackCount != 0) {
            const uint32_t exceptionCount = ++gDestinationExceptionCount;
            LOG_INFO("TransitAccess: destination-count TE fallback lot {} purpose={} count={} totalDestinationFallbacks={}",
                     GetLotDebugLabel(lot),
                     purpose,
                     fallbackCount,
                     exceptionCount);
            return fallbackCount;
        }

        if (shouldLog) {
            LOG_TRACE("TransitAccess: GetConnectedDestinationCount no TE fallback lot {} purpose={}",
                      GetLotDebugLabel(lot),
                      purpose);
        }
        return 0;
    }

    bool InstallTrafficSimulatorHook() {
        if (gTrafficSimulatorHookInstalled) {
            return true;
        }

        const uint32_t attemptCount = ++gTrafficSimulatorHookInstallAttemptCount;
        cISC4TrafficSimulator* trafficSimulator = GetTrafficSimulator();
        if (!trafficSimulator) {
            if (ShouldLogSample(attemptCount)) {
                LOG_DEBUG("TransitAccess: traffic simulator not available for destination-count hook attempt={}",
                          attemptCount);
            }
            return false;
        }

        auto** vtable = *reinterpret_cast<void***>(trafficSimulator);
        if (!vtable || !vtable[kGetConnectedDestinationCountVTableIndex]) {
            LOG_WARN("TransitAccess: traffic simulator {} has invalid vtable attempt={}",
                     reinterpret_cast<void*>(trafficSimulator),
                     attemptCount);
            return false;
        }

        void** slot = &vtable[kGetConnectedDestinationCountVTableIndex];
        LOG_DEBUG("TransitAccess: destination-count hook attempt={} trafficSimulator={} vtable={} slot={} current={}",
                  attemptCount,
                  reinterpret_cast<void*>(trafficSimulator),
                  reinterpret_cast<void*>(vtable),
                  reinterpret_cast<void*>(slot),
                  *slot);
        if (*slot == reinterpret_cast<void*>(&HookGetConnectedDestinationCount)) {
            gTrafficSimulatorVTable = vtable;
            gTrafficSimulatorHookInstalled = true;
            LOG_DEBUG("TransitAccess: destination-count hook already installed at slot {}",
                      reinterpret_cast<void*>(slot));
            return true;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LOG_ERROR("TransitAccess: VirtualProtect failed while installing destination-count hook");
            return false;
        }

        gTrafficSimulatorVTable = vtable;
        gOriginalGetConnectedDestinationCountSlot = *slot;
        gOriginalGetConnectedDestinationCount =
            reinterpret_cast<GetConnectedDestinationCountFn>(gOriginalGetConnectedDestinationCountSlot);
        *slot = reinterpret_cast<void*>(&HookGetConnectedDestinationCount);

        FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
        VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);

        gTrafficSimulatorHookInstalled = true;
        LOG_INFO("TransitAccess: installed destination-count hook trafficSimulator={} vtable={} slot={} original={}",
                 reinterpret_cast<void*>(trafficSimulator),
                 reinterpret_cast<void*>(vtable),
                 reinterpret_cast<void*>(slot),
                 gOriginalGetConnectedDestinationCountSlot);

        void** subnetworksSlot = &vtable[kGetSubnetworksForLotVTableIndex];
        if (*subnetworksSlot != reinterpret_cast<void*>(&HookGetSubnetworksForLot)) {
            DWORD subnetworksOldProtect = 0;
            if (!VirtualProtect(subnetworksSlot, sizeof(void*), PAGE_EXECUTE_READWRITE, &subnetworksOldProtect)) {
                LOG_ERROR("TransitAccess: VirtualProtect failed while installing subnetworks hook");
                return true;
            }
            gOriginalGetSubnetworksForLotSlot = *subnetworksSlot;
            gOriginalGetSubnetworksForLot =
                reinterpret_cast<GetSubnetworksForLotFn>(gOriginalGetSubnetworksForLotSlot);
            *subnetworksSlot = reinterpret_cast<void*>(&HookGetSubnetworksForLot);
            FlushInstructionCache(GetCurrentProcess(), subnetworksSlot, sizeof(void*));
            VirtualProtect(subnetworksSlot, sizeof(void*), subnetworksOldProtect, &subnetworksOldProtect);
            LOG_INFO("TransitAccess: installed subnetworks hook slot={} original={}",
                     reinterpret_cast<void*>(subnetworksSlot),
                     gOriginalGetSubnetworksForLotSlot);
        }
        return true;
    }

    void UninstallTrafficSimulatorHook() {
        if (!gTrafficSimulatorHookInstalled || !gTrafficSimulatorVTable || !gOriginalGetConnectedDestinationCountSlot) {
            return;
        }

        void** slot = &gTrafficSimulatorVTable[kGetConnectedDestinationCountVTableIndex];
        DWORD oldProtect = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            if (*slot == reinterpret_cast<void*>(&HookGetConnectedDestinationCount)) {
                *slot = gOriginalGetConnectedDestinationCountSlot;
                FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
                LOG_DEBUG("TransitAccess: restored destination-count hook slot {}", reinterpret_cast<void*>(slot));
            }
            VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
        }

        if (gOriginalGetSubnetworksForLotSlot) {
            void** subnetworksSlot = &gTrafficSimulatorVTable[kGetSubnetworksForLotVTableIndex];
            DWORD subnetworksOldProtect = 0;
            if (VirtualProtect(subnetworksSlot, sizeof(void*), PAGE_EXECUTE_READWRITE, &subnetworksOldProtect)) {
                if (*subnetworksSlot == reinterpret_cast<void*>(&HookGetSubnetworksForLot)) {
                    *subnetworksSlot = gOriginalGetSubnetworksForLotSlot;
                    FlushInstructionCache(GetCurrentProcess(), subnetworksSlot, sizeof(void*));
                    LOG_DEBUG("TransitAccess: restored subnetworks hook slot {}", reinterpret_cast<void*>(subnetworksSlot));
                }
                VirtualProtect(subnetworksSlot, sizeof(void*), subnetworksOldProtect, &subnetworksOldProtect);
            }
        }

        gTrafficSimulatorVTable = nullptr;
        gOriginalGetConnectedDestinationCountSlot = nullptr;
        gOriginalGetConnectedDestinationCount = nullptr;
        gOriginalGetSubnetworksForLotSlot = nullptr;
        gOriginalGetSubnetworksForLot = nullptr;
        gTrafficSimulatorHookInstalled = false;
    }

    InlineHook gCalculateRoadAccessHook{
        "cSC4Lot::CalculateRoadAccess",
        kCalculateRoadAccessAddress,
        reinterpret_cast<void*>(&HookCalculateRoadAccess)
    };
}

class TransitAccessDirector final : public cRZCOMDllDirector {
public:
    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kTransitAccessDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZCOMDllDirector::OnStart(pCOM);

        Logger::Initialize("SC4TransitAccess", "");
        Logger::Get()->set_level(spdlog::level::trace);
        Logger::Get()->flush_on(spdlog::level::trace);
        LOG_INFO("TransitAccess: OnStart");

        const uint16_t gameVersion = VersionDetection::GetInstance().GetGameVersion();
        LOG_DEBUG("TransitAccess: detected game version {}", gameVersion);
        if (gameVersion != 641) {
            LOG_WARN("TransitAccess: unsupported game version {}, expected 641", gameVersion);
            return true;
        }

        LOG_DEBUG("TransitAccess: globals lotManager=0x{:08X} trafficSimulator=0x{:08X} trafficNetworkMap=0x{:08X}",
                  reinterpret_cast<uint32_t>(GetLotManager()),
                  reinterpret_cast<uint32_t>(GetTrafficSimulator()),
                  reinterpret_cast<uint32_t>(GetTrafficNetworkMap()));
        LOG_DEBUG("TransitAccess: road-like network mask stock=0x{:08X} dirtRoad=0x{:08X} effective=0x{:08X}",
                  kStockRoadLikeNetworkMask,
                  kDirtRoadNetworkMask,
                  GetEffectiveRoadLikeNetworkMask());
        if (kPatchStockRoadAccessForDirtRoad) {
            gDirtRoadMaskPatchInstalled = PatchDirtRoadMasks(true);
        } else {
            LOG_DEBUG("TransitAccess: DirtRoad mask patches disabled");
        }

        if (InstallInlineHook(gCalculateRoadAccessHook)) {
            gOriginalCalculateRoadAccess =
                reinterpret_cast<CalculateRoadAccessFn>(gCalculateRoadAccessHook.trampoline);
            LOG_INFO("TransitAccess: installed road-access hook at 0x{:08X}",
                     static_cast<uint32_t>(gCalculateRoadAccessHook.patchAddress));
        }
        InstallTrafficSimulatorHook();

        return true;
    }

    bool PostAppShutdown() override {
        LOG_INFO("TransitAccess: PostAppShutdown calls={} originalFailures={} exceptionSuccesses={} trafficHookAttempts={} destinationCalls={} destinationFallbacks={}",
                 gHookCallCount.load(),
                 gOriginalFailureCount.load(),
                 gExceptionSuccessCount.load(),
                 gTrafficSimulatorHookInstallAttemptCount.load(),
                 gDestinationHookCallCount.load(),
                 gDestinationExceptionCount.load());
        UninstallTrafficSimulatorHook();
        if (gDirtRoadMaskPatchInstalled) {
            PatchDirtRoadMasks(false);
            gDirtRoadMaskPatchInstalled = false;
        }
        UninstallInlineHook(gCalculateRoadAccessHook);
        gOriginalCalculateRoadAccess = nullptr;
        return true;
    }
};

static TransitAccessDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static bool sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
