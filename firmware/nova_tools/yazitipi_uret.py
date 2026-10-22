#!/usr/bin/env python3
"""Nova ekranı için kenar yumuşatmalı (4 bit) bitmap yazı tipleri üretir.

Çıktı: nova_live/yazitipleri.h. Biçim Adafruit GFXfont'a benzer ama her piksel 4 bit örtü (0-15) taşır;
ekran.cpp bunu arka plan rengiyle karıştırarak çizer. Türkçe harfler 0x80 sonrasına eşlenir (ekran.cpp'deki
harfKodu() aynı tabloyu kullanır). Yazı tipi: Chakra Petch (SIL Open Font License 1.1, nova_live/YAZITIPI_OFL.txt).
Kullanım: python3 yazitipi_uret.py <ttf klasörü> <çıkış .h>
"""
import sys, math
from PIL import Image, ImageFont

# 0x80'den sonraki özel harfler (sıra ekran.cpp'deki harfKodu() OZEL tablosuyla aynı olmalı)
OZEL = ['ğ', 'Ğ', 'ı', 'İ', 'ş', 'Ş', '…', '·', '°', 'ç', 'Ç', 'ö', 'Ö', 'ü', 'Ü', 'â', 'î', 'û', '–', '’']

def karakterler(ilk, son):
    out = []
    for k in range(ilk, son + 1):
        if k < 0x80:
            out.append(chr(k))
        else:
            out.append(OZEL[k - 0x80])
    return out

def maske(ttf, boyut, ch, ss):
    """Harfin örtüsü (L kipi resim) ve sol üst köşesinin taban çizgisindeki kaleme göre yeri.
    ss > 1: harf ss kat büyük çizilip kutu süzgeciyle küçültülür. Küçük boyutta FreeType'ın ızgaraya
    oturtması (hinting) bazı harfleri (ör. 'a') bir piksel kaydırıp aralıkları bozuyor; büyük çizmek bunu önler."""
    if ss == 1:
        m, o = ImageFont.truetype(ttf, boyut).getmask2(ch, mode='L', anchor='ls')
        if not m.size[0] or not m.size[1]:
            return None, 0, 0
        return Image.frombytes('L', m.size, bytes(m)), o[0], o[1]
    m, o = ImageFont.truetype(ttf, boyut * ss).getmask2(ch, mode='L', anchor='ls')
    if not m.size[0] or not m.size[1]:
        return None, 0, 0
    buyuk = Image.frombytes('L', m.size, bytes(m))
    x0 = math.floor(o[0] / ss) * ss
    y0 = math.floor(o[1] / ss) * ss
    pl, pt = o[0] - x0, o[1] - y0
    W = math.ceil((pl + m.size[0]) / ss) * ss
    H = math.ceil((pt + m.size[1]) / ss) * ss
    c = Image.new('L', (W, H), 0)
    c.paste(buyuk, (pl, pt))
    return c.resize((W // ss, H // ss), Image.BOX), x0 // ss, y0 // ss

def uret(ad, ttf, boyut, ilk, son, ss=1):
    font = ImageFont.truetype(ttf, boyut)
    buyuk = ImageFont.truetype(ttf, boyut * ss)
    asc, desc = font.getmetrics()
    bits = bytearray()
    glifler = []
    for ch in karakterler(ilk, son):
        adv = min(255, int(round(buyuk.getlength(ch) / ss * 8)))   # 1/8 piksel
        if ch == ' ':
            glifler.append((len(bits), 0, 0, adv, 0, 0))
            continue
        mask, ox, oy = maske(ttf, boyut, ch, ss)
        if mask is None:
            glifler.append((len(bits), 0, 0, adv, 0, 0))
            continue
        ofs = (ox, oy)
        w, h = mask.size
        # boş kenarları kırp
        px = [[mask.getpixel((x, y)) for x in range(w)] for y in range(h)]
        top = 0
        while top < h and max(px[top]) < 8: top += 1
        bot = h
        while bot > top and max(px[bot - 1]) < 8: bot -= 1
        left = 0
        while left < w and max(px[y][left] for y in range(top, bot)) < 8 if bot > top else False: left += 1
        right = w
        while right > left and max(px[y][right - 1] for y in range(top, bot)) < 8 if bot > top else False: right -= 1
        if bot <= top or right <= left:
            glifler.append((len(bits), 0, 0, adv, 0, 0))
            continue
        gw, gh = right - left, bot - top
        off = len(bits)
        nib = []
        for y in range(top, bot):
            for x in range(left, right):
                nib.append(min(15, (px[y][x] * 15 + 127) // 255))
        if len(nib) % 2: nib.append(0)
        for i in range(0, len(nib), 2):
            bits.append((nib[i] << 4) | nib[i + 1])
        glifler.append((off, gw, gh, adv, ofs[0] + left, ofs[1] + top))
    satir = asc + desc
    return ad, bits, glifler, ilk, son, satir, asc

def yaz(sonuclar, cikis):
    L = ['// Otomatik üretildi: nova_tools/yazitipi_uret.py (Chakra Petch, SIL Open Font License 1.1)',
         '// 4 bit kenar yumuşatmalı bitmap yazı tipleri. Elle düzenleme; betiği yeniden çalıştır.',
         '#pragma once', '#include <stdint.h>', '',
         'struct NGlif { uint16_t ofs; uint8_t w, h, adv8; int8_t xo, yo; };   // adv8: ilerleme, 1/8 piksel',
         'struct NFont { const uint8_t *bmp; const NGlif *g; uint16_t ilk, son; uint8_t satir, ust; };', '']
    for ad, bits, glifler, ilk, son, satir, asc in sonuclar:
        L.append('static const uint8_t %s_bmp[] = {' % ad)
        for i in range(0, len(bits), 24):
            L.append('  ' + ','.join('0x%02X' % b for b in bits[i:i + 24]) + ',')
        L.append('};')
        L.append('static const NGlif %s_g[] = {' % ad)
        for g in glifler:
            L.append('  {%d,%d,%d,%d,%d,%d},' % g)
        L.append('};')
        L.append('static const NFont %s = {%s_bmp, %s_g, 0x%02X, 0x%02X, %d, %d};' % (ad, ad, ad, ilk, son, satir, asc))
        L.append('')
    open(cikis, 'w', encoding='utf-8').write('\n'.join(L))

if __name__ == '__main__':
    d, cikis = sys.argv[1], sys.argv[2]
    son = 0x80 + len(OZEL) - 1
    sonuclar = [
        uret('F_SAAT', d + '/ChakraPetch-SemiBold.ttf', 40, 0x2D, 0x3A),
        uret('F_YAZI', d + '/ChakraPetch-Medium.ttf', 12, 0x20, son),
        uret('F_ETIKET', d + '/ChakraPetch-Medium.ttf', 10, 0x20, son),
        uret('F_KUCUK', d + '/ChakraPetch-Regular.ttf', 10, 0x20, son, ss=4),
    ]
    yaz(sonuclar, cikis)
    for ad, bits, g, ilk, son_, satir, asc in sonuclar:
        print(ad, 'glif', len(g), 'bayt', len(bits), 'satir', satir, 'ust', asc)
