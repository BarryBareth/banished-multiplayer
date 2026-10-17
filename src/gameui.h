#pragma once
#include <string>
#include <vector>
#include <cstdint>
// Thin wrappers around the engine's UI exports (Runtime-steam-x64.dll), for the companion mod's elements that the DLL
// drives inside the game's own windows. Main thread only.
namespace gameui {
bool Ready();                                       // every export resolved
void* Find(void* element, const char* name);        // UI::Element::FindElement (a child, any depth); nullptr if none
void* FindInDialog(void* dialog, const char* name); // UI::DialogInstance::FindElement
void SetId(void* element, int id);
void Show(void* element, bool show);                // Element::Show(visible)
void Enable(void* element, bool on);                // virtual Element::Enable (greyed out when off)
void EnableEdit(void* edit, bool on);  // edit / number edit fields (Element::Enable export, not the button slot)
void SetChecked(void* check, bool on);              // CheckElement::EnableCheck
bool IsChecked(void* check);
void SetParam(void* text, int index, const char* s);  // TextElement::SetParameter (String copy)
void SetColor(void* text, uint32_t argb);         // a text element's colour (vtable +0x50, as the event log does)
void SetTextDeep(void* element, const char* s);     // parameter 0 of every text element in the subtree (buttons, checks)
// combo boxes
void ComboSet(void* combo, const std::vector<std::string>& items);  // replace the items
int ComboGet(void* combo);                          // current item (-1 none)
void ComboSelect(void* combo, int index);
// text / number edits
std::string EditGet(void* edit);
void EditSet(void* edit, const char* s);
int NumberGet(void* edit);
void NumberSet(void* edit, int v);
bool EditActive(void* edit);                        // has the keyboard focus (don't overwrite what's being typed)
// dialogs
void* UiOfDialog(void* dialog);                     // DialogInstance +0x10 = its UserInterface
void Close(void* ui, void* dialog);                 // UserInterface::CloseDialog
}
