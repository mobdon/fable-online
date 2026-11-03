# Front-end (menu) art

Fable's own menu look, so our start-up menu and launcher can match it. All of it
is read at runtime from the player's own install; none of it is committed.

## Where it lives

| File | What |
| --- | --- |
| `data/graphics/pc/frontend.big` | Bank `GBANK_FRONT_END_PC`, 394 textures. Names in `data/Defs/RetailHeaders/pc/front_end_bank.h`. |
| `data/Misc/LoadProgress/*.dds` | Loose DDS (DXT1/DXT5): loading screens `Fable_BG_PC_02..09` (1024x512, torn-edge framed paintings), `guild_seal_*`, `rc_seal_border`, `fable_BG_bar_border` (scrollwork), `slot_*_bar_off`. |
| `data/lang/English/fonts.big` | Bitmap fonts. `fonts.h` lists Tahoma 10/12, Arial 12/16/18/24/28, Times New Roman 16. |

`tools/re/big_extract.py <file.big> <out_dir> [filter]` dumps a .big bank to PNG (format notes in its docstring).
Textures are LZO1X-compressed DXT1 (format 31), DXT3/5 (format 32, alpha) or raw BGRA (format 1).

## Pieces worth using

- Title-menu backdrops, 640x480 built from 6 tiles (256,256,128 / 256,256,128):
  `FRONTEND_BG_0x_SPRITE` (forest, light shafts), `FORREST_n_x`, `COASTAL_n_x`, `SPOOKY_n_x`
  (4 animation frames each), plus `*_SUNBEAM_n_x` alpha overlays (draw additively).
- `FRONTEND_BACKDROP_01` (640x480): the hero walking into a misty forest. Good for hero select.
- Buttons: `FE_BUTTON_OFF` / `FE_BUTTON_ON` (256x64 pewter scroll bar, ON is warmer),
  or 9-slice `FE_BUTTON_{OFF,ON}_{L,M,R}` (64 + 8 + 64), `FE_BUTTON_ACCEPT_ON`, `FE_BUTTON_BACK_ON`.
- `FRONTEND_TITLE_01/02_SPRITE`: the FABLE / THE LOST CHAPTERS logo. Not used: we are "Chapters of the Lost Online".
- Frames: `UI_TABLE_TEST_*` corners/edges, `UI_TEXTBOX_*`, `MOUSE_POINTER_SPRITE_FE`.

## Palette (sampled)

- Pewter button: `#938D7E` (off), `#A28F74` (on). Text: `#E9E4D6`, highlight `#FFF6DC`, muted `#C9C0A8`.
- Logo chrome blue: `#9AA3F4` mid, `#3D4A9A` shadow band, `#F4F7FF` top.
- Backdrop greens: average `#2A2D23`, vignette to black.
