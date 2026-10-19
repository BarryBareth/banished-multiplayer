// In-game multiplayer UI (see overlay.h): Dear ImGui drawn from an IDXGISwapChain::Present hook.
#include <atomic>
#include <thread>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <shellapi.h>
#include "overlay.h"
#include "net.h"
#include "gameload.h"
#include "towns.h"
#include "gamewin.h"
#include "gameui.h"
#include "mpdialog.h"
#include "update.h"
#include "ingame.h"
#include "report.h"
#include "gamedlg.h"
#include "tint.h"
#include "presence.h"
#include "commands.h"
#include "autotest.h"
#include "common.h"
#include "MinHook.h"
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include <algorithm>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace overlay {

using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Resize_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static Present_t o_Present;
static Resize_t o_Resize;
static bool g_ready, g_failed;
static HWND g_hwnd;
static WNDPROC g_origWndProc;
static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static ID3D11RenderTargetView* g_rtv;
static volatile bool g_wantMouse;   // read by FilterMouse (input hook)
static float g_scale = 1.0f;

// UI state
static std::vector<std::string> g_saves;
static int g_saveIdx;
static DWORD g_savesAt;
static bool g_confirmLeave;
static bool g_showPlayers = true;
static int g_mode;                       // 0 = share one town (host's save), 1 = separate towns on a new map
static int g_sepSource;                  // separate towns: 0 = new map, 1 = continue a saved game
static gameload::NewGameOptions g_opts;
static int g_start = 1, g_size = 1, g_terrain, g_climate, g_seed;
static bool g_pickStart = true;
static bool g_showTrade, g_showMessages;
static int g_tradePartner = -1, g_giveIdx, g_askIdx, g_giveN = 10, g_askN = 10;
static DWORD g_settleSentAt;

// Ctrl+V / Shift+Insert as the game window receives them (mpdialog pastes; its own key polling missed the user's)
static std::atomic<int> g_pasteKey;
bool TakePasteKey() { return g_pasteKey.exchange(0) > 0; }
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if ((msg == WM_KEYDOWN && !(l & (1 << 30)) &&
         ((w == 'V' && GetKeyState(VK_CONTROL) < 0) || (w == VK_INSERT && GetKeyState(VK_SHIFT) < 0))) ||
        (msg == WM_CHAR && w == 0x16))
        g_pasteKey++;
    if (g_ready) ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
    return CallWindowProcW(g_origWndProc, h, msg, w, l);
}

void FilterMouse(uint8_t* state) {
    if (!state) return;
    if (presence::BlockLeftClick()) *(uint16_t*)(state + 4 * 0x18 + 4) = 0;  // Alt+click = ping: not a game click
    if (!g_wantMouse) return;
    for (int b = 4; b <= 12; b++) *(uint16_t*)(state + b * 0x18 + 4) = 0;  // buttons 4..11, wheel 12
}

static void Style() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 6;
    s.FrameRounding = 4;
    s.GrabRounding = 4;
    s.WindowBorderSize = 1;
    s.WindowPadding = ImVec2(14, 12);
    s.ItemSpacing = ImVec2(8, 8);
    s.FramePadding = ImVec2(10, 6);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.09f, 0.07f, 0.94f);   // dark wood, close to Banished's panels
    c[ImGuiCol_Border] = ImVec4(0.55f, 0.42f, 0.25f, 0.70f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = ImVec4(0.30f, 0.21f, 0.12f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.42f, 0.30f, 0.16f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.56f, 0.41f, 0.22f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.66f, 0.49f, 0.27f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.22f, 0.17f, 0.11f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.30f, 0.23f, 0.14f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.42f, 0.30f, 0.16f, 0.8f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.56f, 0.41f, 0.22f, 0.9f);
    c[ImGuiCol_Text] = ImVec4(0.95f, 0.91f, 0.82f, 1.0f);
    s.ScaleAllSizes(g_scale);
}

static bool InitImGui(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_dev))) { Log("!! overlay: the swap chain has no D3D11 device"); return false; }
    g_dev->GetImmediateContext(&g_ctx);
    DXGI_SWAP_CHAIN_DESC d;
    sc->GetDesc(&d);
    g_hwnd = d.OutputWindow;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_scale = (std::max)(1.0f, (rc.bottom - rc.top) / 1080.0f);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    char font[MAX_PATH];
    GetWindowsDirectoryA(font, MAX_PATH);
    strcat_s(font, "\\Fonts\\segoeui.ttf");
    if (GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES) io.Fonts->AddFontFromFileTTF(font, 18.0f * g_scale);
    else io.FontGlobalScale = g_scale;
    Style();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
    Log("overlay: ready (hwnd %p, %ldx%ld, scale %.2f, thread %lu)", g_hwnd, rc.right, rc.bottom, g_scale, GetCurrentThreadId());
    return true;
}

static void CreateRtv(IDXGISwapChain* sc) {
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
        g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
        bb->Release();
    }
}

// ---------------------------------------------------------------- panels
static ImVec4 kGood(0.55f, 0.85f, 0.45f, 1), kWarn(1.0f, 0.80f, 0.35f, 1), kBad(1.0f, 0.45f, 0.40f, 1), kDim(0.70f, 0.65f, 0.58f, 1);

static void Roster(const net::UiInfo& u) {
    for (auto& p : u.roster) {
        bool me = p.id == u.me;
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::Text("%s%s%s", p.name.c_str(), p.id == 0 ? "  (host)" : "", me ? "  (you)" : "");
        if (u.hosting && !u.started && p.id != 0) { ImGui::SameLine(); ImGui::TextColored(kGood, "joined"); }
        else if (u.started && !p.synced) { ImGui::SameLine(); ImGui::TextColored(kWarn, "loading..."); }
    }
}

// ---- connection settings (Steam / direct / relay), edited in place and saved on change
static bool g_settingsLoaded;
static int g_connMethod;
static int g_connPort = 7777;
static char g_relayBuf[128], g_nameBuf[64], g_joinBuf[128];

static void LoadConnSettings() {
    if (g_settingsLoaded) return;
    g_settingsLoaded = true;
    net::Settings s = net::GetSettings();
    g_connMethod = s.method;
    g_connPort = s.port;
    strcpy_s(g_relayBuf, s.relay.c_str());
    strcpy_s(g_nameBuf, s.name.c_str());
}

static void SaveConnSettings() {
    net::SetSettings(net::Settings{g_connMethod, g_connPort, g_relayBuf, g_nameBuf});
}

static void ConnectionSettings(bool steamOk) {
    LoadConnSettings();
    bool changed = false;
    ImGui::TextColored(kDim, "Connect through");
    changed |= ImGui::RadioButton("Steam", &g_connMethod, 0);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Direct (IP)", &g_connMethod, 1);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Relay server", &g_connMethod, 2);
    if (g_connMethod == 0) {
        if (!steamOk) ImGui::TextColored(kBad, "Steam is not running - choose Direct or Relay.");
        else ImGui::TextColored(kDim, "Other players join through Steam invites.");
    } else if (g_connMethod == 1) {
        ImGui::TextColored(kDim, "Port when hosting");
        ImGui::SameLine(150 * g_scale);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputInt("##port", &g_connPort, 0)) { if (g_connPort < 1 || g_connPort > 65535) g_connPort = 7777; changed = true; }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 420 * g_scale);
        ImGui::TextColored(kDim, "The host must forward this TCP port on their router to their PC.");
        ImGui::PopTextWrapPos();
    } else {
        ImGui::TextColored(kDim, "Relay address");
        ImGui::SameLine(150 * g_scale);
        ImGui::SetNextItemWidth(-1);
        changed |= ImGui::InputTextWithHint("##relay", "relay.example.com:7780", g_relayBuf, sizeof g_relayBuf);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 420 * g_scale);
        ImGui::TextColored(kDim, "Everybody uses the same relay (tools/bmp_relay.py on any PC or server with an open port).");
        ImGui::PopTextWrapPos();
    }
    if (g_connMethod != 0 || !steamOk) {
        ImGui::TextColored(kDim, "Your name");
        ImGui::SameLine(150 * g_scale);
        ImGui::SetNextItemWidth(-1);
        changed |= ImGui::InputTextWithHint("##name", steamOk ? "(your Steam name)" : "(your Windows name)", g_nameBuf, sizeof g_nameBuf);
    }
    if (changed) SaveConnSettings();
}

