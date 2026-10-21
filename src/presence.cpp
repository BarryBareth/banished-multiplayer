// Other players' cursors and map pings - see presence.h. Visual only: positions are world x/y on the ground, picked by
// casting the mouse ray (tint::ScreenToRay, the main camera of the last rendered frame) onto the terrain
// (towns::TerrainZ), and drawn back with tint::WorldToScreen.
#include "presence.h"
#include "common.h"
#include "net.h"
#include "tint.h"
#include "towns.h"
#include "imgui.h"
#include <cmath>
#include <deque>
#include <map>
#include <mmsystem.h>
#include <vector>
#pragma comment(lib, "winmm.lib")

namespace presence {

struct Remote {
    std::string name;
    int colour = 0;
    bool on = false;
    float tx = 0, ty = 0, tz = 0;   // last received
    float x = 0, y = 0, z = 0;      // drawn (eases towards the target)
    bool placed = false;
    ULONGLONG heard = 0;
};
struct Ping {
    std::string name;
    int colour;
    float x, y, z;
    ULONGLONG at;
};
static std::map<int, Remote> g_remote;
static std::deque<Ping> g_pings;
static bool g_showCursors = true;
static bool g_settingsRead;
static bool g_altClick;           // the left button went down with Alt held: swallow it until it's released
static bool g_wasDown;
static ULONGLONG g_lastSend;
static float g_sentX = 1e9f, g_sentY = 1e9f;
static bool g_sentOn;
static int g_testCursorX = -1, g_testCursorY = -1;  // uiscript: pretend the mouse points here

static const ULONGLONG kPingMs = 7000, kCursorTimeoutMs = 4000;

static void ReadSettings() {
    g_settingsRead = true;
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    wcscpy_s(wcsrchr(p, L'\\') + 1, 64, L"bmp_settings.txt");
    FILE* f = nullptr;
    if (_wfopen_s(&f, p, L"r") || !f) return;
    char line[256];
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "cursors=0", 9)) g_showCursors = false;
    fclose(f);
    if (!g_showCursors) Log("presence: other players' cursors hidden (cursors=0)");
}

