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

## Player targeting (CTCTargetingPlayer)

- Vtable 0x124280C. Slot 9 (0x805600) is the update; every fourth frame, or when forced,
  it calls the target search at 0x804020.
- The search builds a reference position and direction:
  - Position: the hero's position, or the camera's when the targeting definition byte
    +0x38 is set.
  - Direction: the hero's facing (physics +0x118), or the camera's view direction (+0x18 in
    the struct filled by `[[[0x13B8A1C]+0x24]+0x34]` vfunc +4) when any of these hold:
    - definition byte +0x3A is set;
    - the first-person targeting counter [this+0x38] is above 8;
    - the hero mode is 0x2E.
  - Cone half-angle: definition +0x30 or +0x34, in degrees.
- `aim_at_crosshair` patches the +0x3A read at 0x8040ED (`8A 57 3A`) to `mov dl,1`, so the
  search always uses the camera direction.

## Target highlight colour (red target)

- NPlayerGui::CDrawHighlightingTargetingCursor (vtable 0x125A720, built at 0x6542A4, held at
  player GUI +0x1F0). Its update 0x64BFBA sorts the target ([this+0x28] handle) into a kind
  0..4 in [ebp+6Ch]; at 0x64C41D the kind is compared with [esi+44h] and, when it changes,
  the glow colours are reloaded from the player GUI definition [0x13B878C]: start colours
  +0x86C, end colours +0x878, fades +0x884/+0x89C/+0x890/+0x8A8, steps +0x8B4.
- Kind 2 is an enemy (the target's targeting component is aimed at the hero, or 0x76D2F0).
  When the kind is 2 and the player is in combat (mode 2, or mode 0x28 with 2 on the stack)
  the screen-space reticle is drawn too (0x64C5FB..).
- Player mode: 0x449970 (player GUI owner [esi+8]) -> player; 0x633BE0 = current mode id
  ([+0x210]->[+4]->[+8]); 0x634160(id) = mode on the stack.

## Save screen

- 0x499880 draws the save screen. 0x9BF220 (called at 0x4999B9) switches the render target
  back to the screen and calls 0x9BEF80 with the real size, which stores the 2D rect at
  display +0x204 (read by 0xA0AAA0) and sets the viewport. 0x498490 then lays the picture
  out in virtual pixels and 0x9DC030 turns pixels into clip space with that rect, so with
  HUD scaling the picture came out 1/scale size in the top-left. The mod re-sets the rect
  through 0xA0B560 with the whole screen in 2D units ([0x13961E8], [0x13961F0], doubles).

## Pause menu

- The pause menu (UI_TOP_LEVEL_PAUSE_SCREEN, list UI_LIST_PAUSE_MENU, built around 0x483187)
  comes from frontend data; the code only fills description texts (UI_*_DESC). Options rows
  (PC_OPTIONS_*_TEXT_SLIDER) are named widgets from the data too (built at 0x58CC40..).
  A new row needs either data or a runtime-built list (CFrontEndList, as the shop and item
  action lists are filled in code).
