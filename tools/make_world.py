#!/usr/bin/env python3
"""Paints Skylark's world and packs it as 8x8 4bpp tiles + a 64x64 tile map.

    python tools/make_world.py [--preview out/world.png]

Writes src/WorldData.cpp and src/WorldData.h. Do not edit those by hand.

The world is 512x512 texels (one texel = 4 m, so 2 km square). Outside it
the game draws endless farmland procedurally (Skylark.ino, farm()); the
farmland painted here uses the same hash so the edges join up.
"""
import math, os, sys

W = 512
TEX_M = 4  # metres per texel

# Palette indices (must match Skylark.ino)
SKY_TOP, SKY_MID, HAZE, WHITE, GRASS_DK, GRASS, CROP, SOIL, WATER_DK, WATER, \
    ASPHALT, GREY_DK, RED, GOLD, INK, LIME = range(16)

# ---- the same farmland as the game's farm() ---------------------------------
def h32(x, y):
    h = (x * 73856093) ^ (y * 19349663)
    h &= 0xFFFFFFFF
    h ^= h >> 13
    h = (h * 0x5BD1E995) & 0xFFFFFFFF
    h ^= h >> 15
    return h

def farm(tx, ty):
    bx, by = tx >> 5, ty >> 5
    lx, ly = tx & 31, ty & 31
    h = h32(bx & 0xFFFF, by & 0xFFFF)
    if lx == 0 or ly == 0:
        return GRASS_DK                     # hedgerow
    t = h & 7
    s = ly if (h >> 8) & 1 else lx          # stripe axis
    if t <= 1: return CROP if s & 2 else GRASS       # young wheat
    if t == 2: return SOIL if s & 1 else CROP        # stubble
    if t == 3: return SOIL if s & 3 else GREY_DK     # ploughed
    if t == 4: return GRASS if s & 2 else GRASS_DK   # green crop
    if t == 5: return CROP                           # ripe
    return GRASS if ((tx * 7 + ty * 13) & 15) else GRASS_DK  # pasture

# ---- painting -----------------------------------------------------------------
img = [[farm(x, y) for x in range(W)] for y in range(W)]

def put(x, y, c):
    if 0 <= x < W and 0 <= y < W:
        img[y][x] = c

def rect(x0, y0, x1, y1, c):
    for y in range(y0, y1):
        for x in range(x0, x1):
            put(x, y, c)

def speck(x, y):
    # repeats every 16 texels so the forest packs into a few tiles
    x &= 15; y &= 15
    return ((x * 131 + y * 71) ^ (x * y * 3)) & 7

# Lake with a sandy shore
LX, LY, LRX, LRY = 140, 150, 56, 40
for y in range(LY - LRY - 4, LY + LRY + 5):
    for x in range(LX - LRX - 4, LX + LRX + 5):
        d = ((x - LX) / LRX) ** 2 + ((y - LY) / LRY) ** 2
        if d < 1.0:
            put(x, y, WATER_DK if (y % 8 == 0 and (x + (y >> 3) * 4) % 16 < 5) else WATER)
        elif d < 1.12:
            put(x, y, CROP)

# River from the lake out to the south edge
for y in range(LY + LRY - 4, W):
    cx = LX + 30 + 26 * math.sin(y / 37.0) + 10 * math.sin(y / 13.0)
    for x in range(int(cx) - 5, int(cx) + 6):
        d = abs(x - cx)
        if d < 3.2:
            put(x, y, WATER)
        elif d < 4.2:
            put(x, y, GRASS_DK)

# Forests
def forest(x0, y0, x1, y1):
    for y in range(y0, y1):
        for x in range(x0, x1):
            # ragged edge
            e = min(x - x0, x1 - 1 - x, y - y0, y1 - 1 - y)
            if e < 3 and speck(x, y) < 3 - e:
                continue
            s = speck(x, y)
            put(x, y, INK if s == 0 else (GRASS if s == 7 else GRASS_DK))

