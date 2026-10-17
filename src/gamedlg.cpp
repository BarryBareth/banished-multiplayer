// Game-styled windows (see gamedlg.h).
#include "gamedlg.h"
#include "gameui.h"
#include "common.h"
#include "commands.h"
#include <unordered_map>

namespace gamedlg {

static std::unordered_map<void*, Window*> g_open;  // dialog -> its Window
static void* g_vtable[10];
using Dtor_t = void*(__fastcall*)(void* self, unsigned flags);
using Update_t = void(__fastcall*)(void* self, const void* time);
using Event_t = void(__fastcall*)(void* self, void* elem, int evt, int id, int a, int b, int c, void* p);
static Dtor_t o_Dtor;
static Update_t o_Update;
static Event_t o_Event;

static void* __fastcall h_Dtor(void* self, unsigned flags) {
    auto it = g_open.find(self);
    if (it != g_open.end()) {
        Window* w = it->second;
        g_open.erase(it);
        w->dlg = nullptr;
        w->Closed();
    }
    return o_Dtor(self, flags);
}
static void __fastcall h_Update(void* self, const void* time) {
    o_Update(self, time);
    auto it = g_open.find(self);
    if (it != g_open.end()) it->second->Refresh();
}
static void __fastcall h_Event(void* self, void* elem, int evt, int id, int a, int b, int c, void* p) {
    o_Event(self, elem, evt, id, a, b, c, p);
    auto it = g_open.find(self);
    static int logged;  // diagnosis: which mouse events reach the mod's windows (press 2 / release 1 / click 0x12), any id
    if (it != g_open.end() && (evt == 1 || evt == 2 || evt == 0x12) && logged++ < 100)
        Log("gamedlg: %s event 0x%x id %d elem %p", it->second->Resource(), evt, id, elem);
    if (it == g_open.end() || id < 1000) return;
    static int pressed;
    if (id == it->second->TitleButton()) {
        if (evt == 2) pressed = id;
        else if (evt == 1 && pressed == id) { pressed = 0; it->second->OnEvent(id, 0x12); }
        return;
    }
    // A check reports a click and a value change: one action. Only those two take part in the debounce - image buttons
    // (the disaster / copy / paste icons) send press 2, click 0x12, release 1 together, and the press used to swallow
    // the click ("the bug report buttons don't do anything").
    if (evt != 0x12 && evt != 0x15) { it->second->OnEvent(id, evt); return; }
    static DWORD lastAt;
    static int lastId;
    if (id == lastId && GetTickCount() - lastAt < 50) return;
    lastId = id, lastAt = GetTickCount();
    it->second->OnEvent(id, evt);
}

bool Window::Open(void* ui) {
    if (dlg) return true;
    static void* memAlloc = RuntimeExport("?MemAlloc@MemoryAllocator@System@@SAPEAX_KH@Z");
    static void* ctor = RuntimeExport("??0DialogInstance@UI@@QEAA@PEAVUserInterface@1@PEBVDialog@1@V?$InternalPtr@VDialogInstance@UI@@@Object@@PEAVBase@5@PEAXH@Z");
    static void* addDialog = RuntimeExport("?AddDialog@UserInterface@UI@@QEAAXV?$InternalPtr@VDialogInstance@UI@@@Object@@@Z");
    static void** baseVt = (void**)RuntimeExport("??_7DialogInstance@UI@@6B@");
    void* res = commands::DescFromPath(Resource());
    if (!ui || !memAlloc || !ctor || !addDialog || !baseVt || !res) {
        static int logged;
        if (logged++ < 5) Log("gamedlg: can't open %s (ui %p, resource %p) - companion mod missing?", Resource(), ui, res);
        return false;
    }
    if (!o_Dtor) {
        for (int i = 0; i < 10; i++) g_vtable[i] = baseVt[i];
        o_Dtor = (Dtor_t)baseVt[0];
        o_Update = (Update_t)baseVt[7];
        o_Event = (Event_t)baseVt[8];
        g_vtable[0] = (void*)h_Dtor;
        g_vtable[7] = (void*)h_Update;
        g_vtable[8] = (void*)h_Event;
    }
    void* d = ((void*(__fastcall*)(size_t, int))memAlloc)(0xb8, 8);
    void* parent = nullptr;
    using Ctor_t = void*(__fastcall*)(void*, void*, void*, void**, void*, void*, int);
    ((Ctor_t)ctor)(d, ui, res, &parent, nullptr, nullptr, 0);
    *(void***)d = g_vtable;
    dlg = d;
    g_open[d] = this;
    Init();
    Refresh();  // the first frame already shows the right state
    void* ref = d;
    ((void(__fastcall*)(void*, void**))addDialog)(ui, &ref);
    Log("gamedlg: opened %s", Resource());
    return true;
}
void Window::Close() { if (dlg) gameui::Close(Ui(), dlg); }
void* Window::Ui() const { return gameui::UiOfDialog(dlg); }
void* Window::E(const char* name) const { return dlg ? gameui::FindInDialog(dlg, name) : nullptr; }
void Window::Id(const char* name, int id) const { gameui::SetId(E(name), id); }
void Window::Text(const char* name, const std::string& s) const { gameui::SetTextDeep(E(name), s.c_str()); }
void Window::Vis(const char* name, bool on) const { gameui::Show(E(name), on); }
void Window::Check(const char* name, bool on) const { gameui::SetChecked(E(name), on); }
void Window::Enable(const char* name, bool on) const { gameui::Enable(E(name), on); }

void* GameUi() {
    uint8_t* gl = *(uint8_t**)((uintptr_t)GetModuleHandleW(nullptr) + 0x1cbdd8);
    return gl ? *(void**)(gl + 0x98) : nullptr;
}

}  // namespace gamedlg
