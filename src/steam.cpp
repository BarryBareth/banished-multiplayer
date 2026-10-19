// Steam lobby + P2P through the game's steam_api64.dll (see steam.h).
#include "steam.h"
#include "common.h"
#include <softpub.h>
#include <wintrust.h>
#pragma comment(lib, "wintrust.lib")
#include <deque>
#include <map>

namespace steam {

// CSteamID has constructors in the SDK, so MSVC returns it from member functions through a hidden pointer and
// passes it by value in a register; this class reproduces both properties.
struct SteamID {
    uint64_t v;
    SteamID() : v(0) {}
    SteamID(uint64_t x) : v(x) {}
};

#define SLOT(n) virtual void _##n() = 0;

struct ISteamClient {
    SLOT(0) SLOT(1) SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6) SLOT(7) SLOT(8) SLOT(9) SLOT(10) SLOT(11)
    virtual void* GetISteamGenericInterface(int user, int pipe, const char* version) = 0;  // 12
};
struct ISteamUser {  // SteamUser017
    SLOT(0) SLOT(1)
    virtual SteamID GetSteamID() = 0;  // 2
};
struct ISteamMatchmaking {  // SteamMatchMaking009
    SLOT(0) SLOT(1) SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6) SLOT(7) SLOT(8) SLOT(9) SLOT(10) SLOT(11) SLOT(12)
    virtual uint64_t CreateLobby(int type, int maxMembers) = 0;                 // 13
    virtual uint64_t JoinLobby(SteamID lobby) = 0;                              // 14
    virtual void LeaveLobby(SteamID lobby) = 0;                                 // 15
    virtual bool InviteUserToLobby(SteamID lobby, SteamID invitee) = 0;         // 16
    virtual int GetNumLobbyMembers(SteamID lobby) = 0;                          // 17
    virtual SteamID GetLobbyMemberByIndex(SteamID lobby, int i) = 0;            // 18
    virtual const char* GetLobbyData(SteamID lobby, const char* key) = 0;       // 19
    virtual bool SetLobbyData(SteamID lobby, const char* key, const char* v) = 0;  // 20
    SLOT(21) SLOT(22) SLOT(23) SLOT(24) SLOT(25) SLOT(26) SLOT(27) SLOT(28) SLOT(29) SLOT(30) SLOT(31) SLOT(32)
    SLOT(33)
    virtual bool SetLobbyJoinable(SteamID lobby, bool joinable) = 0;            // 34
    virtual SteamID GetLobbyOwner(SteamID lobby) = 0;                           // 35
};
struct ISteamNetworking {  // SteamNetworking006
    virtual bool SendP2PPacket(SteamID to, const void* data, uint32_t size, int sendType, int channel) = 0;  // 0
    virtual bool IsP2PPacketAvailable(uint32_t* size, int channel) = 0;                                       // 1
    virtual bool ReadP2PPacket(void* dst, uint32_t cap, uint32_t* size, SteamID* from, int channel) = 0;     // 2
    virtual bool AcceptP2PSessionWithUser(SteamID remote) = 0;                                                // 3
    virtual bool CloseP2PSessionWithUser(SteamID remote) = 0;                                                 // 4
};
// Newer Steam networking (served by the running Steam client, not the game's 2017 steam_api64.dll). Layouts from
// Valve's GameNetworkingSockets headers (include/steam/isteamnetworkingmessages.h, isteamnetworkingutils.h,
// steamnetworkingtypes.h). Used instead of SteamNetworking006 because that one connects peers DIRECTLY when it can
// (each player sees the others' IP); with ICE off, SteamNetworkingMessages goes through Valve's relays only.
#pragma pack(push, 1)
struct NetIdentity {  // SteamNetworkingIdentity: type 16 = SteamID, 8-byte size, then the id; 136 bytes
    int type = 16;
    int size = 8;
    uint64_t id = 0;
    uint8_t rest[120] = {};
};
#pragma pack(pop)
static_assert(sizeof(NetIdentity) == 136, "SteamNetworkingIdentity is 136 bytes");
struct NetMessage {  // SteamNetworkingMessage_t (pack 8)
    void* data;
    int size;
    uint32_t conn;
    NetIdentity peer;  // +16
    int64_t connUserData, usecReceived, messageNumber;
    void (*freeData)(NetMessage*);
    void (*release)(NetMessage*);  // +184
    int channel, flags;
};
static_assert(offsetof(NetMessage, release) == 184, "SteamNetworkingMessage_t layout");
struct ISteamNetworkingMessages {  // SteamNetworkingMessages002
    virtual int SendMessageToUser(const NetIdentity& to, const void* data, uint32_t size, int flags, int channel) = 0;  // 0
    virtual int ReceiveMessagesOnChannel(int channel, NetMessage** out, int max) = 0;                                 // 1
    virtual bool AcceptSessionWithUser(const NetIdentity& remote) = 0;                                               // 2
    virtual bool CloseSessionWithUser(const NetIdentity& remote) = 0;                                                // 3
};
struct ISteamNetworkingUtils {  // SteamNetworkingUtils004 (InitRelayNetworkAccess is inline = CheckPingDataUpToDate(1e10))
    SLOT(0)
    virtual int GetRelayNetworkStatus(void* details) = 0;  // 1: 100 = available
    SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6)
    virtual bool CheckPingDataUpToDate(float maxAgeSeconds) = 0;  // 7
    SLOT(8) SLOT(9) SLOT(10) SLOT(11) SLOT(12) SLOT(13) SLOT(14) SLOT(15)
    virtual bool SetConfigValue(int value, int scope, intptr_t scopeObj, int dataType, const void* arg) = 0;  // 16
};
static const int kSendReliable = 8, kSendAutoRestart = 32;  // k_nSteamNetworkingSend_*
static const int kCfgIceEnable = 104, kCfgGlobal = 1, kCfgInt32 = 1;  // P2P_Transport_ICE_Enable, global scope, int32
static const int kCfgSendBuffer = 9, kCfgRecvBuffer = 47;  // bytes queued per session (defaults 512 KB / 1 MB)
static const int kResultLimitExceeded = 25;