// --- the ping sound: a short soft two-note chime, made once, played asynchronously from memory
static std::vector<uint8_t> g_chime;
static void Chime() {
    if (g_chime.empty()) {
        const int rate = 22050, n = rate * 30 / 100;
        g_chime.resize(44 + n * 2);
        uint8_t* w = g_chime.data();
        auto u32 = [&](int at, uint32_t v) { memcpy(w + at, &v, 4); };
        auto u16 = [&](int at, uint16_t v) { memcpy(w + at, &v, 2); };
        memcpy(w, "RIFF", 4); u32(4, 36 + n * 2); memcpy(w + 8, "WAVEfmt ", 8); u32(16, 16); u16(20, 1); u16(22, 1);
        u32(24, rate); u32(28, rate * 2); u16(32, 2); u16(34, 16); memcpy(w + 36, "data", 4); u32(40, n * 2);
        for (int i = 0; i < n; i++) {
            double t = (double)i / rate, f = t < 0.12 ? 880.0 : 1318.5, start = t < 0.12 ? 0.0 : 0.12;
            double env = exp(-(t - start) * 18.0) * (t - start < 0.005 ? (t - start) / 0.005 : 1.0);
            int16_t s = (int16_t)(sin(2 * 3.14159265358979 * f * t) * env * 6000);
            memcpy(w + 44 + i * 2, &s, 2);
        }
    }
    PlaySoundW((LPCWSTR)g_chime.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// the ground point under a screen position (client pixels) - false when the ray misses the map
static bool GroundAt(float sx, float sy, float w, float h, float out[3]) {
    float o[3], d[3];
    if (!tint::ScreenToRay(sx, sy, w, h, o, d) || d[2] > -0.02f) return false;
    float t = 0, prev = 0;
    bool entered = false;
    for (int i = 0; i < 2000; i++) {
        float p[3] = {o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t}, z;
        if (!towns::TerrainZ(p[0], p[1], &z)) {
            if (entered) return false;  // left the map
            prev = t;
            t += 1.0f;
            continue;
        }
        entered = true;
        float above = p[2] - z;
        if (above <= 0) {  // crossed the ground between prev and t: bisect
            float a = prev, b = t;
            for (int k = 0; k < 14; k++) {
                float m = (a + b) * 0.5f, q[3] = {o[0] + d[0] * m, o[1] + d[1] * m, o[2] + d[2] * m}, zz;
                if (towns::TerrainZ(q[0], q[1], &zz) && q[2] > zz) a = m; else b = m;
            }
            out[0] = o[0] + d[0] * b;
            out[1] = o[1] + d[1] * b;
            towns::TerrainZ(out[0], out[1], &out[2]);
            return true;
        }
        prev = t;
        t += (std::max)(0.3f, (std::min)(above * 0.6f, 6.0f));
    }
    return false;
}

static float CellToWorld(int c) { return (float)c + 0.5f - (float)(towns::MapSize() / 2); }

static bool LocalPoint(float out[3]) {
    if (g_testCursorX >= 0) {
        out[0] = CellToWorld(g_testCursorX);
        out[1] = CellToWorld(g_testCursorY);
        return towns::TerrainZ(out[0], out[1], &out[2]);
    }
    HWND hwnd = GetActiveWindow();  // the game's window, if it has the focus (main thread owns it)
    if (!hwnd) return false;
    POINT pt;
    RECT rc;
    if (!GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt) || !GetClientRect(hwnd, &rc)) return false;
    float w = (float)(rc.right - rc.left), h = (float)(rc.bottom - rc.top);
    if (pt.x < 0 || pt.y < 0 || pt.x >= w || pt.y >= h) return false;
    tint::OnPresent();  // the camera of the last rendered frame
    return GroundAt((float)pt.x, (float)pt.y, w, h, out);
}

static void SendPing(const float p[3]) {
    char line[64];
    sprintf_s(line, "MARK %.1f %.1f", p[0], p[1]);
    net::SendPresence(line);
    Log("presence: ping at %.1f %.1f", p[0], p[1]);
    std::string name;
    int colour;
    net::PresenceIdentity(net::Me(), &name, &colour);
    OnPing(net::Me(), name, colour, p[0], p[1]);  // my own ping shows here too (the others get it from the host)
}

void Frame(bool inSession) {
    if (!g_settingsRead) ReadSettings();
    if (!inSession) {
        g_remote.clear();
        g_pings.clear();
        g_altClick = g_wasDown = false;
        g_sentOn = false;
        g_sentX = g_sentY = 1e9f;
        return;
    }
    ULONGLONG now = GetTickCount64();
    // Alt + left click = ping (the click itself is kept from the game by BlockLeftClick)
    bool down = GetAsyncKeyState(VK_LBUTTON) < 0, alt = GetAsyncKeyState(VK_MENU) < 0;
    if (down && !g_wasDown && alt && GetActiveWindow()) {
        g_altClick = true;
        float p[3];
        if (LocalPoint(p)) SendPing(p);
    }
    if (!down) g_altClick = false;
    g_wasDown = down;
    // cursor: ~8 updates a second while it moves, one every 2 s while it rests, "CUR -" once when it's gone
    if (now - g_lastSend >= 125) {
        float p[3];
        bool on = LocalPoint(p);
        bool moved = on && (fabsf(p[0] - g_sentX) > 0.2f || fabsf(p[1] - g_sentY) > 0.2f);
        if (on && (moved || !g_sentOn || now - g_lastSend >= 2000)) {
            char line[64];
            sprintf_s(line, "CUR %.1f %.1f", p[0], p[1]);
            net::SendPresence(line);
            g_sentX = p[0];
            g_sentY = p[1];
            g_sentOn = true;
            g_lastSend = now;
        } else if (!on && g_sentOn) {
            net::SendPresence("CUR -");
            g_sentOn = false;
            g_lastSend = now;
        }
    }
    while (!g_pings.empty() && now - g_pings.front().at > kPingMs) g_pings.pop_front();
}

bool BlockLeftClick() { return g_altClick; }

void OnCursor(int player, const std::string& name, int colour, bool on, float x, float y) {
    Remote& r = g_remote[player];
    r.name = name;
    r.colour = colour;
    r.on = on;
    r.heard = GetTickCount64();
    if (!on) return;
    float z;
    if (!towns::TerrainZ(x, y, &z)) { r.on = false; return; }
    r.tx = x; r.ty = y; r.tz = z;
    if (!r.placed) { r.x = x; r.y = y; r.z = z; r.placed = true; }
}

void OnPing(int player, const std::string& name, int colour, float x, float y) {
    float z;
    if (!towns::TerrainZ(x, y, &z)) return;
    if (g_pings.size() >= 12) g_pings.pop_front();
    g_pings.push_back({name, colour, x, y, z, GetTickCount64()});
    Log("presence: %s pinged %.1f %.1f", name.c_str(), x, y);
    Chime();
}

void TestProbe(float sx, float sy) {  // uiscript: the mouse-ray path for a screen pixel, projected back
    RECT rc{};
    float w = 0, h = 0;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hw, LPARAM l) -> BOOL {
        RECT r;
        if (IsWindowVisible(hw) && GetClientRect(hw, &r) && r.right > 640) { *(RECT*)l = r; return FALSE; }
        return TRUE;
    }, (LPARAM)&rc);
    w = (float)rc.right; h = (float)rc.bottom;
    tint::OnPresent();
    if (sx < 0) {  // probe -CX CY: the pixel where cell CX,CY's centre is drawn (then the ray back from it)
        float c[3] = {CellToWorld((int)-sx), CellToWorld((int)sy), 0};
        towns::TerrainZ(c[0], c[1], &c[2]);
        if (!tint::WorldToScreen(c, w, h, &sx, &sy)) { Log("presence: probe cell not on screen"); return; }
        Log("presence: probe: cell centre %.2f %.2f %.2f is drawn at %.1f,%.1f", c[0], c[1], c[2], sx, sy);
    }
    float p[3];
    if (!GroundAt(sx, sy, w, h, p)) { Log("presence: probe %.0f,%.0f (%gx%g) -> no ground", sx, sy, w, h); return; }
    float bx = 0, by = 0;
    tint::WorldToScreen(p, w, h, &bx, &by);
    int size = towns::MapSize();
    Log("presence: probe %.0f,%.0f (%gx%g) -> world %.2f %.2f %.2f = cell %d,%d -> back on screen %.1f,%.1f", sx, sy, w, h,
        p[0], p[1], p[2], (int)floorf(p[0] + size / 2), (int)floorf(p[1] + size / 2), bx, by);
}

