#include "cIGZFrameWork.h"
#include "cRZCOMDllDirector.h"

#include "imgui.h"
#include "public/ImGuiPanelAdapter.h"
#include "public/ImGuiServiceIds.h"
#include "public/cIGZDrawService.h"
#include "public/cIGZImGuiService.h"
#include "utils/Logger.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <windows.h>

namespace {
    constexpr auto kDrawSampleDirectorID = 0xC49D82A7;
    constexpr uint32_t kDrawSamplePanelId = 0x8A32F41C;

    enum class DrawPass : uint8_t {
        Draw = 0,
        PreStatic,
        Static,
        PostStatic,
        PreDynamic,
        Dynamic,
        PostDynamic,
        Count
    };

    constexpr size_t kDrawPassCount = static_cast<size_t>(DrawPass::Count);
    constexpr size_t kHookByteCount = 5;
    constexpr size_t kEventRingCapacity = 2048;

    struct HookEvent {
        uint64_t seq;
        DrawPass pass;
        bool begin;
        uint32_t tickMs;
    };

    struct InlineHook {
        const char* name;
        uintptr_t address;
        uintptr_t patchAddress = 0;
        void* hookFn;
        uint8_t original[kHookByteCount]{};
        void* trampoline = nullptr;
        bool installed = false;
    };

    std::atomic<uint64_t> gEventSeq{0};
    std::array<HookEvent, kEventRingCapacity> gEventRing{};
    std::array<std::atomic<uint32_t>, kDrawPassCount> gBeginCounts{};
    std::array<std::atomic<uint32_t>, kDrawPassCount> gEndCounts{};

    uint32_t(__thiscall* gOrigDraw)(void*) = nullptr;
    void(__thiscall* gOrigPreStatic)(void*) = nullptr;
    void(__thiscall* gOrigStatic)(void*) = nullptr;
    void(__thiscall* gOrigPostStatic)(void*) = nullptr;
    void(__thiscall* gOrigPreDynamic)(void*) = nullptr;
    void(__thiscall* gOrigDynamic)(void*) = nullptr;
    void(__thiscall* gOrigPostDynamic)(void*) = nullptr;

    const char* PassName(const DrawPass pass) {
        switch (pass) {
        case DrawPass::Draw: return "Draw";
        case DrawPass::PreStatic: return "PreStatic";
        case DrawPass::Static: return "Static";
        case DrawPass::PostStatic: return "PostStatic";
        case DrawPass::PreDynamic: return "PreDynamic";
        case DrawPass::Dynamic: return "Dynamic";
        case DrawPass::PostDynamic: return "PostDynamic";
        default: return "Unknown";
        }
    }

    void RecordHookEvent(const DrawPass pass, const bool begin) {
        const uint64_t seq = gEventSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
        auto& slot = gEventRing[seq % kEventRingCapacity];
        slot = HookEvent{seq, pass, begin, GetTickCount()};

        const size_t passIndex = static_cast<size_t>(pass);
        if (begin) {
            gBeginCounts[passIndex].fetch_add(1, std::memory_order_relaxed);
        } else {
            gEndCounts[passIndex].fetch_add(1, std::memory_order_relaxed);
        }
    }

    uint32_t __fastcall HookDraw(void* self, void*) {
        RecordHookEvent(DrawPass::Draw, true);
        const uint32_t result = gOrigDraw ? gOrigDraw(self) : 0;
        RecordHookEvent(DrawPass::Draw, false);
        return result;
    }

    void __fastcall HookPreStatic(void* self, void*) {
        RecordHookEvent(DrawPass::PreStatic, true);
        if (gOrigPreStatic) {
            gOrigPreStatic(self);
        }
        RecordHookEvent(DrawPass::PreStatic, false);
    }

    void __fastcall HookStatic(void* self, void*) {
        RecordHookEvent(DrawPass::Static, true);
        if (gOrigStatic) {
            gOrigStatic(self);
        }
        RecordHookEvent(DrawPass::Static, false);
    }

    void __fastcall HookPostStatic(void* self, void*) {
        RecordHookEvent(DrawPass::PostStatic, true);
        if (gOrigPostStatic) {
            gOrigPostStatic(self);
        }
        RecordHookEvent(DrawPass::PostStatic, false);
    }

