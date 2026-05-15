// ReSharper disable CppDFAConstantConditions
// ReSharper disable CppDFAUnreachableCode
/*
 * DateJumperDirector.cpp
 *
 * Sample DLL that fast-forwards the SimCity 4 game date by running the simulation
 * at maximum speed until a target date is reached, then pausing.
 *
 * HOTKEY (Ctrl+F9): configured via the companion SC4DateJumperSample.dat file.
 *   Maps Ctrl+F9 to message ID 0xDA7E1F09.
 *   - Jan 1 – Aug 31:   fast-forward to September 1 of the current year.
 *   - Sep 1 – Dec 31:   fast-forward to June 1 of the following year.
 *   - Press again while running to cancel and pause immediately.
 *
 * CHEAT CODES (open the cheat dialog with Ctrl+X):
 *   jumpspring  →  next March 1
 *   jumpsummer  →  next June 1
 *   jumpfall    →  next September 1
 *   jumpwinter  →  next December 1
 *
 * The sim runs at full speed so all day/month/year agents fire normally,
 * keeping budgets and statistics consistent.
 */

#include "cGZPersistResourceKey.h"
#include "cIGZCOM.h"
#include "cIGZCheatCodeManager.h"
#include "cIGZDate.h"
#include "cIGZFrameWork.h"
#include "cIGZMessage2.h"
#include "cIGZMessage2Standard.h"
#include "cIGZMessageServer2.h"
#include "cIGZPersistResourceManager.h"
#include "cIGZWin.h"
#include "cIGZWinKeyAcceleratorRes.h"
#include "cISC4App.h"
#include "cISC4Simulator.h"
#include "cISC4View3DWin.h"
#include "cRZAutoRefCount.h"
#include "cRZBaseString.h"
#include "cRZMessage2COMDirector.h"
#include "GZServPtrs.h"
#include "utils/Logger.h"

namespace {
    constexpr uint32_t kDateJumperDirectorID      = 0xDA7E3141;
    constexpr uint32_t kSC4MessagePostCityInit    = 0x26D31EC1;
    constexpr uint32_t kSC4MessagePreCityShutdown = 0x26D31EC2;
    constexpr uint32_t kCheatCodeMessageType      = 0x230E27AC;

    // TGI of the WinKey resource in the companion DAT file
    constexpr uint32_t kKeyConfigType     = 0xA2E3D533;
    constexpr uint32_t kKeyConfigGroup    = 0xDA7E0001;
    constexpr uint32_t kKeyConfigInstance = 0xDA7E0002;

    // Message dispatched by the View3D key accelerator when the hotkey fires
    constexpr uint32_t kDateJumpShortcutID = 0xDA7E1F09;

    // Unique IDs for each cheat code
    constexpr uint32_t kCheatJumpSeason = 0xDA7E1000;
    constexpr uint32_t kCheatJumpSpring = 0xDA7E1001;
    constexpr uint32_t kCheatJumpSummer = 0xDA7E1002;
    constexpr uint32_t kCheatJumpFall   = 0xDA7E1003;
    constexpr uint32_t kCheatJumpWinter = 0xDA7E1004;

    constexpr uint32_t kGZWin_WinSC4App    = 0x6104489A;
    constexpr uint32_t kGZWin_SC4View3DWin = 0x9A47B417;

    const char* kMonthNames[] = {
        "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
}

class DateJumperDirector final : public cRZMessage2COMDirector
{
public:
    DateJumperDirector() = default;

    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kDateJumperDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZMessage2COMDirector::OnStart(pCOM);
        Logger::Initialize("SC4DateJumper", "");
        mpFrameWork->AddHook(this);
        return true;
    }

