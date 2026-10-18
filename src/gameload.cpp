// Save loading from the main menu (see gameload.h). Addresses: exe build 0x59b56a2d.
#include "gameload.h"
#include "autotest.h"
#include "common.h"
#include <shlobj.h>
#include <cctype>

namespace gameload {

static const uintptr_t kRvaGameLoopCtor = 0x9d530;   // GameLoop(mem, SavedState*, InternalBasePtr*, int, String*, int, int, bool, u64, u64, u64)
static const uintptr_t kVtMenuLoop = 0x165120;       // main-menu UpdateLoop vtable
static const uintptr_t kVtLoaderLoop = 0x15e9f0;     // exe LoaderLoop subclass vtable (as used by SaveLoadDialog)
static const uintptr_t kRvaQuitToMenu = 0xb9c60;     // "Quit to main menu": (owner{+0xb8 GameLoop})
static const uintptr_t kRvaNewGame = 0xb9a10;        // New Game dialog's start (owner, name, seed, size, disasters, mapgen, terrain, climate, -1, achievement*)

using SaveLoadGet_t = void*(__fastcall*)();
using SaveLoadSetOp_t = void(__fastcall*)(void* sl, int op, int index);
using SaveLoadGetOp_t = int(__fastcall*)(void* sl);
using SaveLoadUpdate_t = void(__fastcall*)(void* sl, const int64_t* dt);
using SaveLoadGetState_t = void*(__fastcall*)(void* sl);
using SaveLoadCount_t = int(__fastcall*)(void* sl);
using MainLoopGet_t = void*(__fastcall*)();
using SetNextLoop_t = void(__fastcall*)(void* ml, void* loop);
using GetCurrentLoop_t = void*(__fastcall*)(void* ml);
using LoaderLoopCtor_t = void*(__fastcall*)(void* self, void* next, const int64_t* time);
using MemAllocFn_t = void*(__fastcall*)(uint64_t size, int align);
using StringCtor_t = void*(__fastcall*)(void* self, const char* s);
using StringDtor_t = void(__fastcall*)(void* self);
using SetSaveName_t = void(__fastcall*)(void* sl, const void* name, int kind);
using SetSaveSprite_t = void(__fastcall*)(void* sl, const void* sprite16);  // UI::Sprite by value = pointer to a copy
using TimeFromSeconds_t = void*(__fastcall*)(int64_t* self, float seconds);
using SetMinOpTime_t = void(__fastcall*)(void* sl, const int64_t* time);
using GameLoopCtor_t = void*(__fastcall*)(void* self, void* savedState, void** achievement, int a4, void* name,
                                          int a6, int a7, bool a8, uint64_t a9, uint64_t a10, uint64_t a11);

static SaveLoadGet_t p_SaveLoadGet;
static SaveLoadSetOp_t p_SetOp;
static SaveLoadGetOp_t p_GetOp;
static SaveLoadUpdate_t p_SaveLoadUpdate;
static SaveLoadGetState_t p_GetSaveState;
static SaveLoadCount_t p_SaveCount;
static MainLoopGet_t p_MainLoopGet;
static SetNextLoop_t p_SetNextLoop;
static GetCurrentLoop_t p_GetCurrentLoop;
static LoaderLoopCtor_t p_LoaderLoopCtor;
static MemAllocFn_t p_MemAlloc;
static StringCtor_t p_StringCtor;
static StringDtor_t p_StringDtor;
static SetSaveName_t p_SetSaveName;
static SetSaveSprite_t p_SetSaveSprite;
static TimeFromSeconds_t p_TimeFromSeconds;
static SetMinOpTime_t p_SetMinOpTime;
static const uintptr_t kRvaGameUiGlobal = 0x1cbde8;   // exe global (DAT_1401cbde8): +0xb8 = save thumbnail sprite
static const uintptr_t kRvaSaveMinSeconds = 0x17884c; // float constant the autosave uses as minimum operation time
static bool g_saving;

static State g_state = kIdle;
static std::string g_name;
static const char* g_error = "";
static int g_frames;
static int64_t g_lastSlTime;
static uint64_t g_prevSessions;  // session count before this request (a new session = the game started)

void Init() {
    static bool done;
    if (done) return;
    done = true;
    p_SaveLoadGet = (SaveLoadGet_t)RuntimeExport("?Get@SaveLoad@Save@@SAPEAV12@XZ");
    p_SetOp = (SaveLoadSetOp_t)RuntimeExport("?SetOperation@SaveLoad@Save@@QEAAXW4Operation@12@H@Z");
    p_GetOp = (SaveLoadGetOp_t)RuntimeExport("?GetOperation@SaveLoad@Save@@QEBA?AW4Operation@12@XZ");
    p_SaveLoadUpdate = (SaveLoadUpdate_t)RuntimeExport("?Update@SaveLoad@Save@@QEAAXAEBVTime@System@@@Z");
    p_GetSaveState = (SaveLoadGetState_t)RuntimeExport("?GetSaveState@SaveLoad@Save@@QEAAPEAVSavedState@2@XZ");
    p_SaveCount = (SaveLoadCount_t)RuntimeExport("?GetSaveCount@SaveLoad@Save@@QEBAHXZ");
    p_MainLoopGet = (MainLoopGet_t)RuntimeExport("?Get@MainLoop@Engine@@SAPEAV12@XZ");
    p_SetNextLoop = (SetNextLoop_t)RuntimeExport("?SetNextLoop@MainLoop@Engine@@QEAAXPEAVUpdateLoop@2@@Z");
    p_GetCurrentLoop = (GetCurrentLoop_t)RuntimeExport("?GetCurrentLoop@MainLoop@Engine@@QEAAPEAVUpdateLoop@2@XZ");
    p_LoaderLoopCtor = (LoaderLoopCtor_t)RuntimeExport("??0LoaderLoop@Engine@@QEAA@PEAVUpdateLoop@1@AEBVTime@System@@@Z");
    p_MemAlloc = (MemAllocFn_t)RuntimeExport("?MemAlloc@MemoryAllocator@System@@SAPEAX_KH@Z");
    p_StringCtor = (StringCtor_t)RuntimeExport("??0String@System@@QEAA@PEBD@Z");
    p_StringDtor = (StringDtor_t)RuntimeExport("??1String@System@@QEAA@XZ");
    p_SetSaveName = (SetSaveName_t)RuntimeExport("?SetSaveName@SaveLoad@Save@@QEAAXAEBVString@System@@H@Z");
    p_SetSaveSprite = (SetSaveSprite_t)RuntimeExport("?SetSaveSprite@SaveLoad@Save@@QEAAXVSprite@UI@@@Z");
    p_TimeFromSeconds = (TimeFromSeconds_t)RuntimeExport("??0Time@System@@QEAA@M@Z");
    p_SetMinOpTime = (SetMinOpTime_t)RuntimeExport("?SetMinOperationTime@SaveLoad@Save@@QEAAXAEBVTime@System@@@Z");
}

bool BeginAutoSave() {
    Init();
    void* sl = p_SaveLoadGet ? p_SaveLoadGet() : nullptr;
    if (!sl || !ProbeGameLoop() || g_saving || p_GetOp(sl) != 0 || !p_StringCtor || !p_SetSaveName || !p_SetSaveSprite) return false;
    // Same setup as the game's autosave (exe+0x5e820). The thumbnail sprite is required: SaveLoad::Draw renders
    // the save thumbnail with it (without it: access violation in d3d11).
    uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
    uint8_t* gameUi = *(uint8_t**)(exe + Rva(kRvaGameUiGlobal));
    if (!gameUi) return false;
    int64_t minTime = 0;
    p_TimeFromSeconds(&minTime, *(float*)(exe + Rva(kRvaSaveMinSeconds)));
    p_SetMinOpTime(sl, &minTime);
    uint32_t sprite[4];
    memcpy(sprite, gameUi + 0xb8, sizeof sprite);
    p_SetSaveSprite(sl, sprite);
    void* name = nullptr;  // System::String = one char* (refcounted body)
    p_StringCtor(&name, "Multiplayer");
    p_SetSaveName(sl, &name, 2);  // display name + kind 2, as autosave does
    p_StringDtor(&name);
    p_SetOp(sl, 4, -4);           // save into the autosave slot
    g_saving = true;
    g_frames = 0;
    Log("gameload: saving the running game (autosave slot)");
    return true;
}

bool SaveBusy() { return g_saving; }

bool InMenu() {
    void* ml = p_MainLoopGet ? p_MainLoopGet() : nullptr;
    void* cur = ml ? p_GetCurrentLoop(ml) : nullptr;
    return cur && *(uintptr_t*)cur == (uintptr_t)GetModuleHandleW(nullptr) + Rva(kVtMenuLoop) && p_SaveLoadGet && p_SaveLoadGet();
}

static void SetState(State s) { g_state = s; g_frames = 0; Log("gameload: state %d", s); }
static void Fail(const char* why) { g_error = why; Log("!! gameload: %s", why); SetState(kFailed); }

static std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static const char* BStr(const void* strObj) {  // System::String = char* with header before the chars
    const char* s = *(const char* const*)strObj;
    return s ? s : "";
}

static void PumpSaveLoad(void* sl) {
    // Drive the async SaveLoad state machine if nothing else (no dialog open) is updating it.
    int64_t t = *(int64_t*)((uint8_t*)sl + 8);
    if (t == g_lastSlTime) {
        int64_t dt = 16666;
        p_SaveLoadUpdate(sl, &dt);
    }
    g_lastSlTime = *(int64_t*)((uint8_t*)sl + 8);
}

bool Request(const std::string& name) {
    Init();
    if (!p_SaveLoadGet || !p_SaveLoadGet() || (g_state != kIdle && g_state != kDone && g_state != kFailed)) return false;
    if (!InMenu() && !ProbeGameLoop()) return false;  // menu or in-game only (not mid-load)
    g_name = name;
    g_prevSessions = ProbeSessionCount();
    Log("gameload: loading save '%s'", name.c_str());
    p_SetOp(p_SaveLoadGet(), 1, -1);  // list saves
    SetState(kListing);
    return true;
}

State Frame() {
    if (g_saving) {
        void* sl = p_SaveLoadGet();
        PumpSaveLoad(sl);
        if (p_GetOp(sl) == 0 && ++g_frames > 2) { g_saving = false; Log("gameload: save written"); }
        return g_state;
    }
    if (g_state == kIdle || g_state == kDone || g_state == kFailed) return g_state;
    g_frames++;
    uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
    void* ml = p_MainLoopGet();
    void* sl = p_SaveLoadGet();
    switch (g_state) {
    case kListing:
        PumpSaveLoad(sl);
        if (p_GetOp(sl) == 0 && g_frames > 2) {
            int n = p_SaveCount(sl), exact = -1, partial = -1;
            uint8_t* list = *(uint8_t**)((uint8_t*)sl + 0x50);
            std::string want = Lower(g_name);
            for (int i = 0; i < n; i++) {
                std::string name = Lower(BStr(list + i * 0x58 + 0x38));
                Log("gameload: save[%d] = '%s'", i, name.c_str());
                size_t slash = name.find_last_of("/\\");
                std::string stem = slash == std::string::npos ? name : name.substr(slash + 1);
                if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".sav") == 0) stem.resize(stem.size() - 4);
                if (exact < 0 && stem == want) exact = i;
                if (partial < 0 && name.find(want) != std::string::npos) partial = i;
            }
            int found = exact >= 0 ? exact : partial;
            if (found < 0) { Fail("save not found"); break; }
            Log("gameload: loading save[%d]", found);
            p_SetOp(sl, 2, found);
            SetState(kLoading);
        } else if (g_frames > 600) Fail("listing timed out");
        break;
    case kLoading:
        PumpSaveLoad(sl);
        if (p_GetOp(sl) == 0 && g_frames > 2) {
            void* state = p_GetSaveState(sl);
            Log("gameload: save loaded, SavedState=%p", state);
            if (!state) { Fail("no saved state"); break; }
            void* gl = p_MemAlloc(0x160, 8);
            void* noAchievement = nullptr;
            char* emptyName = nullptr;
            ((GameLoopCtor_t)(exe + Rva(kRvaGameLoopCtor)))(gl, state, &noAchievement, -1, &emptyName, 0, 0, true, 0, 0, 0);
            void* loader = p_MemAlloc(0x48, 4);
            int64_t zero = 0;
            p_LoaderLoopCtor(loader, gl, &zero);
            *(uintptr_t*)loader = exe + Rva(kVtLoaderLoop);
            p_SetNextLoop(ml, loader);
            SetState(kStarting);
        } else if (g_frames > 1800) Fail("load timed out");
        break;
    case kStarting:
        if (ProbeSessionCount() != g_prevSessions) SetState(kDone);
        else if (g_frames > 60 * 120) Fail("game never started");
        break;
    default:
        break;
    }
    return g_state;
}

