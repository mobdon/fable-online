# Fonts (fonts.big)

`data/lang/English/fonts.big`, bank `FONT_ENGLISH_MAIN`: 8 bitmap fonts, read at runtime by
`client-mod/src/assets/fable_font.cpp`. Never committed.

| Entry | Face, line height | Overlay use |
| --- | --- | --- |
| `ENG_TAHOMA_12` | Tahoma, 16 px | chat text (Fable's HUD text) |
| `ENG_TAHOMA_10` | Tahoma, 13 px | name tag title and renown |
| `ENG_ARIAL_16` | Arial, 21 px | name tag names |
| `ENG_ARIAL_12/18/24/28`, `ENG_TIMES_NEW_ROMAN_16` (really Arial 21) | | unused |

Entries are stored uncompressed (the entry's subheader is empty):

```
cstring face
u32 height, u32 weight, u8 italic
u32 line_height, tex_w, tex_h, first_char (32), last_char (127), chunk_count
per chunk: u32 index, u32 ?, u32 glyph_count
           glyph_count x { f32 u0, v0, u1, v1; i16 bearing; u16 width; u16 advance }
BGRA texture, tex_w * tex_h * 4 bytes (white glyphs, coverage in alpha), at the end of the entry
```

UVs are normalised by `(tex_w - 1)` and `(tex_h - 1)`, not by the size. Only ASCII 32..127.
`ENG_ARIAL_18` lists 'z' below the bottom of its texture; the parser marks such glyphs invalid.

The bank also has `STREAMING_FONT_ENGLISH_PC` with a 26 MB `GlyphData` entry (not needed).