static void JoinByAddress() {
    if (g_connMethod == 0) {
        ImGui::TextColored(kDim, "Joining a game?");
        ImGui::TextWrapped("Accept their Steam invite, or right-click them in your Steam friends list and choose Join Game.");
        return;
    }
    ImGui::TextColored(kDim, g_connMethod == 1 ? "Join a game: the host's address (IP:port)" : "Join a game: the host's room code");
    ImGui::SetNextItemWidth(-90 * g_scale);
    bool enter = ImGui::InputTextWithHint("##join", g_connMethod == 1 ? "203.0.113.5:7777" : "ABC123", g_joinBuf, sizeof g_joinBuf,
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Join", ImVec2(-1, 0)) || enter) net::UiDo(net::kUiJoin, std::string(g_connMethod == 1 ? "D|" : "R|") + g_joinBuf);
}

// The main menu's "Multiplayer" button (companion mod: MainMenu.rsc buttonMultiplayer, element id 77) opens the window.
// MainMenuDialog vf4 (exe+0xb91a0) finds its buttons by name and numbers them; vf8 (exe+0xb9020) gets the clicks.
static bool g_menuOpen;          // the player opened the multiplayer window
static bool g_menuButton;        // the main menu has our button (else a small fallback button is drawn)
static void* g_menuDialog;       // the main menu (its UserInterface hosts the multiplayer window)
static bool g_nativeMenu = true; // the game-styled window works (else the ImGui panel below)
static const int kMenuButtonId = 77;
using MenuInit_t = void(__fastcall*)(void* dialog);
using MenuEvent_t = void(__fastcall*)(void* dialog, void* elem, int evt, int id);
static MenuInit_t o_MenuInit;
static MenuEvent_t o_MenuEvent;
static void __fastcall h_MenuInit(void* dialog) {
    o_MenuInit(dialog);
    using StrCtor_t = void*(__fastcall*)(void* self, const char* s);
    using StrDtor_t = void(__fastcall*)(void* self);
    using Find_t = void**(__fastcall*)(void* dialog, void** out, const void* name);
    using SetId_t = void(__fastcall*)(void* elem, int id);
    static auto strCtor = (StrCtor_t)RuntimeExport("??0String@System@@QEAA@PEBD@Z");
    static auto strDtor = (StrDtor_t)RuntimeExport("??1String@System@@QEAA@XZ");
    static auto find = (Find_t)RuntimeExport("?FindElement@DialogInstance@UI@@QEAA?AV?$InternalPtr@VElement@UI@@@Object@@AEBVString@System@@@Z");
    static auto setId = (SetId_t)RuntimeExport("?SetElementId@Element@UI@@QEAAXH@Z");
    if (!strCtor || !strDtor || !find || !setId) return;
    void* name = nullptr;
    strCtor(&name, "buttonMultiplayer");
    void* elem = nullptr;
    find(dialog, &elem, &name);
    strDtor(&name);
    g_menuDialog = dialog;
    // the companion mod's "this mod needs the DLL" panel: we are the DLL
    if (void* notice = gameui::FindInDialog(dialog, "bmpNeedsDll")) gameui::Show(notice, false);
    if (elem) {
        setId(elem, kMenuButtonId);
        if (!g_menuButton) Log("overlay: main menu has the Multiplayer button");
        g_menuButton = true;
    }
}
static void __fastcall h_MenuEvent(void* dialog, void* elem, int evt, int id) {
    if (evt == 0x12 && id == kMenuButtonId) {
        if (!g_nativeMenu || !mpdialog::Open(gameui::UiOfDialog(dialog))) { g_nativeMenu = false; g_menuOpen = true; }
        return;
    }
    o_MenuEvent(dialog, elem, evt, id);
}

static void MenuPanel(const net::UiInfo& u) {
    ImGuiIO& io = ImGui::GetIO();
    if (g_nativeMenu && g_menuDialog) {  // the game-styled window (companion mod) - opened by the button or a session
        if (u.active && !mpdialog::IsOpen() && !mpdialog::Open(gameui::UiOfDialog(g_menuDialog))) g_nativeMenu = false;
        if (g_nativeMenu) return;
    }
    if (!u.active && !g_menuOpen) {
        if (!g_menuButton) {  // no companion mod: a small button opens the window
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 30 * g_scale, io.DisplaySize.y - 30 * g_scale), ImGuiCond_Always, ImVec2(1, 1));
            ImGui::Begin("##mpopen", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
            if (ImGui::Button("Multiplayer")) g_menuOpen = true;
            ImGui::End();
        }
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 30 * g_scale, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(1, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(360 * g_scale, 0), ImVec2(520 * g_scale, FLT_MAX));
    bool open = true;
    ImGui::Begin("Multiplayer", u.active ? nullptr : &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    if (!open) g_menuOpen = false;
    if (!u.active) {
        LoadConnSettings();
        bool canHost = g_connMethod != 0 || u.steamOk;
        {
            if (GetTickCount() - g_savesAt > 2000 || g_saves.empty()) {
                std::string cur = g_saveIdx < (int)g_saves.size() ? g_saves[g_saveIdx] : "";
                g_saves = net::UiSaves();
                g_savesAt = GetTickCount();
                auto it = std::find(g_saves.begin(), g_saves.end(), cur);
                g_saveIdx = it == g_saves.end() ? 0 : (int)(it - g_saves.begin());
            }
            ImGui::TextUnformatted("Play Banished together with other players.");
            ImGui::Spacing();
            ImGui::RadioButton("Share one town", &g_mode, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Separate towns", &g_mode, 1);
            ImGui::Spacing();
            if (g_mode == 0) {
                ImGui::TextColored(kDim, "Everybody runs the same town together.");
                if (g_saves.empty()) {
                    ImGui::TextColored(kWarn, "Save a game first - the host shares a saved town.");
                } else {
                    ImGui::TextColored(kDim, "Town to share");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::BeginCombo("##save", g_saves[g_saveIdx].c_str())) {
                        for (int i = 0; i < (int)g_saves.size(); i++)
                            if (ImGui::Selectable(g_saves[i].c_str(), i == g_saveIdx)) g_saveIdx = i;
                        ImGui::EndCombo();
                    }
                    ImGui::BeginDisabled(!canHost);
                    if (ImGui::Button("Host game", ImVec2(-1, 0))) net::UiDo(net::kUiHost, "S|" + g_saves[g_saveIdx]);
                    ImGui::EndDisabled();
                }
            } else {
                ImGui::RadioButton("New map", &g_sepSource, 0);
                ImGui::SameLine();
                ImGui::RadioButton("Continue a saved game", &g_sepSource, 1);
                if (g_sepSource == 1) {
                    ImGui::TextColored(kDim, "Load a saved separate-towns game: the host keeps town 1, the others get");
                    ImGui::TextColored(kDim, "towns 2, 3... in the order they join (a one-town save: they settle new towns).");
                    if (g_saves.empty()) {
                        ImGui::TextColored(kWarn, "No saves yet.");
                    } else {
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::BeginCombo("##sepsave", g_saves[g_saveIdx].c_str())) {
                            for (int i = 0; i < (int)g_saves.size(); i++)
                                if (ImGui::Selectable(g_saves[i].c_str(), i == g_saveIdx)) g_saveIdx = i;
                            ImGui::EndCombo();
                        }
                        ImGui::BeginDisabled(!canHost);
                        if (ImGui::Button("Host game", ImVec2(-1, 0))) net::UiDo(net::kUiHost, "S|" + g_saves[g_saveIdx]);
                        ImGui::EndDisabled();
                    }
                } else {
                    ImGui::TextColored(kDim, "Each player builds their own town on one new map.");
                    if (g_opts.starts.empty()) g_opts = gameload::GetNewGameOptions();
                    if (!g_seed) g_seed = (int)(GetTickCount() % 999999) + 1;
                    auto combo = [](const char* label, int* v, const std::vector<std::string>& items) {
                        if (items.empty()) return;
                        if (*v >= (int)items.size()) *v = 0;
                        ImGui::TextColored(kDim, "%s", label);
                        ImGui::SameLine(130 * g_scale);
                        ImGui::SetNextItemWidth(-1);
                        std::string id = std::string("##") + label;
                        if (ImGui::BeginCombo(id.c_str(), items[*v].c_str())) {
                            for (int i = 0; i < (int)items.size(); i++)
                                if (ImGui::Selectable(items[i].c_str(), i == *v)) *v = i;
                            ImGui::EndCombo();
                        }
                    };
                    combo("Start", &g_start, g_opts.starts);
                    combo("Map size", &g_size, g_opts.sizes);
                    combo("Terrain", &g_terrain, g_opts.terrains);
                    combo("Climate", &g_climate, g_opts.climates);
                    ImGui::TextColored(kDim, "Map seed");
                    ImGui::SameLine(130 * g_scale);
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputInt("##seed", &g_seed);
                    ImGui::Checkbox("Players choose where their town starts", &g_pickStart);
                    ImGui::BeginDisabled(!canHost);
                    if (ImGui::Button("Host game", ImVec2(-1, 0))) {
                        char args[96];
                        sprintf_s(args, "N|%d,%d,%d,%d,%d|%d", g_seed, g_size, g_start, g_terrain, g_climate, g_pickStart ? 1 : 0);
                        net::UiDo(net::kUiHost, args);
                    }
                    ImGui::EndDisabled();
                }
            }
            ImGui::Separator();
            JoinByAddress();
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Connection settings", g_connMethod != 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0))
                ConnectionSettings(u.steamOk);
        }
    } else if (u.hosting && !u.started) {
        if (u.separate) ImGui::TextUnformatted("Hosting  separate towns on a new map");
        else ImGui::Text("Hosting  %s", u.hostSave.c_str());
        ImGui::Separator();
        ImGui::TextColored(kDim, "Players");
        Roster(u);
        ImGui::Spacing();
        if (u.hostVia == 0) {
            if (!u.lobby) ImGui::TextColored(kWarn, "Creating Steam lobby...");
            if (ImGui::Button("Invite players", ImVec2(-1, 0))) net::UiDo(net::kUiInvite);
        } else if (u.hostVia == 1) {
            ImGui::TextWrapped("Players choose Direct (IP) and join your public IP address with port %d.", u.listenPort);
            ImGui::TextColored(kDim, "Forward TCP port %d on your router to this PC.", u.listenPort);
        } else if (u.hostVia == 2) {
            if (!u.relayUp) ImGui::TextColored(kWarn, "Connecting to the relay...");
            else {
                ImGui::Text("Room code:  %s", u.room.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(u.room.c_str());
                ImGui::TextColored(kDim, "Players choose Relay server (same address) and join with this code.");
            }
        }
        int friends = (int)u.roster.size() - 1;
        std::string start = friends > 0 ? "Start game (" + std::to_string(u.roster.size()) + " players)" : "Start game alone";
        if (ImGui::Button(start.c_str(), ImVec2(-1, 0))) net::UiDo(net::kUiStart);
        if (ImGui::Button("Cancel", ImVec2(-1, 0))) net::UiDo(net::kUiLeave);
    } else if (!u.started) {
        if (u.roster.empty()) ImGui::TextUnformatted("Connecting...");
        else {
            ImGui::TextUnformatted("Connected to the host.");
            ImGui::TextColored(kDim, "Waiting for the host to start...");
        }
        ImGui::Separator();
        Roster(u);
        if (ImGui::Button("Leave", ImVec2(-1, 0))) net::UiDo(net::kUiLeave);
    } else {
        ImGui::TextUnformatted("Loading the shared town...");
        Roster(u);
    }
    if (!u.status.empty()) {
        ImGui::Separator();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 400 * g_scale);
        ImGui::TextColored(kDim, "%s", u.status.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::TextColored(kDim, "Mod version %s", u.version.c_str());
    ImGui::End();
}

static void GameBar(const net::UiInfo& u) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 8 * g_scale), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::Begin("##mpbar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing);
    bool desync = u.status.find("Out of sync") == 0;
    ImGui::TextColored(u.resyncing ? kWarn : desync ? kBad : kGood, u.resyncing ? "SYNCING" : desync ? "OUT OF SYNC" : "MULTIPLAYER");
    ImGui::SameLine();
    if (u.towns > 1) {
        ImGui::Text("Town %d of %d  -  %d players", u.town + 1, u.towns, (int)u.roster.size());
        for (int t = 0; t < u.towns; t++) {  // colour legend of the other towns (their buildings are tinted)
            if (t == u.town) continue;
            float rgb[3];
            tint::ColorOf(t, rgb);
            ImGui::SameLine();
            std::string who = "Town " + std::to_string(t + 1);
            for (auto& p : u.roster)
                if (net::TownOfPlayer(p.id) == t) who = p.name + " (town " + std::to_string(t + 1) + ")";
            ImGui::PushID(t);
            if (ImGui::ColorButton("##town", ImVec4(rgb[0], rgb[1], rgb[2], 1), ImGuiColorEditFlags_NoTooltip,
                                   ImVec2(14 * g_scale, 14 * g_scale)))
                tint::SetEnabled(!tint::Enabled());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s - their buildings and fields are outlined in this colour.\nClick to turn the colours %s.", who.c_str(),
                                  tint::Enabled() ? "off" : "on");
            ImGui::PopID();
        }
    }
    else ImGui::Text("%d players", (int)u.roster.size());
    if (u.paused) { ImGui::SameLine(); ImGui::TextColored(kWarn, "paused"); }
    ImGui::SameLine();
    if (ImGui::SmallButton(g_showPlayers ? "Hide players" : "Players")) g_showPlayers = !g_showPlayers;
    if (u.hosting) { ImGui::SameLine(); if (ImGui::SmallButton("Invite")) net::UiDo(net::kUiInvite); }
    if (u.towns > 1) { ImGui::SameLine(); if (ImGui::SmallButton("Trade")) g_showTrade = !g_showTrade; }
    if (u.towns > 1 && !gamewin::HasLogButtons()) { ImGui::SameLine(); if (ImGui::SmallButton("Messages")) g_showMessages = !g_showMessages; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Resync")) net::UiDo(net::kUiResync);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Everybody reloads the host's game (fixes a desync)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Leave")) g_confirmLeave = true;
    if (g_showPlayers) Roster(u);
    if (u.resyncing || desync || (!u.status.empty() && u.status.find("Hosting") != 0 && u.status.find("Playing") != 0)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 520 * g_scale);
        ImGui::TextColored(desync ? kBad : kWarn, "%s", u.status.c_str());
        ImGui::PopTextWrapPos();
    }
    if (g_confirmLeave) {
        ImGui::Separator();
        ImGui::TextUnformatted(u.hosting ? "End the game for everybody?" : "Leave this game? (your town keeps running alone)");
        if (ImGui::Button("Yes, leave")) { net::UiDo(net::kUiLeave); g_confirmLeave = false; }
        ImGui::SameLine();
        if (ImGui::Button("Stay")) g_confirmLeave = false;
    }
    ImGui::End();
}

