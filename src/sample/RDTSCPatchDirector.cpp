#include "cIGZFrameWork.h"
#include "cRZCOMDllDirector.h"
#include "utils/Logger.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {
    constexpr uint32_t kRDTSCPatchDirectorId = 0xA17E1001;
    constexpr std::string_view kLoggerName = "SC4RDTSCPatch";

    constexpr unsigned char kRdtscOpcode0 = 0x0F;
    constexpr unsigned char kRdtscOpcode1 = 0x31;
    constexpr unsigned char kUd2Opcode0 = 0x0F;
    constexpr unsigned char kUd2Opcode1 = 0x0B;
    constexpr unsigned char kXorEaxOpcode0 = 0x33;
    constexpr unsigned char kXorEaxOpcode1 = 0xC0;
    constexpr unsigned char kJnzShortOpcode = 0x75;
    constexpr unsigned char kJmpShortOpcode = 0xEB;

    constexpr uintptr_t kExpectedModuleBase = 0x00400000;

    struct PatchSpec {
        uintptr_t rva;
        unsigned char expected0;
        unsigned char expected1;
        unsigned char replacement0;
        unsigned char replacement1;
        bool trapWithVeh;
    };

    constexpr PatchSpec kPatchSpecs[] = {
        // CRT memset / zero benchmarking blocks. These only use EAX deltas.
        {0x006584D6, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
        {0x006584DD, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
        {0x006584E3, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
        {0x006585C1, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
        {0x006585C8, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
        {0x006585CE, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
        {0x006587A6, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
        {0x006587AD, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
        {0x006587B3, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
        {0x0065886E, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
        {0x00658875, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
        {0x0065887B, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},

        // Timing wrappers around 0x008905xx already have a non-TSC fallback path.
        {0x00490567, kJnzShortOpcode, 0x16, kJmpShortOpcode, 0x16, false},
        {0x00490599, kJnzShortOpcode, 0x17, kJmpShortOpcode, 0x17, false},
        {0x004905C8, kJnzShortOpcode, 0x1A, kJmpShortOpcode, 0x1A, false},

        // Remaining real timestamp reads still use VEH-based synthetic TSC.
        {0x00493153, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
        {0x0049310F, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
        {0x004904E7, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
        {0x004903A6, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
        {0x004902A9, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
        {0x004901C7, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
        {0x00490143, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
        {0x00490137, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
        {0x0048FE80, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
        {0x0048FE45, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
        {0x0048FDEB, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
        {0x00320ADB, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
        {0x00320A76, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
        {0x0032081B, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
        {0x0030E2B4, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
        {0x0028BF81, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
        {0x0028BF55, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
        {0x001D9394, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
    };
}

class RDTSCPatchDirector final : public cRZCOMDllDirector {
public:
    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kRDTSCPatchDirectorId;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZCOMDllDirector::OnStart(pCOM);

        Logger::Initialize(std::string(kLoggerName), "", false);
        LOG_INFO("RDTSCPatchDirector: OnStart");

        if (mpFrameWork) {
            mpFrameWork->AddHook(this);
        }

        return true;
    }

    bool PostAppInit() override {
        if (installed_) {
            return true;
        }

        LOG_INFO("RDTSCPatchDirector: PostAppInit");

        if (!InstallExceptionHandler_()) {
            return true;
        }

        if (!PatchMainModule_()) {
            UninstallExceptionHandler_();
            return true;
        }

        installed_ = true;
        LOG_INFO("RDTSCPatchDirector: installed {} patch(es), {} use VEH",
                 patchedSites_.size(),
                 vehSites_.size());
        return true;
    }

    bool PostAppShutdown() override {
        LOG_INFO("RDTSCPatchDirector: PostAppShutdown");

        RestorePatchedSites_();
        UninstallExceptionHandler_();

        if (mpFrameWork) {
            mpFrameWork->RemoveHook(this);
        }

        Logger::Shutdown();
        return true;
    }

private:
    struct PatchedSite {
        uintptr_t address;
        unsigned char original0;
        unsigned char original1;
    };

    static LONG CALLBACK VectoredHandler_(PEXCEPTION_POINTERS exceptionInfo) {
        if (!sInstance_) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        if (!exceptionInfo || !exceptionInfo->ExceptionRecord || !exceptionInfo->ContextRecord) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        if (exceptionInfo->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        const uintptr_t instructionPointer = static_cast<uintptr_t>(exceptionInfo->ContextRecord->Eip);
        if (!sInstance_->IsVehSite_(instructionPointer)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        const uint64_t fakeTsc = sInstance_->ComputeSyntheticTsc_();
        exceptionInfo->ContextRecord->Eax = static_cast<DWORD>(fakeTsc);
        exceptionInfo->ContextRecord->Edx = static_cast<DWORD>(fakeTsc >> 32);
        exceptionInfo->ContextRecord->Eip += 2;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    bool InstallExceptionHandler_() {
        if (vehHandle_) {
            return true;
        }

        sInstance_ = this;
        vehHandle_ = AddVectoredExceptionHandler(1, &RDTSCPatchDirector::VectoredHandler_);
        if (!vehHandle_) {
            LOG_ERROR("RDTSCPatchDirector: AddVectoredExceptionHandler failed ({})", GetLastError());
            sInstance_ = nullptr;
            return false;
        }

        LARGE_INTEGER qpcFrequency{};
        LARGE_INTEGER qpcStart{};
        if (!QueryPerformanceFrequency(&qpcFrequency) || !QueryPerformanceCounter(&qpcStart) ||
            qpcFrequency.QuadPart <= 0) {
            LOG_ERROR("RDTSCPatchDirector: QueryPerformanceCounter initialization failed");
            RemoveVectoredExceptionHandler(vehHandle_);
            vehHandle_ = nullptr;
            sInstance_ = nullptr;
            return false;
        }

        qpcFrequency_ = static_cast<uint64_t>(qpcFrequency.QuadPart);
        qpcStart_ = static_cast<uint64_t>(qpcStart.QuadPart);
        syntheticTscBase_ = fakeTsc_.load(std::memory_order_relaxed);

        LOG_INFO("RDTSCPatchDirector: vectored exception handler installed");
        return true;
    }

    void UninstallExceptionHandler_() {
        if (vehHandle_) {
            RemoveVectoredExceptionHandler(vehHandle_);
            vehHandle_ = nullptr;
        }
        sInstance_ = nullptr;
    }

    bool PatchMainModule_() {
        HMODULE mainModule = GetModuleHandleW(nullptr);
        if (!mainModule) {
            LOG_ERROR("RDTSCPatchDirector: GetModuleHandleW(nullptr) failed ({})", GetLastError());
            return false;
        }

        const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(mainModule);
        if (moduleBase != kExpectedModuleBase) {
            LOG_WARN("RDTSCPatchDirector: unexpected module base {:08X}", moduleBase);
        }

        for (const PatchSpec& spec : kPatchSpecs) {
            ApplyPatch_(moduleBase + spec.rva, spec);
        }

        std::sort(
            patchedSites_.begin(),
            patchedSites_.end(),
            [](const PatchedSite& left, const PatchedSite& right) {
                return left.address < right.address;
            });

        std::sort(vehSites_.begin(), vehSites_.end());

        if (patchedSites_.empty()) {
            LOG_WARN("RDTSCPatchDirector: no patch sites were applied");
        }

        return !patchedSites_.empty();
    }

    void ApplyPatch_(const uintptr_t address, const PatchSpec& spec) {
        auto* bytes = reinterpret_cast<unsigned char*>(address);
        if (bytes[0] != spec.expected0 || bytes[1] != spec.expected1) {
            LOG_WARN("RDTSCPatchDirector: expected bytes {:02X} {:02X} missing at {:08X}",
                     spec.expected0,
                     spec.expected1,
                     address);
            return;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(bytes, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LOG_WARN("RDTSCPatchDirector: VirtualProtect failed at {:08X} ({})", address, GetLastError());
            return;
        }

        patchedSites_.push_back(PatchedSite{address, bytes[0], bytes[1]});

        if (spec.trapWithVeh) {
            vehSites_.push_back(address);
        }

        bytes[0] = spec.replacement0;
        bytes[1] = spec.replacement1;

        DWORD restoredProtect = 0;
        VirtualProtect(bytes, 2, oldProtect, &restoredProtect);
        FlushInstructionCache(GetCurrentProcess(), bytes, 2);
    }

    void RestorePatchedSites_() {
        if (patchedSites_.empty()) {
            installed_ = false;
            return;
        }

        for (const PatchedSite& site : patchedSites_) {
            auto* address = reinterpret_cast<unsigned char*>(site.address);

            DWORD oldProtect = 0;
            if (!VirtualProtect(address, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                continue;
            }

            address[0] = site.original0;
            address[1] = site.original1;

            DWORD restoredProtect = 0;
            VirtualProtect(address, 2, oldProtect, &restoredProtect);
            FlushInstructionCache(GetCurrentProcess(), address, 2);
        }

        patchedSites_.clear();
        vehSites_.clear();
        installed_ = false;
        LOG_INFO("RDTSCPatchDirector: restored patched sites");
    }

    [[nodiscard]] bool IsVehSite_(const uintptr_t address) const {
        const auto it = std::lower_bound(
            vehSites_.begin(),
            vehSites_.end(),
            address,
            [](const uintptr_t left, const uintptr_t right) {
                return left < right;
            });

        return it != vehSites_.end() && *it == address;
    }

    [[nodiscard]] uint64_t ComputeSyntheticTsc_() {
        if (qpcFrequency_ == 0) {
            return fakeTsc_.fetch_add(1, std::memory_order_relaxed);
        }

        LARGE_INTEGER qpcNow{};
        if (!QueryPerformanceCounter(&qpcNow)) {
            return fakeTsc_.fetch_add(1, std::memory_order_relaxed);
        }

        const uint64_t currentQpc = static_cast<uint64_t>(qpcNow.QuadPart);
        const uint64_t elapsedQpc = currentQpc - qpcStart_;

        constexpr uint64_t kSyntheticCpuHz = 5000000000ULL;
        const uint64_t syntheticTsc =
            syntheticTscBase_ + static_cast<uint64_t>((elapsedQpc * kSyntheticCpuHz) / qpcFrequency_);

        uint64_t observed = fakeTsc_.load(std::memory_order_relaxed);
        while (observed < syntheticTsc &&
               !fakeTsc_.compare_exchange_weak(observed, syntheticTsc, std::memory_order_relaxed)) {
        }

        if (observed >= syntheticTsc) {
            return observed;
        }

        return syntheticTsc;
    }

    inline static RDTSCPatchDirector* sInstance_ = nullptr;

    void* vehHandle_ = nullptr;
    std::vector<PatchedSite> patchedSites_;
    std::vector<uintptr_t> vehSites_;
    std::atomic<uint64_t> fakeTsc_{0};
    uint64_t qpcFrequency_ = 0;
    uint64_t qpcStart_ = 0;
    uint64_t syntheticTscBase_ = 0;
    bool installed_ = false;
};

static RDTSCPatchDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static bool sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
