#pragma once
#include <string>
// Windows in the game's own UI, laid out by the companion mod (Dialog/Bmp*.rsc) and driven from the DLL: a
// UI::DialogInstance whose vtable is a copy of the engine's with OnUpdate (slot 7), OnElementEvent (slot 8) and the
// destructor (slot 0) routed to the Window object that opened it. Main thread only.
namespace gamedlg {
struct Window {
    virtual ~Window() {}
    virtual const char* Resource() const = 0;   // "Dialog/BmpX.rsc"
    virtual void Init() {}                       // just created (set ids, fixed texts, starting values)
    virtual void Refresh() {}                    // every frame while open
    virtual void OnEvent(int id, int evt) {}     // an element numbered by Id() was used (evt 0x12 click, 0x14 combo, 0x15 value)
    virtual void Closed() {}                     // the game destroyed it (closed by the player, session ended, ...)
    virtual int TitleButton() const { return 0; } // id of a button in the title bar: a press there also starts the window
                                                 // drag and no click (0x12) follows - press + release counts as the click

    bool Open(void* userInterface);              // false: the mod's resource is missing
    void Close();
    bool IsOpen() const { return dlg != nullptr; }
    void* Ui() const;                            // its UserInterface

    void* E(const char* name) const;             // element by name
    void Id(const char* name, int id) const;
    void Text(const char* name, const std::string& s) const;  // parameter 0 of every text in that element
    void Vis(const char* name, bool on) const;
    void Check(const char* name, bool on) const;
    void Enable(const char* name, bool on) const;

    void* dlg = nullptr;
};
void* GameUi();                                  // the running game's UserInterface (GameLoop+0x98), nullptr outside
}