    void __fastcall HookPreDynamic(void* self, void*) {
        RecordHookEvent(DrawPass::PreDynamic, true);
        if (gOrigPreDynamic) {
            gOrigPreDynamic(self);
        }
        RecordHookEvent(DrawPass::PreDynamic, false);
    }

    void __fastcall HookDynamic(void* self, void*) {
        RecordHookEvent(DrawPass::Dynamic, true);
        if (gOrigDynamic) {
            gOrigDynamic(self);
        }
        RecordHookEvent(DrawPass::Dynamic, false);
    }

    void __fastcall HookPostDynamic(void* self, void*) {
        RecordHookEvent(DrawPass::PostDynamic, true);
        if (gOrigPostDynamic) {
            gOrigPostDynamic(self);
        }
        RecordHookEvent(DrawPass::PostDynamic, false);
    }

    std::array<InlineHook, kDrawPassCount> gHooks{{
        {"cSC43DRender::Draw", 0x007CB530, 0, reinterpret_cast<void*>(&HookDraw)},
        {"cSC43DRender::DrawPreStaticView_", 0x007C3E90, 0, reinterpret_cast<void*>(&HookPreStatic)},
        {"cSC43DRender::DrawStaticView_", 0x007C7370, 0, reinterpret_cast<void*>(&HookStatic)},
        {"cSC43DRender::DrawPostStaticView_", 0x007C3ED0, 0, reinterpret_cast<void*>(&HookPostStatic)},
        {"cSC43DRender::DrawPreDynamicView_", 0x007C3E10, 0, reinterpret_cast<void*>(&HookPreDynamic)},
        {"cSC43DRender::DrawDynamicView_", 0x007C7830, 0, reinterpret_cast<void*>(&HookDynamic)},
        {"cSC43DRender::DrawPostDynamicView_", 0x007C3E50, 0, reinterpret_cast<void*>(&HookPostDynamic)}
    }};

    uintptr_t ResolvePatchAddress(uintptr_t address) {
        // Resolve common x86 jump stubs (jmp rel32, jmp rel8, jmp [imm32]).
        // We follow a short chain so hooks patch real function bodies.
        uintptr_t current = address;
        for (int i = 0; i < 6; ++i) {
            const auto* p = reinterpret_cast<const uint8_t*>(current);
            const uint8_t op0 = p[0];

            if (op0 == 0xE9) {
                const auto rel = *reinterpret_cast<const int32_t*>(p + 1);
                current = current + 5 + rel;
                continue;
            }
            if (op0 == 0xEB) {
                const auto rel8 = static_cast<int8_t>(p[1]);
                current = current + 2 + rel8;
                continue;
            }
            if (op0 == 0xFF && p[1] == 0x25) {
                const auto mem = *reinterpret_cast<const uintptr_t*>(p + 2);
                current = *reinterpret_cast<const uintptr_t*>(mem);
                continue;
            }
            break;
        }
        return current;
    }