void TestCursor(int cellX, int cellY) { g_testCursorX = cellX; g_testCursorY = cellY; }
void TestPing(int cellX, int cellY) {
    float p[3] = {CellToWorld(cellX), CellToWorld(cellY), 0};
    if (towns::TerrainZ(p[0], p[1], &p[2])) SendPing(p);
}

// ---------------------------------------------------------------- drawing (overlay, ImGui frame)
static ImU32 Colour(int colour, int alpha) {
    float rgb[3];
    tint::ColorOf(colour, rgb);
    return IM_COL32((int)(rgb[0] * 255), (int)(rgb[1] * 255), (int)(rgb[2] * 255), alpha);
}

static void Label(ImDrawList* dl, ImVec2 at, const std::string& text, ImU32 col, float scale) {
    ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    ImVec2 a(at.x, at.y), b(at.x + ts.x + 8 * scale, at.y + ts.y + 4 * scale);
    dl->AddRectFilled(a, b, IM_COL32(20, 16, 10, 190), 3 * scale);
    dl->AddRect(a, b, col, 3 * scale);
    dl->AddText(ImVec2(at.x + 4 * scale, at.y + 2 * scale), IM_COL32(240, 232, 214, 255), text.c_str());
}

static void Pointer(ImDrawList* dl, ImVec2 p, ImU32 fill, float s) {
    // a mouse-pointer arrow with its tip at p
    ImVec2 pts[7] = {{0, 0}, {0, 17}, {4.5f, 13}, {8, 20.5f}, {11, 19}, {7.5f, 12}, {13, 12}};
    s *= 1.35f;
    for (auto& q : pts) q = ImVec2(p.x + q.x * s, p.y + q.y * s);
    dl->AddConcavePolyFilled(pts, 7, fill);
    dl->AddPolyline(pts, 7, IM_COL32(15, 12, 8, 230), ImDrawFlags_Closed, 1.5f * s);
}