forest(40, 330, 130, 430)
forest(330, 400, 420, 470)
forest(20, 20, 70, 90)
forest(440, 260, 500, 330)

# Airfield: runway 09/27, 320 m x 36 m
RX0, RX1, RY0, RY1 = 216, 296, 326, 335
rect(RX0 - 6, RY0 - 4, RX1 + 6, RY1 + 4, GRASS)            # mown strip
rect(RX0, RY0, RX1, RY1, ASPHALT)
for x in range(RX0, RX1):
    put(x, RY0, WHITE); put(x, RY1 - 1, WHITE)            # edge lines
    if 8 <= x - RX0 < 72 and (x // 4) % 2 == 0:
        put(x, (RY0 + RY1) // 2, WHITE)                   # centre dashes
for x in list(range(RX0 + 1, RX0 + 5)) + list(range(RX1 - 5, RX1 - 1)):
    for y in range(RY0 + 2, RY1 - 2):
        if y % 2 == 0:
            put(x, y, WHITE)                              # threshold bars
rect(244, 316, 268, 326, ASPHALT)                         # apron
rect(254, 322, 258, 326, ASPHALT)
rect(244, 306, 254, 314, RED); rect(244, 314, 254, 315, INK)   # hangars
rect(257, 306, 267, 314, GREY_DK); rect(257, 314, 267, 315, INK)
put(272, 312, RED); put(272, 313, WHITE); put(272, 311, INK)    # windsock

# Road from the airfield to town
def road(points, c=GREY_DK):
    for (ax, ay), (bx, by) in zip(points, points[1:]):
        n = max(abs(bx - ax), abs(by - ay))
        for i in range(n + 1):
            x = round(ax + (bx - ax) * i / n); y = round(ay + (by - ay) * i / n)
            rect(x - 1, y - 1, x + 1, y + 1, c)

road([(256, 306), (256, 264), (368, 264), (368, 170)])

# Town
TX0, TY0 = 360, 96
for by in range(4):
    for bx in range(5):
        x0, y0 = TX0 + bx * 18, TY0 + by * 18
        rect(x0, y0, x0 + 18, y0 + 2, GREY_DK)
        rect(x0, y0, x0 + 2, y0 + 18, GREY_DK)
        for hy in range(2):
            for hx in range(2):
                hx0, hy0 = x0 + 4 + hx * 7, y0 + 4 + hy * 7
                k = (bx * 3 + by * 5 + hx + hy * 2) % 4
                if k == 3:
                    put(hx0 + 1, hy0 + 1, GRASS_DK); put(hx0 + 2, hy0 + 2, INK)  # a tree
                    continue
                roof = (RED, SOIL, GREY_DK)[k]
                rect(hx0, hy0, hx0 + 4, hy0 + 3, roof)
                rect(hx0 + 1, hy0 + 3, hx0 + 5, hy0 + 4, INK)              # shadow
rect(TX0, TY0 + 72, TX0 + 92, TY0 + 74, GREY_DK)
rect(TX0 + 90, TY0, TX0 + 92, TY0 + 74, GREY_DK)
# church / water tower
rect(TX0 + 40, TY0 + 40, TX0 + 50, TY0 + 46, WHITE); rect(TX0 + 41, TY0 + 46, TX0 + 51, TY0 + 47, INK)

# Landing target: concentric rings on a mown clearing
GX, GY = 300, 230
for y in range(GY - 14, GY + 15):
    for x in range(GX - 14, GX + 15):
        d = math.hypot(x + 0.5 - GX, y + 0.5 - GY)
        if d < 12:
            put(x, y, GRASS)
        if d < 1.5: put(x, y, RED)
        elif d < 3: put(x, y, WHITE)
        elif d < 4.5: put(x, y, RED)
        elif d < 6: put(x, y, WHITE)
        elif d < 7.5: put(x, y, RED)
        elif d < 8.5: put(x, y, WHITE)

# ---- tiling -------------------------------------------------------------------
tiles, index, tmap = [], {}, []
for ty in range(64):
    for tx in range(64):
        t = tuple(img[ty * 8 + y][tx * 8 + x] for y in range(8) for x in range(8))
        if t not in index:
            index[t] = len(tiles)
            tiles.append(t)
        tmap.append(index[t])

def dist(a, b):
    return sum(1 for p, q in zip(a, b) if p != q)

print('unique tiles before merging:', len(tiles))

# Merge the closest tiles until 256 fit in a byte
while len(tiles) > 256:
    use = [0] * len(tiles)
    for i in tmap: use[i] += 1
    # drop the tile whose replacement changes the fewest texels on the map
    best_cost = None
    for i in range(len(tiles)):
        j = min((k for k in range(len(tiles)) if k != i), key=lambda k: dist(tiles[i], tiles[k]))
        c = dist(tiles[i], tiles[j]) * use[i]
        if best_cost is None or c < best_cost:
            best_cost, r, best = c, i, j
    tmap = [best if i == r else i for i in tmap]
    tiles.pop(r)
    tmap = [i - 1 if i > r else i for i in tmap]

def pack(t):
    out = []
    for y in range(8):
        for x in range(0, 8, 2):
            out.append(t[y * 8 + x] | (t[y * 8 + x + 1] << 4))   # even x low nibble
    return out

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, '..', 'src')
os.makedirs(src, exist_ok=True)
with open(os.path.join(src, 'WorldData.h'), 'w') as f:
    f.write('// Generated by tools/make_world.py. Do not edit.\n#pragma once\n#include <stdint.h>\n')
    f.write('#define WORLD_TEXELS %d\n#define WORLD_TILE_COUNT %d\n' % (W, len(tiles)))
    f.write('extern const uint8_t WORLD_MAP[64 * 64];\nextern const uint8_t WORLD_TILES[%d * 32];\n' % len(tiles))
    f.write('extern const uint8_t WORLD_TILE_FAR[%d];   // each tile\'s commonest colour, for distant ground\n' % len(tiles))
with open(os.path.join(src, 'WorldData.cpp'), 'w') as f:
    f.write('// Generated by tools/make_world.py. Do not edit.\n#include "WorldData.h"\n')
    f.write('const uint8_t WORLD_MAP[64 * 64] = {\n')
    for r in range(64):
        f.write('  ' + ','.join(str(v) for v in tmap[r * 64:(r + 1) * 64]) + ',\n')
    f.write('};\nconst uint8_t WORLD_TILES[%d * 32] = {\n' % len(tiles))
    for t in tiles:
        f.write('  ' + ','.join('0x%02X' % b for b in pack(t)) + ',\n')
    f.write('};\nconst uint8_t WORLD_TILE_FAR[%d] = {\n  ' % len(tiles))
    f.write(','.join(str(max(set(t), key=t.count)) for t in tiles))
    f.write('\n};\n')
print('tiles: %d (%d B), map: 4096 B' % (len(tiles), len(tiles) * 32))

if '--preview' in sys.argv:
    from PIL import Image
    PAL = [(42,93,176),(79,134,208),(196,214,232),(244,244,236),(56,110,44),(93,155,58),
           (208,184,84),(138,106,62),(40,90,138),(79,143,191),(142,142,136),(78,80,84),
           (216,52,44),(240,170,36),(16,18,20),(124,240,106)]
    out = Image.new('RGB', (W, W))
    for y in range(W):
        for x in range(W):
            t = tiles[tmap[(y >> 3) * 64 + (x >> 3)]]
            out.putpixel((x, y), PAL[t[(y & 7) * 8 + (x & 7)]])
    p = sys.argv[sys.argv.index('--preview') + 1]
    os.makedirs(os.path.dirname(p) or '.', exist_ok=True)
    out.save(p)