static void GoodsCombo(const char* id, int* idx, const std::vector<towns::Good>& goods) {
    if (goods.empty()) { ImGui::TextColored(kDim, "(nothing in storage)"); return; }
    if (*idx >= (int)goods.size()) *idx = 0;
    char cur[96];
    sprintf_s(cur, "%s  (%d)", commands::DescName(goods[*idx].desc).c_str(), goods[*idx].count);
    ImGui::SetNextItemWidth(220 * g_scale);
    if (ImGui::BeginCombo(id, cur)) {
        for (int i = 0; i < (int)goods.size(); i++) {
            char item[96];
            sprintf_s(item, "%s  (%d)", commands::DescName(goods[i].desc).c_str(), goods[i].count);
            if (ImGui::Selectable(item, i == *idx)) *idx = i;
        }
        ImGui::EndCombo();
    }
}

// standing trades the local town is part of: what goes where, how often, how the last delivery went, Cancel
static int g_tradePeriod;  // 0 once, 1 every season, 2 every year
static std::string AmountOf(int n, void* desc) { return desc && n ? std::to_string(n) + " " + commands::DescName(desc) : "nothing"; }
static void StandingTrades() {
    auto list = towns::Agreements();
    bool header = false;
    for (auto& g : list) {
        int me = towns::Local();
        if (g.a != me && g.b != me) continue;
        if (!header) { ImGui::Separator(); ImGui::TextColored(kDim, "Standing trades"); header = true; }
        bool iAmA = g.a == me;  // a gives nA of descA
        std::string give = iAmA ? AmountOf(g.nA, g.descA) : AmountOf(g.nB, g.descB);
        std::string get = iAmA ? AmountOf(g.nB, g.descB) : AmountOf(g.nA, g.descA);
        ImGui::Text("You give %s, you get %s - every %s", give.c_str(), get.c_str(), g.period == 1 ? "season" : "year");
        ImGui::SameLine();
        ImGui::PushID(g.id);
        if (ImGui::SmallButton("Cancel")) commands::RequestEndAgreement(g.id);
        ImGui::PopID();
        if (g.lastOk == 0) ImGui::TextColored(kWarn, "   last time skipped: not enough goods on one side");
        else if (g.lastOk == 1) ImGui::TextColored(kDim, "   last delivery made");
    }
}