bool RequestNewGame(uint32_t seed, int mapSize, int startIdx, int terrainIdx, int climateIdx, bool disasters) {
    Init();
    if (!InMenu() || (g_state != kIdle && g_state != kDone && g_state != kFailed)) return false;
    uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
    uint8_t* lists = *(uint8_t**)(exe + Rva(kRvaGameUiGlobal));  // DAT_1401cbde8: +0x18 start conditions, +0x20 terrain, +0x10 climate
    auto item = [&](int off, int i) -> void* {
        uint8_t* list = lists ? *(uint8_t**)(lists + off) : nullptr;
        return list ? (*(void***)(list + 0x20))[i] : nullptr;
    };
    void* mapGen = item(0x18, startIdx);
    void* terrain = item(0x20, terrainIdx);
    void* climate = item(0x10, climateIdx);
    if (!mapGen || !terrain || !climate) { Fail("new game: generator lists not found"); return false; }
    g_prevSessions = ProbeSessionCount();
    // Same path as the New Game dialog (exe+0x75750 -> FUN_1400b9a10): it wraps the GameLoop in the "generating
    // map" DialogLoaderLoopT, which also brings up the game UI and camera. Its first argument only needs +0xc0 =
    // the main-menu loop (the loader is handed over through menuLoop+0xb68 after the fade).
    alignas(16) static uint8_t owner[0xd0];
    memset(owner, 0, sizeof owner);
    *(void**)(owner + 0xc0) = p_GetCurrentLoop(p_MainLoopGet());
    void* name = nullptr;
    p_StringCtor(&name, "Multiplayer");
    void* noAchievement = nullptr;
    using NewGame_t = void(__fastcall*)(void* owner, void* name, int seed, int mapSize, bool disasters, void* mapGen,
                                        void* terrain, void* climate, int achievementIdx, void** achievement);
    ((NewGame_t)(exe + Rva(kRvaNewGame)))(owner, &name, (int)seed, mapSize, disasters, mapGen, terrain, climate, -1, &noAchievement);
    p_StringDtor(&name);
    Log("gameload: new game seed=%u size=%d start=%d terrain=%d climate=%d disasters=%d", seed, mapSize, startIdx,
        terrainIdx, climateIdx, disasters);
    SetState(kStarting);
    return true;
}

