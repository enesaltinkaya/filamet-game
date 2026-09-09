import math
import random

from PIL import Image, ImageDraw, ImageFilter

random.seed(36330)

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

leaflets = []
slots = []
for ix in range(5):
    for iy in range(3):
        cx = 22 + ix * 25 + random.uniform(-7, 7)
        cy = 58 + iy * 30 + random.uniform(-9, 9)
        slots.append((cx, cy))
for (cx, cy) in slots:
    lw = random.randint(16, 24)
    lh = random.randint(20, 30)
    col = (
        random.randint(80, 122),
        random.randint(136, 168),
        random.randint(52, 84),
    )
    leaflets.append((cx, cy, lw, lh, col))

for cx, cy, lw, lh, col in leaflets:
    layer = Image.new("RGBA", (lw + 4, lh + 4), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.ellipse([0, 0, lw + 3, lh + 3], fill=(col[0], col[1], col[2], 255))
    for _ in range(240):
        px = random.randint(1, lw + 2)
        py = random.randint(1, lh + 2)
        if layer.getpixel((px, py))[3] > 200:
            layer.putpixel((px, py), (max(0, col[0] + random.randint(-10, 10)),
                                      max(0, col[1] + random.randint(-10, 10)),
                                      max(0, col[2] + random.randint(-10, 10)), 255))
    layer = layer.rotate(random.randint(0, 359), resample=Image.BICUBIC, expand=False)
    layer = layer.filter(ImageFilter.GaussianBlur(0.9))
    img.paste(layer, (int(cx) - layer.width // 2, int(cy) - layer.height // 2), layer)

img = Image.alpha_composite(img, bark)
img.save("/tmp/ash_leaf_sprite.png")

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
print("strip_rows_0_40_opaque:", strip_ok)
print("bark_rect_opaque:", bark_ok)
print("leaf_region_fill: %.2f" % (kept / region))