static void TradeWindow(const net::UiInfo& u) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 90 * g_scale), ImGuiCond_Appearing, ImVec2(0.5f, 0));
    if (!ImGui::Begin("Trade", &g_showTrade, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    std::vector<const net::UiPlayer*> others;
    for (auto& p : u.roster) if (p.id != u.me) others.push_back(&p);
    if (others.empty()) { ImGui::TextUnformatted("Nobody to trade with."); ImGui::End(); return; }
    if (g_tradePartner < 0 || std::none_of(others.begin(), others.end(), [](auto* p) { return p->id == g_tradePartner; }))
        g_tradePartner = others[0]->id;
    ImGui::TextColored(kDim, "Trade with");
    for (auto* p : others) {
        ImGui::SameLine();
        if (ImGui::RadioButton(p->name.c_str(), g_tradePartner == p->id)) g_tradePartner = p->id;
    }
    auto mine = towns::Goods(towns::Local());
    auto theirs = towns::Goods(net::TownOfPlayer(g_tradePartner));
    ImGui::Separator();
    ImGui::TextColored(kDim, "You give");
    GoodsCombo("##give", &g_giveIdx, mine);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * g_scale);
    ImGui::InputInt("##giveN", &g_giveN, 5, 50);
    if (g_giveN < 0) g_giveN = 0;
    ImGui::TextColored(kDim, "You get");
    GoodsCombo("##ask", &g_askIdx, theirs);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * g_scale);
    ImGui::InputInt("##askN", &g_askN, 5, 50);
    if (g_askN < 0) g_askN = 0;
    std::string give = mine.empty() || !g_giveN ? "" : commands::DescPath(mine[g_giveIdx < (int)mine.size() ? g_giveIdx : 0].desc);
    std::string ask = theirs.empty() || !g_askN ? "" : commands::DescPath(theirs[g_askIdx < (int)theirs.size() ? g_askIdx : 0].desc);
    ImGui::TextColored(kDim, "How often");
    ImGui::RadioButton("Once", &g_tradePeriod, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Every season", &g_tradePeriod, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Every year", &g_tradePeriod, 2);
    bool valid = (!give.empty() || !ask.empty()) && (give.empty() || g_tradePeriod || g_giveN <= mine[g_giveIdx].count);
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button(g_tradePeriod ? "Propose standing trade" : "Send offer", ImVec2(-1, 0)))
        net::SendOffer(g_tradePartner, give, give.empty() ? 0 : g_giveN, ask, ask.empty() ? 0 : g_askN, g_tradePeriod);
    ImGui::EndDisabled();
    ImGui::TextColored(kDim, "Leave an amount at 0 to give or ask for nothing in return.");
    if (g_tradePeriod)
        ImGui::TextColored(kDim, "First delivery when accepted, then at the start of every %s until one of you cancels.",
                           g_tradePeriod == 1 ? "season" : "year");
    StandingTrades();
    std::string news = net::TradeNews();
    if (!news.empty()) ImGui::TextColored(kWarn, "%s", news.c_str());
    auto deliveries = towns::Deliveries();
    bool header = false, myStuck = false;
    for (auto& d : deliveries) {
        if (d.from != towns::Local() && d.to != towns::Local()) continue;
        if (!header) { ImGui::Separator(); ImGui::TextColored(kDim, "Waiting to be carried"); header = true; }
        if (d.from == towns::Local())
            ImGui::Text("%d %s: your villagers take it over", d.count, commands::DescName(d.desc).c_str());
        else
            ImGui::Text("%d %s: on its way to you", d.count, commands::DescName(d.desc).c_str());
        if (d.stuck) {
            ImGui::SameLine();
            ImGui::TextColored(kWarn, "(no walking route)");
            myStuck = myStuck || d.from == towns::Local();
        }
    }
    if (myStuck) {
        ImGui::TextColored(kDim, "Your villagers can't walk to the other town (river or cliffs).");
        ImGui::TextColored(kDim, "Build a bridge, or hand the goods over directly:");
        if (ImGui::Button("Hand over directly", ImVec2(-1, 0))) commands::RequestDeliverNow();
    }
    ImGui::End();
}

