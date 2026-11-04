#pragma once
// Diagnostics for turning the view every frame: which code sets Fable's engine camera
// (0x00B30B50, called with the view rectangle, a 4x3 view matrix and the camera position),
// how often, and from where.

namespace fo::display::camera_probe {
void install();
// One line with how many engine camera sets each caller made since the last call (0 updates:
// just start counting again). Game thread only.
void log_counts(unsigned updates);
}  // namespace fo::display::camera_probe
