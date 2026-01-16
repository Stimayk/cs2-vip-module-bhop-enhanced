#include <cstdio>
#include <string>
#include "vip_bhop.h"
#include "schemasystem/schemasystem.h"
#include <networksystem/inetworkserializer.h>
#include <networksystem/inetworkmessages.h>
#include "protobuf/generated/networkbasetypes.pb.h"
#include <igameeventsystem.h>
#include <irecipientfilter.h>

vip_bhop g_vip_bhop;

IVIPApi* g_pVIPCore;
IUtilsApi* g_pUtils;

IVEngineServer2* engine = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;
extern INetworkMessages* g_pNetworkMessages;
IGameEventSystem* g_pGameEventSystem = nullptr;

PLUGIN_EXPOSE(vip_bhop, g_vip_bhop);

struct User {
    int LastButtons;
    int LastFlags;
    int JumpsCount;
    bool bOnCooldown;
    CTimer* pCooldownTimer;
    CTimer* pResetTimer;
};

User UserSettings[64];
static CTimer* pActivationTimers[64] = { nullptr };

static inline std::string MakeMetaUnloadCommand(PluginId id) {
    return std::string("meta unload ") + std::to_string(id);
}

bool vip_bhop::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) {
    PLUGIN_SAVEVARS();

    GET_V_IFACE_CURRENT(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, g_pNetworkMessages, INetworkMessages, NETWORKMESSAGES_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetEngineFactory, g_pGameEventSystem, IGameEventSystem, GAMEEVENTSYSTEM_INTERFACE_VERSION);

    if (g_SMAPI)
        g_SMAPI->AddListener(this, this);
    return true;
}

void ReplicateConVar(int playerSlot, const char* pszName, const char* pszValue) {
    if (!g_pNetworkMessages || !pszName || !pszValue) return;

    INetworkMessageInternal* pNetMsg = g_pNetworkMessages->FindNetworkMessagePartial("SetConVar");
    if (!pNetMsg) return;

    CNetMessage* pMsg = pNetMsg->AllocateMessage();
    if (!pMsg) return;

    CNETMsg_SetConVar* data = pMsg->ToPB<CNETMsg_SetConVar>();
    if (!data) return;

    CMsg_CVars_CVar* cvarMsg = data->mutable_convars()->add_cvars();
    if (!cvarMsg) return;

    cvarMsg->set_name(pszName);
    cvarMsg->set_value(pszValue);

    CPlayerBitVec filter;
    filter.Set(playerSlot);

    if (g_pGameEventSystem) {
        g_pGameEventSystem->PostEventAbstract(
            -1,
            false,
            1,
            reinterpret_cast<const uint64*>(filter.Base()),
            pNetMsg,
            reinterpret_cast<const CNetMessage*>(pMsg),
            0u,
            NetChannelBufType_t::BUF_RELIABLE
        );
    }
}

void SetPlayerBhopState(int iSlot, bool bEnable) {
    ReplicateConVar(iSlot, "sv_autobunnyhopping", bEnable ? "1" : "0");
    ReplicateConVar(iSlot, "sv_enablebunnyhopping", bEnable ? "1" : "0");
}

void ChangeVelocity(CCSPlayerPawnBase* pPawn, float newVelocity)
{
    if (!pPawn)
        return;

    Vector currentVelocity = pPawn->m_vecAbsVelocity();

    float currentSpeed = sqrtf(
        currentVelocity.x * currentVelocity.x +
        currentVelocity.y * currentVelocity.y +
        currentVelocity.z * currentVelocity.z
    );

    if (currentSpeed <= 0.0f)
        return;

    currentVelocity.x = (currentVelocity.x / currentSpeed) * newVelocity;
    currentVelocity.y = (currentVelocity.y / currentSpeed) * newVelocity;

    pPawn->SetAbsVelocity(currentVelocity);
}

void ClearPlayerTimers(int iSlot) {
    if (iSlot < 0 || iSlot >= 64 || !g_pUtils) return;

    if (pActivationTimers[iSlot]) {
        g_pUtils->RemoveTimer(pActivationTimers[iSlot]);
        pActivationTimers[iSlot] = nullptr;
    }

    auto& user = UserSettings[iSlot];
    if (user.pCooldownTimer) {
        g_pUtils->RemoveTimer(user.pCooldownTimer);
        user.pCooldownTimer = nullptr;
    }

    if (user.pResetTimer) {
        g_pUtils->RemoveTimer(user.pResetTimer);
        user.pResetTimer = nullptr;
    }
}

void ResetPlayerJumps(int iSlot) {
    if (iSlot < 0 || iSlot >= 64) return;

    auto& user = UserSettings[iSlot];
    user.JumpsCount = 0;
    user.pResetTimer = nullptr;
}

