import colorsys
import math
import random
import sys

from PIL import Image, ImageDraw, ImageFilter

random.seed(42617)

W = H = 256
BARK_ROWS = 80
MOAT = 8
CX, CY = 128.0, 168.0
RX, RY = 104.0, 60.0
CORE = 0.55
CONTENT = (MOAT, BARK_ROWS + MOAT, W - 1 - MOAT, H - 1 - MOAT)

img = Image.new("RGBA", (W, H), (0, 0, 0, 0))


def drawLeaflet(cx, cy, w, h):
    hue = random.uniform(78, 158)
    sat = random.uniform(0.35, 0.75)
    val = random.uniform(0.35, 0.78)
    mr, mg, mb = [int(c * 255) for c in colorsys.hsv_to_rgb(hue / 360.0, sat, val)]
    pad = 8
    side = int(max(w, h)) + pad * 2
    layer = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    c0 = (side // 2, side // 2)
    blobs = [(0.0, 0.0, w, h)]
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
                j = random.randint(-14, 14)
                px[x, y] = (max(0, min(255, r + j)),
                            max(0, min(255, g + j)),
                            max(0, min(255, b + j)), a)
    layer = layer.filter(ImageFilter.GaussianBlur(0.55))
    bbox = layer.getbbox()
    if bbox is None:
        return False
    crop = layer.crop(bbox)
    dx = int(cx - (bbox[0] + bbox[2]) / 2.0)
    dy = int(cy - (bbox[1] + bbox[3]) / 2.0)
    x0 = max(0, -dx)
    y0 = max(0, -dy)
    x1 = min(crop.width, W - dx)
    y1 = min(crop.height, H - dy)
    if x1 <= x0 or y1 <= y0:
        return False
    sub = crop.crop((x0, y0, x1, y1))
    img.paste(sub, (dx + x0, dy + y0), sub)
    return True


def fits(cx, cy, w, h):
    half = max(w, h) / 2.0 + 10.0
    return (CONTENT[0] + half <= cx <= CONTENT[2] - half and
            CONTENT[1] + half <= cy <= CONTENT[3] - half)


def place(n, wmin, wmax, stretch=None, rlo=None, rhi=None, triesPer=40):
    placed = 0
    tries = 0
    while placed < n and tries < n * triesPer:
        tries += 1
        if rlo is not None:
            ang = random.uniform(0.0, 2.0 * math.pi)
            f = random.uniform(rlo, rhi)
            px_ = CX + RX * f * math.cos(ang)
            py_ = CY + RY * f * math.sin(ang)
        else:
            px_ = CX + random.gauss(0.0, RX * 0.5 * stretch)
            py_ = CY + random.gauss(0.0, RY * 0.5 * stretch)
            if ((px_ - CX) / (RX * stretch)) ** 2 + ((py_ - CY) / (RY * stretch)) ** 2 > 1.0:
                continue
        w = random.uniform(wmin, wmax)
        h = random.uniform(wmin, wmax)
        if not fits(px_, py_, w, h):
            continue
        if drawLeaflet(px_, py_, w, h):
            placed += 1
    return placed


count = place(105, 6, 14, stretch=0.7)
count += place(60, 6, 12, stretch=1.0)
count += place(45, 6, 12, rlo=0.55, rhi=0.85)
count += place(30, 6, 11, rlo=0.80, rhi=1.02)
count += place(6, 5, 9, rlo=0.95, rhi=1.15, triesPer=60)


def coreCoverage(a):
    rx = RX * CORE
    ry = RY * CORE
    area = 0
    cov = 0
    for y in range(int(CY - ry), int(CY + ry) + 2):
        for x in range(int(CX - rx), int(CX + rx) + 2):
            if 0 <= x < W and 0 <= y < H:
                if ((x + 0.5 - CX) / rx) ** 2 + ((y + 0.5 - CY) / ry) ** 2 <= 1.0:
                    area += 1
                    if a[x, y][3] >= 128:
                        cov += 1
    return cov / area if area else 0.0


a = img.load()
while count < 250:
    if coreCoverage(a) >= 0.70:
        break
    added = 0
    for _ in range(50):
        ang = random.uniform(0.0, 2.0 * math.pi)
        f = math.sqrt(random.random())
        px_ = CX + RX * CORE * f * math.cos(ang)
        py_ = CY + RY * CORE * f * math.sin(ang)
        if a[int(px_), int(py_)][3] >= 128:
            continue
        w = random.uniform(6, 11)
        h = random.uniform(6, 11)
        if not fits(px_, py_, w, h):
            continue
        if drawLeaflet(px_, py_, w, h):
            added += 1
    count += added
    if added == 0:
        break

pa = img.load()
for y in range(BARK_ROWS, H):
    for x in range(W):
        if y < BARK_ROWS + MOAT or y >= H - MOAT or x < MOAT or x >= W - MOAT:
            pa[x, y] = (0, 0, 0, 0)


def barkBand():
    band = Image.new("RGBA", (W, BARK_ROWS), (0, 0, 0, 0))
    for y in range(BARK_ROWS):
        for x in range(W):
            n = random.random()
            streak = math.sin(x * 0.35 + y * 0.02) * 9.0
            r = int(104 + n * 14 + streak)
            g = int(78 + n * 10 + streak * 0.7)
            b = int(54 + n * 8 + streak * 0.4)
            band.putpixel((x, y), (min(255, r), min(255, g), min(255, b), 255))
    return band


overlay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
overlay.paste(barkBand(), (0, 0))
img = Image.alpha_composite(img, overlay)
img.save("/tmp/ash_leaf_sprite.png")
img.save("c-game/data/pak_1/images/leaf-textures/ash.png")

a = img.load()
bark_ok = all(a[x, y][3] == 255 for y in range(BARK_ROWS) for x in range(W))
moat_ok = True
for y in range(BARK_ROWS, H):
    for x in range(W):
        if y < BARK_ROWS + MOAT or y >= H - MOAT or x < MOAT or x >= W - MOAT:
            if a[x, y][3] >= 128:
                moat_ok = False
region_area = 0
region_cov = 0
for y in range(BARK_ROWS, H):
    for x in range(W):
        region_area += 1
        if a[x, y][3] >= 128:
            region_cov += 1
core_cov = coreCoverage(a)
count_ok = 150 <= count <= 250
ok = bark_ok and moat_ok and core_cov >= 0.6 and count_ok
print("bark_rows_0_%d_opaque: %s" % (BARK_ROWS - 1, "OK" if bark_ok else "FAIL"))
print("moat_%dpx_clear: %s" % (MOAT, "OK" if moat_ok else "FAIL"))
print("core_coverage: %.3f (>= 0.6: %s)" % (core_cov, "OK" if core_cov >= 0.6 else "FAIL"))
print("leaf_region_fill: %.3f" % (region_cov / region_area))
print("leaflet_count: %d (150-250: %s)" % (count, "OK" if count_ok else "FAIL"))
sys.exit(0 if ok else 1)
