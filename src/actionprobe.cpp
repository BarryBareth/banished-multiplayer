// Logs every citizen-AI Action start (vf4) and every update (vf5) that ends/fails, per sim tick, into the
// draws log as kinds 'a' (start) and 'z' (end). Actions are identified by their offset in the sim heap,
// which is identical across deterministic runs - so the first differing line names the first action whose
// timing diverged.
#include "common.h"
#include "MinHook.h"
#include "simheap.h"
#include "actionprobe.h"

namespace actionprobe {

struct ActionType { const char* name; uintptr_t vf4, vf5; };
// exe build 0x59b56a2d; vf5 0xc9f60 = shared "instant" update
static const ActionType kTypes[] = {
    {"AnimateNaturalResource", 0xcaad0, 0xcaa80}, {"CompleteRoad", 0xcad20, 0},       {"CompleteTask", 0xcb010, 0},
    {"ConsumeRawMaterial", 0xcb420, 0},           {"CreateNaturalResource", 0xcbb20, 0}, {"CreateRawMaterial", 0xcc010, 0},
    {"DropOffRawMaterial", 0xcc7d0, 0},           {"HuntAnimal", 0xcd720, 0xcd710},  {"MoveTo", 0xcdf80, 0xcda40},
    {"PerformWork", 0xce8f0, 0xce890},            {"PickupRawMaterial", 0xceda0, 0}, {"PickupStoredRawMaterial", 0xcf210, 0},
    {"PlayAnimation", 0x42d80, 0x42d60},          {"RemoveAnimal", 0xcfcc0, 0},      {"RemoveNaturalResource", 0xcffe0, 0},
    {"SendComponentMessage", 0xd03c0, 0},         {"ShowTool", 0x43070, 0},
};
static const int kCount = sizeof(kTypes) / sizeof(kTypes[0]);

static Sink g_sink;
static void (*g_dropOff)(void* action);
void SetDropOffHandler(void (*fn)(void* action)) { g_dropOff = fn; }
static uint64_t ObjId(void* p) { return simheap::Contains(p) ? simheap::Offset(p) : 0xFFFFFFFFull; }
static const int kDropOff = 6;  // index of DropOffRawMaterial in kTypes

template <int I> struct Hooks {
    static inline uint64_t(__fastcall* o4)(void*);
    static inline uint64_t(__fastcall* o5)(void*, void*, int64_t*);
    static uint64_t __fastcall Start(void* self) {
        uint64_t r = o4(self);
        if (I == kDropOff && g_dropOff) g_dropOff(self);
        if (g_sink) g_sink('a', I, ObjId(self), r);
        return r;
    }
    static uint64_t __fastcall Update(void* self, void* q, int64_t* dt) {
        uint64_t r = o5(self, q, dt);
        if (g_sink && (uint8_t)r != 1) g_sink('z', I, ObjId(self), (uint8_t)r);
        return r;
    }
    static void Install(uintptr_t exe) {
        MH_STATUS a = MH_CreateHook((void*)(exe + Rva(kTypes[I].vf4)), (void*)Start, (void**)&o4);
        if (a == MH_OK) MH_EnableHook((void*)(exe + Rva(kTypes[I].vf4)));
        MH_STATUS b = MH_OK;
        if (kTypes[I].vf5) {
            b = MH_CreateHook((void*)(exe + Rva(kTypes[I].vf5)), (void*)Update, (void**)&o5);
            if (b == MH_OK) MH_EnableHook((void*)(exe + Rva(kTypes[I].vf5)));
        }
        if (a != MH_OK || b != MH_OK) Log("actionprobe: %s hook vf4=%s vf5=%s", kTypes[I].name, MH_StatusToString(a), MH_StatusToString(b));
    }
};

template <int... Is> static void InstallAll(uintptr_t exe, std::integer_sequence<int, Is...>) {
    (Hooks<Is>::Install(exe), ...);
}

void Install(uintptr_t exeBase, Sink sink) {
    g_sink = sink;
    InstallAll(exeBase, std::make_integer_sequence<int, kCount>{});
    Log("actionprobe: %d action types hooked", kCount);
}

const char* Name(int type) { return type >= 0 && type < kCount ? kTypes[type].name : "?"; }

}  // namespace actionprobe
