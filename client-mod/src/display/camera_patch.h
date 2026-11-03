#pragma once
// Widescreen field of view for Fable's camera (portable: builds bytes, never touches memory).
//
// Fable's camera setup (Fable.exe 0x00B30B50, Steam build of 2005-07-15) turns the camera's
// horizontal field of view into projection scales. Its automatic path stretches the view
// half-way between "keep the vertical view" and "keep the horizontal view":
//     xs = 1 / tan(fov/2 * (1 + 0.5 * (0.75 - h/w))),   ys = xs * w/h
// so a 16:9 screen gains only part of the width it should and loses some height.
//
// The patch sends that path through a small block of code that keeps the 4:3 vertical
// view and widens the horizontal one to the screen (Hor+):
//     ys = 1 / (tan(fov/2) * 0.75 * fov_scale),          xs = ys * h/w
// At 4:3 and fov_scale 1 this is exactly Fable's own result. Targets that are 4:3 or
// narrower (shadow maps, reflections) and cameras with an explicit vertical angle
// (scripted shots) still run Fable's original code.
#include <cstdint>
#include <vector>

namespace fo::display {

struct CameraSite {
    uint32_t hook = 0x00B30C41;      // mov al,[ebx+54h] / test al,al / je auto  (7 bytes)
    uint32_t explicit_fov = 0x00B30C48;  // original path for cameras with fov x and y
    uint32_t auto_fov = 0x00B30C7E;      // original automatic path
    uint32_t done = 0x00B30CBC;          // after both paths
};

// Bytes expected at each site, to refuse a Fable.exe we haven't studied.
extern const uint8_t kHookBytes[7];
extern const uint8_t kAutoBytes[4];
extern const uint8_t kDoneBytes[6];

// The code block, to be copied to `cave` (executable memory). Its constants live in the
// block itself; `fov_scale` multiplies the vertical half-angle's tangent (1 = Fable's view).
std::vector<uint8_t> build_camera_cave(uint32_t cave, const CameraSite& site, float fov_scale);

// The 7 bytes written over the hook site: jmp cave, then two nops.
std::vector<uint8_t> build_camera_jump(uint32_t cave, const CameraSite& site);

// Offset of the patchable fov constant inside the block (0.75 * fov_scale), so a running
// game can change the field of view without rebuilding the block.
constexpr uint32_t kCaveFovConstOffset = 0x58;

// Reference maths, for tests and logs: returns {xs, ys} as the patched game computes them.
struct Scales { float xs, ys; };
Scales fable_original(float fov, float w, float h);
Scales hor_plus(float fov, float w, float h, float fov_scale);

}  // namespace fo::display