struct ISteamFriends {  // SteamFriends017
    virtual const char* GetPersonaName() = 0;  // 0
    SLOT(1) SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6) SLOT(7) SLOT(8) SLOT(9) SLOT(10) SLOT(11) SLOT(12) SLOT(13)
    SLOT(14) SLOT(15) SLOT(16) SLOT(17) SLOT(18) SLOT(19) SLOT(20) SLOT(21) SLOT(22) SLOT(23) SLOT(24) SLOT(25)
    SLOT(26) SLOT(27) SLOT(28) SLOT(29) SLOT(30) SLOT(31) SLOT(32)
    virtual void ActivateGameOverlayInviteDialog(SteamID lobby) = 0;  // 33
    SLOT(34) SLOT(35) SLOT(36) SLOT(37) SLOT(38) SLOT(39) SLOT(40) SLOT(41) SLOT(42)
    virtual bool SetRichPresence(const char* key, const char* value) = 0;               // 43
    virtual void ClearRichPresence() = 0;                                               // 44
    virtual const char* GetFriendRichPresence(SteamID user, const char* key) = 0;      // 45
};

// Same declaration as the SDK's CCallbackBase so MSVC produces the same vtable/field layout.
class CCallbackBase {
public:
    CCallbackBase() { m_nCallbackFlags = 0; m_iCallback = 0; }
    virtual void Run(void* pvParam) = 0;
    virtual void Run(void* pvParam, bool bIOFailure, uint64_t hSteamAPICall) = 0;
    virtual int GetCallbackSizeBytes() = 0;
    uint8_t m_nCallbackFlags;
    int m_iCallback;
};

