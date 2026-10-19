// Town colours: other players' buildings and fields get their town's colour - an outline and a light fill over their
// footprint, drawn by the overlay. (Recolouring the models themselves doesn't work: MeshInstance::Draw does set a
// per-object colour on the render context, but the building shaders only use its alpha.)
//
// Projection: Graphics::MeshInstance::Draw (Runtime export) receives the camera it draws with; the main view is the
// perspective camera above ground (others: shadow maps, the mirrored water reflection). Camera layout: +0x64 camera-to-
// world = 3x3 rotation rows (right, forward, up) + position; +0x94 projection, row-vector convention:
// clip = (cx, cy, cz, 1) * P with cx/cy/cz = (p - pos) . right/forward/up. World: z up, cell (x, y) spans
// [x - size/2, x + 1 - size/2]. Purely visual: nothing in the simulation reads any of this.
#include "tint.h"
#include "common.h"
#include "towns.h"
#include "MinHook.h"
#include <cmath>
#include <cstring>

namespace tint {

static bool g_enabled = true;
static void* g_mainCam;           // main view camera (seen in this frame's draws)
static float g_cam[28];           // its camera-to-world (12) + projection (16), copied each frame

static const float kTownColors[][3] = {
    {0.95f, 0.30f, 0.25f},  // red
    {0.25f, 0.55f, 1.00f},  // blue
    {0.30f, 0.90f, 0.30f},  // green
    {1.00f, 0.85f, 0.20f},  // yellow
    {0.75f, 0.35f, 1.00f},  // purple
    {0.20f, 0.90f, 0.90f},  // cyan
    {1.00f, 0.55f, 0.15f},  // orange
    {0.85f, 0.85f, 0.85f},  // grey
};

void ColorOf(int town, float rgb[3]) {
    const float* c = kTownColors[(unsigned)town % (sizeof kTownColors / sizeof kTownColors[0])];
    rgb[0] = c[0];
    rgb[1] = c[1];
    rgb[2] = c[2];
}

void SetEnabled(bool on) { g_enabled = on; }
bool Enabled() { return g_enabled; }

static bool IsMainCamera(const uint8_t* cam) {
    const float* t = (const float*)(cam + 0x64);
    const float* p = (const float*)(cam + 0x94);
    return p[7] > 0.5f && t[11] > 0.f;  // perspective (w = depth) and above the ground (not the reflection)
}

using Draw_t = void(__fastcall*)(void* mesh, void* ctx, void* camera, void* material, float alpha, int a, int b, int c);
static Draw_t o_Draw;
static void __fastcall h_Draw(void* mesh, void* ctx, void* camera, void* material, float alpha, int a, int b, int c) {
    if (camera != g_mainCam && camera && IsMainCamera((uint8_t*)camera)) g_mainCam = camera;
    o_Draw(mesh, ctx, camera, material, alpha, a, b, c);
}

void OnPresent() {  // overlay, before drawing: snapshot the camera of the frame just rendered
    if (g_mainCam) memcpy(g_cam, (uint8_t*)g_mainCam + 0x64, sizeof g_cam);
}

bool WorldToScreen(const float p[3], float width, float height, float* sx, float* sy) {
    if (!g_mainCam) return false;
    const float* r = g_cam;        // rows: right (0..2), forward (3..5), up (6..8); position 9..11
    const float* P = g_cam + 12;
    float d[3] = {p[0] - r[9], p[1] - r[10], p[2] - r[11]};
    float c[3] = {d[0] * r[0] + d[1] * r[1] + d[2] * r[2], d[0] * r[3] + d[1] * r[4] + d[2] * r[5],
                  d[0] * r[6] + d[1] * r[7] + d[2] * r[8]};
    float clip[4];
    for (int j = 0; j < 4; j++) clip[j] = c[0] * P[j] + c[1] * P[4 + j] + c[2] * P[8 + j] + P[12 + j];
    if (clip[3] <= 0.05f) return false;  // behind the camera
    *sx = (clip[0] / clip[3] * 0.5f + 0.5f) * width;
    *sy = (0.5f - clip[1] / clip[3] * 0.5f) * height;
    return true;
}

// 4x4 inverse (general, Gauss-Jordan); false when singular
static bool Invert4(const float* m, float* out) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; c++) {
        int piv = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[piv][c])) piv = r;
        if (fabs(a[piv][c]) < 1e-12) return false;
        if (piv != c) for (int k = 0; k < 8; k++) { double t = a[c][k]; a[c][k] = a[piv][k]; a[piv][k] = t; }
        double d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            double f = a[r][c];
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r * 4 + c] = (float)a[r][c + 4];
    return true;
}

bool ScreenToRay(float sx, float sy, float width, float height, float origin[3], float dir[3]) {
    if (!g_mainCam || width <= 0 || height <= 0) return false;
    const float* r = g_cam;
    float inv[16];
    if (!Invert4(g_cam + 12, inv)) return false;
    float nx = sx / width * 2.f - 1.f, ny = 1.f - sy / height * 2.f;
    float pts[2][3];
    for (int k = 0; k < 2; k++) {  // ndc depth 0 and 1 (clip = (c, 1) * P, row vectors) -> camera space -> world
        float ndc[4] = {nx, ny, (float)k, 1.f}, c[4];
        for (int j = 0; j < 4; j++) c[j] = ndc[0] * inv[j] + ndc[1] * inv[4 + j] + ndc[2] * inv[8 + j] + ndc[3] * inv[12 + j];
        if (fabsf(c[3]) < 1e-9f) return false;
        for (int j = 0; j < 3; j++) c[j] /= c[3];
        for (int j = 0; j < 3; j++) pts[k][j] = r[9 + j] + c[0] * r[j] + c[1] * r[3 + j] + c[2] * r[6 + j];
    }
    float d[3] = {pts[1][0] - pts[0][0], pts[1][1] - pts[0][1], pts[1][2] - pts[0][2]};
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1e-6f) return false;
    for (int j = 0; j < 3; j++) { origin[j] = pts[0][j]; dir[j] = d[j] / len; }
    return true;
}

bool CameraSpace(const float p[3], float c[3]) {  // (right, forward, up) components of p relative to the camera
    if (!g_mainCam) return false;
    const float* r = g_cam;
    float d[3] = {p[0] - r[9], p[1] - r[10], p[2] - r[11]};
    for (int k = 0; k < 3; k++) c[k] = d[0] * r[k * 3] + d[1] * r[k * 3 + 1] + d[2] * r[k * 3 + 2];
    return true;
}

bool CameraPosition(float out[3]) {
    if (!g_mainCam) return false;
    for (int j = 0; j < 3; j++) out[j] = g_cam[9 + j];
    return true;
}

void Install() {
    void* draw = RuntimeExport("?Draw@MeshInstance@Graphics@@UEBAXAEAVContext@Video@@AEBVCamera@2@V?$ExternalPtr@VMaterialInstance@Graphics@@@Object@@MHHH@Z");
    if (!draw) { Log("!! tint: MeshInstance::Draw export missing - town colours off"); return; }
    if (MH_CreateHook(draw, (void*)h_Draw, (void**)&o_Draw) != MH_OK || MH_EnableHook(draw) != MH_OK) {
        Log("!! tint: Draw hook failed - town colours off");
        return;
    }
    Log("tint: town colours installed");
}

}  // namespace tint
