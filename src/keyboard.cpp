// Typing in the game's text boxes with the player's own keyboard layout. UI::EditElement::SelectCharacter maps the
// engine's key index (DirectInput = physical key position) to a character with a fixed US table (Shift = the upper
// row), so on e.g. a Finnish keyboard Shift+. gives '>' instead of ':' and AltGr characters can't be typed at all.
// The hook turns the key index into its scan code and asks Windows what the active layout makes of it with the current
// Shift / AltGr state. Numpad keys, space and anything unknown keep the game's answer.
#include "common.h"
#include "MinHook.h"

namespace keyboard {

// engine key index -> PC scan code (set 1) of the same physical key, from SelectCharacter's US table
static uint8_t ScanOf(int key) {
    static const struct { int key; uint8_t scan; } kMap[] = {
        {0x0d, 0x0b}, {0x0e, 0x02}, {0x0f, 0x03}, {0x10, 0x04}, {0x11, 0x05}, {0x12, 0x06}, {0x13, 0x07}, {0x14, 0x08},
        {0x15, 0x09}, {0x16, 0x0a},                                                              // 0 1 .. 9
        {0x17, 0x1e}, {0x18, 0x30}, {0x19, 0x2e}, {0x1a, 0x20}, {0x1b, 0x12}, {0x1c, 0x21}, {0x1d, 0x22}, {0x1e, 0x23},
        {0x1f, 0x17}, {0x20, 0x24}, {0x21, 0x25}, {0x22, 0x26}, {0x23, 0x32}, {0x24, 0x31}, {0x25, 0x18}, {0x26, 0x19},
        {0x27, 0x10}, {0x28, 0x13}, {0x29, 0x1f}, {0x2a, 0x14}, {0x2b, 0x16}, {0x2c, 0x2f}, {0x2d, 0x11}, {0x2e, 0x2d},
        {0x2f, 0x15}, {0x30, 0x2c},                                                              // a .. z
        {0x31, 0x29}, {0x32, 0x0c}, {0x33, 0x0d}, {0x36, 0x1a}, {0x37, 0x1b}, {0x38, 0x2b},      // ` - = [ ] backslash
        {0x3a, 0x27}, {0x3b, 0x28}, {0x3f, 0x33}, {0x40, 0x34}, {0x41, 0x35},                    // ; ' , . /
    };
    for (auto& m : kMap) if (m.key == key) return m.scan;
    return 0;
}

using Select_t = char(__fastcall*)(const void* self, int key);
static Select_t o_Select;
static char __fastcall h_Select(const void* self, int key) {
    char game = o_Select(self, key);
    uint8_t scan = ScanOf(key);
    if (!scan) return game;
    HKL layout = GetKeyboardLayout(0);
    UINT vk = MapVirtualKeyExW(scan, MAPVK_VSC_TO_VK, layout);
    if (!vk) return game;
    BYTE ks[256] = {};
    if (GetKeyState(VK_SHIFT) < 0) ks[VK_SHIFT] = 0x80;
    if (GetKeyState(VK_CAPITAL) & 1) ks[VK_CAPITAL] = 0x01;
    if (GetKeyState(VK_RMENU) < 0) ks[VK_CONTROL] = ks[VK_MENU] = ks[VK_RMENU] = ks[VK_LCONTROL] = 0x80;  // AltGr
    wchar_t buf[4];
    int n = ToUnicodeEx(vk, scan, ks, buf, 4, 0x4 /* don't change the keyboard state (dead keys) */, layout);
    if (n != 1 || buf[0] < 0x20 || buf[0] > 0x7e) return n == 1 || n < 0 ? 0 : game;  // dead keys / non-ASCII: nothing
    if ((char)buf[0] != game) {
        static int logged;
        if (logged++ < 20) Log("keyboard: key %#x -> '%c' with this layout (the game's US table says '%c')", key, (char)buf[0], game ? game : '?');
    }
    return (char)buf[0];
}

void Install() {
    void* f = RuntimeExport("?SelectCharacter@EditElement@UI@@AEBADH@Z");
    if (!f || MH_CreateHook(f, (void*)h_Select, (void**)&o_Select) != MH_OK || MH_EnableHook(f) != MH_OK) {
        Log("!! keyboard: SelectCharacter hook failed - text boxes keep the US layout");
        return;
    }
    Log("keyboard: text boxes follow the keyboard layout (%08llx)", (unsigned long long)(uintptr_t)GetKeyboardLayout(0));
}

}  // namespace keyboard