// Callback payloads (pack 8 on Windows; CSteamID = uint64)
struct LobbyEnter_t { uint64_t lobby; uint32_t permissions; bool locked; uint32_t response; };   // 504
struct LobbyChatUpdate_t { uint64_t lobby, changed, by; uint32_t state; };                        // 506
struct LobbyCreated_t { int result; uint64_t lobby; };                                          // 513
struct GameLobbyJoinRequested_t { uint64_t lobby, friendId; };                                   // 333
struct P2PSessionRequest_t { uint64_t remote; };                                                // 1202
struct GameRichPresenceJoinRequested_t { uint64_t friendId; char connect[256]; };                 // 337
struct P2PSessionConnectFail_t { uint64_t remote; uint8_t error; };                              // 1203
#pragma pack(push, 1)
struct MsgSessionRequest_t { NetIdentity remote; };  // 1251 (k_iSteamNetworkingMessagesCallbacks + 1)
struct MsgSessionFailed_t { NetIdentity remote; uint8_t rest[560]; };  // 1252: SteamNetConnectionInfo_t starts with the identity
#pragma pack(pop)

using RegisterCallback_t = void(__cdecl*)(CCallbackBase*, int);
using GetHandle_t = int(__cdecl*)();
using SteamClient_t = ISteamClient*(__cdecl*)();
using RunCallbacks_t = void(__cdecl*)();

static ISteamUser* g_user;
static ISteamMatchmaking* g_mm;
static ISteamNetworking* g_net;
static ISteamFriends* g_friends;
static ISteamNetworkingMessages* g_msgs;  // set = relay-only transport in use
static ISteamNetworkingUtils* g_utils;
static NetIdentity Ident(uint64_t id) { NetIdentity n; n.id = id; return n; }
// Messages Steam refused with LimitExceeded (send buffer full - a 6.7 MB save in 60 KB lines overflowed the default
// 512 KB and arrived as 360 KB: "save transfer corrupt", 2026-10-15): kept per peer in order and retried every
// frame. While a peer has a queue, new messages go behind it.
static std::map<uint64_t, std::deque<std::string>> g_sendQueue;
static void FlushQueues() {
    for (auto it = g_sendQueue.begin(); it != g_sendQueue.end();) {
        auto& q = it->second;
        while (!q.empty()) {
            int r = g_msgs->SendMessageToUser(Ident(it->first), q.front().data(), (uint32_t)q.front().size(), kSendReliable | kSendAutoRestart, 0);
            if (r == kResultLimitExceeded) break;
            if (r != 1) Log("!! steam: queued relay send of %zu bytes to %llu failed (EResult %d)", q.front().size(), it->first, r);
            q.pop_front();
        }
        it = q.empty() ? g_sendQueue.erase(it) : std::next(it);
    }
}
static uint64_t g_lobby;
static std::function<void(Event, uint64_t)> g_handler;
static bool g_tried, g_ok;

static void Fire(Event e, uint64_t a) { if (g_handler) g_handler(e, a); }

template <class T, int Id>
class Callback : public CCallbackBase {
public:
    explicit Callback(void (*fn)(const T&)) : m_fn(fn) {}
    void Run(void* p) override { m_fn(*(const T*)p); }
    void Run(void* p, bool, uint64_t) override { m_fn(*(const T*)p); }
    int GetCallbackSizeBytes() override { return sizeof(T); }
    static constexpr int kId = Id;
private:
    void (*m_fn)(const T&);
};

static void OnLobbyCreated(const LobbyCreated_t& c) {
    Log("steam: LobbyCreated result=%d lobby=%llu", c.result, c.lobby);
    if (c.result != 1) Fire(kLobbyFailed, 0);
    else { g_lobby = c.lobby; Fire(kLobbyCreated, c.lobby); }
}
static void OnLobbyEnter(const LobbyEnter_t& c) {
    Log("steam: LobbyEnter lobby=%llu response=%u", c.lobby, c.response);
    if (c.response != 1) { Fire(kLobbyFailed, c.lobby); return; }  // k_EChatRoomEnterResponseSuccess
    g_lobby = c.lobby;
    Fire(kLobbyEntered, c.lobby);
}
static void OnLobbyChatUpdate(const LobbyChatUpdate_t& c) {
    Log("steam: LobbyChatUpdate lobby=%llu user=%llu state=0x%x", c.lobby, c.changed, c.state);
    if (c.lobby == g_lobby) Fire(kMemberChange, c.changed);
}
static void OnJoinRequested(const GameLobbyJoinRequested_t& c) {
    Log("steam: GameLobbyJoinRequested lobby=%llu friend=%llu", c.lobby, c.friendId);
    Fire(kJoinRequested, c.lobby);
}
static void OnP2PRequest(const P2PSessionRequest_t& c) {
    Log("steam: P2PSessionRequest from %llu", c.remote);
    Fire(kP2PRequest, c.remote);
}
static void OnP2PFail(const P2PSessionConnectFail_t& c) {
    Log("steam: P2PSessionConnectFail %llu error=%u", c.remote, c.error);
    Fire(kP2PFailed, c.remote);
}

