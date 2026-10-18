#include "gamebuild.h"
#include "common.h"
#include "gog_rvas.h"
#include <algorithm>
#include <mutex>
#include <vector>

namespace gamebuild {

static const uint32_t kSteamExeStamp = 0x59b56a2d;

Build Get() {
    static Build b = [] {
        auto exe = (uint8_t*)GetModuleHandleW(nullptr);
        auto nt = (IMAGE_NT_HEADERS*)(exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
        if (GetFileAttributesW(L"bmp_fakeexe.txt") != INVALID_FILE_ATTRIBUTES) return kUnknown;
        uint32_t stamp = nt->FileHeader.TimeDateStamp;
        return stamp == kSteamExeStamp ? kSteam : stamp == kGogExeStamp ? kGog : kUnknown;
    }();
    return b;
}

const char* Name() { return Get() == kSteam ? "Steam" : Get() == kGog ? "GOG" : "unknown"; }

const wchar_t* RuntimeDll() { return Get() == kGog ? L"Runtime-x64.dll" : L"Runtime-steam-x64.dll"; }

bool HasSteam() { return GetModuleHandleW(L"steam_api64.dll") != nullptr; }

}  // namespace gamebuild

uintptr_t Rva(uintptr_t steamRva) {
    if (gamebuild::Get() != gamebuild::kGog) return steamRva;
    auto end = kGogRvas + sizeof kGogRvas / sizeof kGogRvas[0];
    auto it = std::lower_bound(kGogRvas, end, steamRva, [](const uint32_t* e, uintptr_t v) { return e[0] < v; });
    if (it != end && (*it)[0] == steamRva) return (*it)[1];
    static std::once_flag once;
    std::call_once(once, [&] { Log("!! gamebuild: no GOG address for Steam exe+%#llx (tools/gog_table.py)",
                                   (unsigned long long)steamRva); });
    return steamRva;
}

uintptr_t SteamRva(uintptr_t rva) {
    if (gamebuild::Get() != gamebuild::kGog) return rva;
    static std::vector<std::pair<uint32_t, uint32_t>> back = [] {
        std::vector<std::pair<uint32_t, uint32_t>> v;
        for (auto& e : kGogRvas) v.push_back({e[1], e[0]});
        std::sort(v.begin(), v.end());
        return v;
    }();
    auto it = std::lower_bound(back.begin(), back.end(), std::pair<uint32_t, uint32_t>((uint32_t)rva, 0));
    return it != back.end() && it->first == rva ? it->second : rva;
}