// Shared storage: while the game window of one of my barns / stockpiles is open, a small panel sits on top of it with
// a "Share with <player>" checkbox per player (that one building only).
static void StoragePanel(const net::UiInfo& u) {
    if (u.towns < 2) return;
    float x = 0, y = 0;
    void* storage = commands::OpenStorageWindow(&x, &y);
    if (!storage) return;
    std::vector<const net::UiPlayer*> others;
    for (auto& p : u.roster) if (p.id != u.me && net::TownOfPlayer(p.id) >= 0) others.push_back(&p);
    if (others.empty()) return;
    ImGui::SetNextWindowPos(ImVec2(x, y - 4 * g_scale), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::Begin("Share this storage##bmpStorage", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoMove);
    for (auto* p : others) {
        int town = net::TownOfPlayer(p->id);
        bool on = towns::SharedWith(storage, town);
        std::string label = "Share with " + p->name + "##share" + std::to_string(p->id);
        if (ImGui::Checkbox(label.c_str(), &on)) commands::RequestShareStorage(storage, town, on);
    }
    ImGui::TextColored(kDim, "Their villagers may store and take goods here.");
    ImGui::End();
}

// Shared workplaces: while the game window of one of my workplaces is open, a panel above it lets other players' towns
// staff it (their villagers of that profession get hired there; what they make is mine).
static void WorkplacePanel(const net::UiInfo& u) {
    float x = 0, y = 0;
    if (gamewin::HasWorkButton()) return;  // the mod's Share check is in the game's window
    void* building = commands::OpenWorkplaceWindow(&x, &y);
    if (!building) return;
    std::vector<const net::UiPlayer*> others;
    for (auto& p : u.roster) if (p.id != u.me && net::TownOfPlayer(p.id) >= 0) others.push_back(&p);
    if (others.empty()) return;
    ImGui::SetNextWindowPos(ImVec2(x, y - 4 * g_scale), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::Begin("Shared workplace##bmpWork", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoMove);
    for (auto* p : others) {
        int town = net::TownOfPlayer(p->id);
        bool on = towns::WorkSharedWith(building, town);
        std::string label = "Let " + p->name + "'s villagers work here##work" + std::to_string(p->id);
        if (ImGui::Checkbox(label.c_str(), &on)) commands::RequestShareWork(building, town, on);
    }
    int foreign = towns::ForeignWorkersAt(building);
    if (foreign) ImGui::TextColored(kDim, "%d of their villagers work here now.", foreign);
    ImGui::TextColored(kDim, "They fill free places (your worker limit); what they make is yours.");
    ImGui::End();
}

// Messages: the game's event log with the town each message came from; "Mine" also hides other towns' popups.
static std::string TownName(const net::UiInfo& u, int t) {
    if (t < 0) return "?";
    if (t == u.town) return "You";
    for (auto& p : u.roster)
        if (net::TownOfPlayer(p.id) == t) return p.name;
    return "Town " + std::to_string(t + 1);
}
static void MessagesWindow(const net::UiInfo& u) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 30 * g_scale, 120 * g_scale), ImGuiCond_FirstUseEver, ImVec2(1, 0));
    ImGui::SetNextWindowSize(ImVec2(460 * g_scale, 340 * g_scale), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Messages##bmpMessages", &g_showMessages, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    bool mine = towns::EventsMineOnly();
    if (ImGui::RadioButton("Only mine", mine)) towns::SetEventsMineOnly(true);
    ImGui::SameLine();
    if (ImGui::RadioButton("Everyone's", !mine)) towns::SetEventsMineOnly(false);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Also decides whose messages pop up in the game.");
    ImGui::Separator();
    ImGui::BeginChild("##list");
    int shown = 0;
    for (auto& e : towns::Events(500)) {
        if (mine && e.town >= 0 && e.town != u.town) continue;
        float rgb[3] = {0.85f, 0.85f, 0.85f};
        if (e.town >= 0 && e.town != u.town) tint::ColorOf(e.town, rgb);
        ImGui::PushID(e.index);
        ImGui::TextColored(ImVec4(rgb[0], rgb[1], rgb[2], 1), "[%s]", TownName(u, e.town).c_str());
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(e.text);
        ImGui::PopTextWrapPos();
        if (e.cell >= 0 && ImGui::IsItemHovered()) ImGui::SetTooltip("Click to look at it");
        if (e.cell >= 0 && ImGui::IsItemClicked()) towns::FocusCell(e.cell);
        ImGui::PopID();
        shown++;
    }
    if (!shown) ImGui::TextColored(kDim, "No messages yet.");
    ImGui::EndChild();
    ImGui::End();
}

static void OfferPopups(const net::UiInfo& u) {
    auto offers = net::IncomingOffers();
    if (offers.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 30 * g_scale, 120 * g_scale), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::Begin("Trade offers", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    for (auto& o : offers) {
        std::string who = "A player";
        for (auto& p : u.roster) if (p.id == o.from) who = p.name;
        void* gd = o.give == "-" ? nullptr : commands::DescFromPath(o.give);
        void* ad = o.ask == "-" ? nullptr : commands::DescFromPath(o.ask);
        std::string gives = gd ? std::to_string(o.nGive) + " " + commands::DescName(gd) : "nothing";
        std::string wants = ad ? std::to_string(o.nAsk) + " " + commands::DescName(ad) : "nothing";
        ImGui::Text("%s offers %s", who.c_str(), gives.c_str());
        ImGui::Text("for %s%s", wants.c_str(), o.period == 1 ? " - every season" : o.period == 2 ? " - every year" : "");
        if (ad) ImGui::TextColored(kDim, "(you have %d)", towns::CountOf(towns::Local(), ad));
        ImGui::PushID(o.id);
        if (ImGui::Button("Accept")) net::AnswerOffer(o.id, true);
        ImGui::SameLine();
        if (ImGui::Button("Decline")) net::AnswerOffer(o.id, false);
        ImGui::PopID();
        ImGui::Separator();
    }
    ImGui::End();
}

static void SettlePanel(bool markerOnly) {
    ImGuiIO& io = ImGui::GetIO();
    int x = 0, y = 0;
    const char* why = "";
    bool ok = towns::CameraCell(&x, &y) && towns::CanSettle(towns::Local(), x, y, &why);
    // marker at the screen centre = the camera's look-at point
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    ImU32 col = ok ? IM_COL32(120, 230, 90, 230) : IM_COL32(240, 90, 80, 230);
    float r = 26 * g_scale;
    dl->AddCircle(c, r, col, 32, 3 * g_scale);
    dl->AddLine(ImVec2(c.x - r * 1.6f, c.y), ImVec2(c.x - r * 0.5f, c.y), col, 3 * g_scale);
    dl->AddLine(ImVec2(c.x + r * 0.5f, c.y), ImVec2(c.x + r * 1.6f, c.y), col, 3 * g_scale);
    dl->AddLine(ImVec2(c.x, c.y - r * 1.6f), ImVec2(c.x, c.y - r * 0.5f), col, 3 * g_scale);
    dl->AddLine(ImVec2(c.x, c.y + r * 0.5f), ImVec2(c.x, c.y + r * 1.6f), col, 3 * g_scale);
    if (markerOnly) return;  // the game-styled window (ingame.cpp) has the text and the button
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 40 * g_scale), ImGuiCond_Always, ImVec2(0.5f, 1));
    ImGui::Begin("Choose your town's location", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    ImGui::TextUnformatted("Move the camera until the marker is where your town should start.");
    ImGui::TextColored(kDim, "Map cell %d, %d", x, y);
    if (ok) ImGui::TextColored(kGood, "Good spot.");
    else ImGui::TextColored(kBad, "Can't settle here: %s", why);
    bool waiting = g_settleSentAt && GetTickCount() - g_settleSentAt < 4000;
    ImGui::BeginDisabled(!ok || waiting);
    if (ImGui::Button(waiting ? "Settling..." : "Settle here", ImVec2(-1, 0))) {
        commands::RequestSettle(x, y);
        g_settleSentAt = GetTickCount();
    }
    ImGui::EndDisabled();
    ImGui::End();
}