static Callback<LobbyCreated_t, 513> cbCreated(OnLobbyCreated);
static Callback<LobbyEnter_t, 504> cbEnter(OnLobbyEnter);
static Callback<LobbyChatUpdate_t, 506> cbChat(OnLobbyChatUpdate);
static Callback<GameLobbyJoinRequested_t, 333> cbJoin(OnJoinRequested);
static Callback<P2PSessionRequest_t, 1202> cbP2P(OnP2PRequest);
static Callback<P2PSessionConnectFail_t, 1203> cbP2PFail(OnP2PFail);
static void OnMsgRequest(const MsgSessionRequest_t& c) {
    Log("steam: relay session request from %llu", c.remote.id);
    Fire(kP2PRequest, c.remote.id);
}
static void OnMsgFailed(const MsgSessionFailed_t& c) {
    Log("steam: relay session with %llu failed", c.remote.id);
    Fire(kP2PFailed, c.remote.id);
}
static Callback<MsgSessionRequest_t, 1251> cbMsgReq(OnMsgRequest);
// "Join Game" in a friend's right-click menu while the game runs (not running: Steam starts it with the connect string
// on the command line, handled by net's "+connect_lobby" check)
static void OnPresenceJoin(const GameRichPresenceJoinRequested_t& c) {
    Log("steam: join via the friends list: '%s'", c.connect);
    if (const char* s = strstr(c.connect, "+connect_lobby")) Fire(kJoinRequested, _strtoui64(s + 14, nullptr, 10));
}
static Callback<GameRichPresenceJoinRequested_t, 337> cbPresenceJoin(OnPresenceJoin);
static Callback<MsgSessionFailed_t, 1252> cbMsgFail(OnMsgFailed);

// The game starts Steam itself before its main loop runs. Asking the API earlier, or from another thread, can wait forever
// inside a Steam emulator (a pirated copy hung on its loading screen, 2026-10-19 - the game's own Steam start then
// waited for ours). Until the main loop has run once (SetReady, main thread) Init just answers "not yet".
static DWORD g_readyThread;
void SetReady() { if (!g_readyThread) g_readyThread = GetCurrentThreadId(); }

// Valve signs steam_api64.dll; Steam emulators (cracked copies) replace it with an unsigned one that has no lobbies or
// networking and can block forever when asked (a pirated copy froze before its first frame, 2026-10-19).
static bool SignedSteamApi(HMODULE api) {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(api, path, MAX_PATH)) return false;
    WINTRUST_FILE_INFO file{sizeof file};
    file.pcwszFilePath = path;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd{sizeof wd};
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &file;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    LONG r = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    return r == ERROR_SUCCESS;
}

