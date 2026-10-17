// Automated determinism test driver: loads a save from the main menu through the engine's own
// Save::SaveLoad state machine, drives "player" input by writing into Input::Device's keyboard/mouse
// State after the real hardware is read, and quits after N sim ticks.
#include "autotest.h"
#include "gameload.h"
#include "commands.h"
#include "uiscript.h"
#include "net.h"
#include "common.h"
#include "MinHook.h"
#include "overlay.h"
#include "towns.h"
#include <random>
#include <string>
#include <cctype>

namespace autotest {

// ---- engine exports ----
using AppGet_t = uint8_t*(__fastcall*)();
using GetState_t = bool(__fastcall*)(void* device, uint8_t* state);

static AppGet_t p_AppGet;
static GetState_t o_GetKeyboardState, o_GetMouseState;

// ---- config ----
static bool g_active;
static std::string g_save = "dettest";
static bool g_chaos;
static uint64_t g_ticks = 20000;
static int g_speed = 5;
static uint32_t g_seed = 1;
static bool g_speedChaos = true;    // chaos also randomizes game speed
static uint32_t g_pauseEvery = 1500; // networked chaos: ~1 pause tap per this many frames
static std::string g_newGame;          // "seed,size,start,terrain,climate": start a new map instead of loading
static std::string g_warmup;          // play this save first (warmupticks ticks), then load `save` in-process
static uint64_t g_warmupTicks = 3000;
static bool g_warmupDone;
// networked tests: play this save single-player first (like a player who played before hosting/joining); when the
// session STARTs, the net code loads the shared game from inside it
static std::string g_prewarm;
static bool g_prewarming;
static uint64_t g_prewarmTicks = 600;
static uint64_t g_prewarmSessions;

// ---- input injection ----
// Engine key codes = low byte of the KeyMap ids stored in the registry (device 1 = keyboard).
enum Key : int {
    kCancel = 0, kToolbar1 = 1, kTool0 = 13, kLeft = 23, kRight = 26, kRotRight = 27, kStructType = 28,
    kRotLeft = 39, kStructLeft = 40, kBack = 41, kStructRight = 42, kForward = 45, kZoomIn = 77,
    kPitchUp = 79, kZoomOut = 80, kPitchDown = 82, kTogglePause = 71,
};
static int g_heldKey = -1, g_heldFrames;
static int g_tapKey = -1;
static int g_mouseX = -1, g_mouseY = -1;

static bool __fastcall h_GetKeyboardState(void* device, uint8_t* state) {
    bool r = o_GetKeyboardState(device, state);
    if (g_active) {
        if (g_heldKey >= 0) *(uint16_t*)(state + g_heldKey * 0x18 + 4) = 0x7fff;
        if (g_tapKey >= 0) { *(uint16_t*)(state + g_tapKey * 0x18 + 4) = 0x7fff; g_tapKey = -1; }
    }
    return r;
}
static int g_smX = -1, g_smY;  // ScriptMouse (uiscript "mouse")
static bool g_smDown;
void ScriptMouse(int x, int y, bool leftDown) { g_smX = x, g_smY = y, g_smDown = leftDown; }
static bool __fastcall h_GetMouseState(void* device, uint8_t* state) {
    bool r = o_GetMouseState(device, state);
    if (g_smX >= 0) {
        *(int16_t*)(state + 4) = (int16_t)g_smX;
        *(int16_t*)(state + 0x1c) = (int16_t)g_smY;
        *(int16_t*)(state + 0x34) = 0;  // entries 2/3: movement deltas of the real mouse (a title bar took them as a drag)
        *(int16_t*)(state + 0x4c) = 0;
        for (int b = 4; b <= 11; b++) *(uint16_t*)(state + b * 0x18 + 4) = 0;
        if (g_smDown) *(uint16_t*)(state + 4 * 0x18 + 4) = 0x7fff;  // entry 4 = left button
        return r;
    }
    if (g_active && g_chaos && g_mouseX >= 0) {
        *(int16_t*)(state + 4) = (int16_t)g_mouseX;
        *(int16_t*)(state + 0x1c) = (int16_t)g_mouseY;
        for (int b = 4; b <= 11; b++) *(uint16_t*)(state + b * 0x18 + 4) = 0;  // never click
    }
    overlay::FilterMouse(state);  // clicks on the multiplayer UI don't reach the game
    return r;
}

// ---- state machine ----
enum Phase { kWaitMenu, kLoading, kInGame, kDone };
static Phase g_phase = kWaitMenu;
static int g_frames, g_phaseFrames;
static uint64_t g_lastTick, g_lastTickFrame;
static std::mt19937 g_rng;

static void SetPhase(Phase p) { g_phase = p; g_phaseFrames = 0; Log("autotest: phase %d", p); }

static void Quit(const char* why) {
    Log("autotest: quitting (%s) at tick %llu", why, ProbeTick());
    ProbeFlush();
    TerminateProcess(GetCurrentProcess(), 0);
}

static void ChaosStep() {
    // keep the window "active" so the engine never idles
    if (p_AppGet) if (uint8_t* app = p_AppGet()) app[0x11] = 1;
    if (g_heldKey >= 0 && --g_heldFrames <= 0) g_heldKey = -1;
    if (!g_chaos) return;
    if (g_frames % 7 == 0) {  // wander the cursor (moves placement previews / hover highlights)
        std::uniform_int_distribution<int> x(50, 2500), y(50, 1300);
        g_mouseX = x(g_rng);
        g_mouseY = y(g_rng);
    }
    if (net::Active()) {  // shared pause: chaos pauses now and then and unpauses after ~1-2 s
        uint8_t* loop = (uint8_t*)ProbeGameLoop();
        uint8_t* speedCtl = loop ? *(uint8_t**)(loop + 0x120) : nullptr;
        if (speedCtl && g_tapKey < 0 && g_rng() % (speedCtl[0x30] ? 90 : g_pauseEvery) == 0) {
            Log("autotest: chaos taps TogglePause (paused=%d)", speedCtl[0x30]);
            g_tapKey = kTogglePause;
            return;
        }
    }
    if (g_heldKey >= 0 || (g_rng() % 30) != 0) return;
    static const int camKeys[] = {kForward, kBack, kLeft, kRight, kRotLeft, kRotRight, kZoomIn, kZoomOut, kPitchUp, kPitchDown};
    uint32_t r = g_rng() % 100;
    if (r < 55) {
        g_heldKey = camKeys[g_rng() % 10];
        g_heldFrames = 20 + g_rng() % 100;
    } else if (r < 75) {
        g_tapKey = kToolbar1 + g_rng() % 9;           // open a build/toolbar menu
    } else if (r < 85) {
        g_tapKey = kTool0 + g_rng() % 10;             // pick a tool (placement preview follows cursor)
    } else if (r < 90) {
        g_tapKey = (g_rng() & 1) ? kStructLeft : kStructRight;
    } else {
        g_tapKey = kCancel;
    }
    if (uint8_t* loop = (uint8_t*)ProbeGameLoop()) {  // random game speed
        if (g_speedChaos && g_rng() % 4 == 0) {
            static const int speeds[] = {1, 2, 5, 10};
            uint8_t* speedCtl = *(uint8_t**)(loop + 0x120);
            if (speedCtl) *(int*)(speedCtl + 0x34) = speeds[g_rng() % 4];
        }
    }
}

void Frame() {
    if (!g_active) return;
    g_frames++;
    g_phaseFrames++;
    switch (g_phase) {
    case kWaitMenu:
        if (!g_prewarm.empty() && !net::Started() && gameload::InMenu() && g_phaseFrames > 120) {
            if (gameload::Request(g_prewarm)) {
                Log("autotest: prewarm: playing %s single-player until the session starts", g_prewarm.c_str());
                g_prewarm.clear();
                g_prewarming = true;
                SetPhase(kLoading);
            }
            break;
        }
        if (net::Started() && gameload::Current() != gameload::kIdle) { SetPhase(kLoading); break; }  // net is loading
        if (gameload::InMenu() && g_phaseFrames > 120 && (!net::Active() || net::Started())) {
            // a multiplayer host may have sent its own save; otherwise load the configured one
            std::string save = net::SaveName().empty() ? g_save : net::SaveName();
            if (!g_warmup.empty()) save = g_warmup;
            if (!g_newGame.empty()) {
                unsigned seed = 1;
                int size = 0, start = 0, terrain = 0, climate = 0;
                sscanf_s(g_newGame.c_str(), "%u,%d,%d,%d,%d", &seed, &size, &start, &terrain, &climate);
                if (gameload::RequestNewGame(seed, size, start, terrain, climate, false)) SetPhase(kLoading);
            } else if (gameload::Request(save)) SetPhase(kLoading);
        }
        break;
    case kLoading:
        switch (gameload::Current()) {
        case gameload::kDone:
            if (!g_warmup.empty() && g_warmupDone) { g_warmup.clear(); Log("autotest: warm-up over, test session loaded"); }
            g_rng.seed(g_seed);
            g_lastTickFrame = g_frames;
            SetPhase(kInGame);
            break;
        case gameload::kFailed:
            Quit(gameload::Error());
            return;
        default:
            break;
        }
        break;
    case kInGame: {
        uint8_t* loop = (uint8_t*)ProbeGameLoop();
        if (loop) {
            uint8_t* speedCtl = *(uint8_t**)(loop + 0x120);
            if (speedCtl) {
                if (!net::Active()) *(uint8_t*)(speedCtl + 0x30) = 0;  // never paused (also defeats auto-pause on events)
                // networked: speed is shared, set it once (a write every frame would be a "change" every frame)
                if ((!g_chaos || !g_speedChaos) && (!net::Active() || g_phaseFrames == 1)) *(int*)(speedCtl + 0x34) = g_speed;
            }
        }
        if (g_warmupDone && !g_warmup.empty()) {  // warm-up finished: load the real test save from in-game
            if (gameload::Request(g_save)) SetPhase(kLoading);
            break;
        }
        if (g_prewarming) {  // single-player prewarm: idle until the net session's game is running
            static bool asked;
            if (!g_prewarmSessions) g_prewarmSessions = ProbeSessionCount();
            // like a player: after a while (prewarmticks, or 20 s if a dialog holds the game) quit to the main menu and
            // host/join from there (prewarmticks=0: stay in-game, the session's save is then loaded from inside it)
            if (g_prewarmTicks && !net::Started() && (ProbeTick() >= g_prewarmTicks || g_phaseFrames > 1200) &&
                gameload::RequestMainMenu()) {
                Log("autotest: prewarm over at tick %llu, quit to the main menu", ProbeTick());
                g_prewarming = false;
                SetPhase(kWaitMenu);
                break;
            }
            if (net::Started() && !asked && ProbeSessionCount() == g_prewarmSessions) {  // the host's save, from in-game
                std::string save = net::SaveName().empty() ? g_save : net::SaveName();
                if (gameload::Request(save)) { asked = true; Log("autotest: prewarm: session started, loading %s", save.c_str()); }
            }
            if (ProbeSessionCount() != g_prewarmSessions && net::Started()) {
                g_prewarming = false;
                g_rng.seed(g_seed);
                Log("autotest: prewarm over, the session's game is running");
            }
            g_lastTickFrame = g_frames;
            break;
        }
        ChaosStep();
        uint64_t t = ProbeTick();
        // waiting for other players (lockstep gate) is normal, not a stall
        if (t != g_lastTick || (net::Active() && net::TicksAllowed(t) <= 0)) { g_lastTick = t; g_lastTickFrame = g_frames; }
        else if (g_frames - g_lastTickFrame >= 180 && (g_frames - g_lastTickFrame) % 180 == 0 && ProbeUiPaused()) {
            Log("autotest: a dialog is pausing the game, tapping Cancel");
            g_tapKey = kCancel;
        } else if (g_frames - g_lastTickFrame > 60 * 60) Quit("stalled");
        break;
    }
    case kDone:
        break;
    }
}

void OnSimTick(uint64_t tick) {
    if (g_active && g_phase == kInGame && !g_warmup.empty()) {
        if (tick >= g_warmupTicks && !g_warmupDone) { g_warmupDone = true; Log("autotest: warm-up reached %llu ticks", tick); }
        return;
    }
    if (g_active && g_phase == kInGame && tick >= g_ticks && !g_prewarming) {
        g_phase = kDone;
        Quit("tick limit reached");
    }
}

bool Active() { return g_active; }

static void InstallInputHooks() {
}

void Init() {
    InstallInputHooks();  // always: the overlay filters mouse input through them too
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wcscpy_s(wcsrchr(path, L'\\') + 1, 64, L"bmp_autotest.txt");
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[64], val[192];
        if (sscanf_s(line, " %63[^= ] = %191[^\r\n]", key, (unsigned)sizeof key, val, (unsigned)sizeof val) != 2) continue;
        std::string k = key, v = val;
        size_t at = k.find('@');
        if (at != std::string::npos) {
            if (atoi(k.c_str() + at + 1) != g_slot) continue;
            k = k.substr(0, at);
        }
        if (k == "save") g_save = v;
        else if (k == "mode") g_chaos = v == "chaos";
        else if (k == "ticks") g_ticks = strtoull(v.c_str(), nullptr, 10);
        else if (k == "speed") g_speed = atoi(v.c_str());
        else if (k == "seed") g_seed = (uint32_t)strtoul(v.c_str(), nullptr, 10);
        else if (k == "speedchaos") g_speedChaos = v != "0";
        else if (k == "warmup") g_warmup = v;
        else if (k == "prewarm") g_prewarm = v;
        else if (k == "prewarmticks") g_prewarmTicks = strtoull(v.c_str(), nullptr, 10);
        else if (k == "newgame") g_newGame = v;
        else if (k == "towns") { int n = 1, me = 0, pick = 0; sscanf_s(v.c_str(), "%d,%d,%d", &n, &me, &pick); towns::Configure(n, me, pick != 0); }
        else if (k == "warmupticks") g_warmupTicks = strtoull(v.c_str(), nullptr, 10);
        else if (k == "pauseevery") g_pauseEvery = (uint32_t)strtoul(v.c_str(), nullptr, 10) | 1;
        else if (k == "alloclog") {
            unsigned long long a = 0, b = 0;
            if (sscanf_s(v.c_str(), "%llu-%llu", &a, &b) == 2) ProbeSetAllocLog(a, b);
        }
        else if (k == "commands") {
            std::wstring w(v.begin(), v.end());
            commands::LoadReplay(w.c_str());
        }
        else if (k == "uiscript") {
            std::wstring w(v.begin(), v.end());
            uiscript::Load(w.c_str());
        }
    }
    fclose(f);
    g_active = true;
    Log("autotest: ACTIVE save='%s' mode=%s ticks=%llu speed=%d seed=%u", g_save.c_str(), g_chaos ? "chaos" : "idle",
        g_ticks, g_speed, g_seed);

    gameload::Init();
    p_AppGet = (AppGet_t)RuntimeExport("?Get@Application@1@SAPEAV11@XZ");

    void* kb = RuntimeExport("?PlatformGetKeyboardState@Device@Input@@AEAA_NPEAUState@12@@Z");
    void* ms = RuntimeExport("?PlatformGetMouseState@Device@Input@@AEAA_NPEAUState@12@@Z");
    if (kb && MH_CreateHook(kb, (void*)h_GetKeyboardState, (void**)&o_GetKeyboardState) == MH_OK) MH_EnableHook(kb);
    if (ms && MH_CreateHook(ms, (void*)h_GetMouseState, (void**)&o_GetMouseState) == MH_OK) MH_EnableHook(ms);
}

}  // namespace autotest