// Other towns' buildings and fields: outline + light fill in their town's colour, projected from the world.
// The outlines are drawn at Present, after the game's own UI - so they're cut around every open game window
// (UserInterface GameLoop+0x98 list; DialogInstance +0x20 bit 2 = hidden, +0x50 root element, whose +0x68/+0x6c/+0x70/
// +0x74 = left/top/right/bottom in UI units, / UserInterface+0xdc (the UI scale) = pixels - as DialogInstance::Contains reads them).
static void GameWindowRects(std::vector<ImVec4>& out, float screenW, float screenH) {
    out.clear();
    uint8_t* gl = (uint8_t*)ProbeGameLoop();
    uint8_t* ui = gl ? *(uint8_t**)(gl + 0x98) : nullptr;
    if (!ui) return;
    float scale = *(float*)(ui + 0xdc) > 0 ? 1.0f / *(float*)(ui + 0xdc) : 1.0f;  // UI units -> pixels (UI scale 0.9 = units x 1.11)
    int guard = 0;
    for (uint8_t* node = *(uint8_t**)ui; node && guard < 200; node = *(uint8_t**)(node + 0x10), guard++) {
        uint8_t* dlg = *(uint8_t**)node;
        if (!dlg || (*(uint32_t*)(dlg + 0x20) & 4)) continue;
        uint8_t* root = *(uint8_t**)(dlg + 0x50);
        if (!root) continue;
        ImVec4 r(*(int*)(root + 0x68) * scale, *(int*)(root + 0x6c) * scale, *(int*)(root + 0x70) * scale, *(int*)(root + 0x74) * scale);
        if (r.z <= r.x || r.w <= r.y) continue;
        if ((r.z - r.x) * (r.w - r.y) > 0.5f * screenW * screenH) continue;  // a full-screen container, not a window
        out.push_back(r);
    }
}
using Poly = std::vector<ImVec2>;
// Sutherland-Hodgman against one axis-aligned half-plane: keep (axis value < v) if less, else (> v)
static Poly ClipHalf(const Poly& p, int axis, float v, bool less) {
    Poly out;
    auto in = [&](const ImVec2& q) { float c = axis ? q.y : q.x; return less ? c <= v : c >= v; };
    for (size_t i = 0; i < p.size(); i++) {
        const ImVec2 &a = p[i], &b = p[(i + 1) % p.size()];
        bool ia = in(a), ib = in(b);
        if (ia) out.push_back(a);
        if (ia != ib) {
            float ca = axis ? a.y : a.x, cb = axis ? b.y : b.x, t = (v - ca) / (cb - ca);
            out.push_back(ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t));
        }
    }
    return out;
}
// convex polygon minus a rectangle = up to 4 convex pieces (left, right, above, below)
static void SubtractRect(const Poly& p, const ImVec4& r, std::vector<Poly>& out) {
    Poly pieces[4] = {ClipHalf(p, 0, r.x, true), ClipHalf(p, 0, r.z, false)};
    Poly mid = ClipHalf(ClipHalf(p, 0, r.x, false), 0, r.z, true);
    pieces[2] = ClipHalf(mid, 1, r.y, true);
    pieces[3] = ClipHalf(mid, 1, r.w, false);
    for (auto& q : pieces) if (q.size() >= 3) out.push_back(q);
}
// the parts of segment a-b outside every rectangle (Liang-Barsky for the part inside each, then the rest)
static void SegmentOutside(ImVec2 a, ImVec2 b, const std::vector<ImVec4>& rects, size_t from, std::vector<std::pair<ImVec2, ImVec2>>& out) {
    for (size_t k = from; k < rects.size(); k++) {
        const ImVec4& r = rects[k];
        float t0 = 0, t1 = 1, dx = b.x - a.x, dy = b.y - a.y;
        float pp[4] = {-dx, dx, -dy, dy}, qq[4] = {a.x - r.x, r.z - a.x, a.y - r.y, r.w - a.y};
        bool hit = true;
        for (int i = 0; i < 4 && hit; i++) {
            if (pp[i] == 0) { if (qq[i] < 0) hit = false; continue; }
            float t = qq[i] / pp[i];
            if (pp[i] < 0) { if (t > t1) hit = false; else if (t > t0) t0 = t; }
            else { if (t < t0) hit = false; else if (t < t1) t1 = t; }
        }
        if (!hit || t1 <= t0) continue;
        ImVec2 p0(a.x + dx * t0, a.y + dy * t0), p1(a.x + dx * t1, a.y + dy * t1);
        if (t0 > 0) SegmentOutside(a, p0, rects, k + 1, out);
        if (t1 < 1) SegmentOutside(p1, b, rects, k + 1, out);
        return;
    }
    out.push_back({a, b});
}

static void TownColours(const net::UiInfo& u) {
    if (!u.inGame || u.towns < 2 || !tint::Enabled()) return;
    tint::OnPresent();
    static std::vector<towns::Footprint> prints;
    static int frame;
    if (frame++ % 15 == 0) towns::OtherTownFootprints(prints);
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    static std::vector<ImVec4> windows;
    GameWindowRects(windows, io.DisplaySize.x, io.DisplaySize.y);
    for (auto& f : prints) {
        float corners[4][3] = {{f.x0, f.y0, f.z + 0.05f}, {f.x1, f.y0, f.z + 0.05f}, {f.x1, f.y1, f.z + 0.05f}, {f.x0, f.y1, f.z + 0.05f}};
        ImVec2 s[4];
        bool ok = true;
        for (int k = 0; k < 4 && ok; k++) ok = tint::WorldToScreen(corners[k], io.DisplaySize.x, io.DisplaySize.y, &s[k].x, &s[k].y);
        if (!ok) continue;
        float rgb[3];
        tint::ColorOf(f.town, rgb);
        int r = (int)(rgb[0] * 255), g = (int)(rgb[1] * 255), b = (int)(rgb[2] * 255);
        ImU32 fill = IM_COL32(r, g, b, 45), line = IM_COL32(r, g, b, 210);
        float minX = (std::min)({s[0].x, s[1].x, s[2].x, s[3].x}), maxX = (std::max)({s[0].x, s[1].x, s[2].x, s[3].x});
        float minY = (std::min)({s[0].y, s[1].y, s[2].y, s[3].y}), maxY = (std::max)({s[0].y, s[1].y, s[2].y, s[3].y});
        std::vector<ImVec4> over;  // windows this footprint touches
        for (auto& w : windows) if (w.x < maxX && w.z > minX && w.y < maxY && w.w > minY) over.push_back(w);
        if (over.empty()) {
            dl->AddQuadFilled(s[0], s[1], s[2], s[3], fill);
            dl->AddQuad(s[0], s[1], s[2], s[3], line, 2.0f * g_scale);
            continue;
        }
        std::vector<Poly> pieces{Poly(s, s + 4)}, next;
        for (auto& w : over) {
            next.clear();
            for (auto& p : pieces) SubtractRect(p, w, next);
            pieces.swap(next);
        }
        for (auto& p : pieces) dl->AddConvexPolyFilled(p.data(), (int)p.size(), fill);
        std::vector<std::pair<ImVec2, ImVec2>> segs;
        for (int k = 0; k < 4; k++) SegmentOutside(s[k], s[(k + 1) % 4], over, 0, segs);
        for (auto& [a, b] : segs) dl->AddLine(a, b, line, 2.0f * g_scale);
    }
}

