# Widescreen: camera field of view and HUD scaling

Steam Fable.exe, build 2005-07-15.

## Camera (fixed by `client-mod/src/display`)

`0x00B30B50` is the camera setup (thiscall, one argument: the camera settings). It reads the
target rectangle (settings +0x00..+0x0C), takes the target size from the current render
target descriptor (`[0x01436EB0]` +0x08 width, +0x0C height, floats) when one is set, and
writes the projection scales to camera +0xD4 (x) and +0xD8 (y).

- Settings +0x4C: horizontal field of view in radians, designed for 4:3.
- Settings +0x50: vertical field of view, used only when settings +0x54 is set (cameras with
  both angles given).
- Automatic path (`0x00B30C7E`): `xs = 1 / tan(fov/2 * (1 + 0.5 * (0.75 - h/w)))`,
  `ys = xs * w/h`. At 16:9 that is about 9% more width and 15% less height than 4:3, a
  half-way mix that neither keeps the vertical view nor the horizontal one.

The mod jumps from `0x00B30C41` to a small block that, for targets wider than 4:3 on the
automatic path, keeps the 4:3 vertical view and derives the horizontal one (Hor+). Narrower
targets and the explicit path run the original code.

The direct3D projection itself (`0x00A0B320`) has unit x/y scales; the scales above go into the
camera's own matrices, which the vertex shaders use, so a `SetTransform` hook can't fix this.

## HUD and menus (not fixed yet)

The 2D layer is laid out on a 640x480 page: `0x01375CD4` (640.0) and `0x01375CD8` (480.0) in
`.data`. When `[0x013B8768]` is set, page coordinates are scaled to the screen size held in
`0x013B876C` / `0x013B8770` (written from the device at `0x0040AC42` and `0x0040AEC4`). Helpers:
`0x0052E470` (x page to screen), `0x0052E4A0` (x screen to page), `0x0052E4D0` / `0x0052E500` (y),
`0x0052E540` / `0x0052E580` (points). Many GUI functions also inline the same scaling.
Separate x and y factors are what stretch the HUD. Positions come from data laid out for a
640-wide page, so changing the page width alone moves centred and right-hand elements; the
fix needs to know how the 2D layer reaches Direct3D (`[display] frame_probe=1`, F10).

The in-game Options have a "Screen Aspect Ratio" setting (4:3 or 16:9, profile field
`ScreenAspectRatio`, text `PC_OPTIONS_SCREEN_ASPECT_RATIO_TEXT_SLIDER`). Its effect isn't
mapped yet.

## HUD scaling: what broke on the front end (to fix before turning hud_scale back on)

A front-end capture with hud_scale on showed the layout using the virtual size, but three
renderer-side primitive managers turn layout pixels into clip space with the real size, so
their output landed in the top-left 1/scale of the screen:

| caller of GetScreenSize | class (from the vtable's RTTI)            | what it does                       |
|-------------------------|-------------------------------------------|------------------------------------|
| 0xBB047A (vt slot 13)   | CEnginePrimitiveManager2DSprites          | x*2/W-1, 1-y*2/H for sprite quads  |
| 0xBB1286 (vt slot 14)   | CEnginePrimitiveManager2DSprites          | same                               |
| 0xBC6E59 (vt slot 5)    | CEnginePrimitiveManagerText               | passes 2/W, 2/H to 0x989B00        |
| 0xBE1C94 (vt slot 5)    | CEngineInternalPrimitive2DViewport        | viewport rect to clip space        |
| 0xBE1FA5                | CEngineInternalPrimitive2DViewport        | (sets a 2D viewport)               |

The front end also called SetViewport(0,0,1601,901), i.e. the virtual size + 1. Scaled up,
that exceeds the back buffer, so a SetViewport hook must clamp to the real size.

Next attempt: send the virtual size to those return addresses as well (a list in the code
block, next to the 0x9E1D00-0x9E2430 range), scale and clamp 2D viewports, and test the
front end, the intro video and the pause menu before the in-game HUD. 3D-projected screen
positions (camera code at 0xB327A1, 0xB4A881, 0xB8E315) must keep the real size.