bool Init() {
    if (g_tried) return g_ok;
    if (!g_readyThread || GetCurrentThreadId() != g_readyThread) return false;  // too early / not the main thread
    g_tried = true;
    HMODULE api = GetModuleHandleW(L"steam_api64.dll");
    if (!api) { Log("steam: steam_api64.dll not loaded"); return false; }
    if (!SignedSteamApi(api)) { Log("steam: steam_api64.dll isn't Valve's (a Steam emulator?) - Steam features off"); return false; }
    auto client = (SteamClient_t)GetProcAddress(api, "SteamClient");
    auto getUser = (GetHandle_t)GetProcAddress(api, "SteamAPI_GetHSteamUser");
    auto getPipe = (GetHandle_t)GetProcAddress(api, "SteamAPI_GetHSteamPipe");
    auto reg = (RegisterCallback_t)GetProcAddress(api, "SteamAPI_RegisterCallback");
    if (!client || !getUser || !getPipe || !reg) { Log("steam: missing exports"); return false; }
    ISteamClient* c = client();
    int user = getUser(), pipe = getPipe();
    if (!c || !user || !pipe) { Log("steam: not initialized (client=%p user=%d pipe=%d)", c, user, pipe); return false; }
    g_user = (ISteamUser*)c->GetISteamGenericInterface(user, pipe, "SteamUser017");
    g_mm = (ISteamMatchmaking*)c->GetISteamGenericInterface(user, pipe, "SteamMatchMaking009");
    g_net = (ISteamNetworking*)c->GetISteamGenericInterface(user, pipe, "SteamNetworking006");
    g_friends = (ISteamFriends*)c->GetISteamGenericInterface(user, pipe, "SteamFriends017");
    Log("steam: user=%p matchmaking=%p networking=%p friends=%p", g_user, g_mm, g_net, g_friends);
    if (!g_user || !g_mm || !g_net || !g_friends) return false;
    // Relay-only transport: no ICE = no direct connection, no IP addresses shared with peers. Set before any session.
    // <game>\bmp_oldp2p.txt = the old direct-capable SteamNetworking006 (both sides must use the same one).
    wchar_t sw[MAX_PATH];
    GetModuleFileNameW(nullptr, sw, MAX_PATH);
    wcscpy_s(wcsrchr(sw, L'\\') + 1, 32, L"bmp_oldp2p.txt");
    bool oldP2P = GetFileAttributesW(sw) != INVALID_FILE_ATTRIBUTES;
    auto msgs = (ISteamNetworkingMessages*)c->GetISteamGenericInterface(user, pipe, "SteamNetworkingMessages002");
    g_utils = (ISteamNetworkingUtils*)c->GetISteamGenericInterface(user, pipe, "SteamNetworkingUtils004");
    if (msgs && g_utils && !oldP2P) {
        int off = 0;  // k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Disable
        bool set = g_utils->SetConfigValue(kCfgIceEnable, kCfgGlobal, 0, kCfgInt32, &off);
        int big = 32 << 20;  // a whole save fits in the buffers (the queue above covers anything bigger)
        bool bufs = g_utils->SetConfigValue(kCfgSendBuffer, kCfgGlobal, 0, kCfgInt32, &big) &&
                    g_utils->SetConfigValue(kCfgRecvBuffer, kCfgGlobal, 0, kCfgInt32, &big);
        Log("steam: relay buffers 32 MB: %s", bufs ? "ok" : "FAILED (sends queue instead)");
        g_utils->CheckPingDataUpToDate(1e10f);  // InitRelayNetworkAccess
        if (set) g_msgs = msgs;
        Log("steam: networking = Valve relays only (IPs hidden): ICE off %s, relay network status %d",
            set ? "ok" : "FAILED - using the old P2P", g_utils->GetRelayNetworkStatus(nullptr));
    } else {
        Log("steam: networking = old P2P (SteamNetworking006, may connect directly)%s", oldP2P ? " - bmp_oldp2p.txt" : "");
    }
    reg(&cbCreated, cbCreated.kId);
    reg(&cbEnter, cbEnter.kId);
    reg(&cbChat, cbChat.kId);
    reg(&cbJoin, cbJoin.kId);
    reg(&cbP2P, cbP2P.kId);
    reg(&cbP2PFail, cbP2PFail.kId);
    reg(&cbMsgReq, cbMsgReq.kId);
    reg(&cbPresenceJoin, cbPresenceJoin.kId);
    reg(&cbMsgFail, cbMsgFail.kId);
    g_ok = true;
    Log("steam: ready, I am %llu (%s)", MySteamId(), PersonaName().c_str());
    return true;
}

