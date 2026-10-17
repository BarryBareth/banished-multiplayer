// Signature-agnostic "visual scope" wrappers. For an engine function with unknown arguments (incl. stack args and
// float regs) we emit a tiny x64 stub that bumps g_visualStub, swaps the return address so the function returns
// into the stub, then restores it and decrements. No argument register is touched (only rax before the call -
// not an argument register - and r10/r11). Single-threaded use (sim code on the main thread / loader thread).
#include "common.h"
#include "MinHook.h"
#include "visualstub.h"
#include <cstring>
#include <initializer_list>

volatile long g_visualStub;              // > 0 while inside a wrapped visual entry point
static uintptr_t g_retStack[4096];
static uintptr_t* g_retTop = g_retStack;

static uint8_t* g_code;                  // executable stub area
static size_t g_used;

static void Put(uint8_t*& p, std::initializer_list<uint8_t> bytes) { for (auto b : bytes) *p++ = b; }
static void Put64(uint8_t*& p, uint64_t v) { memcpy(p, &v, 8); p += 8; }

// Builds a stub; returns its address and where to store the original-function pointer.
static uint8_t* BuildStub(uintptr_t** originalSlot) {
    if (!g_code) {
        g_code = (uint8_t*)VirtualAlloc(nullptr, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    uint8_t* start = g_code + g_used;
    uint8_t* p = start;
    // ---- pre ----
    Put(p, {0x48, 0x8B, 0x04, 0x24});                    // mov rax, [rsp]          (caller's return address)
    Put(p, {0x49, 0xBB}); Put64(p, (uint64_t)&g_retTop); // mov r11, &g_retTop
    Put(p, {0x4D, 0x8B, 0x13});                          // mov r10, [r11]
    Put(p, {0x49, 0x89, 0x02});                          // mov [r10], rax
    Put(p, {0x49, 0x83, 0xC2, 0x08});                    // add r10, 8
    Put(p, {0x4D, 0x89, 0x13});                          // mov [r11], r10
    Put(p, {0x49, 0xBB}); Put64(p, (uint64_t)&g_visualStub);  // mov r11, &g_visualStub
    Put(p, {0x41, 0xFF, 0x03});                          // inc dword [r11]
    uint8_t* leaRax = p;
    Put(p, {0x48, 0x8D, 0x05, 0, 0, 0, 0});              // lea rax, [rip + post]
    Put(p, {0x48, 0x89, 0x04, 0x24});                    // mov [rsp], rax
    Put(p, {0xFF, 0x25, 0, 0, 0, 0});                    // jmp [rip+0]
    *originalSlot = (uintptr_t*)p;
    Put64(p, 0);                                         //   -> original (filled in later)
    // ---- post ----
    uint8_t* post = p;
    int32_t rel = (int32_t)(post - (leaRax + 7));
    memcpy(leaRax + 3, &rel, 4);
    Put(p, {0x49, 0xBB}); Put64(p, (uint64_t)&g_retTop); // mov r11, &g_retTop
    Put(p, {0x4D, 0x8B, 0x13});                          // mov r10, [r11]
    Put(p, {0x49, 0x83, 0xEA, 0x08});                    // sub r10, 8
    Put(p, {0x4D, 0x89, 0x13});                          // mov [r11], r10
    Put(p, {0x49, 0xBB}); Put64(p, (uint64_t)&g_visualStub);  // mov r11, &g_visualStub
    Put(p, {0x41, 0xFF, 0x0B});                          // dec dword [r11]
    Put(p, {0x41, 0xFF, 0x22});                          // jmp [r10]   (back to the real caller)
    g_used += ((p - start) + 15) & ~(size_t)15;
    return start;
}

void WrapVisual(void* target, const char* name) {
    uintptr_t* slot;
    uint8_t* stub = BuildStub(&slot);
    void* original = nullptr;
    MH_STATUS s = MH_CreateHook(target, stub, &original);
    if (s == MH_OK) {
        *slot = (uintptr_t)original;
        s = MH_EnableHook(target);
    }
    if (s != MH_OK) Log("visualstub: %s -> %s", name, MH_StatusToString(s));
}