void ActivateBhopAfterDelay(int iSlot) {
    if (!g_pVIPCore || !g_pUtils || !g_pVIPCore->VIP_GetClientFeatureBool(iSlot, "bhop_enhanced")) return;

    auto& user = UserSettings[iSlot];
    user.bOnCooldown = true;
    SetPlayerBhopState(iSlot, false);

    char szBuffer[128];
    snprintf(szBuffer, sizeof(szBuffer), g_pVIPCore->VIP_GetTranslate("BhopActivate"), g_pVIPCore->VIP_GetClientFeatureFloat(iSlot, "bhop_start_time"));
    g_pUtils->PrintToCenter(iSlot, szBuffer);

    ClearPlayerTimers(iSlot);

    pActivationTimers[iSlot] = g_pUtils->CreateTimer(g_pVIPCore->VIP_GetClientFeatureFloat(iSlot, "bhop_start_time"), [iSlot]() -> float {
        if (iSlot < 0 || iSlot >= 64) return -1.0f;
        auto& userRef = UserSettings[iSlot];
        userRef.JumpsCount = 0;
        userRef.bOnCooldown = false;
        SetPlayerBhopState(iSlot, true);
        char szBuffer[128];
        snprintf(szBuffer, sizeof(szBuffer), g_pVIPCore->VIP_GetTranslate("BhopAvailable"), g_pVIPCore->VIP_GetClientFeatureInt(iSlot, "bhop_max_jumps"));
        g_pUtils->PrintToCenter(iSlot, szBuffer);
        pActivationTimers[iSlot] = nullptr;
        return -1.0f;
    });
}

void VIP_OnPlayerSpawn(int iSlot, int iTeam, bool bIsVIP) {
    ActivateBhopAfterDelay(iSlot);
}

void StartBhopCooldown(int iSlot) {
    if (iSlot < 0 || iSlot >= 64) return;

    auto& user = UserSettings[iSlot];
    if (user.bOnCooldown) return;

    user.bOnCooldown = true;
    SetPlayerBhopState(iSlot, false);

    if (g_pUtils && g_pVIPCore) {
        char szCooldown[128];
        snprintf(szCooldown, sizeof(szCooldown), g_pVIPCore->VIP_GetTranslate("BhopCooldown"), g_pVIPCore->VIP_GetClientFeatureFloat(iSlot, "bhop_cooldown_time"));
        g_pUtils->PrintToChat(iSlot, "%s %s", g_pVIPCore->VIP_GetTranslate("Prefix"), szCooldown);
    }

    if (user.pCooldownTimer && g_pUtils) {
        g_pUtils->RemoveTimer(user.pCooldownTimer);
        user.pCooldownTimer = nullptr;
    }

    if (!g_pUtils) return;

    user.pCooldownTimer = g_pUtils->CreateTimer(g_pVIPCore->VIP_GetClientFeatureFloat(iSlot, "bhop_cooldown_time"), [iSlot]() -> float {
        if (iSlot < 0 || iSlot >= 64 || !g_pUtils) return -1.0f;

        auto& usr = UserSettings[iSlot];
        usr.JumpsCount = 0;
        usr.bOnCooldown = false;
        SetPlayerBhopState(iSlot, true);

        if (g_pUtils && g_pVIPCore) {
            g_pUtils->PrintToChat(iSlot, "%s %s", g_pVIPCore->VIP_GetTranslate("Prefix"), g_pVIPCore->VIP_GetTranslate("BhopReady"));
        }

        usr.pCooldownTimer = nullptr;
        return -1.0f;
    });
}

bool vip_bhop::Unload(char* error, size_t maxlen) {
    ConVar_Unregister();

    if (g_pUtils) {
        for (int i = 0; i < 64; i++) {
            ClearPlayerTimers(i);
        }
    }

    g_pUtils = nullptr;
    g_pVIPCore = nullptr;
    g_pGameEventSystem = nullptr;
    g_pNetworkMessages = nullptr;

    return true;
}

CGameEntitySystem* GameEntitySystem() {
    return (g_pUtils) ? g_pUtils->GetCGameEntitySystem() : nullptr;
}

bool OnToggle(int iSlot, const char* szFeature, VIP_ToggleState eOldStatus, VIP_ToggleState& eNewStatus) {
    SetPlayerBhopState(iSlot, eNewStatus == ENABLED);
    return false;
}

void OnRoundStart(const char* szName, IGameEvent* pEvent, bool bDontBroadcast) {
    if (!g_pVIPCore || !g_pUtils) return;

    for (int iSlot = 0; iSlot < 64; iSlot++) {
        ActivateBhopAfterDelay(iSlot);
    }
}

void VIP_OnVIPLoaded() {
    g_pGameEntitySystem = GameEntitySystem();
    g_pEntitySystem = g_pGameEntitySystem;
    g_pUtils->HookEvent(g_PLID, "round_start", OnRoundStart);
    if (g_pVIPCore)
        g_pVIPCore->VIP_OnPlayerSpawn(VIP_OnPlayerSpawn);
}

