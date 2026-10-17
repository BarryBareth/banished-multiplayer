// dinput8.dll proxy. Runtime-steam-x64.dll imports exactly one function from DINPUT8.dll
// (DirectInput8Create), so dropping this DLL next to the exe gets us loaded at startup.
// We forward to the real system dinput8 and use the first call (on the main thread,
// outside loader lock) as our init point.
#include "common.h"

using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static DirectInput8Create_t g_realCreate;
static INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK DoInit(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\dinput8.dll");
    HMODULE real = LoadLibraryW(path);
    g_realCreate = real ? (DirectInput8Create_t)GetProcAddress(real, "DirectInput8Create") : nullptr;
    Log("real dinput8=%p DirectInput8Create=%p main thread=%lu", real, g_realCreate, GetCurrentThreadId());
    InstallHooks();
    return TRUE;
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    InitOnceExecuteOnce(&g_initOnce, DoInit, nullptr, nullptr);
    if (!g_realCreate) return E_FAIL;
    return g_realCreate(inst, ver, riid, out, outer);
}

// Autotest: the engine's launcher dialog (Options / Video Options / Play) can appear before any of our hooks
// exist. When bmp_autotest.txt is present, a watcher thread presses its Play button.
static BOOL CALLBACK FindPlay(HWND child, LPARAM out) {
    wchar_t text[32];
    if (GetWindowTextW(child, text, 32) && wcscmp(text, L"Play") == 0) { *(HWND*)out = child; return FALSE; }
    return TRUE;
}
static BOOL CALLBACK FindLauncher(HWND wnd, LPARAM out) {
    DWORD pid;
    GetWindowThreadProcessId(wnd, &pid);
    wchar_t cls[16];
    if (pid == GetCurrentProcessId() && GetClassNameW(wnd, cls, 16) && wcscmp(cls, L"#32770") == 0) {
        HWND play = nullptr;
        EnumChildWindows(wnd, FindPlay, (LPARAM)&play);
        if (play) { *(HWND*)out = play; return FALSE; }
    }
    return TRUE;
}
static DWORD WINAPI LauncherWatcher(LPVOID) {
    // BM_CLICK can be ignored by an inactive window (always the case with two instances), so send the dialog the
    // button's WM_COMMAND directly and repeat until the launcher is gone.
    bool seen = false;
    for (int i = 0; i < 1200; i++) {  // up to 2 min
        HWND play = nullptr;
        EnumWindows(FindLauncher, (LPARAM)&play);
        if (play) {
            if (!seen) Log("autotest: launcher dialog found, pressing Play");
            seen = true;
            HWND dlg = GetParent(play);
            PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(play), BN_CLICKED), (LPARAM)play);
            Sleep(1000);
        } else if (seen) {
            Log("autotest: launcher dismissed");
            return 0;
        } else {
            Sleep(100);
        }
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        SlotClaim();
        LogInit();
        Log("bmp loaded (pid %lu, slot %d)", GetCurrentProcessId(), g_slot);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        wcscpy_s(wcsrchr(path, L'\\') + 1, 64, L"bmp_autotest.txt");
        bool test = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
        wcscpy_s(wcsrchr(path, L'\\') + 1, 64, L"bmp_menutest.txt");
        test = test || GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
        if (test) CloseHandle(CreateThread(nullptr, 0, LauncherWatcher, nullptr, 0, nullptr));
    }
    return TRUE;
}