    bool PostAppInit() override {
        cIGZMessageServer2Ptr pMS2;
        if (pMS2) {
            pMS2->AddNotification(this, kSC4MessagePostCityInit);
            pMS2->AddNotification(this, kSC4MessagePreCityShutdown);
            messageServer_ = pMS2;
        }

        const cISC4AppPtr app;
        if (app) {
            auto* cheats = app->GetCheatCodeManager();
            if (cheats) {
                cheats->RegisterCheatCode(kCheatJumpSeason, cRZBaseString("jumpseason"));
                cheats->RegisterCheatCode(kCheatJumpSpring, cRZBaseString("jumpspring"));
                cheats->RegisterCheatCode(kCheatJumpSummer, cRZBaseString("jumpsummer"));
                cheats->RegisterCheatCode(kCheatJumpFall,   cRZBaseString("jumpfall"));
                cheats->RegisterCheatCode(kCheatJumpWinter, cRZBaseString("jumpwinter"));
                cheats->AddNotification2(this, 0);
                cheatManager_ = cheats;
                LOG_INFO("DateJumper: cheat codes registered (jumpseason/jumpspring/jumpsummer/jumpfall/jumpwinter)");
            }
        }

        return true;
    }

    bool DoMessage(cIGZMessage2* pMsg) override {
        auto* stdMsg = static_cast<cIGZMessage2Standard*>(pMsg);
        switch (stdMsg->GetType()) {
            case kSC4MessagePostCityInit:
                PostCityInit_();
                break;
            case kSC4MessagePreCityShutdown:
                PreCityShutdown_();
                break;
            case kDateJumpShortcutID:
                OnHotkeyFired_();
                break;
            case kCheatCodeMessageType:
                OnCheat_(static_cast<uint32_t>(stdMsg->GetData1()));
                break;
            default:
                if (agentRegistered_) {
                    CheckArrival_();
                }
                break;
        }
        return true;
    }

    bool PostAppShutdown() override {
        CancelFastForward_();

        if (cheatManager_) {
            cheatManager_->UnregisterCheatCode(kCheatJumpSeason);
            cheatManager_->UnregisterCheatCode(kCheatJumpSpring);
            cheatManager_->UnregisterCheatCode(kCheatJumpSummer);
            cheatManager_->UnregisterCheatCode(kCheatJumpFall);
            cheatManager_->UnregisterCheatCode(kCheatJumpWinter);
            cheatManager_->RemoveNotification2(this, 0);
            cheatManager_.Reset();
        }

        if (messageServer_) {
            messageServer_->RemoveNotification(this, kSC4MessagePostCityInit);
            messageServer_->RemoveNotification(this, kSC4MessagePreCityShutdown);
            messageServer_ = nullptr;
        }

        if (mpFrameWork) {
            mpFrameWork->RemoveHook(this);
        }

        return true;
    }

private:
    void PostCityInit_() {
        cityLoaded_ = true;

        const cISC4AppPtr app;
        if (!app) return;

        cIGZWin* pMain = app->GetMainWindow();
        if (!pMain) return;

        cIGZWin* pSC4App = pMain->GetChildWindowFromID(kGZWin_WinSC4App);
        if (!pSC4App) return;

        if (!pSC4App->GetChildAs(kGZWin_SC4View3DWin, kGZIID_cISC4View3DWin,
                                  reinterpret_cast<void**>(&pView3D_))) {
            LOG_WARN("DateJumper: failed to acquire View3D window");
            return;
        }

        cIGZPersistResourceManagerPtr pRM;
        if (!pRM) {
            LOG_WARN("DateJumper: resource manager not available");
            return;
        }

        cRZAutoRefCount<cIGZWinKeyAcceleratorRes> accelRes;
        const cGZPersistResourceKey key(kKeyConfigType, kKeyConfigGroup, kKeyConfigInstance);
        if (!pRM->GetPrivateResource(key, kGZIID_cIGZWinKeyAcceleratorRes,
                                     accelRes.AsPPVoid(), 0, nullptr)) {
            LOG_WARN("DateJumper: key config DAT not found (TGI {:08X}/{:08X}/{:08X}). "
                     "Hotkey will not work without the companion DAT file.",
                     kKeyConfigType, kKeyConfigGroup, kKeyConfigInstance);
            return;
        }

        accelRes->RegisterResources(pView3D_->GetKeyAccelerator());
        if (messageServer_) {
            messageServer_->AddNotification(this, kDateJumpShortcutID);
        }
        LOG_INFO("DateJumper: hotkey registered");
    }