void RunCallbacks() {
    static RunCallbacks_t run = (RunCallbacks_t)GetProcAddress(GetModuleHandleW(L"steam_api64.dll"), "SteamAPI_RunCallbacks");
    if (g_ok && run) run();
    if (g_msgs && !g_sendQueue.empty()) FlushQueues();
    // Test (<game>\bmp_steamselftest.txt): one message to myself through the relay transport; logs the relay network
    // status and whether it came back (one Steam account can't play itself, but this exercises the interfaces).
    static int test = -1;
    static ULONGLONG start, sentAt;
    if (test < 0 && g_ok) {
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(nullptr, p, MAX_PATH);
        wcscpy_s(wcsrchr(p, L'\\') + 1, 32, L"bmp_steamselftest.txt");
        test = g_ok && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
        start = GetTickCount64();
        if (test) Log("steam selftest: transport %s", g_msgs ? "relay-only" : "old P2P");
    }
    if (test != 1 && test != 3) return;
    ULONGLONG now = GetTickCount64();
    static int lastStatus = -999;
    int st = RelayStatus();
    if (st != lastStatus) { Log("steam selftest: relay network status %d after %llu ms", st, now - start); lastStatus = st; }
    if (!sentAt && (st == 100 || now - start > 15000)) {
        const char msg[] = "bmp-selftest";
        sentAt = now;
        Log("steam selftest: send to myself -> %s", Send(MySteamId(), msg, sizeof msg - 1) ? "accepted" : "refused");
    }
    std::string got;
    uint64_t from = 0;
    static int burstGot, burstBad;
    static ULONGLONG burstAt;
    if (test == 1 && sentAt && Receive(got, from)) {
        Log("steam selftest: PASS - got '%s' from %llu after %llu ms", got.c_str(), from, now - sentAt);
        std::string chunk(60000, 'x');
        for (int i = 0; i < 140; i++) {  // 8.4 MB, numbered
            memcpy(chunk.data(), &i, sizeof i);
            Send(MySteamId(), chunk.data(), (uint32_t)chunk.size());
        }
        burstAt = now;
        test = 3;
        Log("steam selftest: burst of 140 x 60000 bytes sent (%zu queued)", g_sendQueue.empty() ? 0 : g_sendQueue.begin()->second.size());
    } else if (test == 1 && sentAt && now - sentAt > 20000) {
        Log("steam selftest: nothing came back in 20 s (relay status %d)", st);
        test = 2;
    }
    while (test == 3 && Receive(got, from)) {
        int i = -1;
        if (got.size() == 60000) memcpy(&i, got.data(), sizeof i);
        if (i != burstGot) burstBad++;
        burstGot++;
    }
    if (test == 3 && (burstGot >= 140 || now - burstAt > 60000)) {
        Log("steam selftest: burst %s - %d of 140 arrived, %d out of order, %llu ms", burstGot == 140 && !burstBad ? "PASS" : "FAIL",
            burstGot, burstBad, now - burstAt);
        test = 2;
    }
}

uint64_t MySteamId() { return g_user ? g_user->GetSteamID().v : 0; }
std::string PersonaName() { const char* n = g_friends ? g_friends->GetPersonaName() : nullptr; return n ? n : "?"; }

void CreateLobby(int maxMembers) { if (g_mm) g_mm->CreateLobby(1 /* k_ELobbyTypeFriendsOnly */, maxMembers); }
void JoinLobby(uint64_t lobby) { if (g_mm) g_mm->JoinLobby(lobby); }
void LeaveLobby() {
    if (g_mm && g_lobby) g_mm->LeaveLobby(g_lobby);
    g_lobby = 0;
}
void OpenInviteOverlay() { if (g_friends && g_lobby) g_friends->ActivateGameOverlayInviteDialog(g_lobby); }
uint64_t Lobby() { return g_lobby; }
uint64_t LobbyOwner() { return g_mm && g_lobby ? g_mm->GetLobbyOwner(g_lobby).v : 0; }
int LobbyMembers() { return g_mm && g_lobby ? g_mm->GetNumLobbyMembers(g_lobby) : 0; }
void SetLobbyData(const char* key, const char* value) { if (g_mm && g_lobby) g_mm->SetLobbyData(g_lobby, key, value); }
void SetEventHandler(std::function<void(Event, uint64_t)> fn) { g_handler = std::move(fn); }