    bool InstallInlineHook(InlineHook& hook) {
        if (hook.installed) {
            return true;
        }

        hook.patchAddress = ResolvePatchAddress(hook.address);
        auto* target = reinterpret_cast<uint8_t*>(hook.patchAddress);
        if (!target) {
            LOG_ERROR("DrawServiceSample: resolved null patch target for {}", hook.name);
            return false;
        }
        std::memcpy(hook.original, target, kHookByteCount);

        auto* trampoline = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kHookByteCount + kHookByteCount, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        if (!trampoline) {
            LOG_ERROR("DrawServiceSample: failed to allocate trampoline for {}", hook.name);
            return false;
        }

        std::memcpy(trampoline, target, kHookByteCount);
        trampoline[kHookByteCount] = 0xE9;

        const auto trampolineJmpSrc = reinterpret_cast<intptr_t>(trampoline + kHookByteCount);
        const auto trampolineJmpDst = reinterpret_cast<intptr_t>(target + kHookByteCount);
        const auto trampolineRel = static_cast<int32_t>(trampolineJmpDst - (trampolineJmpSrc + kHookByteCount));
        std::memcpy(trampoline + kHookByteCount + 1, &trampolineRel, sizeof(trampolineRel));

        DWORD oldProtect = 0;
        if (!VirtualProtect(target, kHookByteCount, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LOG_ERROR("DrawServiceSample: VirtualProtect failed while installing {} at 0x{:08X}",
                      hook.name, static_cast<uint32_t>(hook.patchAddress));
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return false;
        }

        target[0] = 0xE9;
        const auto hookJmpSrc = reinterpret_cast<intptr_t>(target);
        const auto hookJmpDst = reinterpret_cast<intptr_t>(hook.hookFn);
        const auto hookRel = static_cast<int32_t>(hookJmpDst - (hookJmpSrc + kHookByteCount));
        std::memcpy(target + 1, &hookRel, sizeof(hookRel));

        FlushInstructionCache(GetCurrentProcess(), target, kHookByteCount);
        VirtualProtect(target, kHookByteCount, oldProtect, &oldProtect);

        hook.trampoline = trampoline;
        hook.installed = true;
        return true;
    }

    void UninstallInlineHook(InlineHook& hook) {
        if (!hook.installed) {
            return;
        }

        auto* target = reinterpret_cast<uint8_t*>(hook.patchAddress);

        DWORD oldProtect = 0;
        if (VirtualProtect(target, kHookByteCount, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            std::memcpy(target, hook.original, kHookByteCount);
            FlushInstructionCache(GetCurrentProcess(), target, kHookByteCount);
            VirtualProtect(target, kHookByteCount, oldProtect, &oldProtect);
        }

        if (hook.trampoline) {
            VirtualFree(hook.trampoline, 0, MEM_RELEASE);
        }

        hook.trampoline = nullptr;
        hook.patchAddress = 0;
        hook.installed = false;
    }

    void RefreshOriginalHookTargets() {
        gOrigDraw = gHooks[0].trampoline
                        ? reinterpret_cast<uint32_t(__thiscall*)(void*)>(gHooks[0].trampoline)
                        : nullptr;
        gOrigPreStatic = gHooks[1].trampoline
                             ? reinterpret_cast<void(__thiscall*)(void*)>(gHooks[1].trampoline)
                             : nullptr;
        gOrigStatic = gHooks[2].trampoline
                          ? reinterpret_cast<void(__thiscall*)(void*)>(gHooks[2].trampoline)
                          : nullptr;
        gOrigPostStatic = gHooks[3].trampoline
                              ? reinterpret_cast<void(__thiscall*)(void*)>(gHooks[3].trampoline)
                              : nullptr;
        gOrigPreDynamic = gHooks[4].trampoline
                              ? reinterpret_cast<void(__thiscall*)(void*)>(gHooks[4].trampoline)
                              : nullptr;
        gOrigDynamic = gHooks[5].trampoline
                           ? reinterpret_cast<void(__thiscall*)(void*)>(gHooks[5].trampoline)
                           : nullptr;
        gOrigPostDynamic = gHooks[6].trampoline
                               ? reinterpret_cast<void(__thiscall*)(void*)>(gHooks[6].trampoline)
                               : nullptr;
    }

    bool InstallSingleDrawSequenceHook(const size_t index) {
        if (index >= gHooks.size()) {
            return false;
        }
        auto& hook = gHooks[index];
        if (!InstallInlineHook(hook)) {
            return false;
        }
        LOG_INFO("DrawServiceSample: installed hook {} entry=0x{:08X} patch=0x{:08X}",
                 hook.name,
                 static_cast<uint32_t>(hook.address),
                 static_cast<uint32_t>(hook.patchAddress));
        RefreshOriginalHookTargets();
        return true;
    }

    void RemoveSingleDrawSequenceHook(const size_t index) {
        if (index >= gHooks.size()) {
            return;
        }
        auto& hook = gHooks[index];
        if (hook.installed) {
            LOG_INFO("DrawServiceSample: removed hook {}", hook.name);
        }
        UninstallInlineHook(hook);
        RefreshOriginalHookTargets();
    }

    bool InstallDrawSequenceHooks(const bool drawOnly) {
        bool ok = true;
        for (size_t i = 0; i < gHooks.size(); ++i) {
            if (drawOnly && i != 0) {
                continue;
            }
            if (!InstallSingleDrawSequenceHook(i)) {
                ok = false;
                break;
            }
        }

        if (!ok) {
            for (auto& hook : gHooks) {
                UninstallInlineHook(hook);
            }
            gOrigDraw = nullptr;
            gOrigPreStatic = nullptr;
            gOrigStatic = nullptr;
            gOrigPostStatic = nullptr;
            gOrigPreDynamic = nullptr;
            gOrigDynamic = nullptr;
            gOrigPostDynamic = nullptr;
            return false;
        }

        RefreshOriginalHookTargets();
        return true;
    }

    void UninstallDrawSequenceHooks() {
        for (auto& hook : gHooks) {
            if (hook.installed) {
                LOG_INFO("DrawServiceSample: removed hook {}", hook.name);
            }
            UninstallInlineHook(hook);
        }

        gOrigDraw = nullptr;
        gOrigPreStatic = nullptr;
        gOrigStatic = nullptr;
        gOrigPostStatic = nullptr;
        gOrigPreDynamic = nullptr;
        gOrigDynamic = nullptr;
        gOrigPostDynamic = nullptr;
    }

    bool AreDrawSequenceHooksInstalled() {
        for (const auto& hook : gHooks) {
            if (!hook.installed) {
                return false;
            }
        }
        return true;
    }

    bool AreAnyDrawSequenceHooksInstalled() {
        for (const auto& hook : gHooks) {
            if (hook.installed) {
                return true;
            }
        }
        return false;
    }

    class DrawServicePanel final : public ImGuiPanel {
    public:
        explicit DrawServicePanel(cIGZDrawService* drawService)
            : drawService_(drawService) {
            std::snprintf(status_, sizeof(status_), "Idle");
        }

        void OnInit() override {
            RefreshContext();
            LOG_INFO("DrawServiceSample: panel initialized");
        }

        void OnShutdown() override {
            LOG_INFO("DrawServiceSample: panel shutdown");
            delete this;
        }

        void OnRender() override {
            PullHookEvents();
            RenderHookOverlay();

            ImGui::Begin("Draw Service Sample", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

            ImGui::Text("Service: %p", drawService_);
            if (!drawService_) {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "Draw service unavailable.");
                ImGui::End();
                return;
            }

            ImGui::SeparatorText("Hooked Draw Sequence");
            ImGui::Text("Hooks installed: %s", AreDrawSequenceHooksInstalled() ? "yes" : "no");
            if (!AreAnyDrawSequenceHooksInstalled()) {
                if (ImGui::Button("Install Draw Hook Only")) {
                    if (InstallDrawSequenceHooks(true)) {
                        SetStatus("Draw-only hook installed");
                    } else {
                        SetStatus("Failed to install draw-only hook");
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Install All Hooks (Unsafe)")) {
                    if (InstallDrawSequenceHooks(false)) {
                        SetStatus("All draw sequence hooks installed");
                    } else {
                        SetStatus("Failed to install all draw sequence hooks");
                    }
                }
            } else {
                if (ImGui::Button("Remove Hooks")) {
                    UninstallDrawSequenceHooks();
                    SetStatus("Draw sequence hooks removed");
                }
            }
            ImGui::TextUnformatted("Private pass hook isolation:");
            const struct HookUiEntry {
                size_t index;
                const char* shortName;
            } hookUiEntries[] = {
                {1, "PreStatic"},
                {2, "Static"},
                {3, "PostStatic"},
                {4, "PreDynamic"},
                {5, "Dynamic"},
                {6, "PostDynamic"},
            };
            for (const auto& entry : hookUiEntries) {
                const bool installed = gHooks[entry.index].installed;
                char label[64]{};
                std::snprintf(label, sizeof(label), "%s [%s]", entry.shortName, installed ? "ON" : "OFF");
                if (ImGui::Button(label)) {
                    if (installed) {
                        RemoveSingleDrawSequenceHook(entry.index);
                        SetStatus("Removed one private pass hook");
                    } else if (InstallSingleDrawSequenceHook(entry.index)) {
                        SetStatus("Installed one private pass hook");
                    } else {
                        SetStatus("Failed to install private pass hook");
                    }
                }
                ImGui::SameLine();
            }
            ImGui::NewLine();
            ImGui::Checkbox("Overlay begin/end lines", &showHookOverlay_);
            ImGui::SameLine();
            ImGui::SliderInt("History", &overlayHistory_, 8, 128);

            for (size_t i = 0; i < kDrawPassCount; ++i) {
                const auto begin = gBeginCounts[i].load(std::memory_order_relaxed);
                const auto end = gEndCounts[i].load(std::memory_order_relaxed);
                ImGui::Text("%-12s begin=%u end=%u", PassName(static_cast<DrawPass>(i)), begin, end);
            }

            ImGui::SeparatorText("Recent Hook Events");
            const size_t beginIndex = recentEvents_.size() > static_cast<size_t>(overlayHistory_)
                                          ? recentEvents_.size() - static_cast<size_t>(overlayHistory_)
                                          : 0;
            for (size_t i = beginIndex; i < recentEvents_.size(); ++i) {
                const auto& ev = recentEvents_[i];
                ImGui::Text("%s %s (t=%u)", PassName(ev.pass), ev.begin ? "BEGIN" : "END", ev.tickMs);
            }

            ImGui::SeparatorText("Context");
            if (ImGui::Button("Wrap Active Renderer Context")) {
                RefreshContext();
            }
            ImGui::SameLine();
            ImGui::Checkbox("Auto-refresh", &autoRefresh_);
            if (autoRefresh_) {
                RefreshContext();
            }

            ImGui::Text("Handle ptr=%p ver=%u", drawContext_.ptr, drawContext_.version);
            const bool hasContext = drawContext_.ptr != nullptr;
            if (!hasContext) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "No draw context available.");
                ImGui::TextWrapped("%s", status_);
                ImGui::End();
                return;
            }

            ImGui::SeparatorText("Renderer Passes");
            if (ImGui::Button("Draw()")) {
                lastDrawResult_ = drawService_->RendererDraw();
                drawCallCount_++;
                SetStatus("RendererDraw called");
            }
            ImGui::SameLine();
            if (ImGui::Button("PreStatic")) {
                drawService_->RendererDrawPreStaticView();
                preStaticCount_++;
                SetStatus("RendererDrawPreStaticView called");
            }
            ImGui::SameLine();
            if (ImGui::Button("Static")) {
                drawService_->RendererDrawStaticView();
                staticCount_++;
                SetStatus("RendererDrawStaticView called");
            }
            ImGui::SameLine();
            if (ImGui::Button("PostStatic")) {
                drawService_->RendererDrawPostStaticView();
                postStaticCount_++;
                SetStatus("RendererDrawPostStaticView called");
            }
            if (ImGui::Button("PreDynamic")) {
                drawService_->RendererDrawPreDynamicView();
                preDynamicCount_++;
                SetStatus("RendererDrawPreDynamicView called");
            }
            ImGui::SameLine();
            if (ImGui::Button("Dynamic")) {
                drawService_->RendererDrawDynamicView();
                dynamicCount_++;
                SetStatus("RendererDrawDynamicView called");
            }
            ImGui::SameLine();
            if (ImGui::Button("PostDynamic")) {
                drawService_->RendererDrawPostDynamicView();
                postDynamicCount_++;
                SetStatus("RendererDrawPostDynamicView called");
            }

            ImGui::Checkbox("Auto Draw", &autoDraw_);
            ImGui::SameLine();
            ImGui::Checkbox("Auto Dynamic Trio", &autoDynamicTrio_);
            if (autoDraw_) {
                lastDrawResult_ = drawService_->RendererDraw();
                drawCallCount_++;
            }
            if (autoDynamicTrio_) {
                drawService_->RendererDrawPreDynamicView();
                drawService_->RendererDrawDynamicView();
                drawService_->RendererDrawPostDynamicView();
                preDynamicCount_++;
                dynamicCount_++;
                postDynamicCount_++;
            }
            ImGui::Text("Result=0x%08X | Draw=%u PreS=%u S=%u PostS=%u PreD=%u D=%u PostD=%u",
                        lastDrawResult_, drawCallCount_, preStaticCount_, staticCount_, postStaticCount_,
                        preDynamicCount_, dynamicCount_, postDynamicCount_);

            ImGui::SeparatorText("Highlight");
            ImGui::SliderInt("Highlight Type", &highlightType_, 0, 15);
            ImGui::ColorEdit4("Highlight RGBA", highlightColor_);
            if (ImGui::Button("Set Highlight Color")) {
                drawService_->SetHighlightColor(drawContext_, highlightType_,
                                                highlightColor_[0], highlightColor_[1],
                                                highlightColor_[2], highlightColor_[3]);
                SetStatus("SetHighlightColor called");
            }
            ImGui::SameLine();
            if (ImGui::Button("Set Highlight State")) {
                drawService_->SetRenderStateHighlight(drawContext_, highlightType_);
                SetStatus("SetRenderStateHighlight(type) called");
            }

            ImGui::SeparatorText("Render State");
            if (ImGui::Button("Default Render State")) {
                drawService_->SetDefaultRenderState(drawContext_);
                SetStatus("SetDefaultRenderState called");
            }
            ImGui::SameLine();
            if (ImGui::Button("Default Unilateral")) {
                drawService_->SetDefaultRenderStateUnilaterally(drawContext_);
                SetStatus("SetDefaultRenderStateUnilaterally called");
            }

            ImGui::Checkbox("Blend", &blend_);
            ImGui::SameLine();
            ImGui::Checkbox("Alpha Test", &alphaTest_);
            ImGui::SameLine();
            ImGui::Checkbox("Depth Test", &depthTest_);
            ImGui::SameLine();
            ImGui::Checkbox("Depth Mask", &depthMask_);
            ImGui::SameLine();
            ImGui::Checkbox("Cull", &cullFace_);
            ImGui::SameLine();
            ImGui::Checkbox("Color Mask", &colorMask_);
            if (ImGui::Button("Apply State Flags")) {
                drawService_->EnableBlendStateFlag(drawContext_, blend_);
                drawService_->EnableAlphaTestFlag(drawContext_, alphaTest_);
                drawService_->EnableDepthTestFlag(drawContext_, depthTest_);
                drawService_->EnableDepthMaskFlag(drawContext_, depthMask_);
                drawService_->EnableCullFaceFlag(drawContext_, cullFace_);
                drawService_->EnableColorMaskFlag(drawContext_, colorMask_);
                SetStatus("Applied blend/alpha/depth/cull/color-mask flags");
            }

            ImGui::SeparatorText("Texture / Lighting / Fog");
            ImGui::SliderInt("Texture Stage", &textureStage_, 0, 3);
            ImGui::Checkbox("Texture State Enabled", &textureStateEnabled_);
            if (ImGui::Button("Apply Texture Stage Flag")) {
                drawService_->EnableTextureStateFlag(drawContext_, textureStateEnabled_, textureStage_);
                SetStatus("EnableTextureStateFlag called");
            }

            ImGui::ColorEdit4("Texture Color", texColor_);
            if (ImGui::Button("Set Tex Color")) {
                drawService_->SetTexColor(drawContext_, texColor_[0], texColor_[1], texColor_[2], texColor_[3]);
                SetStatus("SetTexColor called");
            }

            ImGui::Checkbox("Lighting Enabled", &lightingEnabled_);
            ImGui::SameLine();
            if (ImGui::Button("Apply Lighting")) {
                drawService_->SetLighting(drawContext_, lightingEnabled_);
                SetStatus("SetLighting called");
            }
            const bool lightingNow = drawService_->GetLighting(drawContext_);
            ImGui::Text("GetLighting: %s", lightingNow ? "true" : "false");

            ImGui::Checkbox("Fog Enabled", &fogEnabled_);
            ImGui::ColorEdit3("Fog Color", fogColor_);
            ImGui::InputFloat("Fog Start", &fogStart_, 1.0f, 10.0f, "%.2f");
            ImGui::InputFloat("Fog End", &fogEnd_, 1.0f, 10.0f, "%.2f");
            if (ImGui::Button("Apply Fog")) {
                drawService_->SetFog(drawContext_, fogEnabled_, fogColor_, fogStart_, fogEnd_);
                SetStatus("SetFog called");
            }

            ImGui::Separator();
            ImGui::TextWrapped("%s", status_);
            ImGui::End();
        }

    private:
        static ImU32 PassColor(const DrawPass pass, const bool begin) {
            switch (pass) {
            case DrawPass::Draw: return begin ? IM_COL32(250, 220, 120, 255) : IM_COL32(160, 140, 80, 255);
            case DrawPass::PreStatic: return begin ? IM_COL32(120, 220, 255, 255) : IM_COL32(60, 140, 190, 255);
            case DrawPass::Static: return begin ? IM_COL32(80, 255, 180, 255) : IM_COL32(60, 170, 120, 255);
            case DrawPass::PostStatic: return begin ? IM_COL32(180, 220, 255, 255) : IM_COL32(95, 120, 150, 255);
            case DrawPass::PreDynamic: return begin ? IM_COL32(255, 180, 120, 255) : IM_COL32(200, 110, 70, 255);
            case DrawPass::Dynamic: return begin ? IM_COL32(255, 120, 120, 255) : IM_COL32(180, 70, 70, 255);
            case DrawPass::PostDynamic: return begin ? IM_COL32(220, 160, 255, 255) : IM_COL32(135, 90, 180, 255);
            default: return begin ? IM_COL32(180, 255, 180, 255) : IM_COL32(120, 120, 120, 255);
            }
        }

        void PullHookEvents() {
            const uint64_t latestSeq = gEventSeq.load(std::memory_order_acquire);
            if (latestSeq == lastSeenSeq_) {
                return;
            }

            const uint64_t oldestAvailable = latestSeq > kEventRingCapacity ? latestSeq - kEventRingCapacity + 1 : 1;
            uint64_t nextSeq = lastSeenSeq_ + 1;
            if (nextSeq < oldestAvailable) {
                nextSeq = oldestAvailable;
            }

            for (uint64_t seq = nextSeq; seq <= latestSeq; ++seq) {
                const HookEvent ev = gEventRing[seq % kEventRingCapacity];
                if (ev.seq == seq) {
                    recentEvents_.push_back(ev);
                }
            }
            lastSeenSeq_ = latestSeq;

            constexpr size_t kMaxRetainedEvents = 512;
            if (recentEvents_.size() > kMaxRetainedEvents) {
                recentEvents_.erase(recentEvents_.begin(),
                                    recentEvents_.begin() + (recentEvents_.size() - kMaxRetainedEvents));
            }
        }

        void RenderHookOverlay() const {
            if (!showHookOverlay_ || recentEvents_.empty()) {
                return;
            }

            ImDrawList* drawList = ImGui::GetForegroundDrawList();
            const ImVec2 origin(20.0f, 110.0f);
            const float width = 180.0f;
            const float rowHeight = 8.0f;

            const size_t count = recentEvents_.size() > static_cast<size_t>(overlayHistory_)
                                     ? static_cast<size_t>(overlayHistory_)
                                     : recentEvents_.size();
            const size_t start = recentEvents_.size() - count;

            for (size_t i = 0; i < count; ++i) {
                const auto& ev = recentEvents_[start + i];
                const float y = origin.y + rowHeight * static_cast<float>(i);
                const ImU32 color = PassColor(ev.pass, ev.begin);
                drawList->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + width, y), color, 2.0f);

                char label[64]{};
                std::snprintf(label, sizeof(label), "%s %s", PassName(ev.pass), ev.begin ? "BEGIN" : "END");
                drawList->AddText(ImVec2(origin.x + width + 6.0f, y - 6.0f), IM_COL32(255, 255, 255, 230), label);
            }
        }