void vip_bhop::AllPluginsLoaded() {
    int ret = 0;
    g_pUtils = reinterpret_cast<IUtilsApi*>(g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, nullptr));
    if (ret == META_IFACE_FAILED || !g_pUtils) {
        ConColorMsg(Color(255, 0, 0, 255), "[%s] Failed to lookup utils api. Aborting\n", GetLogTag());
        engine->ServerCommand(MakeMetaUnloadCommand(g_PLID).c_str());
        return;
    }

    g_pVIPCore = reinterpret_cast<IVIPApi*>(g_SMAPI->MetaFactory(VIP_INTERFACE, &ret, nullptr));
    if (ret == META_IFACE_FAILED || !g_pVIPCore) {
        g_pUtils->ErrorLog("[%s] Failed to lookup vip core. Aborting", GetLogTag());
        engine->ServerCommand(MakeMetaUnloadCommand(g_PLID).c_str());
        return;
    }

    g_pVIPCore->VIP_OnVIPLoaded(VIP_OnVIPLoaded);
    g_pVIPCore->VIP_RegisterFeature("bhop_enhanced", VIP_BOOL, TOGGLABLE, nullptr, OnToggle);
    g_pVIPCore->VIP_RegisterFeature("bhop_max_jumps", VIP_INT, HIDE);
    g_pVIPCore->VIP_RegisterFeature("bhop_start_time", VIP_FLOAT, HIDE);
    g_pVIPCore->VIP_RegisterFeature("bhop_cooldown_time", VIP_FLOAT, HIDE);
    g_pVIPCore->VIP_RegisterFeature("bhop_reset_time", VIP_FLOAT, HIDE);
    g_pVIPCore->VIP_RegisterFeature("bhop_max_speed", VIP_FLOAT, HIDE);
    g_pVIPCore->VIP_RegisterFeature("bhop_jump_power", VIP_FLOAT, HIDE);

    g_pUtils->CreateTimer(0.0f, []() {
        if (g_pVIPCore && g_pVIPCore->VIP_IsVIPLoaded()) {
            for (int i = 0; i < 64; i++) {
                CCSPlayerController* pPlayerController = CCSPlayerController::FromSlot(i);
                if (!pPlayerController) continue;
                CCSPlayerPawnBase* pPlayerPawn = pPlayerController->m_hPlayerPawn();
                if (!pPlayerPawn || !pPlayerPawn->IsAlive()) continue;
                if (!pPlayerPawn->m_pMovementServices()) continue;
                if (!g_pVIPCore->VIP_GetClientFeatureBool(i, "bhop_enhanced")) continue;

                auto& user = UserSettings[i];
                if (user.bOnCooldown) continue;

                int flags = pPlayerPawn->m_fFlags();
                int buttons = pPlayerPawn->m_pMovementServices()->m_nButtons().m_pButtonStates()[0];

                if ((buttons & (1 << 1)) && (flags & FL_ONGROUND) && pPlayerPawn->m_MoveType() != MOVETYPE_LADDER) {
                    float maxSpeed = g_pVIPCore->VIP_GetClientFeatureFloat(i, "bhop_max_speed");

                    if (maxSpeed > 0.0f && std::round(pPlayerPawn->m_vecAbsVelocity().Length2D()) > maxSpeed)
                    {
                        ChangeVelocity(pPlayerPawn, maxSpeed);
                    }
                    
                    pPlayerPawn->m_vecAbsVelocity().z = g_pVIPCore->VIP_GetClientFeatureFloat(i, "bhop_jump_power");
                    user.JumpsCount++;

                    if (user.pResetTimer && g_pUtils) {
                        g_pUtils->RemoveTimer(user.pResetTimer);
                        user.pResetTimer = nullptr;
                    }

                    if (user.JumpsCount >= g_pVIPCore->VIP_GetClientFeatureInt(i, "bhop_max_jumps")) {
                        StartBhopCooldown(i);
                    }
                } else if (user.JumpsCount > 0 && !user.bOnCooldown && !user.pResetTimer && g_pUtils) {
                    user.pResetTimer = g_pUtils->CreateTimer(g_pVIPCore->VIP_GetClientFeatureFloat(i, "bhop_reset_time"), [i]() -> float {
                        ResetPlayerJumps(i);
                        return -1.0f;
                    });
                }
            }
        }
        return 0.0f;
    });
}

const char* vip_bhop::GetLicense() {
    return "Public";
}
const char* vip_bhop::GetVersion() {
    return "1.1.0";
}
const char* vip_bhop::GetDate() {
    return __DATE__;
}
const char* vip_bhop::GetLogTag() {
    return "[VIP-BHOP-ENHANCED]";
}
const char* vip_bhop::GetAuthor() {
    return "E!N with NovaHost";
}
const char* vip_bhop::GetDescription() {
    return "";
}
const char* vip_bhop::GetName() {
    return "[VIP] BHOP (Enhanced)";
}
const char* vip_bhop::GetURL() {
    return "https://nova-hosting.ru?ref=ein";
}