void Draw(float width, float height, float scale) {
    if (g_remote.empty() && g_pings.empty()) return;
    tint::OnPresent();
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ULONGLONG now = GetTickCount64();
    float ease = (std::min)(1.0f, io.DeltaTime * 12.0f);
    if (g_showCursors) {
        for (auto& [id, r] : g_remote) {
            if (!r.on || !r.placed || now - r.heard > kCursorTimeoutMs) continue;
            r.x += (r.tx - r.x) * ease;
            r.y += (r.ty - r.y) * ease;
            r.z += (r.tz - r.z) * ease;
            float p[3] = {r.x, r.y, r.z + 0.05f}, sx, sy;
            if (!tint::WorldToScreen(p, width, height, &sx, &sy) || sx < -20 || sy < -20 || sx > width + 20 || sy > height + 20) continue;
            Pointer(dl, ImVec2(sx, sy), Colour(r.colour, 235), scale);
            Label(dl, ImVec2(sx + 18 * scale, sy + 24 * scale), r.name, Colour(r.colour, 220), scale);
        }
    }
    int offscreen = 0;
    for (size_t i = 0; i < g_pings.size(); i++) {
        const Ping& g = g_pings[i];
        float age = (now - g.at) / 1000.0f, fade = age > kPingMs / 1000.0f - 1.5f ? (kPingMs / 1000.0f - age) / 1.5f : 1.0f;
        int a = (int)(255 * (std::max)(0.0f, fade));
        float p[3] = {g.x, g.y, g.z + 0.05f}, sx = 0, sy = 0;
        bool front = tint::WorldToScreen(p, width, height, &sx, &sy);
        float margin = 40 * scale;
        if (front && sx >= margin && sy >= margin && sx <= width - margin && sy <= height - margin) {
            for (int k = 0; k < 3; k++) {  // rings rippling out
                float ph = fmodf(age * 0.9f + k / 3.0f, 1.0f);
                dl->AddCircle(ImVec2(sx, sy), (8 + ph * 46) * scale, Colour(g.colour, (int)(a * (1 - ph))), 0, 3 * scale);
            }
            dl->AddCircleFilled(ImVec2(sx, sy), 6 * scale, Colour(g.colour, a));
            dl->AddCircle(ImVec2(sx, sy), 6 * scale, IM_COL32(15, 12, 8, a), 0, 2 * scale);
            Label(dl, ImVec2(sx + 12 * scale, sy - 26 * scale), g.name, Colour(g.colour, a), scale);
            continue;
        }
        // off screen: an arrow at the screen edge towards it; its label is a button that flies the camera there
        float dx, dy;
        if (front) { dx = sx - width * 0.5f; dy = sy - height * 0.5f; }
        else {
            float c[3];
            if (!tint::CameraSpace(p, c)) continue;
            dx = c[0]; dy = -c[1];  // behind: right stays right, "ahead" becomes up
            if (fabsf(dx) < 1e-3f && fabsf(dy) < 1e-3f) dy = 1;
        }
        float len = sqrtf(dx * dx + dy * dy);
        dx /= len; dy /= len;
        float hw = width * 0.5f - margin, hh = height * 0.5f - margin;
        float k = (std::min)(fabsf(dx) > 1e-4f ? hw / fabsf(dx) : 1e9f, fabsf(dy) > 1e-4f ? hh / fabsf(dy) : 1e9f);
        ImVec2 tip(width * 0.5f + dx * k, height * 0.5f + dy * k);
        float al = 30 * scale, aw = 15 * scale;
        ImVec2 back(tip.x - dx * al, tip.y - dy * al), side(-dy * aw, dx * aw);
        float pulse = 0.7f + 0.3f * sinf(age * 8.0f);
        ImVec2 t1(back.x + side.x, back.y + side.y), t2(back.x - side.x, back.y - side.y);
        dl->AddTriangleFilled(tip, t1, t2, Colour(g.colour, (int)(a * pulse)));
        dl->AddTriangle(tip, t1, t2, IM_COL32(15, 12, 8, a), 2.5f * scale);
        // the label sits further in along the same direction, centred, so it never covers the arrow
        std::string text = g.name + " pinged - go there";
        ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        float bw = ts.x + 20 * scale, bh = ts.y + 12 * scale;
        float reach = al + 8 * scale + 0.5f * (fabsf(dx) * bw + fabsf(dy) * bh);
        ImVec2 at(tip.x - dx * reach, tip.y - dy * reach);
        at.x = (std::max)(bw * 0.5f + 4, (std::min)(at.x, width - bw * 0.5f - 4));
        at.y = (std::max)(bh * 0.5f + 4, (std::min)(at.y, height - bh * 0.5f - 4));
        char wid[48];
        sprintf_s(wid, "##bmpPing%zu", i + (size_t)offscreen * 100);
        offscreen++;
        ImGui::SetNextWindowPos(at, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin(wid, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, (std::max)(0.2f, fade));
        ImGui::PushStyleColor(ImGuiCol_Border, Colour(g.colour, a));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f * scale);
        if (ImGui::Button(text.c_str())) {
            int size = towns::MapSize(), cx = (int)floorf(g.x + size / 2), cy = (int)floorf(g.y + size / 2);
            towns::FocusCell(cy * size + cx);
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
}

}  // namespace presence