        void RefreshContext() {
            drawContext_ = drawService_->WrapActiveRendererDrawContext();
            if (drawContext_.ptr) {
                SetStatus("Wrapped active renderer draw context");
            } else {
                SetStatus("No active renderer draw context");
            }
        }

        void SetStatus(const char* text) {
            std::snprintf(status_, sizeof(status_), "%s", text);
            LOG_INFO("DrawServiceSample: {}", text);
        }

    private:
        cIGZDrawService* drawService_;
        SC4DrawContextHandle drawContext_{nullptr, 0};
        bool autoRefresh_ = false;

        int highlightType_ = 0;
        float highlightColor_[4] = {1.0f, 0.2f, 0.2f, 1.0f};

        bool blend_ = true;
        bool alphaTest_ = false;
        bool depthTest_ = true;
        bool depthMask_ = true;
        bool cullFace_ = true;
        bool colorMask_ = true;

        int textureStage_ = 0;
        bool textureStateEnabled_ = true;
        float texColor_[4] = {1.0f, 1.0f, 1.0f, 1.0f};

        bool lightingEnabled_ = true;
        bool fogEnabled_ = false;
        float fogColor_[3] = {0.65f, 0.75f, 0.90f};
        float fogStart_ = 400.0f;
        float fogEnd_ = 1800.0f;
        bool autoDraw_ = false;
        bool autoDynamicTrio_ = false;
        uint32_t lastDrawResult_ = 0;
        uint32_t drawCallCount_ = 0;
        uint32_t preStaticCount_ = 0;
        uint32_t staticCount_ = 0;
        uint32_t postStaticCount_ = 0;
        uint32_t preDynamicCount_ = 0;
        uint32_t dynamicCount_ = 0;
        uint32_t postDynamicCount_ = 0;

