# Formula 1 UI theme

Branch: `Formula_one_theme`

The firmware now has two visual languages that can be switched at runtime:

- `PIXEL` — existing 5x7 pixel-font dashboard, unchanged.
- `FORMULA 1` — carbon/broadcast styling with Formula1 Display Bold bitmap glyphs,
  timing-tower rows, team-colour accents, tyre compound markers, DRS/BOX strips,
  purple fastest-lap rows, and checkered-flag details.

Switch it on the device:

`SETTINGS > UI STYLE > PUSH`

The choice is saved in NVS (`uistyle`) and restored on boot. Theme light/dark and
all existing screens remain compatible; the F1 style uses its own dark/light
palettes and font metrics.

## Font

`src/font_f1.h` is generated from **Formula1 Display Bold**:

- Source: `F1_Lap_Timing/assets/fonts/Formula1-Bold_web_0.ttf`
- Reference project: https://github.com/wertewrt/F1_Lap_Timing
- Copyright (c) 2017 Marc Rouault / W+K. Personal/non-commercial use.
- Generated sizes: 8, 12, 16, 24, 40 px, proportional ASCII glyphs.
- Generator: `firmware/tools/gen_f1_font.py` (requires Pillow).

The font is not an official Formula 1 / FIA product; F1 is used only as a visual
theme reference.

## Compatibility

- `ui_style = 0` keeps the original pixel layout byte-for-byte where possible.
- `ui_style = 1` changes palette, font and selected home/race chrome.
- Switching happens immediately on the next rendered frame.
- Track/session storage and all APIs are independent of the theme.

## Preview renders

Host-rendered 3x previews with the real Formula1 Display Bold and the F1 palette:

- [Home](theme/f1-home.png)
- [Race map / live](theme/f1-race-map.png)
- [Race timer](theme/f1-race-timer.png)
- [Race speed / G](theme/f1-race-speed.png)
- [Sector splits](theme/f1-race-sectors.png)
- [Settings / UI STYLE](theme/f1-settings.png)

These are design previews; the on-device version uses the same font, palette and
home/race layout, with live telemetry substituted for the sample values.
