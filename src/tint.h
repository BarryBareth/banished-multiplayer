#pragma once
// Town colours: other towns' buildings and fields are outlined and lightly filled in their town's colour by the
// overlay (visual only). See tint.cpp.
namespace tint {
void Install();                         // hook MeshInstance::Draw (main camera capture)
void ColorOf(int town, float rgb[3]);   // the town's colour
void SetEnabled(bool on);
bool Enabled();
void OnPresent();                       // overlay, each frame before drawing
bool WorldToScreen(const float p[3], float width, float height, float* sx, float* sy);  // false = off screen / behind
bool ScreenToRay(float sx, float sy, float width, float height, float origin[3], float dir[3]);  // the inverse, as a ray
bool CameraPosition(float out[3]);
bool CameraSpace(const float p[3], float c[3]);  // p relative to the camera: right, forward, up
}