// The game-styled windows (bar, settle, trade, offers) are game UI: driven once per game frame from the main loop
// (GameFrame), NOT from Present - a PC whose Present never reaches the DX11 hook (DX9 renderer, ReShade-style dxgi
// proxies) got no multiplayer bar and no Settle window at all (a player's PC, 2026-10-15). The ImGui windows are only the
// fallback/extras on top.
static bool g_native = true;
static ULONGLONG g_inGameSince;
static bool g_noPresentLogged;
std::string RenderInfo() {
    auto path = [](const wchar_t* dll) {
        wchar_t p[MAX_PATH] = L"-";
        if (HMODULE m = GetModuleHandleW(dll)) GetModuleFileNameW(m, p, MAX_PATH);
        char a[MAX_PATH * 2];
        WideCharToMultiByte(CP_UTF8, 0, p, -1, a, sizeof a, nullptr, nullptr);
        return std::string(a);
    };
    return std::string(g_ready ? "DX11 overlay running" : g_failed ? "DX11 overlay setup failed" : "no DX11 frame seen (DX9 or a proxy?)") +
           "; d3d9=" + path(L"d3d9.dll") + " d3d11=" + path(L"d3d11.dll") + " dxgi=" + path(L"dxgi.dll");
}

void GameFrame() {
    net::UiInfo u = net::GetUiInfo();
    if (u.inMenu) CompanionMod();  // looked up here (main thread); the warning below reads the answer
    // no overlay on this PC (DX9 renderer, another overlay): the companion mod warning as a plain message box instead
    static ULONGLONG menuSince;
    static bool boxShown;
    if (!u.inMenu) menuSince = 0;
    else if (!menuSince) menuSince = GetTickCount64();
    if (u.inMenu && !g_ready && !boxShown && CompanionModKnown() != kModOk && GetTickCount64() - menuSince > 10000) {
        boxShown = true;
        bool missing = CompanionModKnown() == kModMissing;
        Log("overlay: no overlay - companion mod warning as a message box");
        std::thread([missing] {
            MessageBoxW(nullptr, missing
                ? L"The mod's second file, bmpShare.pkm, is missing.\n\nPut it in Banished\\WinData (next to the game's .pkg files), "
                  L"then restart Banished. Without it there is no Multiplayer button."
                : L"bmpShare.pkm doesn't match this version of the mod (dinput8.dll).\n\nGet both files from the same release, "
                  L"put bmpShare.pkm in Banished\\WinData and restart Banished.",
                L"Banished Multiplayer", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
        }).detach();
    }
    g_native = ingame::Frame(u);
    presence::Frame(u.active && u.started && u.inGame);
    report::Frame(u.inMenu ? (g_menuDialog ? gameui::UiOfDialog(g_menuDialog) : nullptr) : gamedlg::GameUi(), u.inMenu);
    if (!u.inGame) { g_inGameSince = 0; return; }
    if (!g_inGameSince) g_inGameSince = GetTickCount64();
    if (!g_ready && !g_noPresentLogged && GetTickCount64() - g_inGameSince > 20000) {
        g_noPresentLogged = true;
        auto path = [](const wchar_t* dll) {
            wchar_t p[MAX_PATH] = L"-";
            if (HMODULE m = GetModuleHandleW(dll)) GetModuleFileNameW(m, p, MAX_PATH);
            return std::wstring(p);
        };
        Log("!! overlay: no DX11 frame reached the overlay in 20 s (%s) - the game windows still work, ImGui extras are off. "
            "d3d9=%ls d3d11=%ls dxgi=%ls", g_failed ? "its setup failed" : "Present never called: DX9 renderer or another overlay/proxy?",
            path(L"d3d9.dll").c_str(), path(L"d3d11.dll").c_str(), path(L"dxgi.dll").c_str());
    }
}

// Main menu: the companion mod (WinData\bmpShare.pkm) is missing or from another build - say what to do. Its own
// windows can't do it (they ARE that file), so this one is ImGui. Closed with OK until the game restarts.
static bool g_modWarnClosed;
static void ModWarning() {
    int mod = CompanionModKnown();
    if (mod == kModOk || g_modWarnClosed) return;
    static std::string dir;
    static std::wstring wdir;
    if (dir.empty()) {
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(nullptr, p, MAX_PATH);
        *wcsrchr(p, L'\\') = 0;
        char a[MAX_PATH * 2];
        WideCharToMultiByte(CP_UTF8, 0, p, -1, a, sizeof a, nullptr, nullptr);
        dir = std::string(a) + "\\WinData";
        wdir = std::wstring(p) + L"\\WinData";
        Log("overlay: companion mod warning shown (%s)", mod == kModMissing ? "missing" : "another build");
    }
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 40 * g_scale), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(560 * g_scale, FLT_MAX));
    ImGui::Begin("Banished Multiplayer##bmpModWarn", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    ImGui::PushTextWrapPos(540 * g_scale);
    if (mod == kModMissing) {
        ImGui::TextColored(kWarn, "The mod's second file, bmpShare.pkm, is missing.");
        ImGui::TextUnformatted("Without it there is no Multiplayer button in the menu and no multiplayer windows in the game "
                               "(the small Multiplayer button in the corner still works).");
    } else {
        ImGui::TextColored(kWarn, "bmpShare.pkm doesn't match this version of the mod.");
        ImGui::TextUnformatted("Your dinput8.dll and bmpShare.pkm come from different releases, so some multiplayer windows "
                               "won't open. Get both files from the same release.");
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Put bmpShare.pkm in this folder, then restart Banished:");
    ImGui::TextColored(kDim, "%s", dir.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (ImGui::Button("Open the folder")) ShellExecuteW(nullptr, L"open", L"explorer.exe", wdir.c_str(), nullptr, SW_SHOWNORMAL);
    ImGui::SameLine();
    if (ImGui::Button("OK")) g_modWarnClosed = true;
    ImGui::End();
}

// Main menu: a newer release is out (update::Start asked the server at startup). Not while a session runs; closed with
// Later until the game restarts. (The mod warning above takes the spot first.)
static bool g_updateClosed;
static void UpdateNotice(const net::UiInfo& u) {
    update::Info upd = update::Get();
    if (!upd.newer || g_updateClosed || u.active || (CompanionModKnown() != kModOk && !g_modWarnClosed)) return;
    static bool logged;
    if (!logged) { logged = true; Log("overlay: update notice shown (%s)", upd.version.c_str()); }
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 40 * g_scale), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::Begin("Banished Multiplayer##bmpUpdate", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    ImGui::PushTextWrapPos(540 * g_scale);
    ImGui::TextColored(kGood, "Version %s is out (you have %s).", upd.version.c_str(), u.version.substr(0, u.version.find('-')).c_str());
    if (!upd.notes.empty()) ImGui::TextUnformatted(upd.notes.c_str());
    ImGui::TextColored(kDim, "Everybody in a game needs the same version, so update together.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (!upd.url.empty() && ImGui::Button("Download")) {
        std::wstring w(upd.url.begin(), upd.url.end());
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        g_updateClosed = true;
    }
    if (!upd.url.empty()) ImGui::SameLine();
    if (ImGui::Button("Later")) g_updateClosed = true;
    ImGui::End();
}

static void DrawUi() {
    net::UiInfo u = net::GetUiInfo();
    TownColours(u);
    if (u.active && u.started && u.inGame) presence::Draw(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y, g_scale);
    if (!u.inMenu) g_menuDialog = nullptr;  // the menu's dialog dies with it (set again by h_MenuInit)
    mpdialog::MenuTest(g_menuDialog ? gameui::UiOfDialog(g_menuDialog) : nullptr, RequestScreenshot);  // tests only
    bool native = g_native;  // the game-styled windows work (GameFrame); else the ImGui ones below
    if (u.inGame && towns::PickMode() && !towns::Settled(towns::Local())) SettlePanel(native);
    if (u.active && u.inGame && u.towns > 1) {
        if (g_showTrade && !native) TradeWindow(u);
        if (g_showMessages) MessagesWindow(u);
        StoragePanel(u);
        WorkplacePanel(u);
        if (!native) OfferPopups(u);
    }
    if (u.inMenu) { ModWarning(); UpdateNotice(u); MenuPanel(u); }
    else if (u.active && u.inGame) { if (!native) GameBar(u); }
    else if (u.active && u.started) {  // loading between sessions
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 8 * g_scale), ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::Begin("##mpload", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextColored(kWarn, "%s", u.status.empty() ? "Loading..." : u.status.c_str());
        ImGui::End();
    }
}

// ---------------------------------------------------------------- screenshots (tests): the finished back buffer -> PNG
static std::wstring g_shotPath;
void RequestScreenshot(const wchar_t* path) { g_shotPath = path; }
static void SaveBackBuffer(IDXGISwapChain* sc, const std::wstring& path) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return;
    D3D11_TEXTURE2D_DESC d;
    back->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.SampleDesc = {1, 0};
    ID3D11Texture2D* staging = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&sd, nullptr, &staging))) {
        if (d.SampleDesc.Count > 1) g_ctx->ResolveSubresource(staging, 0, back, 0, d.Format);
        else g_ctx->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            std::vector<uint8_t> px(d.Width * d.Height * 4);
            for (UINT y = 0; y < d.Height; y++) {
                const uint8_t* row = (const uint8_t*)m.pData + y * m.RowPitch;
                for (UINT x = 0; x < d.Width; x++) {
                    uint8_t* o = &px[(y * d.Width + x) * 4];
                    const uint8_t* i = row + x * 4;
                    o[0] = bgra ? i[0] : i[2]; o[1] = i[1]; o[2] = bgra ? i[2] : i[0]; o[3] = 255;
                }
            }
            g_ctx->Unmap(staging, 0);
            IWICImagingFactory* wic = nullptr;
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
                IWICStream* st = nullptr;
                IWICBitmapEncoder* enc = nullptr;
                IWICBitmapFrameEncode* fr = nullptr;
                WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
                if (SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
                    SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                    SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
                    SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(d.Width, d.Height)) &&
                    SUCCEEDED(fr->SetPixelFormat(&fmt))) {
                    fr->WritePixels(d.Height, d.Width * 4, (UINT)px.size(), px.data());
                    fr->Commit();
                    enc->Commit();
                    Log("overlay: screenshot %ls (%ux%u)", path.c_str(), d.Width, d.Height);
                }
                if (fr) fr->Release();
                if (enc) enc->Release();
                if (st) st->Release();
                wic->Release();
            }
        }
        staging->Release();
    }
    back->Release();
}

