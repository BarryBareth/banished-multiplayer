// Engine UI exports (see gameui.h).
#include "gameui.h"
#include "common.h"

namespace gameui {

using StrCtor_t = void*(__fastcall*)(void* self, const char* s);
using StrDtor_t = void(__fastcall*)(void* self);
using Find_t = void**(__fastcall*)(void* self, void** out, const void* name);
using SetId_t = void(__fastcall*)(void* elem, int id);
using Bool_t = void(__fastcall*)(void* elem, bool on);
using IsChecked_t = bool(__fastcall*)(void* check);
using SetParam_t = void(__fastcall*)(void* text, int index, const void* str);

static StrCtor_t p_strCtor;
static StrDtor_t p_strDtor;
static Find_t p_find, p_findDlg;
static SetId_t p_setId;
static Bool_t p_show, p_hide, p_check;
static IsChecked_t p_isChecked;
static SetParam_t p_setParam;
static bool g_tried, g_ready;

bool Ready() {
    if (g_tried) return g_ready;
    g_tried = true;
    p_strCtor = (StrCtor_t)RuntimeExport("??0String@System@@QEAA@PEBD@Z");
    p_strDtor = (StrDtor_t)RuntimeExport("??1String@System@@QEAA@XZ");
    p_find = (Find_t)RuntimeExport("?FindElement@Element@UI@@QEAA?AV?$InternalPtr@VElement@UI@@@Object@@AEBVString@System@@@Z");
    p_findDlg = (Find_t)RuntimeExport("?FindElement@DialogInstance@UI@@QEAA?AV?$InternalPtr@VElement@UI@@@Object@@AEBVString@System@@@Z");
    p_setId = (SetId_t)RuntimeExport("?SetElementId@Element@UI@@QEAAXH@Z");
    p_show = (Bool_t)RuntimeExport("?Show@Element@UI@@QEAAX_N@Z");
    p_hide = (Bool_t)RuntimeExport("?Hide@Element@UI@@QEAAX_N@Z");
    p_check = (Bool_t)RuntimeExport("?EnableCheck@CheckElement@UI@@QEAAX_N@Z");
    p_isChecked = (IsChecked_t)RuntimeExport("?IsChecked@CheckElement@UI@@QEBA_NXZ");
    p_setParam = (SetParam_t)RuntimeExport("?SetParameter@TextElement@UI@@QEAAXHAEBVString@System@@@Z");
    g_ready = p_strCtor && p_strDtor && p_find && p_findDlg && p_setId && p_show && p_hide && p_check && p_isChecked && p_setParam;
    if (!g_ready) Log("!! gameui: UI exports missing");
    return g_ready;
}

static void* FindWith(bool inDialog, void* self, const char* name) {
    if (!self || !Ready()) return nullptr;
    void* str = nullptr;
    p_strCtor(&str, name);
    void* out = nullptr;
    (inDialog ? p_findDlg : p_find)(self, &out, &str);
    p_strDtor(&str);
    return out;
}
void* Find(void* element, const char* name) { return FindWith(false, element, name); }
void* FindInDialog(void* dialog, const char* name) { return FindWith(true, dialog, name); }
void SetId(void* e, int id) { if (e && Ready()) p_setId(e, id); }
void Show(void* e, bool show) { if (e && Ready()) p_show(e, show); }  // Element::Show(bool visible) (Hide(bool hidden))
void Enable(void* e, bool on) {  // Element vtable +0x40 (EventLogUI's refresh enables its buttons through it)
    if (e) (*(void(__fastcall**)(void*, bool))(*(uint8_t**)e + 0x40))(e, on);
}
// Edit fields: vtable slot 0x40 is NOT Enable there (it crashed in the Runtime); the exported base Element::Enable is.
void EnableEdit(void* e, bool on) {
    static auto fn = (void(__fastcall*)(void*, bool))RuntimeExport("?Enable@Element@UI@@UEAAX_N@Z");
    if (e && fn) fn(e, on);
}
void SetChecked(void* c, bool on) { if (c && Ready()) p_check(c, on); }
bool IsChecked(void* c) { return c && Ready() && p_isChecked(c); }
// ---- subtree walk, combos, edits, dialogs (resolved on first use)
using Child_t = void**(__fastcall*)(void* self, void** out, int index);
using Count_t = int(__fastcall*)(void* self);
using ComboAdd_t = int(__fastcall*)(void* combo, const void* sprite, const void* str, void* data);
using Int_t = int(__fastcall*)(void* self);
using SetInt_t = void(__fastcall*)(void* self, int v);
using Void_t = void(__fastcall*)(void* self);
using GetText_t = const char**(__fastcall*)(void* edit);  // returns const String& (String = one char*)
using SetText_t = void(__fastcall*)(void* edit, const void* str);
using Close_t = void(__fastcall*)(void* ui, void** dialogRef);
struct More {
    Child_t child; Count_t count; uintptr_t textVt;
    ComboAdd_t comboAdd; Void_t comboClear; Int_t comboGet; SetInt_t comboSet; Void_t spriteCtor;
    GetText_t editGet; SetText_t editSet; Int_t numGet; SetInt_t numSet; Int_t editActive; Close_t close;
};
static More* M() {
    static More m;
    static bool done;
    if (!done) {
        done = true;
        m.child = (Child_t)RuntimeExport("?Child@Element@UI@@QEAA?AV?$InternalPtr@VElement@UI@@@Object@@H@Z");
        m.count = (Count_t)RuntimeExport("?ChildCount@Element@UI@@QEBAHXZ");
        m.textVt = (uintptr_t)RuntimeExport("??_7TextElement@UI@@6B@");
        m.comboAdd = (ComboAdd_t)RuntimeExport("?AddItem@ComboElement@UI@@QEAAHAEBVSprite@2@AEBVString@System@@PEAX@Z");
        m.comboClear = (Void_t)RuntimeExport("?RemoveAllItems@ComboElement@UI@@QEAAXXZ");
        m.comboGet = (Int_t)RuntimeExport("?GetCurrentItem@ComboElement@UI@@QEBAHXZ");
        m.comboSet = (SetInt_t)RuntimeExport("?SetCurrentItem@ComboElement@UI@@QEAAXH@Z");
        m.spriteCtor = (Void_t)RuntimeExport("??0Sprite@UI@@QEAA@XZ");
        m.editGet = (GetText_t)RuntimeExport("?GetText@EditElement@UI@@QEBAAEBVString@System@@XZ");
        m.editSet = (SetText_t)RuntimeExport("?SetText@EditElement@UI@@QEAAXAEBVString@System@@@Z");
        m.numGet = (Int_t)RuntimeExport("?GetValue@NumberEditElement@UI@@QEBAHXZ");
        m.numSet = (SetInt_t)RuntimeExport("?SetValue@NumberEditElement@UI@@QEAAXH@Z");
        m.editActive = (Int_t)RuntimeExport("?IsActive@EditElement@UI@@QEBA_NXZ");
        m.close = (Close_t)RuntimeExport("?CloseDialog@UserInterface@UI@@QEAAXV?$InternalPtr@VDialogInstance@UI@@@Object@@@Z");
    }
    return &m;
}
static void TextDeep(void* e, const char* s, int depth) {
    if (!e || depth > 12) return;
    if (*(uintptr_t*)e == M()->textVt) SetParam(e, 0, s);
    int n = M()->count ? M()->count(e) : 0;
    for (int i = 0; i < n; i++) {
        void* c = nullptr;
        M()->child(e, &c, i);
        TextDeep(c, s, depth + 1);
    }
}
void SetColor(void* t, uint32_t argb) { if (t) (*(void(__fastcall**)(void*, uint32_t))(*(uint8_t**)t + 0x50))(t, argb); }
void SetTextDeep(void* e, const char* s) { if (M()->child && M()->count && M()->textVt) TextDeep(e, s, 0); }
void ComboSet(void* c, const std::vector<std::string>& items) {
    if (!c || !M()->comboAdd || !M()->comboClear || !M()->spriteCtor || !Ready()) return;
    M()->comboClear(c);
    for (auto& s : items) {
        alignas(16) uint8_t sprite[64] = {};
        M()->spriteCtor(sprite);
        void* str = nullptr;
        p_strCtor(&str, s.c_str());
        M()->comboAdd(c, sprite, &str, nullptr);
        p_strDtor(&str);
    }
}
int ComboGet(void* c) { return c && M()->comboGet ? M()->comboGet(c) : -1; }
void ComboSelect(void* c, int i) { if (c && M()->comboSet) M()->comboSet(c, i); }
std::string EditGet(void* e) {
    if (!e || !M()->editGet) return "";
    const char** s = M()->editGet(e);
    return s && *s ? *s : "";
}
void EditSet(void* e, const char* s) {
    if (!e || !M()->editSet || !Ready()) return;
    void* str = nullptr;
    p_strCtor(&str, s);
    // EditElement::SetText does nothing while the field has the keyboard (+0xe0 active, text +0xc8, cursor +0xe4):
    // drop the flag for the call, then put the cursor at the end (pasting into a focused field)
    uint8_t* el = (uint8_t*)e;
    bool active = el[0xe0] != 0;
    el[0xe0] = 0;
    M()->editSet(e, &str);
    if (active) {
        el[0xe0] = 1;
        *(int*)(el + 0xe4) = (int)strlen(s);
    }
    p_strDtor(&str);
}
int NumberGet(void* e) { return e && M()->numGet ? M()->numGet(e) : 0; }
void NumberSet(void* e, int v) { if (e && M()->numSet) M()->numSet(e, v); }
bool EditActive(void* e) { return e && M()->editActive && (M()->editActive(e) & 0xff); }
void* UiOfDialog(void* d) { return d ? *(void**)((uint8_t*)d + 0x10) : nullptr; }
void Close(void* ui, void* d) {
    if (!ui || !d || !M()->close) return;
    void* ref = d;
    M()->close(ui, &ref);
}

void SetParam(void* text, int index, const char* s) {
    if (!text || !Ready()) return;
    void* str = nullptr;
    p_strCtor(&str, s);
    p_setParam(text, index, &str);
    p_strDtor(&str);
}

}  // namespace gameui
