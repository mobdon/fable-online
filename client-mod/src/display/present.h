#pragma once
// Refresh rate, vsync and frame latency for Fable's Direct3D device.
#include <d3d9.h>

namespace fo::display {
// Call before the game's CreateDevice / Reset: picks the refresh rate and vsync from
// [display] refresh and vsync, and logs what the game asked for and what it gets.
// [display] window: fullscreen (Fable's own), windowed (a normal window), or borderless (a
// window covering the monitor, like full screen but alt-tab friendly). `window` is the
// game's window, restyled to match.
void adjust_present(IDirect3D9* d3d, UINT adapter, D3DPRESENT_PARAMETERS* pp, HWND window, const char* when);
// Re-applies the window style after the device exists (the game may restyle it).
void fix_window(HWND window);
// Call around the game's Present: limits how far the CPU runs ahead of the GPU (mouse lag)
// and measures the frame rate.
void before_present(IDirect3DDevice9* dev);
void after_present(IDirect3DDevice9* dev);
void on_device_lost();
void log_frame_rate();
void log_failure(const char* call, HRESULT hr);
}  // namespace fo::display
