import sys, glob, os
from PIL import Image, ImageDraw
d = sys.argv[1]; olcek = 3
dosyalar = sorted(glob.glob(os.path.join(d, '*.raw')))
ims = []
for f in dosyalar:
    raw = open(f, 'rb').read()
    im = Image.new('RGB', (160, 128))
    px = im.load()
    for i in range(160 * 128):
        v = raw[2 * i] | (raw[2 * i + 1] << 8)
        r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31
        px[i % 160, i // 160] = (r * 255 // 31, g * 255 // 63, b * 255 // 31)
    ims.append((os.path.basename(f), im.resize((160 * olcek, 128 * olcek), Image.NEAREST)))
sut = 3; W = 160 * olcek; H = 128 * olcek; pad = 16
satir = (len(ims) + sut - 1) // sut
pafta = Image.new('RGB', (sut * (W + pad) + pad, satir * (H + pad + 18) + pad), (40, 30, 24))
dr = ImageDraw.Draw(pafta)
for i, (ad, im) in enumerate(ims):
    x = pad + (i % sut) * (W + pad); y = pad + (i // sut) * (H + pad + 18)
    pafta.paste(im, (x, y + 18)); dr.text((x, y + 2), ad, fill=(220, 200, 180))
pafta.save(sys.argv[2])
print(sys.argv[2], pafta.size)
