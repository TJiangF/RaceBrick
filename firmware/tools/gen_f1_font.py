from PIL import Image, ImageDraw, ImageFont
from pathlib import Path
FONT='assets/fonts/Formula1-Bold_web_0.ttf'
sizes=[8,12,16,24,32]
out=[]
out.append('/* Auto-generated from Formula1 Display Bold, size-specific ASCII bitmaps.\n'
           ' * Source: F1_Lap_Timing/assets/fonts/Formula1-Bold_web_0.ttf\n'
           ' * Copyright (c) 2017 Marc Rouault / W+K. Personal/non-commercial use.\n'
           ' * Row bits: MSB = left, one bit per pixel, row-major. */\n')
out.append('#pragma once\n#include <stdint.h>\n\n')
out.append('typedef struct { uint16_t off; uint8_t w; uint8_t adv; } f1glyph_t;\n')
out.append('typedef struct { uint8_t h; const f1glyph_t *glyphs; const uint8_t *bits; } f1font_t;\n\n')
for size in sizes:
    font=ImageFont.truetype(FONT,size)
    asc,desc=font.getmetrics()
    h=asc+desc
    glyphs=[]; bits=bytearray()
    for code in range(32,128):
        ch=chr(code)
        adv=max(1, int(round(font.getlength(ch))))
        if ch==' ':
            glyphs.append((len(bits),0,adv))
            continue
        # render baseline at asc, left bearing may shift
        bb=font.getbbox(ch, anchor='ls')
        x0=max(0,bb[0]); y0=min(0,bb[1]); x1=bb[2]; y1=max(h,bb[3])
        # cell width: advance (so spacing controlled by font metrics), may clip overhang
        cw=max(adv, x1 - min(0,bb[0]))
        img=Image.new('L',(cw,h),0)
        d=ImageDraw.Draw(img)
        d.text((-min(0,bb[0]),asc),ch,font=font,fill=255,anchor='ls')
        # pack rows, MSB left, pad to bytes
        off=len(bits)
        for y in range(h):
            row=0
            for x in range(cw):
                row=(row<<1)|(1 if img.getpixel((x,y))>=96 else 0)
            # pack each row into ceil(cw/8) bytes, MSB across width
            nbytes=(cw+7)//8
            val=row << (nbytes*8-cw)
            for k in range(nbytes): bits.append((val>>(8*(nbytes-1-k)))&0xff)
        glyphs.append((off,cw,adv))
    # arrays
    gname=f'F1_{size}_GLYPHS'; bname=f'F1_{size}_BITS'
    out.append(f'#define F1_{size}_H {h}\n')
    out.append(f'static const f1glyph_t {gname}[96] = {{\n')
    for i,g in enumerate(glyphs):
        out.append(f'  {{{g[0]},{g[1]},{g[2]}}},')
        if i%8==7: out.append('\n')
    out.append('\n};\n')
    out.append(f'static const uint8_t {bname}[] = {{\n')
    for i,b in enumerate(bits):
        out.append(f'{b},')
        if i%32==31: out.append('\n')
    out.append('\n};\n')
    out.append(f'static const f1font_t F1_{size} = {{ F1_{size}_H, {gname}, {bname} }};\n\n')
out.append('static const f1font_t *const F1_FONTS[5] = { &F1_8, &F1_12, &F1_16, &F1_24, &F1_32 };\n')
Path(__file__).resolve().parents[1].joinpath('src/font_f1.h').write_text(''.join(out))
print('written', Path(__file__).resolve().parents[1].joinpath('src/font_f1.h').stat().st_size)
