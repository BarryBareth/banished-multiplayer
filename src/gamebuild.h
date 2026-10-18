#pragma once
#include <cstdint>

// Which build of Banished 1.0.7 is running. The mod's exe addresses are the Steam build's (found by reverse
// engineering Application-steam-x64.exe); the GOG build (Application-x64.exe, compiled from the same code 154 s
// earlier, without Steam) has the same functions and data at other addresses. Every exe address the mod uses goes
// through Rva(), which translates it with src/gog_rvas.h (tools/gog_table.py) on the GOG build.
namespace gamebuild {
enum Build { kUnknown, kSteam, kGog };
Build Get();                    // from the exe's TimeDateStamp (bmp_fakeexe.txt: kUnknown, for tests)
const char* Name();             // "Steam" / "GOG" / "unknown"
const wchar_t* RuntimeDll();    // the engine DLL: Runtime-steam-x64.dll / Runtime-x64.dll
bool HasSteam();                // steam_api64.dll is loaded (the Steam build; also copies that ship it)
}

// Steam exe RVA -> this build's RVA. Unknown address on the GOG build: logged once, returns the Steam RVA.
uintptr_t Rva(uintptr_t steamRva);
// This build's RVA -> the Steam RVA (vtables and typeinfos sent to other players, or compared with constants);
// addresses not in the table come back unchanged.
uintptr_t SteamRva(uintptr_t rva);