    void PreCityShutdown_() {
        CancelFastForward_();
        cityLoaded_ = false;

        if (messageServer_) {
            messageServer_->RemoveNotification(this, kDateJumpShortcutID);
        }

        if (pView3D_) {
            pView3D_->Release();
            pView3D_ = nullptr;
        }
    }

    void OnHotkeyFired_() {
        if (!cityLoaded_) return;

        if (agentRegistered_) {
            CancelFastForward_();
            return;
        }

        const cISC4SimulatorPtr sim;
        if (!sim) return;

        cIGZDate* date = sim->GetSimDate();
        if (!date) return;

        // Sep–Dec → jump to June (summer); Jan–Aug → jump to September (fall)
        uint32_t targetMonth = (date->Month() >= 9) ? 6u : 9u;
        BeginFastForwardToMonth_(targetMonth);
    }

    void OnCheat_(uint32_t cheatId) {
        if (!cityLoaded_) {
            LOG_INFO("DateJumper: cheat ignored (not in city view)");
            return;
        }
        switch (cheatId) {
            case kCheatJumpSeason: OnHotkeyFired_();              break;
            case kCheatJumpSpring: BeginFastForwardToMonth_(3);  break;
            case kCheatJumpSummer: BeginFastForwardToMonth_(6);  break;
            case kCheatJumpFall:   BeginFastForwardToMonth_(9);  break;
            case kCheatJumpWinter: BeginFastForwardToMonth_(12); break;
            default: break;
        }
    }

    void BeginFastForwardToMonth_(uint32_t targetMonth) {
        if (agentRegistered_) {
            CancelFastForward_();
        }

        const cISC4SimulatorPtr sim;
        if (!sim) return;

        cIGZDate* date = sim->GetSimDate();
        if (!date) return;

        uint32_t year        = date->Year();
        uint32_t currentJDay = date->DayNumber();

        uint32_t targetYear = year;
        uint32_t targetJDay = date->Jday(targetMonth, 1, year);
        if (targetJDay <= currentJDay) {
            targetYear = year + 1;
            targetJDay = date->Jday(targetMonth, 1, targetYear);
        }

        targetJDay_ = targetJDay;

        cRZBaseString agentName("DateJumper");
        auto flags = static_cast<cISC4Simulator::eAgentFlags>(
            cISC4Simulator::AgentFlagEnabledForUnestablishedCities |
            cISC4Simulator::AgentFlagEnabledForEstablishedCities);

        if (!sim->AddAgent(this, cISC4Simulator::AgentTypeSimNewDay, agentName, flags)) {
            LOG_WARN("DateJumper: failed to register day agent");
            return;
        }
        agentRegistered_ = true;

        if (sim->IsAnyPaused()) {
            sim->Resume();
        }
        sim->SetSimSpeed(2);

        LOG_INFO("DateJumper: fast-forwarding to {} 1, {} ({} days ahead)",
                 kMonthNames[targetMonth], targetYear, targetJDay - currentJDay);
    }

    void CheckArrival_() {
        const cISC4SimulatorPtr sim;
        if (!sim) return;

        cIGZDate* date = sim->GetSimDate();
        if (!date || date->DayNumber() < targetJDay_) return;

        sim->RemoveAgent(this, cISC4Simulator::AgentTypeSimNewDay);
        agentRegistered_ = false;
        sim->Pause();

        LOG_INFO("DateJumper: arrived at {} 1, {}, paused",
                 kMonthNames[date->Month()], date->Year());
    }

    void CancelFastForward_() {
        if (!agentRegistered_) return;

        const cISC4SimulatorPtr sim;
        if (sim) {
            sim->RemoveAgent(this, cISC4Simulator::AgentTypeSimNewDay);
            sim->Pause();
        }
        agentRegistered_ = false;
        LOG_INFO("DateJumper: fast-forward cancelled");
    }

    bool cityLoaded_      = false;
    bool agentRegistered_ = false;
    uint32_t targetJDay_  = 0;
    cISC4View3DWin* pView3D_ = nullptr;
    cRZAutoRefCount<cIGZMessageServer2>    messageServer_;
    cRZAutoRefCount<cIGZCheatCodeManager>  cheatManager_;
};

static DateJumperDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static bool sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