bool Send(uint64_t to, const void* data, uint32_t size) {
    if (g_msgs) {
        auto q = g_sendQueue.find(to);
        if (q != g_sendQueue.end()) { q->second.emplace_back((const char*)data, size); return true; }  // keep the order
        int r = g_msgs->SendMessageToUser(Ident(to), data, size, kSendReliable | kSendAutoRestart, 0);
        if (r == kResultLimitExceeded) {
            g_sendQueue[to].emplace_back((const char*)data, size);
            Log("steam: send buffer to %llu full - queueing", to);
            return true;
        }
        if (r != 1) Log("!! steam: relay send of %u bytes to %llu failed (EResult %d)", size, to, r);
        return r == 1;  // k_EResultOK
    }
    return g_net && g_net->SendP2PPacket(to, data, size, 2 /* k_EP2PSendReliable */, 0);
}
bool Receive(std::string& out, uint64_t& from) {
    if (g_msgs) {
        NetMessage* m = nullptr;
        if (g_msgs->ReceiveMessagesOnChannel(0, &m, 1) != 1 || !m) return false;
        out.assign((const char*)m->data, (size_t)m->size);
        from = m->peer.id;
        m->release(m);
        return true;
    }
    uint32_t size = 0;
    if (!g_net || !g_net->IsP2PPacketAvailable(&size, 0)) return false;
    out.resize(size);
    SteamID id;
    if (!g_net->ReadP2PPacket(out.data(), size, &size, &id, 0)) return false;
    out.resize(size);
    from = id.v;
    return true;
}
void Accept(uint64_t remote) {
    if (g_msgs) g_msgs->AcceptSessionWithUser(Ident(remote));
    else if (g_net) g_net->AcceptP2PSessionWithUser(remote);
}
void Close(uint64_t remote) {
    if (g_msgs) g_msgs->CloseSessionWithUser(Ident(remote));
    else if (g_net) g_net->CloseP2PSessionWithUser(remote);
}
bool RelayOnly() { return g_msgs != nullptr; }

// Rich presence. The friends list's own line ("Playing Banished") can only show text from localization tokens the
// game's developer uploads (steam_display) - not possible for a mod. What a mod can set: "status" (the friends list's
// "View game info" dialog), steam_player_group / _size (Steam groups the players of one game together) and "connect"
// ("Join Game" in the friends list -> GameRichPresenceJoinRequested / the command line).
void SetPresence(const std::string& status, const std::string& group, int groupSize, const std::string& connect) {
    if (!g_friends) return;
    static std::string last;
    std::string key = status + "|" + group + "|" + std::to_string(groupSize) + "|" + connect;
    if (key == last) return;
    last = key;
    if (status.empty()) {
        g_friends->ClearRichPresence();
        Log("steam: rich presence cleared");
        return;
    }
    bool ok = g_friends->SetRichPresence("status", status.c_str());
    g_friends->SetRichPresence("steam_player_group", group.empty() ? nullptr : group.c_str());
    g_friends->SetRichPresence("steam_player_group_size", group.empty() ? nullptr : std::to_string(groupSize).c_str());
    g_friends->SetRichPresence("connect", connect.empty() ? nullptr : connect.c_str());
    const char* back = g_friends->GetFriendRichPresence(MySteamId(), "status");
    Log("steam: rich presence '%s' group '%s' x%d connect '%s' -> %s (reads back '%s')", status.c_str(), group.c_str(),
        groupSize, connect.c_str(), ok ? "set" : "REFUSED", back ? back : "");
}
int RelayStatus() { return g_utils ? g_utils->GetRelayNetworkStatus(nullptr) : 0; }

}  // namespace steam