        bool showHookOverlay_ = true;
        int overlayHistory_ = 32;
        uint64_t lastSeenSeq_ = 0;
        std::vector<HookEvent> recentEvents_{};

        char status_[160]{};
    };
}

class DrawServiceSampleDirector final : public cRZCOMDllDirector {
public:
    DrawServiceSampleDirector() = default;

    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kDrawSampleDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZCOMDllDirector::OnStart(pCOM);
        Logger::Initialize("SC4DrawServiceSample", "");
        LOG_INFO("DrawServiceSample: OnStart");
        if (mpFrameWork) {
            mpFrameWork->AddHook(this);
        } else {
            LOG_WARN("DrawServiceSample: framework unavailable in OnStart");
        }
        return true;
    }

    bool PostAppInit() override {
        if (!mpFrameWork || panelRegistered_) {
            return true;
        }

        if (!mpFrameWork->GetSystemService(kImGuiServiceID, GZIID_cIGZImGuiService,
                                           reinterpret_cast<void**>(&imguiService_))) {
            LOG_WARN("DrawServiceSample: ImGui service not available");
            return true;
        }

        if (!mpFrameWork->GetSystemService(kDrawServiceID, GZIID_cIGZDrawService,
                                           reinterpret_cast<void**>(&drawService_))) {
            LOG_WARN("DrawServiceSample: Draw service not available");
            imguiService_->Release();
            imguiService_ = nullptr;
            return true;
        }

        auto* panel = new DrawServicePanel(drawService_);
        const ImGuiPanelDesc desc =
            ImGuiPanelAdapter<DrawServicePanel>::MakeDesc(panel, kDrawSamplePanelId, 150, true);

        if (!imguiService_->RegisterPanel(desc)) {
            LOG_WARN("DrawServiceSample: failed to register panel");
            delete panel;
            UninstallDrawSequenceHooks();
            drawService_->Release();
            imguiService_->Release();
            drawService_ = nullptr;
            imguiService_ = nullptr;
            return true;
        }

        panelRegistered_ = true;
        LOG_INFO("DrawServiceSample: panel registered");
        return true;
    }

    bool PostAppShutdown() override {
        if (imguiService_) {
            imguiService_->UnregisterPanel(kDrawSamplePanelId);
            imguiService_->Release();
            imguiService_ = nullptr;
        }
        UninstallDrawSequenceHooks();
        if (drawService_) {
            drawService_->Release();
            drawService_ = nullptr;
        }
        panelRegistered_ = false;
        return true;
    }

private:
    cIGZImGuiService* imguiService_ = nullptr;
    cIGZDrawService* drawService_ = nullptr;
    bool panelRegistered_ = false;
};

static DrawServiceSampleDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static auto sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