// ---------------------------------------------------------------- hooks
static HRESULT STDMETHODCALLTYPE h_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!g_ready && !g_failed) {
        g_ready = InitImGui(sc);
        g_failed = !g_ready;
    }
    if (g_ready) {
        if (!g_rtv) CreateRtv(sc);
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUi();
        ImGui::Render();
        g_wantMouse = ImGui::GetIO().WantCaptureMouse;
        ID3D11RenderTargetView* prevRtv = nullptr;
        ID3D11DepthStencilView* prevDsv = nullptr;
        g_ctx->OMGetRenderTargets(1, &prevRtv, &prevDsv);
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_ctx->OMSetRenderTargets(1, &prevRtv, prevDsv);
        if (prevRtv) prevRtv->Release();
        if (prevDsv) prevDsv->Release();
    }
    if (!g_shotPath.empty() && g_dev) {
        SaveBackBuffer(sc, g_shotPath);
        g_shotPath.clear();
    }
    return o_Present(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE h_Resize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    return o_Resize(sc, n, w, h, f, fl);
}

static bool NoOverlayTest() {  // test (<game>\bmp_nooverlay<suffix>.txt): no Present hook, like a PC whose frames never reach it
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    swprintf_s(wcsrchr(p, L'\\') + 1, 64, L"bmp_nooverlay%s.txt", SlotSuffix());
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

// A throwaway device + swap chain gives the IDXGISwapChain vtable (Present = 8, ResizeBuffers = 13). Made on a helper
// thread with a time limit: on a PC where creating it hangs (seen 2026-10-19: a copy with a Steam emulator, after a
// graphics-driver hiccup) the game must still start - without the overlay - instead of freezing on the loading screen.
static void* g_probePresent;
static void* g_probeResize;
static DWORD WINAPI ProbeThread(void*) {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"bmp_overlay_probe";
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = w;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                               &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr) || !sc) {
        Log("!! overlay: probe swap chain failed (0x%08lx)", hr);
        DestroyWindow(w);
        return 0;
    }
    void** vt = *(void***)sc;
    g_probePresent = vt[8];
    g_probeResize = vt[13];
    sc->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(w);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

static void HookPresent() {
    if (NoOverlayTest()) { Log("test: overlay Present hook skipped (bmp_nooverlay)"); return; }
    ULONGLONG t0 = GetTickCount64();
    HANDLE th = CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr);
    if (!th) { Log("!! overlay: no probe thread"); return; }
    DWORD wr = WaitForSingleObject(th, 10000);
    CloseHandle(th);
    if (wr != WAIT_OBJECT_0) {
        Log("!! overlay: creating the probe graphics device hasn't finished in 10 s - overlay off for this run (cursors, "
            "pings and town colours need it; everything else works)");
        return;
    }
    if (!g_probePresent) return;
    if (GetTickCount64() - t0 > 2000) Log("overlay: the probe graphics device took %llu ms", GetTickCount64() - t0);
    void* present = g_probePresent;
    void* resize = g_probeResize;
    MH_STATUS a = MH_CreateHook(present, (void*)h_Present, (void**)&o_Present);
    MH_STATUS b = MH_CreateHook(resize, (void*)h_Resize, (void**)&o_Resize);
    MH_EnableHook(present);
    MH_EnableHook(resize);
    Log("overlay: hooked Present %s, ResizeBuffers %s", MH_StatusToString(a), MH_StatusToString(b));
}

void Init() {
    HookPresent();  // may fail or be skipped: the main menu button and the game windows don't depend on it
    uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
    void* menuInit = (void*)(exe + Rva(0xb91a0));   // MainMenuDialog::vf4
    void* menuEvent = (void*)(exe + Rva(0xb9020));  // MainMenuDialog::vf8
    MH_STATUS c = MH_CreateHook(menuInit, (void*)h_MenuInit, (void**)&o_MenuInit);
    MH_STATUS d = MH_CreateHook(menuEvent, (void*)h_MenuEvent, (void**)&o_MenuEvent);
    if (c == MH_OK) MH_EnableHook(menuInit);
    if (d == MH_OK) MH_EnableHook(menuEvent);
    Log("overlay: main menu button hooks %s %s", MH_StatusToString(c), MH_StatusToString(d));
}

}  // namespace overlay
