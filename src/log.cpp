#include "common.h"
#include <cstdarg>
#include <share.h>

static FILE* g_log;
int g_slot;
static wchar_t g_suffix[8];

void SlotClaim() {
    for (int i = 0; i < 8; i++) {
        wchar_t name[64];
        swprintf_s(name, L"Local\\bmp_slot_%d", i);
        HANDLE m = CreateMutexW(nullptr, TRUE, name);  // held for the life of the process
        if (m && GetLastError() != ERROR_ALREADY_EXISTS) {
            g_slot = i;
            if (i) swprintf_s(g_suffix, L"_p%d", i);
            return;
        }
        if (m) CloseHandle(m);
    }
}

const wchar_t* SlotSuffix() { return g_suffix; }
static SRWLOCK g_logLock = SRWLOCK_INIT;

void LogInit() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    wchar_t prev[MAX_PATH];
    wcscpy_s(prev, path);
    swprintf_s(slash + 1, MAX_PATH - (slash + 1 - path), L"bmp%s.log", g_suffix);
    swprintf_s(prev + (slash + 1 - path), MAX_PATH - (slash + 1 - path), L"bmp_prev%s.log", g_suffix);
    MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING);  // the last run's log stays for a crash report (report.cpp)
    g_log = _wfsopen(path, L"w", _SH_DENYNO);  // readable while the game runs
}

void Log(const char* fmt, ...) {
    if (!g_log) return;
    AcquireSRWLockExclusive(&g_logLock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    ReleaseSRWLockExclusive(&g_logLock);
}

void* RuntimeExport(const char* mangled) {
    static HMODULE rt = GetModuleHandleW(L"Runtime-steam-x64.dll");
    void* p = rt ? (void*)GetProcAddress(rt, mangled) : nullptr;
    if (!p) Log("!! missing export %s", mangled);
    return p;
}
