import math
import random

from PIL import Image, ImageDraw, ImageFilter

random.seed(42617)

W = H = 128
img = Image.new("RGBA", (W, H), (0, 0, 0, 0))

bark = Image.new("RGBA", (W, H), (0, 0, 0, 0))
for y in range(0, 41):
    for x in range(W):
        n = random.random()
        streak = math.sin(x * 0.35 + y * 0.02) * 9.0
        r = int(104 + n * 14 + streak)
        g = int(78 + n * 10 + streak * 0.7)
        b = int(54 + n * 8 + streak * 0.4)
        bark.putpixel((x, y), (min(255, r), min(255, g), min(255, b), 255))
img = Image.alpha_composite(img, bark)

def leaflet(cx, cy, box):
    w = random.uniform(8, 15)
    h = random.uniform(8, 15)
    if random.random() < 0.5:
        w, h = h, w
    hue = random.uniform(78, 158)
    sat = random.uniform(0.35, 0.7)
    val = random.uniform(0.38, 0.72)
    mr, mg, mb = [int(c * 255) for c in __import__("colorsys").hsv_to_rgb(hue / 360.0, sat, val)]
    pad = 8
    side = int(max(w, h)) + pad * 2
    layer = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    c0 = (side // 2, side // 2)
    blobs = [(0, 0, w, h)]
    if random.random() < 0.45:
        blobs.append((random.uniform(-3, 3), random.uniform(-3, 3), w * 0.75, h * 0.75))
    for ox, oy, bw, bh in blobs:
        x0 = c0[0] + ox - bw / 2.0
        y0 = c0[1] + oy - bh / 2.0
        d.ellipse([x0, y0, x0 + bw, y0 + bh], fill=(mr, mg, mb, 255))
    layer = layer.rotate(random.uniform(0, 360), resample=Image.BICUBIC, expand=False)
    px = layer.load()
    for y in range(side):
        for x in range(side):
            r, g, b, a = px[x, y]
            if a > 40:
                jitter = random.randint(-14, 14)
                px[x, y] = (max(0, min(255, r + jitter)),
                            max(0, min(255, g + jitter)),
                            max(0, min(255, b + jitter)), a)
    layer = layer.filter(ImageFilter.GaussianBlur(0.55))
    bbox = layer.getbbox()
    if bbox is None:
        return
    crop = layer.crop(bbox)
    pasteClipped(crop, cx, cy, box)

def pasteClipped(layer, cx, cy, box):
    dx = int(cx - layer.width / 2)
    dy = int(cy - layer.height / 2)
    x0 = max(0, box[0] - dx)
    y0 = max(0, box[1] - dy)
    x1 = min(layer.width, box[2] - dx)
    y1 = min(layer.height, box[3] - dy)
    if x1 <= x0 or y1 <= y0:
        return
    sub = layer.crop((x0, y0, x1, y1))
    img.paste(sub, (dx + x0, dy + y0), sub)

innerBox = (5, 46, 123, 124)
for _ in range(70):
    cx = random.uniform(20, 108)
    cy = random.uniform(56, 114)
    leaflet(cx, cy, innerBox)

img = Image.alpha_composite(img, bark)
img.save("/tmp/ash_leaf_sprite.png")
img.save("c-game/data/pak_1/images/leaf-textures/ash.png")

a = img.load()
strip_ok = all(a[x, y][3] == 255 for y in range(0, 41) for x in range(W))
bark_ok = all(a[x, y][3] == 255 for y in range(13, 29) for x in range(25, 103))
region = 0
kept = 0
for y in range(44, 127):
    for x in range(3, 126):
        region += 1
        if a[x, y][3] > 128:
            kept += 1
border_ok = True
for y in range(46, 124):
    for x in (3, 4, 5, 121, 122, 123, 124):
        if a[x, y][3] > 128:
            border_ok = False
for x in range(5, 122):
    for y in (44, 45, 46, 123, 124, 125):
        if a[x, y][3] > 128:
            border_ok = False
print("strip_rows_0_40_opaque:", strip_ok)
print("bark_rect_opaque:", bark_ok)
print("leaf_region_border_clear:", border_ok)
print("leaf_region_fill: %.2f" % (kept / region))
