#include "cIGZFrameWork.h"
#include "cRZCOMDllDirector.h"
#include "cRZBaseString.h"
#include "cISC4BuildingOccupant.h"
#include "cISC4Lot.h"
#include "cISC4LotConfiguration.h"
#include "cISC4LotManager.h"
#include "cISC4Occupant.h"
#include "cISCPropertyHolder.h"
#include "SC4List.h"
#include "SC4Rect.h"
#include "utils/Logger.h"
#include "utils/VersionDetection.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
    constexpr uint32_t kTransitAccessDirectorID = 0x6A4F5142;

    constexpr uintptr_t kGetLotFacingStreetCountNetworkMaskPushAddress = 0x004BE6DC;
    constexpr uintptr_t kGetLotFacingStreetCountScoringMaskTestAddress = 0x004BE70B;
    constexpr uintptr_t kFerryTerminalRoadAccessNetworkMaskPushAddress = 0x006C1726;
    constexpr uintptr_t kCalculateRoadAccessAddress = 0x006C1A30;
    constexpr uintptr_t kCalculateRoadAccessNetworkMaskPushAddress = 0x006C1BD1;
    constexpr uintptr_t kRoadAccessMapLookupAddress = 0x006C0F30;
    constexpr uintptr_t kCreateStartNodesAddress = 0x006D8A90;
    constexpr uintptr_t kSetupPathFinderForLotAddress = 0x00711610;
    constexpr uintptr_t kSpLotManagerAddress = 0x00B43D08;
    constexpr uintptr_t kSpTrafficNetworkMapAddress = 0x00B43D54;

    constexpr size_t kJumpByteCount = 5;
    constexpr size_t kPatchByteCount = 6;
    constexpr uint32_t kTransitSwitchPointProperty = 0xE90E25A1;
    constexpr uint32_t kStockRoadLikeNetworkMask = 0x00000449;
    constexpr uint32_t kStockLowPriorityFacingNetworkMask = 0x00000408;
    constexpr uint32_t kDirtRoadNetworkMask = 1u << 11;
    constexpr uint32_t kTrafficNetworkExcludedFlag = 0x00200000;
    constexpr bool kPatchStockRoadAccessForDirtRoad = false;

    // Raise to debug or trace when diagnosing in-game behavior. Trace logging is
    // deliberately off by default because the hooks run on hot simulator paths.
    constexpr spdlog::level::level_enum kTransitAccessSampleLogLevel = spdlog::level::info;
    constexpr spdlog::level::level_enum kTransitAccessSampleFlushLevel = spdlog::level::warn;

    constexpr ptrdiff_t kPathFinderTrafficSimulatorOffset = 0x0C;
    constexpr ptrdiff_t kPathFinderSourceMinXOffset = 0x18;
    constexpr ptrdiff_t kPathFinderSourceMinZOffset = 0x1C;
    constexpr ptrdiff_t kPathFinderSourceMaxXOffset = 0x20;
    constexpr ptrdiff_t kPathFinderSourceMaxZOffset = 0x24;
    constexpr ptrdiff_t kPathFinderCityWidthOffset = 0xB8;
    constexpr ptrdiff_t kPathFinderCityHeightOffset = 0xBC;

    constexpr uint32_t GetEffectiveRoadLikeNetworkMask() {
        if constexpr (kPatchStockRoadAccessForDirtRoad) {
            return kStockRoadLikeNetworkMask | kDirtRoadNetworkMask;
        }
        return kStockRoadLikeNetworkMask;
    }

    constexpr uint32_t GetEffectiveLowPriorityFacingNetworkMask() {
        if constexpr (kPatchStockRoadAccessForDirtRoad) {
            return kStockLowPriorityFacingNetworkMask | kDirtRoadNetworkMask;
        }
        return kStockLowPriorityFacingNetworkMask;
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
    using CreateStartNodesFn = bool(__thiscall*)(void*);
    using SetupPathFinderForLotFn = bool(__thiscall*)(void*, void*, cISC4Lot*);
    using RoadAccessMapLookupFn = uint8_t*(__thiscall*)(void*, int32_t*);

    CalculateRoadAccessFn gOriginalCalculateRoadAccess = nullptr;
    CreateStartNodesFn gOriginalCreateStartNodes = nullptr;
    SetupPathFinderForLotFn gOriginalSetupPathFinderForLot = nullptr;
    std::atomic<uint32_t> gHookCallCount{0};
    std::atomic<uint32_t> gOriginalFailureCount{0};
    std::atomic<uint32_t> gExceptionSuccessCount{0};
    std::atomic<uint32_t> gSetupPathFinderHookCallCount{0};
    std::atomic<uint32_t> gSetupPathFinderSuccessCount{0};
    std::atomic<uint32_t> gStartNodesHookCallCount{0};
    std::atomic<uint32_t> gStartNodesOriginalFailureCount{0};
    std::atomic<uint32_t> gStartNodesSourceLotSideTableHitCount{0};
    std::atomic<uint32_t> gStartNodesSourceLotFallbackCount{0};
    std::atomic<uint32_t> gStartNodesMissingSourceLotCount{0};
    std::atomic<uint32_t> gStartNodesRetrySuccessCount{0};
    std::mutex gPathFinderSourceLotsMutex;
    std::unordered_map<void*, cISC4Lot*> gPathFinderSourceLots;
    thread_local bool gInsideCreateStartNodesRetry = false;
    bool gDirtRoadMaskPatchInstalled = false;

    const char* LevelName(spdlog::level::level_enum level) {
        switch (level) {
            case spdlog::level::trace: return "trace";
            case spdlog::level::debug: return "debug";
            case spdlog::level::info: return "info";
            case spdlog::level::warn: return "warn";
            case spdlog::level::err: return "error";
            case spdlog::level::critical: return "critical";
            case spdlog::level::off: return "off";
            default: return "unknown";
        }
    }

    bool ShouldLog(spdlog::level::level_enum level) {
        auto logger = Logger::Get();
        return logger && logger->should_log(level);
    }

    bool ShouldLogTrace() {
        return ShouldLog(spdlog::level::trace);
    }

    bool ShouldLogDebug() {
        return ShouldLog(spdlog::level::debug);
    }

    bool ShouldLogSample(uint32_t count) {
        return count <= 64 || (count % 1024) == 0;
    }

#define TRANSIT_LOG_TRACE(...) \
    do { \
        if (ShouldLogTrace()) { \
            LOG_TRACE(__VA_ARGS__); \
        } \
    } while (false)

#define TRANSIT_LOG_DEBUG(...) \
    do { \
        if (ShouldLogDebug()) { \
            LOG_DEBUG(__VA_ARGS__); \
        } \
    } while (false)

    cISC4LotManager* GetLotManager() {
        return *reinterpret_cast<cISC4LotManager**>(kSpLotManagerAddress);
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

    const std::array<uint8_t, kPatchByteCount>* GetExpectedHookPrologue(uintptr_t address) {
        static constexpr std::array<uint8_t, kPatchByteCount> kExpectedRoadAccessPrologue{
            0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8
        };
        static constexpr std::array<uint8_t, kPatchByteCount> kExpectedCreateStartNodesPrologue{
            0x83, 0xEC, 0x5C, 0x53, 0x55, 0x56
        };
        static constexpr std::array<uint8_t, kPatchByteCount> kExpectedSetupPathFinderForLotPrologue{
            0x83, 0xEC, 0x14, 0x53, 0x55, 0x56
        };

        if (address == kCalculateRoadAccessAddress) {
            return &kExpectedRoadAccessPrologue;
        }
        if (address == kCreateStartNodesAddress) {
            return &kExpectedCreateStartNodesPrologue;
        }
        if (address == kSetupPathFinderForLotAddress) {
            return &kExpectedSetupPathFinderForLotPrologue;
        }
        return nullptr;
    }

    bool InstallInlineHook(InlineHook& hook) {
        if (hook.installed) {
            TRANSIT_LOG_DEBUG("TransitAccess: {} hook already installed at 0x{:08X}",
                      hook.name,
                      static_cast<uint32_t>(hook.patchAddress));
            return true;
        }

        hook.patchAddress = ResolvePatchAddress(hook.address);
        auto* target = reinterpret_cast<uint8_t*>(hook.patchAddress);
        TRANSIT_LOG_DEBUG("TransitAccess: resolved {} from 0x{:08X} to 0x{:08X}",
                  hook.name,
                  static_cast<uint32_t>(hook.address),
                  static_cast<uint32_t>(hook.patchAddress));

        const auto* expectedPrologue = GetExpectedHookPrologue(hook.address);
        if (expectedPrologue &&
            std::memcmp(target, expectedPrologue->data(), expectedPrologue->size()) != 0) {
            LOG_ERROR("TransitAccess: unexpected {} prologue at 0x{:08X}",
                      hook.name,
                      static_cast<uint32_t>(hook.patchAddress));
            return false;
        }

        std::memcpy(hook.original.data(), target, hook.original.size());
        TRANSIT_LOG_TRACE("TransitAccess: original {} prologue {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
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
        TRANSIT_LOG_DEBUG("TransitAccess: installed {} trampoline at 0x{:08X}",
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
            TRANSIT_LOG_DEBUG("TransitAccess: restored {} at 0x{:08X}",
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
            TRANSIT_LOG_DEBUG("TransitAccess: {} immediate already patched at 0x{:08X}: 0x{:08X}",
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

    bool RectsEqual(const SC4Rect<int32_t>& a, const SC4Rect<int32_t>& b) {
        return a.topLeftX == b.topLeftX &&
               a.topLeftY == b.topLeftY &&
               a.bottomRightX == b.bottomRightX &&
               a.bottomRightY == b.bottomRightY;
    }

    bool IsValidRect(const SC4Rect<int32_t>& rect) {
        return rect.topLeftX <= rect.bottomRightX && rect.topLeftY <= rect.bottomRightY;
    }

    struct ScopedBoolFlag {
        explicit ScopedBoolFlag(bool& flag)
            : flag(flag) {
            flag = true;
        }

        ScopedBoolFlag(const ScopedBoolFlag&) = delete;
        ScopedBoolFlag& operator=(const ScopedBoolFlag&) = delete;

        ~ScopedBoolFlag() {
            flag = false;
        }

        bool& flag;
    };

    void RecordSourceLotForPathFinder(void* pathFinder, cISC4Lot* lot) {
        if (!pathFinder || !lot) {
            return;
        }

        std::lock_guard<std::mutex> lock(gPathFinderSourceLotsMutex);
        gPathFinderSourceLots[pathFinder] = lot;
    }

    cISC4Lot* FindRecordedSourceLotForPathFinder(void* pathFinder) {
        if (!pathFinder) {
            return nullptr;
        }

        std::lock_guard<std::mutex> lock(gPathFinderSourceLotsMutex);
        const auto it = gPathFinderSourceLots.find(pathFinder);
        return it != gPathFinderSourceLots.end() ? it->second : nullptr;
    }

    void EraseRecordedSourceLotForPathFinder(void* pathFinder) {
        if (!pathFinder) {
            return;
        }

        std::lock_guard<std::mutex> lock(gPathFinderSourceLotsMutex);
        gPathFinderSourceLots.erase(pathFinder);
    }

    size_t GetRecordedSourceLotCount() {
        std::lock_guard<std::mutex> lock(gPathFinderSourceLotsMutex);
        return gPathFinderSourceLots.size();
    }

    void ClearRecordedSourceLots() {
        std::lock_guard<std::mutex> lock(gPathFinderSourceLotsMutex);
        gPathFinderSourceLots.clear();
    }

    struct ScopedPathFinderSourceLotRecordErase {
        explicit ScopedPathFinderSourceLotRecordErase(void* pathFinder)
            : pathFinder(pathFinder) {
        }

        ScopedPathFinderSourceLotRecordErase(const ScopedPathFinderSourceLotRecordErase&) = delete;
        ScopedPathFinderSourceLotRecordErase& operator=(const ScopedPathFinderSourceLotRecordErase&) = delete;

        ~ScopedPathFinderSourceLotRecordErase() {
            EraseRecordedSourceLotForPathFinder(pathFinder);
        }

        void* pathFinder;
    };

    std::string CopyGZString(const cIGZString* value) {
        if (!value || value->Strlen() == 0 || !value->ToChar()) {
            return {};
        }
        return value->ToChar();
    }

    std::string ToHex(uint64_t value, uint32_t width = 0) {
        char buffer[32]{};
        if (width == 0) {
            std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
        } else {
            std::snprintf(buffer, sizeof(buffer), "0x%0*llX", static_cast<int>(width), static_cast<unsigned long long>(value));
        }
        return buffer;
    }

    std::string GetLotDisplayName(cISC4Lot* lot) {
        if (!lot) {
            return {};
        }

        cISC4BuildingOccupant* building = lot->GetBuilding();
        if (building) {
            cRZBaseString occupantName;
            if (building->GetName(occupantName) && occupantName.Strlen() != 0) {
                return occupantName.ToChar();
            }

            std::string buildingName = CopyGZString(building->GetBuildingName());
            if (!buildingName.empty()) {
                return buildingName;
            }

            std::string exemplarName = CopyGZString(building->GetExemplarName());
            if (!exemplarName.empty()) {
                return exemplarName;
            }
        }

        cISC4LotConfiguration* configuration = lot->GetLotConfiguration();
        if (configuration) {
            cRZBaseString configurationName;
            if (configuration->GetName(configurationName) && configurationName.Strlen() != 0) {
                return configurationName.ToChar();
            }
        }

        return {};
    }

    std::string DescribeLot(cISC4Lot* lot) {
        if (!lot) {
            return "lot=null";
        }

        std::string result = "lot=" + ToHex(reinterpret_cast<uintptr_t>(lot), 8);
        if (!ShouldLog(spdlog::level::info)) {
            return result;
        }

        const std::string name = GetLotDisplayName(lot);
        if (!name.empty()) {
            result += " name=\"" + name + "\"";
        } else {
            result += " name=<unknown>";
        }

        int32_t locationX = 0;
        int32_t locationZ = 0;
        if (lot->GetLocation(locationX, locationZ)) {
            result += " loc=(" + std::to_string(locationX) + "," + std::to_string(locationZ) + ")";
        }

        uint8_t sizeX = 0;
        uint8_t sizeZ = 0;
        if (lot->GetSize(sizeX, sizeZ)) {
            result += " size=(" +
                      std::to_string(static_cast<uint32_t>(sizeX)) +
                      "," +
                      std::to_string(static_cast<uint32_t>(sizeZ)) +
                      ")";
        }

        SC4Rect<int32_t> bounds;
        if (lot->GetBoundingRect(bounds)) {
            result += " bounds=[" +
                      std::to_string(bounds.topLeftX) +
                      "," +
                      std::to_string(bounds.topLeftY) +
                      "," +
                      std::to_string(bounds.bottomRightX) +
                      "," +
                      std::to_string(bounds.bottomRightY) +
                      "]";
        }

        result += " facing=" + std::to_string(lot->GetFacing());
        result += " zone=" + std::to_string(static_cast<uint32_t>(lot->GetZoneType()));
        result += " flags=" + ToHex(lot->GetFlags(), 8);
        return result;
    }

    bool IsTransitEnabledLot(cISC4Lot* lot) {
        if (!lot) {
            return false;
        }

        cISCPropertyHolder* lotPropertyHolder = lot->AsPropertyHolder();
        if (lotPropertyHolder && lotPropertyHolder->HasProperty(kTransitSwitchPointProperty)) {
            TRANSIT_LOG_TRACE("TransitAccess: TE property present on lot property holder: {}",
                      DescribeLot(lot));
            return true;
        }

        cISC4BuildingOccupant* building = lot->GetBuilding();
        if (!building) {
            TRANSIT_LOG_TRACE("TransitAccess: lot has no building for TE property check: {}",
                      DescribeLot(lot));
            return false;
        }

        cISC4Occupant* buildingOccupant = building->AsOccupant();
        if (!buildingOccupant) {
            TRANSIT_LOG_TRACE("TransitAccess: lot building has no occupant for TE property check: {}",
                      DescribeLot(lot));
            return false;
        }

        cISCPropertyHolder* buildingPropertyHolder = buildingOccupant->AsPropertyHolder();
        const bool result = buildingPropertyHolder &&
                            buildingPropertyHolder->HasProperty(kTransitSwitchPointProperty);
        TRANSIT_LOG_TRACE("TransitAccess: TE property {} on building property holder: {}",
                  result ? "present" : "absent",
                  DescribeLot(lot));
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
            TRANSIT_LOG_TRACE("TransitAccess: road-like network hit at cell {},{} entry {}",
                      x,
                      z,
                      entry);
        }
        return result;
    }

    bool HasRoadLikeNetworkOnOrAroundLot(cISC4Lot* lot) {
        SC4Rect<int32_t> bounds;
        if (!lot || !lot->GetBoundingRect(bounds)) {
            TRANSIT_LOG_TRACE("TransitAccess: could not get TE lot bounds for {}", DescribeLot(lot));
            return false;
        }

        TRANSIT_LOG_TRACE("TransitAccess: checking TE lot network contact: {}", DescribeLot(lot));

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
            TRANSIT_LOG_DEBUG("TransitAccess: wrote road-access cache for {} = {}",
                      DescribeLot(lot),
                      value);
        } else {
            LOG_WARN("TransitAccess: road-access cache lookup returned null for {}",
                     DescribeLot(lot));
        }
    }

    void CollectAdjacentTransitEnabledRoadAccessLots(
        cISC4Lot* lot,
        std::vector<cISC4Lot*>& matches,
        std::unordered_set<cISC4Lot*>& seenMatches) {
        cISC4LotManager* lotManager = GetLotManager();
        if (!lot || !lotManager) {
            TRANSIT_LOG_TRACE("TransitAccess: missing lot or lot manager lot={} lotManager={}",
                      DescribeLot(lot),
                      reinterpret_cast<void*>(lotManager));
            return;
        }

        SC4Rect<int32_t> sourceBounds;
        if (!lot->GetBoundingRect(sourceBounds)) {
            TRANSIT_LOG_TRACE("TransitAccess: could not get source lot bounds for {}", DescribeLot(lot));
            return;
        }

        TRANSIT_LOG_TRACE("TransitAccess: scanning adjacent lots for source {}", DescribeLot(lot));

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
        auto addCandidateAt = [&](int32_t x, int32_t z) {
            if (x < 0 || z < 0) {
                return;
            }
            addCandidate(lotManager->GetLot(x, z, true));
        };

        for (int32_t x = sourceBounds.topLeftX; x <= sourceBounds.bottomRightX; ++x) {
            addCandidateAt(x, sourceBounds.topLeftY - 1);
            addCandidateAt(x, sourceBounds.bottomRightY + 1);
        }
        for (int32_t z = sourceBounds.topLeftY; z <= sourceBounds.bottomRightY; ++z) {
            addCandidateAt(sourceBounds.topLeftX - 1, z);
            addCandidateAt(sourceBounds.bottomRightX + 1, z);
        }

        TRANSIT_LOG_TRACE("TransitAccess: found {} unique adjacent lot candidates for source {}",
                  candidates.size(),
                  DescribeLot(lot));

        for (cISC4Lot* candidate : candidates) {
            SC4Rect<int32_t> candidateBounds;
            if (!candidate->GetBoundingRect(candidateBounds) ||
                !RectsTouchBySide(sourceBounds, candidateBounds)) {
                TRANSIT_LOG_TRACE("TransitAccess: skipping non-touching candidate {}", DescribeLot(candidate));
                continue;
            }

            TRANSIT_LOG_TRACE("TransitAccess: checking adjacent candidate {}", DescribeLot(candidate));

            if (IsTransitEnabledLot(candidate) && HasRoadLikeNetworkOnOrAroundLot(candidate)) {
                TRANSIT_LOG_DEBUG("TransitAccess: source lot gets road access through TE lot source=[{}] te=[{}]",
                          DescribeLot(lot),
                          DescribeLot(candidate));
                if (seenMatches.insert(candidate).second) {
                    matches.push_back(candidate);
                }
            }
        }
    }

    bool HasAdjacentTransitEnabledRoadAccess(cISC4Lot* lot) {
        std::vector<cISC4Lot*> matches;
        std::unordered_set<cISC4Lot*> seenMatches;
        CollectAdjacentTransitEnabledRoadAccessLots(lot, matches, seenMatches);
        return !matches.empty();
    }

    int32_t ReadPathFinderInt32(void* pathFinder, ptrdiff_t offset) {
        return *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(pathFinder) + offset);
    }

    void WritePathFinderInt32(void* pathFinder, ptrdiff_t offset, int32_t value) {
        *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(pathFinder) + offset) = value;
    }

    SC4Rect<int32_t> GetPathFinderSourceRect(void* pathFinder) {
        SC4Rect<int32_t> rect{};
        rect.topLeftX = ReadPathFinderInt32(pathFinder, kPathFinderSourceMinXOffset);
        rect.topLeftY = ReadPathFinderInt32(pathFinder, kPathFinderSourceMinZOffset);
        rect.bottomRightX = ReadPathFinderInt32(pathFinder, kPathFinderSourceMaxXOffset);
        rect.bottomRightY = ReadPathFinderInt32(pathFinder, kPathFinderSourceMaxZOffset);
        return rect;
    }

    void SetPathFinderSourceRect(void* pathFinder, const SC4Rect<int32_t>& rect) {
        WritePathFinderInt32(pathFinder, kPathFinderSourceMinXOffset, rect.topLeftX);
        WritePathFinderInt32(pathFinder, kPathFinderSourceMinZOffset, rect.topLeftY);
        WritePathFinderInt32(pathFinder, kPathFinderSourceMaxXOffset, rect.bottomRightX);
        WritePathFinderInt32(pathFinder, kPathFinderSourceMaxZOffset, rect.bottomRightY);
    }

    cISC4Lot* FindSourceLotForPathFinder(void* pathFinder) {
        cISC4LotManager* lotManager = GetLotManager();
        if (!pathFinder || !lotManager) {
            TRANSIT_LOG_TRACE("TransitAccess: missing pathFinder or lot manager pathFinder={} lotManager={}",
                      pathFinder,
                      reinterpret_cast<void*>(lotManager));
            return nullptr;
        }

        const SC4Rect<int32_t> sourceBounds = GetPathFinderSourceRect(pathFinder);
        if (!IsValidRect(sourceBounds)) {
            TRANSIT_LOG_DEBUG("TransitAccess: invalid pathfinder source rect [{},{},{},{}]",
                      sourceBounds.topLeftX,
                      sourceBounds.topLeftY,
                      sourceBounds.bottomRightX,
                      sourceBounds.bottomRightY);
            return nullptr;
        }

        const int32_t cityWidth = ReadPathFinderInt32(pathFinder, kPathFinderCityWidthOffset);
        const int32_t cityHeight = ReadPathFinderInt32(pathFinder, kPathFinderCityHeightOffset);
        if (cityWidth <= 0 || cityHeight <= 0) {
            TRANSIT_LOG_DEBUG("TransitAccess: invalid pathfinder city size {}x{} for source rect [{},{},{},{}]",
                      cityWidth,
                      cityHeight,
                      sourceBounds.topLeftX,
                      sourceBounds.topLeftY,
                      sourceBounds.bottomRightX,
                      sourceBounds.bottomRightY);
            return nullptr;
        }

        const int32_t minX = sourceBounds.topLeftX < 0 ? 0 : sourceBounds.topLeftX;
        const int32_t minZ = sourceBounds.topLeftY < 0 ? 0 : sourceBounds.topLeftY;
        const int32_t maxX = sourceBounds.bottomRightX >= cityWidth ? cityWidth - 1 : sourceBounds.bottomRightX;
        const int32_t maxZ = sourceBounds.bottomRightY >= cityHeight ? cityHeight - 1 : sourceBounds.bottomRightY;
        if (minX > maxX || minZ > maxZ) {
            TRANSIT_LOG_TRACE("TransitAccess: pathfinder source rect [{},{},{},{}] is outside city bounds {}x{}",
                      sourceBounds.topLeftX,
                      sourceBounds.topLeftY,
                      sourceBounds.bottomRightX,
                      sourceBounds.bottomRightY,
                      cityWidth,
                      cityHeight);
            return nullptr;
        }

        cISC4Lot* fallback = nullptr;
        std::unordered_set<cISC4Lot*> seenLots;
        seenLots.reserve(static_cast<size_t>((maxX - minX + 1) * (maxZ - minZ + 1)));

        for (int32_t z = minZ; z <= maxZ; ++z) {
            for (int32_t x = minX; x <= maxX; ++x) {
                cISC4Lot* candidate = lotManager->GetLot(x, z, true);
                if (!candidate || !seenLots.insert(candidate).second) {
                    continue;
                }

                SC4Rect<int32_t> candidateBounds;
                if (candidate->GetBoundingRect(candidateBounds) && RectsEqual(candidateBounds, sourceBounds)) {
                    TRANSIT_LOG_TRACE("TransitAccess: matched pathfinder source rect to exact source lot {}",
                              DescribeLot(candidate));
                    return candidate;
                }

                if (!fallback) {
                    fallback = candidate;
                }
            }
        }

        if (fallback) {
            TRANSIT_LOG_DEBUG("TransitAccess: using fallback source lot {} for pathfinder source rect [{},{},{},{}]",
                      DescribeLot(fallback),
                      sourceBounds.topLeftX,
                      sourceBounds.topLeftY,
                      sourceBounds.bottomRightX,
                      sourceBounds.bottomRightY);
        }

        return fallback;
    }

    bool RetryCreateStartNodesThroughTransitEnabledLot(void* pathFinder, cISC4Lot* sourceLot) {
        std::vector<cISC4Lot*> candidates;
        std::unordered_set<cISC4Lot*> seenCandidates;
        CollectAdjacentTransitEnabledRoadAccessLots(sourceLot, candidates, seenCandidates);
        if (candidates.empty()) {
            TRANSIT_LOG_TRACE("TransitAccess: no adjacent TE start-node retry candidates for source lot {}",
                      DescribeLot(sourceLot));
            return false;
        }

        const SC4Rect<int32_t> originalRect = GetPathFinderSourceRect(pathFinder);
        TRANSIT_LOG_DEBUG("TransitAccess: retrying CreateStartNodes for source lot {} through {} TE candidates",
                  DescribeLot(sourceLot),
                  candidates.size());

        for (cISC4Lot* candidate : candidates) {
            SC4Rect<int32_t> candidateBounds;
            if (!candidate || !candidate->GetBoundingRect(candidateBounds) || !IsValidRect(candidateBounds)) {
                TRANSIT_LOG_TRACE("TransitAccess: skipping retry candidate {} with invalid bounds",
                          DescribeLot(candidate));
                continue;
            }

            TRANSIT_LOG_TRACE("TransitAccess: retrying CreateStartNodes source lot [{}] via TE lot [{}]",
                      DescribeLot(sourceLot),
                      DescribeLot(candidate));

            SetPathFinderSourceRect(pathFinder, candidateBounds);
            const bool retrySucceeded = gOriginalCreateStartNodes && gOriginalCreateStartNodes(pathFinder);
            SetPathFinderSourceRect(pathFinder, originalRect);

            if (retrySucceeded) {
                const uint32_t retrySuccessCount = ++gStartNodesRetrySuccessCount;
                if (ShouldLog(spdlog::level::info) && ShouldLogSample(retrySuccessCount)) {
                    LOG_INFO("TransitAccess: CreateStartNodes TE retry success sourceLot=[{}] teLot=[{}] originalRect=[{},{},{},{}] retryRect=[{},{},{},{}] totalSuccess={}",
                             DescribeLot(sourceLot),
                             DescribeLot(candidate),
                             originalRect.topLeftX,
                             originalRect.topLeftY,
                             originalRect.bottomRightX,
                             originalRect.bottomRightY,
                             candidateBounds.topLeftX,
                             candidateBounds.topLeftY,
                             candidateBounds.bottomRightX,
                             candidateBounds.bottomRightY,
                             retrySuccessCount);
                }
                return true;
            }
        }

        SetPathFinderSourceRect(pathFinder, originalRect);
        TRANSIT_LOG_TRACE("TransitAccess: CreateStartNodes TE retries failed for source lot {}",
                  DescribeLot(sourceLot));
        return false;
    }

    bool __fastcall HookCalculateRoadAccess(cISC4Lot* lot, void*) {
        const uint32_t hookCallCount = ++gHookCallCount;
        if (!lot) {
            LOG_WARN("TransitAccess: CalculateRoadAccess hook received null lot");
            return false;
        }

        if (ShouldLogSample(hookCallCount)) {
            TRANSIT_LOG_TRACE("TransitAccess: CalculateRoadAccess hook call {} {}",
                      hookCallCount,
                      DescribeLot(lot));
        }

        if (gOriginalCalculateRoadAccess && gOriginalCalculateRoadAccess(lot)) {
            if (ShouldLogSample(hookCallCount)) {
                TRANSIT_LOG_TRACE("TransitAccess: original road access succeeded for {}", DescribeLot(lot));
            }
            return true;
        }

        const uint32_t originalFailureCount = ++gOriginalFailureCount;
        if (ShouldLogSample(originalFailureCount)) {
            TRANSIT_LOG_DEBUG("TransitAccess: original road access failed for {} failureCount={}",
                      DescribeLot(lot),
                      originalFailureCount);
        }

        if (HasAdjacentTransitEnabledRoadAccess(lot)) {
            SetRoadAccessCache(lot, true);
            const uint32_t exceptionSuccessCount = ++gExceptionSuccessCount;
            if (ShouldLog(spdlog::level::info) && ShouldLogSample(exceptionSuccessCount)) {
                LOG_INFO("TransitAccess: TE road-access exception success for {} totalSuccess={}",
                         DescribeLot(lot),
                         exceptionSuccessCount);
            }
            return true;
        }

        if (ShouldLogSample(originalFailureCount)) {
            TRANSIT_LOG_TRACE("TransitAccess: no TE road-access exception for {}", DescribeLot(lot));
        }
        return false;
    }

    bool __fastcall HookCreateStartNodes(void* pathFinder, void*) {
        const uint32_t hookCallCount = ++gStartNodesHookCallCount;
        if (!pathFinder) {
            LOG_WARN("TransitAccess: CreateStartNodes hook received null pathFinder");
            return false;
        }
        ScopedPathFinderSourceLotRecordErase sourceLotRecordErase(pathFinder);

        if (ShouldLogSample(hookCallCount)) {
            void* trafficSimulator =
                *reinterpret_cast<void**>(static_cast<uint8_t*>(pathFinder) + kPathFinderTrafficSimulatorOffset);
            const SC4Rect<int32_t> sourceRect = GetPathFinderSourceRect(pathFinder);
            TRANSIT_LOG_TRACE("TransitAccess: CreateStartNodes hook call {} pathFinder={} trafficSimulator={} sourceRect=[{},{},{},{}]",
                      hookCallCount,
                      pathFinder,
                      trafficSimulator,
                      sourceRect.topLeftX,
                      sourceRect.topLeftY,
                      sourceRect.bottomRightX,
                      sourceRect.bottomRightY);
        }

        if (gOriginalCreateStartNodes && gOriginalCreateStartNodes(pathFinder)) {
            if (ShouldLogSample(hookCallCount)) {
                TRANSIT_LOG_TRACE("TransitAccess: original CreateStartNodes succeeded for pathFinder={}", pathFinder);
            }
            return true;
        }

        const uint32_t originalFailureCount = ++gStartNodesOriginalFailureCount;
        if (ShouldLogSample(originalFailureCount)) {
            TRANSIT_LOG_DEBUG("TransitAccess: original CreateStartNodes failed for pathFinder={} failureCount={}",
                      pathFinder,
                      originalFailureCount);
        }

        if (gInsideCreateStartNodesRetry) {
            return false;
        }

        cISC4Lot* sourceLot = FindRecordedSourceLotForPathFinder(pathFinder);
        if (sourceLot) {
            const uint32_t sideTableHitCount = ++gStartNodesSourceLotSideTableHitCount;
            if (ShouldLogSample(sideTableHitCount)) {
                TRANSIT_LOG_DEBUG("TransitAccess: matched CreateStartNodes pathFinder={} to recorded source lot {} hitCount={}",
                          pathFinder,
                          DescribeLot(sourceLot),
                          sideTableHitCount);
            }
        } else {
            const uint32_t fallbackCount = ++gStartNodesSourceLotFallbackCount;
            if (ShouldLogSample(fallbackCount)) {
                TRANSIT_LOG_DEBUG("TransitAccess: no recorded source lot for pathFinder={}, falling back to source-rect lookup fallbackCount={}",
                          pathFinder,
                          fallbackCount);
            }
            sourceLot = FindSourceLotForPathFinder(pathFinder);
        }

        if (!sourceLot) {
            const uint32_t missingSourceLotCount = ++gStartNodesMissingSourceLotCount;
            if (ShouldLogSample(missingSourceLotCount)) {
                const SC4Rect<int32_t> sourceRect = GetPathFinderSourceRect(pathFinder);
                TRANSIT_LOG_DEBUG("TransitAccess: no source lot found for CreateStartNodes sourceRect=[{},{},{},{}] missingCount={}",
                          sourceRect.topLeftX,
                          sourceRect.topLeftY,
                          sourceRect.bottomRightX,
                          sourceRect.bottomRightY,
                          missingSourceLotCount);
            }
            return false;
        }

        ScopedBoolFlag retryGuard(gInsideCreateStartNodesRetry);
        const bool retrySucceeded = RetryCreateStartNodesThroughTransitEnabledLot(pathFinder, sourceLot);
        return retrySucceeded;
    }

    bool __fastcall HookSetupPathFinderForLot(
        void* trafficSimulator,
        void*,
        void* pathFinder,
        cISC4Lot* lot) {

        const uint32_t hookCallCount = ++gSetupPathFinderHookCallCount;
        if (ShouldLogSample(hookCallCount)) {
            TRANSIT_LOG_TRACE("TransitAccess: SetupPathFinderForLot hook call {} trafficSimulator={} pathFinder={} sourceLot=[{}]",
                      hookCallCount,
                      trafficSimulator,
                      pathFinder,
                      DescribeLot(lot));
        }

        bool setupSucceeded = false;
        if (gOriginalSetupPathFinderForLot) {
            setupSucceeded = gOriginalSetupPathFinderForLot(trafficSimulator, pathFinder, lot);
        } else {
            LOG_WARN("TransitAccess: SetupPathFinderForLot hook has no original trampoline");
        }

        if (setupSucceeded && pathFinder && lot) {
            RecordSourceLotForPathFinder(pathFinder, lot);
            const uint32_t successCount = ++gSetupPathFinderSuccessCount;
            if (ShouldLogSample(successCount)) {
                TRANSIT_LOG_DEBUG("TransitAccess: recorded pathFinder={} source lot [{}] setupSuccessCount={}",
                          pathFinder,
                          DescribeLot(lot),
                          successCount);
            }
        } else {
            EraseRecordedSourceLotForPathFinder(pathFinder);
            if (ShouldLogSample(hookCallCount)) {
                TRANSIT_LOG_DEBUG("TransitAccess: SetupPathFinderForLot failed or incomplete pathFinder={} sourceLot=[{}]",
                          pathFinder,
                          DescribeLot(lot));
            }
        }

        return setupSucceeded;
    }

    InlineHook gCalculateRoadAccessHook{
        "cSC4Lot::CalculateRoadAccess",
        kCalculateRoadAccessAddress,
        reinterpret_cast<void*>(&HookCalculateRoadAccess)
    };

    InlineHook gCreateStartNodesHook{
        "cSC4PathFinder::CreateStartNodes",
        kCreateStartNodesAddress,
        reinterpret_cast<void*>(&HookCreateStartNodes)
    };

    InlineHook gSetupPathFinderForLotHook{
        "cSC4TrafficSimulator::SetupPathFinderForLot",
        kSetupPathFinderForLotAddress,
        reinterpret_cast<void*>(&HookSetupPathFinderForLot)
    };
}

class TransitAccessSampleDirector final : public cRZCOMDllDirector {
public:
    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kTransitAccessDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZCOMDllDirector::OnStart(pCOM);

        Logger::Initialize("SC4TransitAccessSample", "");
        Logger::Get()->set_level(kTransitAccessSampleLogLevel);
        Logger::Get()->flush_on(kTransitAccessSampleFlushLevel);
        LOG_INFO("TransitAccess: OnStart logLevel={} flushLevel={}",
                 LevelName(kTransitAccessSampleLogLevel),
                 LevelName(kTransitAccessSampleFlushLevel));

        const uint16_t gameVersion = VersionDetection::GetInstance().GetGameVersion();
        TRANSIT_LOG_DEBUG("TransitAccess: detected game version {}", gameVersion);
        if (gameVersion != 641) {
            LOG_WARN("TransitAccess: unsupported game version {}, expected 641", gameVersion);
            return true;
        }

        TRANSIT_LOG_DEBUG("TransitAccess: globals lotManager=0x{:08X} trafficNetworkMap=0x{:08X}",
                  reinterpret_cast<uint32_t>(GetLotManager()),
                  reinterpret_cast<uint32_t>(GetTrafficNetworkMap()));
        TRANSIT_LOG_DEBUG("TransitAccess: road-like network mask stock=0x{:08X} dirtRoad=0x{:08X} effective=0x{:08X}",
                  kStockRoadLikeNetworkMask,
                  kDirtRoadNetworkMask,
                  GetEffectiveRoadLikeNetworkMask());
        if (kPatchStockRoadAccessForDirtRoad) {
            gDirtRoadMaskPatchInstalled = PatchDirtRoadMasks(true);
        } else {
            TRANSIT_LOG_DEBUG("TransitAccess: DirtRoad mask patches disabled");
        }

        if (InstallInlineHook(gCalculateRoadAccessHook)) {
            gOriginalCalculateRoadAccess =
                reinterpret_cast<CalculateRoadAccessFn>(gCalculateRoadAccessHook.trampoline);
            LOG_INFO("TransitAccess: installed road-access hook at 0x{:08X}",
                     static_cast<uint32_t>(gCalculateRoadAccessHook.patchAddress));
        }

        if (InstallInlineHook(gCreateStartNodesHook)) {
            gOriginalCreateStartNodes =
                reinterpret_cast<CreateStartNodesFn>(gCreateStartNodesHook.trampoline);
            LOG_INFO("TransitAccess: installed pathfinder start-node hook at 0x{:08X}",
                     static_cast<uint32_t>(gCreateStartNodesHook.patchAddress));

            if (InstallInlineHook(gSetupPathFinderForLotHook)) {
                gOriginalSetupPathFinderForLot =
                    reinterpret_cast<SetupPathFinderForLotFn>(gSetupPathFinderForLotHook.trampoline);
                LOG_INFO("TransitAccess: installed pathfinder source-lot setup hook at 0x{:08X}",
                         static_cast<uint32_t>(gSetupPathFinderForLotHook.patchAddress));
            }
        } else {
            LOG_WARN("TransitAccess: start-node hook was not installed; source-lot setup hook disabled");
        }

        return true;
    }

    bool PostAppShutdown() override {
        LOG_INFO("TransitAccess: PostAppShutdown roadAccessCalls={} roadAccessOriginalFailures={} roadAccessExceptionSuccesses={} setupPathFinderCalls={} setupPathFinderSuccesses={} startNodeCalls={} startNodeOriginalFailures={} startNodeSideTableHits={} startNodeFallbacks={} startNodeMissingSourceLots={} startNodeRetrySuccesses={} outstandingSourceLotRecords={}",
                 gHookCallCount.load(),
                 gOriginalFailureCount.load(),
                 gExceptionSuccessCount.load(),
                 gSetupPathFinderHookCallCount.load(),
                 gSetupPathFinderSuccessCount.load(),
                 gStartNodesHookCallCount.load(),
                 gStartNodesOriginalFailureCount.load(),
                 gStartNodesSourceLotSideTableHitCount.load(),
                 gStartNodesSourceLotFallbackCount.load(),
                 gStartNodesMissingSourceLotCount.load(),
                 gStartNodesRetrySuccessCount.load(),
                 GetRecordedSourceLotCount());
        if (gDirtRoadMaskPatchInstalled) {
            PatchDirtRoadMasks(false);
            gDirtRoadMaskPatchInstalled = false;
        }
        UninstallInlineHook(gSetupPathFinderForLotHook);
        gOriginalSetupPathFinderForLot = nullptr;
        UninstallInlineHook(gCreateStartNodesHook);
        gOriginalCreateStartNodes = nullptr;
        UninstallInlineHook(gCalculateRoadAccessHook);
        gOriginalCalculateRoadAccess = nullptr;
        ClearRecordedSourceLots();
        return true;
    }
};

static TransitAccessSampleDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static bool sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