// The in-game menu's "Quit to main menu" (exe+0x72110 after its confirm dialog) calls exe+0xb9c60(owner): owner+0xb8 =
// the running GameLoop (+0xc0 = menu loop when called from the menu). It frees the GameLoop's SaveLoad (+0x140), builds
// the main-menu DialogLoopT ("Dialog/MainMenu.rsc") behind a "Dialog/Loading.rsc" loader and hands it over through
// GameLoop+0xc8 with a fade, like any other loop change.
bool RequestMainMenu() {
    Init();
    void* gl = ProbeGameLoop();
    if (!gl || InMenu() || (g_state != kIdle && g_state != kDone && g_state != kFailed)) return false;
    alignas(16) static uint8_t owner[0xd0];
    memset(owner, 0, sizeof owner);
    *(void**)(owner + 0xb8) = gl;
    ((void(__fastcall*)(void*))((uintptr_t)GetModuleHandleW(nullptr) + Rva(kRvaQuitToMenu)))(owner);
    Log("gameload: quitting to the main menu");
    SetState(kIdle);
    return true;
}

NewGameOptions GetNewGameOptions() {
    Init();
    NewGameOptions o;
    uint8_t* lists = *(uint8_t**)((uintptr_t)GetModuleHandleW(nullptr) + Rva(kRvaGameUiGlobal));
    if (!lists) return o;
    using Count_t = int(__fastcall*)(void* list);
    static Count_t count = (Count_t)RuntimeExport("?GetResourceCount@ExternalList@Object@@QEBAHXZ");
    auto names = [&](int off, std::vector<std::string>& out) {
        uint8_t* list = *(uint8_t**)(lists + off);
        int n = list && count ? count(list) : 0;
        for (int i = 0; i < n; i++) {
            uint8_t* item = (*(uint8_t***)(list + 0x20))[i];
            const char* s = item ? *(const char**)(item + 0x28) : nullptr;  // display name (System::String)
            out.push_back(s && *s ? s : "#" + std::to_string(i + 1));
        }
    };
    names(0x18, o.starts);
    names(0x20, o.terrains);
    names(0x10, o.climates);
    o.sizes = {"Small", "Medium", "Large"};  // the New Game dialog's size choices (index into the generator's table)
    for (auto& s : o.starts)  // "HardName" etc. are string-table keys
        if (s.size() > 4 && s.compare(s.size() - 4, 4, "Name") == 0) s.resize(s.size() - 4);
    return o;
}

const char* Error() { return g_error; }
State Current() { return g_state; }

std::wstring SaveDir() {
    PWSTR docs = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) dir = std::wstring(docs) + L"\\Banished\\Save";
    CoTaskMemFree(docs);
    return dir;
}

}  // namespace gameload
