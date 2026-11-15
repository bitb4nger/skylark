// SKYLARK: a Pilotwings-style flight demake for the CHGame.
//
// Events
//   LIGHT PLANE  fly the ring course over town and lake, land on runway 09.
//   HANG GLIDER  ride thermals up through the rings, land on the target.
//   JET          fast ring courses: speed gates, and a low-level run through
//                the downtown towers. Max throttle + A = afterburner.
//   FREE FLIGHT  take off from the runway at golden hour and go anywhere.
//   Each of the first three has several courses: LEFT/RIGHT on the title.
//
// Controls
//   D-pad        stick: LEFT/RIGHT roll, UP/DOWN pitch (UP = nose down,
//                like a real stick; SELECT on the title screen flips it)
//   A / B        plane, jet: throttle up / down (B also brakes on the ground;
//                jet: B held at idle in the air = speedbrake)
//                glider: B = airbrake
//   START        pause (SELECT while paused quits to the title)
//
// The ground is ray-cast per pixel pair against a flat world, so the horizon
// banks with the aircraft. It runs from SRAM (RAMFUNC) and writes the 4 bpp
// framebuffer directly; everything else is CHGfx drawing on top.
#include <CHGame.h>
#if defined(CHSIM)
#include <stdio.h>
#endif
#include "src/WorldData.h"
#include "src/Math3.h"
#include "src/Game.h"

// ---------------------------------------------------------------------------
// Palette

#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
static const uint16_t PAL_DAY[16] = {
    RGB(40, 88, 170),  RGB(86, 140, 214), RGB(186, 210, 232), RGB(244, 244, 236),
    RGB(52, 104, 44),  RGB(92, 152, 58),  RGB(206, 182, 84),  RGB(136, 102, 60),
    RGB(36, 84, 132),  RGB(74, 138, 190), RGB(130, 130, 126), RGB(74, 76, 80),
    RGB(214, 50, 42),  RGB(244, 176, 32), RGB(14, 16, 20),    RGB(120, 236, 100),
};
static const uint16_t PAL_DUSK[16] = {
    RGB(78, 62, 126),  RGB(168, 100, 132), RGB(250, 170, 110), RGB(255, 232, 200),
    RGB(54, 70, 40),   RGB(96, 104, 50),  RGB(186, 124, 62),  RGB(112, 70, 48),
    RGB(56, 50, 96),   RGB(170, 96, 120), RGB(128, 104, 100), RGB(66, 54, 58),
    RGB(255, 80, 60),  RGB(255, 190, 70), RGB(14, 12, 16),    RGB(255, 214, 120),
};

// ---------------------------------------------------------------------------
// Sound
AUDIO_STEPS(S_CHIME) = { AUDIO_STEP(1200, 1800, 60), AUDIO_REST(10), AUDIO_STEP(2400, 0, 90) };
AUDIO_STEPS(S_CRASH) = { AUDIO_STEP(500, 120, 260), AUDIO_STEP(180, 60, 200) };
AUDIO_STEPS(S_LAND)  = { AUDIO_STEP(800, 0, 50), AUDIO_STEP(1000, 0, 50), AUDIO_STEP(1300, 0, 50), AUDIO_STEP(1600, 0, 120) };
AUDIO_STEPS(S_MENU)  = { AUDIO_STEP(1500, 0, 25) };
AUDIO_STEPS(S_MISS)  = { AUDIO_STEP(600, 300, 120) };
AUDIO_STEPS(S_BURN)  = { AUDIO_STEP(180, 420, 220) };
enum { SFX_CHIME, SFX_CRASH, SFX_LAND, SFX_MENU, SFX_MISS, SFX_BURN };
const audio::Effect SOUNDS[] = {
    AUDIO_EFFECT(S_CHIME, 2), AUDIO_EFFECT(S_CRASH, 3), AUDIO_EFFECT(S_LAND, 3),
    AUDIO_EFFECT(S_MENU, 1), AUDIO_EFFECT(S_MISS, 2), AUDIO_EFFECT(S_BURN, 2),
};

// ---------------------------------------------------------------------------
// Save
struct SaveData { uint16_t best[3][3]; uint8_t invert; uint8_t course[3]; };
const uint32_t MAGIC = save::magic("SKYL");
static SaveData sv = {};
static bool svDirty;

// ---------------------------------------------------------------------------
// Float helpers and the vector type are in src/Math3.h (a header, so the
// IDE's prototype generator sees V3 before any function that takes one).
// World axes: x east, y south (map rows), z up; metres. Heading 0 = north,
// clockwise. A texel is 4 m.
static void basis(float yaw, float pitch, float roll, V3 &f, V3 &r, V3 &u) {
    float sy = fsin(yaw), cy = fcos(yaw), sp = fsin(pitch), cp = fcos(pitch);
    float sr = fsin(roll), cr = fcos(roll);
    V3 f0 = v3(sy, -cy, 0), r0 = v3(cy, sy, 0), u0 = v3(0, 0, 1);
    f = f0 * cp + u0 * sp;
    V3 u1 = u0 * cp - f0 * sp;
    r = r0 * cr - u1 * sr;              // roll > 0: right wing down
    u = u1 * cr + r0 * sr;
}

// ---------------------------------------------------------------------------
// The ground renderer
static const int FOC = 80;              // focal length in pixels (~77 deg across)
static const int H8SHIFT = 13;          // q = (H Q8 << 13) / -dz: H/dz in Q12
static const int32_t QFOG = 122880;     // H/dz (Q12) where the haze is solid (~2.4 km)
static const int32_t QHAZE = 57344;     // ... where it starts (~1.1 km)
static const int32_t QMIP = 24000;      // beyond ~500 m, use each tile's single colour

static uint8_t BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

struct GCam {
    int32_t cx, cy;                     // camera position, texels Q8
    int32_t hq8;                        // height above ground, m Q8
    int32_t f2[3];                      // forward * FOC * 512
    int32_t r[3], u[3];                 // right, up * 256
};
static GCam gc;

static inline uint8_t farm(int32_t tx, int32_t ty) {   // endless fields (same as make_world.py)
    uint32_t bx = (uint32_t)(tx >> 5) & 0xFFFF, by = (uint32_t)(ty >> 5) & 0xFFFF;
    uint32_t lx = tx & 31, ly = ty & 31;
    if (lx == 0 || ly == 0) return C_GRASS_DK;
    uint32_t h = (bx * 73856093u) ^ (by * 19349663u);
    h ^= h >> 13; h *= 0x5BD1E995u; h ^= h >> 15;
    uint32_t t = h & 7, s = ((h >> 8) & 1) ? ly : lx;
    if (t <= 1) return (s & 2) ? C_CROP : C_GRASS;
    if (t == 2) return (s & 1) ? C_SOIL : C_CROP;
    if (t == 3) return (s & 3) ? C_SOIL : C_GREY_DK;
    if (t == 4) return (s & 2) ? C_GRASS : C_GRASS_DK;
    if (t == 5) return C_CROP;
    return ((tx * 7 + ty * 13) & 15) ? C_GRASS : C_GRASS_DK;
}

static inline uint8_t texel(int32_t gx, int32_t gy) {  // Q8 texel coords -> colour
    int32_t tx = gx >> 8, ty = gy >> 8;
    if ((uint32_t)tx < WORLD_TEXELS && (uint32_t)ty < WORLD_TEXELS) {
        uint8_t t = WORLD_MAP[((ty >> 3) << 6) | (tx >> 3)];
        uint8_t b = WORLD_TILES[(t << 5) | ((ty & 7) << 2) | ((tx & 7) >> 1)];
        return (tx & 1) ? (b >> 4) : (b & 15);
    }
    return farm(tx, ty);
}

// Distant ground: one colour per 8x8 tile, so far-off fields don't sparkle
static inline uint8_t texelFar(int32_t gx, int32_t gy) {
    int32_t tx = gx >> 8, ty = gy >> 8;
    if ((uint32_t)tx < WORLD_TEXELS && (uint32_t)ty < WORLD_TEXELS)
        return WORLD_TILE_FAR[WORLD_MAP[((ty >> 3) << 6) | (tx >> 3)]];
    return farm((tx & ~7) | 4, (ty & ~7) | 4);
}
static inline uint8_t ground(int32_t gx, int32_t gy, int32_t q) {
    return q > QMIP ? texelFar(gx, gy) : texel(gx, gy);
}

static inline uint8_t skyColour(int32_t dz, int c, int y) {
    int32_t e = dz >> 9;                // roughly pixels above the horizon
    uint8_t b = BAYER[((y & 3) << 2) | (c & 3)];
    if (e <= 0) return C_HAZE;
    if (e < 34) return (int32_t)b < e * 16 / 34 ? C_SKY_MID : C_HAZE;
    if (e < 100) return (int32_t)b < (e - 34) * 16 / 66 ? C_SKY_TOP : C_SKY_MID;
    return C_SKY_TOP;
}

// One frame of ground and sky into gfx_fb, two pixels per sample.
RAMFUNC(ground) static void renderGround(int y0, int y1) {
    const int32_t h8 = gc.hq8 << H8SHIFT;
    const int32_t dzLim = (h8 + QFOG - 1) / QFOG;     // ground iff -dz >= dzLim
    const int32_t sx = gc.r[0] * 4, sy = gc.r[1] * 4, sz = gc.r[2] * 4;   // per pair
    for (int y = y0; y < y1; y++) {
        int32_t k = 127 - 2 * y;
        int32_t ax = gc.f2[0] + gc.u[0] * k - gc.r[0] * 125;
        int32_t ay = gc.f2[1] + gc.u[1] * k - gc.r[1] * 125;
        int32_t az = gc.f2[2] + gc.u[2] * k - gc.r[2] * 125;
        uint8_t *row = gfx_fb + y * GFX_FB_STRIDE;
        const uint8_t *bay = BAYER + ((y & 3) << 2);
        // endpoints of 8 spans of 8 pairs
        int32_t ex[9], ey[9], el[9], eq[9]; bool eg[9];
        for (int e = 0; e < 9; e++) {
            int32_t c = e * 8;
            int32_t dz = az + sz * c;
            eg[e] = -dz >= dzLim;
            if (eg[e]) {
                int32_t q = h8 / -dz;
                eq[e] = q;
                ex[e] = gc.cx + ((q * ((ax + sx * c) >> 4)) >> 11);
                ey[e] = gc.cy + ((q * ((ay + sy * c) >> 4)) >> 11);
                int32_t l = (q - QHAZE) * (17 << 8) / (QFOG - QHAZE);
                el[e] = l < 0 ? 0 : l;
            }
        }
        for (int s = 0; s < 8; s++) {
            int c0 = s * 8;
            int32_t qa = eq[s], qb = eq[s + 1];
            if (eg[s] && eg[s + 1] && qa < qb + (qb >> 2) && qb < qa + (qa >> 2)) {
                // depth nearly constant across the span: affine is exact enough
                int32_t gx = ex[s], gy = ey[s], l = el[s];
                int32_t dx = (ex[s + 1] - gx) >> 3, dy = (ey[s + 1] - gy) >> 3, dl = (el[s + 1] - l) >> 3;
                bool far = qa > QMIP && qb > QMIP;
                for (int c = c0; c < c0 + 8; c++) {
                    uint8_t col = ((int32_t)bay[c & 3] << 8) < l ? C_HAZE : far ? texelFar(gx, gy) : texel(gx, gy);
                    row[c] = col * 0x11;
                    gx += dx; gy += dy; l += dl;
                }
            } else if (!eg[s] && !eg[s + 1]) {
                int32_t dz = az + sz * c0;
                for (int c = c0; c < c0 + 8; c++) { row[c] = skyColour(dz, c, y) * 0x11; dz += sz; }
            } else {                    // the horizon crosses it, or depth changes fast (steep bank): exact per pair
                for (int c = c0; c < c0 + 8; c++) {
                    int32_t dz = az + sz * c;
                    uint8_t col;
                    if (-dz >= dzLim) {
                        int32_t q = h8 / -dz;
                        int32_t l = (q - QHAZE) * (17 << 8) / (QFOG - QHAZE);
                        col = ((int32_t)bay[c & 3] << 8) < l ? C_HAZE
                            : ground(gc.cx + ((q * ((ax + sx * c) >> 4)) >> 11), gc.cy + ((q * ((ay + sy * c) >> 4)) >> 11), q);
                    } else col = skyColour(dz, c, y);
                    row[c] = col * 0x11;
                }
            }
        }
    }
}

// Camera (float side), shared by the ground and the objects
static V3 camP, camF, camR, camU;

static void setCamera(V3 p, V3 f, V3 r, V3 u) {
    if (p.z < 1.0f) p.z = 1.0f;
    camP = p; camF = f; camR = r; camU = u;
    gc.cx = (int32_t)(p.x * 64.0f);     // metres -> texels Q8 (/4 * 256)
    gc.cy = (int32_t)(p.y * 64.0f);
    float h = p.z > 900 ? 900 : p.z;
    gc.hq8 = (int32_t)(h * 256.0f);
    float f2 = FOC * 512.0f;
    gc.f2[0] = (int32_t)(camF.x * f2); gc.f2[1] = (int32_t)(camF.y * f2); gc.f2[2] = (int32_t)(camF.z * f2);
    gc.r[0] = (int32_t)(camR.x * 256); gc.r[1] = (int32_t)(camR.y * 256); gc.r[2] = (int32_t)(camR.z * 256);
    gc.u[0] = (int32_t)(camU.x * 256); gc.u[1] = (int32_t)(camU.y * 256); gc.u[2] = (int32_t)(camU.z * 256);
}

static bool project(V3 p, int &sx, int &sy, float &z) {
    V3 v = p - camP;
    z = dot(v, camF);
    if (z < 0.8f) return false;
    float k = FOC / z;
    float x = 64 + dot(v, camR) * k, y = 64 - dot(v, camU) * k;
    if (x < -400 || x > 528 || y < -400 || y > 528) return false;
    sx = (int)x; sy = (int)y;
    return true;
}

// ---------------------------------------------------------------------------
// Courses

static const RingDef P1_RINGS[] = {     // the ring course over town and lake
    { 1600, 1400, 150, 0 },   { 1560, 1000, 165, 350 }, { 1500, 650, 110, 335 },
    { 1260, 430, 150, 290 },  { 900, 360, 175, 265 },   { 560, 420, 130, 235 },
    { 380, 760, 70, 185 },    { 440, 1322, 40, 90 },     // last: the final approach gate
};
static const RingDef P2_RINGS[] = {     // up the avenue between the towers, back over the town, land westbound
    { 1916, 800, 60, 0 },     { 1916, 640, 40, 0 },     { 1916, 470, 45, 0 },
    { 1780, 280, 70, 290 },   { 1500, 480, 45, 190 },   { 1560, 740, 55, 160 },
    { 1625, 950, 60, 160 },   { 1480, 1322, 32, 270 },
};
static const RingDef P3_RINGS[] = {     // low up the river, skim the lake, round to the gate
    { 745, 1960, 30, 350 },   { 636, 1720, 30, 345 },   { 616, 1480, 30, 11 },
    { 732, 1240, 30, 15 },    { 742, 1000, 30, 346 },   { 618, 780, 30, 339 },
    { 560, 560, 12, 0 },      { 330, 380, 50, 250 },    { 300, 800, 60, 175 },
    { 440, 1322, 40, 90 },
};
static const RingDef G1_RINGS[] = {
    { 1200, 1560, 170, 0 },   { 1200, 1450, 255, 0 },   { 940, 1230, 235, 315 },
    { 800, 1100, 330, 300 },  { 1000, 820, 210, 60 },
};
static const RingDef G2_RINGS[] = {     // thermal to thermal, then glide home to the target
    { 1560, 1560, 240, 0 },   { 1700, 1200, 200, 20 },  { 1480, 840, 215, 330 },
    { 1640, 560, 240, 30 },
};
static const RingDef J1_RINGS[] = {     // a big loop out past the edge of the map
    { 1024, 2000, 250, 0 },   { 1100, 1300, 180, 5 },   { 1450, 650, 200, 30 },
    { 2200, -150, 380, 40 },  { 3300, -450, 500, 90 },  { 4300, 300, 380, 150 },
    { 3900, 1600, 220, 210 }, { 2700, 2350, 120, 250 }, { 1400, 2500, 80, 280 },
    { 250, 1900, 120, 330 },
};
static const RingDef J2_RINGS[] = {     // the downtown avenue, the lake, a pass down the runway
    { 1916, 900, 60, 0 },     { 1916, 600, 40, 0 },     { 1916, 420, 45, 0 },
    { 1450, 20, 100, 270 },   { 560, 560, 15, 210 },    { 550, 950, 40, 165 },
    { 950, 1322, 20, 90 },    { 1900, 1322, 80, 90 },
};
#define NR(a) (uint8_t)(sizeof(a) / sizeof(a[0]))
static const CourseDef COURSES[3][3] = {
    { { "RING COURSE", P1_RINGS, NR(P1_RINGS), 1600, 1720, 160, 0, 42, FIN_RUNWAY, 120 },
      { "TOWN SLALOM", P2_RINGS, NR(P2_RINGS), 1916, 1100, 100, 0, 42, FIN_RUNWAY, 70 },
      { "RIVER RUN",   P3_RINGS, NR(P3_RINGS), 750, 2200, 70, 0, 42, FIN_RUNWAY, 95 } },
    { { "THERMALS",     G1_RINGS, NR(G1_RINGS), 1200, 1720, 185, 0, 10, FIN_TARGET, 0 },
      { "CLOUD STREET", G2_RINGS, NR(G2_RINGS), 1560, 1900, 160, 0, 10, FIN_TARGET, 0 } },
    { { "SPEED GATES", J1_RINGS, NR(J1_RINGS), 1024, 2900, 300, 0, 140, FIN_GATE, 70 },
      { "LOW LEVEL",   J2_RINGS, NR(J2_RINGS), 1916, 1900, 140, 0, 120, FIN_GATE, 40 } },
};
static const uint8_t COURSE_N[3] = { 3, 2, 2 };
static const ThermDef THERMALS[] = { { 1200, 1440, 34 }, { 800, 1090, 32 }, { 1480, 820, 26 }, { 560, 1500, 28 },
                                     { 1560, 1560, 32 }, { 1700, 1200, 30 } };
#define NTHERM NR(THERMALS)

static const float RWY_X0 = 864, RWY_X1 = 1184, RWY_Y0 = 1304, RWY_Y1 = 1340;  // metres
static const float TGT_X = 1202, TGT_Y = 922;

static Ring rings[12];
static const CourseDef *cd;            // the course being flown (null in free flight)
static uint8_t course;

// ---------------------------------------------------------------------------
// Buildings: solid boxes, flat-shaded (sun from the south-west), drawn far to near.
// The houses are generated from the same grid make_world.py paints their roofs on;
// the landmarks' footprints match the painted ground.
static const Bldg LANDMARKS[] = {
    { 1848, 420, 24, 24, 70, 3 }, { 1848, 520, 24, 24, 90, 3 }, { 1852, 624, 20, 20, 55, 3 },   // downtown towers,
    { 1960, 456, 24, 28, 80, 3 }, { 1960, 572, 24, 28, 64, 3 },                                 // either side of the avenue
    { 1600, 544, 40, 24, 10, 0 }, { 1588, 550, 12, 12, 28, 6 },                                 // church and steeple
    { 976, 1224, 40, 32, 11, 4 }, { 1028, 1224, 40, 32, 11, 5 },                                // hangars
    { 1056, 1270, 10, 10, 16, 6 },                                                              // control tower
};
// walls lit (south, west), walls in shade (north, east), roof
static const uint8_t MAT[8][3] = {
    { C_WHITE, C_HAZE, C_RED },  { C_WHITE, C_HAZE, C_SOIL },     { C_CROP, C_SOIL, C_GREY_DK },
    { C_WATER, C_WATER_DK, C_GREY_DK },  { C_ASPHALT, C_GREY_DK, C_RED }, { C_ASPHALT, C_GREY_DK, C_GREY_DK },
    { C_WHITE, C_HAZE, C_GREY_DK }, { C_HAZE, C_SKY_MID, C_HAZE },  // last: far away, in the haze
};
static Bldg town[72];
static uint8_t nTown;

static void makeTown() {
    nTown = 0;
    for (const Bldg &b : LANDMARKS) town[nTown++] = b;
    for (uint8_t by = 0; by < 4; by++)
        for (uint8_t bx = 0; bx < 5; bx++)
            for (uint8_t hy = 0; hy < 2; hy++)
                for (uint8_t hx = 0; hx < 2; hx++) {
                    uint8_t k = (bx * 3 + by * 5 + hx + hy * 2) % 4;
                    if (k == 3) continue;                            // a garden with a tree
                    town[nTown++] = { (int16_t)((364 + bx * 18 + hx * 7) * 4), (int16_t)((100 + by * 18 + hy * 7) * 4),
                                      16, 12, (uint8_t)(6 + ((bx + by * 3 + hx * 2 + hy) & 3) * 2), k };
                }
}

static bool hitBuilding(V3 p) {
    if (p.z > 95) return false;
    for (uint8_t i = 0; i < nTown; i++) {
        const Bldg &b = town[i];
        if (p.z < b.h && p.x > b.x0 && p.x < b.x0 + b.w && p.y > b.y0 && p.y < b.y0 + b.d) return true;
    }
    return false;
}

static const float NEAR_Z = 1.5f;
static float bc[8][3];                  // the box's corners in camera space: right, up, forward
static const uint8_t FACE[5][4] = { { 0, 2, 6, 4 }, { 1, 3, 7, 5 }, { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 4, 5, 7, 6 } };

static int16_t sub16(float v) { return (int16_t)fclamp(v, -16000, 16000); }

static void face(const uint8_t *ix, uint8_t col) {
    float p[6][3];
    uint8_t n = 0;
    for (uint8_t i = 0; i < 4; i++) {                  // clip against the near plane
        const float *a = bc[ix[i]], *b = bc[ix[(i + 1) & 3]];
        bool ia = a[2] >= NEAR_Z, ib = b[2] >= NEAR_Z;
        if (ia) { p[n][0] = a[0]; p[n][1] = a[1]; p[n][2] = a[2]; n++; }
        if (ia != ib) {
            float t = (NEAR_Z - a[2]) / (b[2] - a[2]);
            for (uint8_t j = 0; j < 3; j++) p[n][j] = a[j] + (b[j] - a[j]) * t;
            n++;
        }
    }
    if (n < 3) return;
    int16_t xy[12];
    for (uint8_t i = 0; i < n; i++) {
        float k = FOC * 16 / p[i][2];
        xy[i * 2] = sub16(64 * 16 + p[i][0] * k);
        xy[i * 2 + 1] = sub16(64 * 16 - p[i][1] * k);
    }
    fillConvex(xy, n, col);
}

// Floor lines on a tower's wall: bottom corners a, b (the top ones are a|4, b|4)
static void floors(uint8_t a, uint8_t b, uint8_t h, uint8_t col) {
    for (uint8_t k = 12; k < h; k += 12) {
        float t = (float)k / h;
        int x[2], y[2];
        for (uint8_t j = 0; j < 2; j++) {
            const float *lo = bc[j ? b : a], *hi = bc[(j ? b : a) | 4];
            float z = lo[2] + (hi[2] - lo[2]) * t;
            if (z < NEAR_Z) return;
            x[j] = (int)fclamp(64 + (lo[0] + (hi[0] - lo[0]) * t) * FOC / z, -999, 999);
            y[j] = (int)fclamp(64 - (lo[1] + (hi[1] - lo[1]) * t) * FOC / z, -999, 999);
        }
        gfx_line(x[0], y[0], x[1], y[1], col);
    }
}

static void drawBldg(const Bldg &b, bool hazy) {
    V3 v = v3(b.x0, b.y0, 0) - camP;
    float base[3] = { dot(v, camR), dot(v, camU), dot(v, camF) };
    float ax[3] = { b.w * camR.x, b.w * camU.x, b.w * camF.x };   // the box's edges, in camera space
    float ay[3] = { b.d * camR.y, b.d * camU.y, b.d * camF.y };
    float az[3] = { b.h * camR.z, b.h * camU.z, b.h * camF.z };
    bool any = false;
    for (uint8_t i = 0; i < 8; i++) {
        for (uint8_t j = 0; j < 3; j++)
            bc[i][j] = base[j] + ((i & 1) ? ax[j] : 0) + ((i & 2) ? ay[j] : 0) + ((i & 4) ? az[j] : 0);
        if (bc[i][2] >= NEAR_Z) any = true;
    }
    if (!any) return;
    const uint8_t *m = MAT[hazy ? 7 : b.mat];
    uint8_t fl = (b.mat == 3 && !hazy) ? b.h : 0, d1 = C_WATER_DK, d2 = C_INK;   // towers get floor lines
    if (camP.x < b.x0) { face(FACE[0], m[0]); if (fl) floors(0, 2, fl, d1); }
    else if (camP.x > b.x0 + b.w) { face(FACE[1], m[1]); if (fl) floors(1, 3, fl, d2); }
    if (camP.y < b.y0) { face(FACE[2], m[1]); if (fl) floors(0, 1, fl, d2); }
    else if (camP.y > b.y0 + b.d) { face(FACE[3], m[0]); if (fl) floors(2, 3, fl, d1); }
    if (camP.z > b.h) face(FACE[4], m[2]);
}
static uint8_t ringCount, ringsDone;

// ---------------------------------------------------------------------------
// Aircraft
static const Craft PLANE  = { 0.0125f, 5.5f, 0.25f, 15 * DEG, 0.05f, 0.045f, 3.6f, 2.0f * DEG, 0.9f, 1.6f, 80 * DEG, 14, 3.0f, 0 };
static const Craft GLIDER = { 0.12f, 4.5f, 0.30f, 14 * DEG, 0.05f, 0.05f, 0, 5.6f * DEG, 0.7f, 1.1f, 60 * DEG, 9, 2.0f, 0 };
// Jet: stalls near 80 knots, cruises at 270 on light throttle. Low drag: climbs about 55 degrees on
// dry thrust and straight up on afterburner. Drag rises steeply past ~420 knots (the "sound barrier"),
// which holds it under about 460 level. Pulls up to 9 g.
static const Craft JET    = { 0.004f, 4.5f, 0.10f, 16 * DEG, 0.016f, 0.05f, 9.5f, 1.0f * DEG, 1.4f, 3.5f, 80 * DEG, 18, 3.5f, 9.0f };

static Event ev;
static const Craft *cr;
static V3 pos, vel;                    // vel is air-relative
static float yaw, pitch, roll, thr;
static V3 bF, bR, bU;                    // body axes: nose, right wing, top. Free to point anywhere.
static V3 kF, kU;                        // the chase camera's lagging forward and up
// yaw/pitch/roll above are derived from the body axes each frame (HUD, scoring)
static bool airborne, stalled, landed, crashed, brakes, burner, gateDone;
static float gNow;                       // load factor, for the jet's G meter
static float windX, drift;               // thermals drift with the wind
static uint16_t ticks, endTicks;
static int16_t touchScore, landPts;
static float touchSink, touchOff;
static const char *msg; static uint8_t msgT;
static uint8_t varioT, flashT;
static float vz;                        // ground-relative climb rate

static float thermalLift(float x, float y, float z) {
    if (ev != EV_GLIDER && ev != EV_FREE) return 0;
    float w = -0.1f;
    for (const ThermDef &t : THERMALS) {
        float dx = x - (t.x + drift), dy = y - t.y, d2 = dx * dx + dy * dy;
        const float R = 70;
        if (d2 < R * R) w = t.w10 * 0.1f * (1 - d2 / (R * R));
        else if (d2 < 2.6f * R * R && w > -0.6f) w = -0.6f;
    }
    if (z > 400) w *= fclamp((440 - z) / 40, 0, 1);
    return w;
}

static void setPalette(bool dusk) { gfx_setPalette(dusk ? PAL_DUSK : PAL_DAY, 16); }

static float ringR() { return ev == EV_GLIDER ? 8.0f : ev == EV_JET ? 18.0f : 10.0f; }

static void say(const char *m, uint8_t t = 45) { msg = m; msgT = t; }

static void startEvent(Event e) {
    ev = e;
    cr = (e == EV_GLIDER) ? &GLIDER : (e == EV_JET) ? &JET : &PLANE;
    setPalette(e == EV_FREE);
    ringCount = ringsDone = 0;
    cd = nullptr; course = 0;
    if (e != EV_FREE) {
        course = sv.course[e] < COURSE_N[e] ? sv.course[e] : 0;
        cd = &COURSES[e][course];
        ringCount = cd->count;
        for (uint8_t i = 0; i < ringCount; i++)
            rings[i] = { (float)cd->rings[i].x, (float)cd->rings[i].y, (float)cd->rings[i].z, cd->rings[i].hdg * DEG, false, false };
    }
    roll = 0; brakes = burner = gateDone = false; stalled = landed = crashed = false;
    ticks = endTicks = 0; msg = nullptr; flashT = 0; touchScore = landPts = 0;
    windX = 0; drift = 0; gNow = 1;
    if (cd) {
        pos = v3(cd->sx, cd->sy, cd->sz); yaw = cd->shdg * DEG;
        float sp = e == EV_GLIDER ? 10.5f : cd->speed;
        vel = v3(fsin(yaw) * sp, -fcos(yaw) * sp, 0);
        thr = e == EV_JET ? 0.14f : e == EV_PLANE ? 0.36f : 0;     // trimmed for level flight
        pitch = cr->aTrim; airborne = true;
        if (e == EV_GLIDER) windX = course ? 0.5f : 1.0f;
        say(cd->name, 60);
    } else {
        pos = v3(RWY_X0 + 20, (RWY_Y0 + RWY_Y1) / 2, 0); yaw = 90 * DEG; vel = v3(0, 0, 0);
        thr = 0; pitch = 0; airborne = false;
    }
#if defined(CHSIM) && defined(SKYL_TEST)
    // simulator test starts: 1 plane on final, 2 glider at the first thermal, 3 glider near the target
    if (SKYL_TEST == 1 && e == EV_PLANE) { for (uint8_t i = 0; i < ringCount; i++) rings[i].done = true; ringsDone = ringCount;
        pos = v3(430, 1322, 42); yaw = 90 * DEG; vel = v3(42, 0, 0); }
    if (SKYL_TEST == 2 && e == EV_GLIDER) { rings[0].done = true; ringsDone = 1; pos = v3(1205, 1470, 175); }
    if (SKYL_TEST == 3 && e == EV_GLIDER) { for (uint8_t i = 0; i < ringCount; i++) rings[i].done = true; ringsDone = ringCount;
        pos = v3(1000, 820, 70); yaw = 60 * DEG; vel = v3(9, -5, 0); }
    if (SKYL_TEST == 6 && e == EV_JET) {                     // jet: just the runway pass and the last gate left
        for (uint8_t i = 0; i + 2 < ringCount; i++) rings[i].done = rings[i].hit = true;
        ringsDone = ringCount - 2; pos = v3(300, 1322, 40); yaw = 90 * DEG; vel = v3(130, 0, 0); }
    if (SKYL_TEST == 7 && e == EV_PLANE) { pos = v3(1760, 1000, 130); yaw = 0; vel = v3(0, -42, 0); }   // worst case: town + towers ahead
    if (SKYL_TEST == 5 && e == EV_PLANE) { pos = v3(1862, 800, 40); yaw = 0; vel = v3(0, -42, 0); }   // straight at a tower
    if (SKYL_TEST == 4 && e == EV_PLANE) { pos = v3(1300, 1700, 220); roll = 90 * DEG; yaw = 340 * DEG; vel = v3(-15, -60, 0); }
#endif
    basis(yaw, pitch, roll, bF, bR, bU);
    kF = bF; kU = bU;
    gfx_setFade(0);
}

static void crash() {
    crashed = true; landed = true; airborne = false; flashT = 24;
    audio::sfx(SFX_CRASH);
    say("CRASH!", 90);
    vel = vel * 0.15f;
}

static void touchdown() {
    float sink = -vz;
    bool onRwy = pos.x > RWY_X0 && pos.x < RWY_X1 && pos.y > RWY_Y0 - 6 && pos.y < RWY_Y1 + 6;
    float maxSink = (ev == EV_GLIDER) ? 3.2f : 3.4f;
    float maxRoll = (ev == EV_GLIDER) ? 25 * DEG : 14 * DEG;
    touchSink = sink;
    if (sink > maxSink || fabs_(roll) > maxRoll || pitch < -8 * DEG || (ev != EV_GLIDER && sink > 1.5f && !onRwy)) {
        crash();
        return;
    }
    landed = true;
    audio::sfx(SFX_LAND);
    if (ev == EV_PLANE) {
        touchOff = fabs_(pos.y - (RWY_Y0 + RWY_Y1) / 2);
        if (onRwy) {
            landPts = (int16_t)fclamp(30 - sink * 10, 0, 30) + (int16_t)fclamp(10 - touchOff * 0.6f, 0, 10);
            say("TOUCHDOWN", 70);
        } else { landPts = 5; say("ROUGH FIELD", 70); }
    } else if (ev == EV_GLIDER) {
        float dx = pos.x - TGT_X, dy = pos.y - TGT_Y;
        touchOff = fsqrt(dx * dx + dy * dy);
        landPts = touchOff < 6 ? 50 : touchOff < 18 ? 30 : touchOff < 30 ? 10 : 0;
        say(landPts == 50 ? "BULLSEYE!" : landPts ? "ON TARGET" : "LANDED", 70);
    } else say("LANDED", 60);
}

static int8_t nextRing() {
    for (uint8_t i = 0; i < ringCount; i++) if (!rings[i].done) return i;
    return -1;
}

static void checkRings(V3 prev) {
    float R = ringR();
    int8_t nx = nextRing();
    for (uint8_t i = 0; i < ringCount; i++) {
        Ring &g = rings[i];
        if (g.done || (ev == EV_JET && i != nx)) continue;
        V3 n = v3(fsin(g.hdg), -fcos(g.hdg), 0), c = v3(g.x, g.y, g.z);
        float a = dot(prev - c, n), b = dot(pos - c, n);
        if ((a < 0) == (b < 0)) continue;
        float t = a / (a - b);
        V3 hit = prev + (pos - prev) * t;
        float d = len(hit - c);
        if (d < R) { g.done = g.hit = true; ringsDone++; audio::sfx(SFX_CHIME); say("+10", 30); }
        else if (d < R * 2.2f || (ev == EV_JET && d < 160)) {
            audio::sfx(SFX_MISS); say("MISSED", 30);
            if (ev == EV_JET) g.done = true;            // gates are in order: on to the next one
        }
    }
    if (cd && cd->fin == FIN_GATE && !gateDone && nextRing() < 0) { gateDone = true; audio::sfx(SFX_LAND); say("FINISH!", 70); }
}

// Rotate the body axes by small roll (about the nose), pitch (about the wing)
// and yaw (about the top) angles, then re-derive the angles for the HUD.
static void rotate(float rl, float pt, float yw) {
    float c = fcos(rl), s = fsin(rl);
    V3 r = bR * c - bU * s; bU = bU * c + bR * s; bR = r;
    c = fcos(pt); s = fsin(pt);
    V3 f = bF * c + bU * s; bU = bU * c - bF * s; bF = f;
    c = fcos(yw); s = fsin(yw);
    f = bF * c + bR * s; bR = bR * c - bF * s; bF = f;
    bF = norm(bF); bR = norm(cross(bU, bF)); bU = cross(bF, bR);
    pitch = fatan2(bF.z, fsqrt(bF.x * bF.x + bF.y * bF.y));
    roll = fatan2(-bR.z, bU.z);
    // heading: from the nose, or from the top when pointing straight up/down
    yaw = (fabs_(bF.z) < 0.97f) ? fatan2(bF.x, -bF.y) : fatan2(-bU.x * (bF.z > 0 ? 1 : -1), bU.y * (bF.z > 0 ? 1 : -1));
}

static void physics(int8_t stickX, int8_t stickY) {
    const float dt = 1.0f / 30;
    const Craft &k = *cr;
    V3 prev = pos;

    float V = len(vel);
    // angle of attack and sideslip from the body axes (works upside down too)
    float vf = dot(vel, bF);
    float alpha = V > 1 ? fatan2(-dot(vel, bU), vf) : 0;
    float beta = V > 1 ? fatan2(dot(vel, bR), vf) : 0;
    float CL;
    stalled = false;
    const float aNeg = -12 * DEG;
    if (alpha > k.aStall) {
        CL = k.cl0 + k.cla * k.aStall - (alpha - k.aStall) * 3.0f; if (CL < 0.45f) CL = 0.45f; stalled = airborne;
    } else if (alpha < aNeg) {
        CL = k.cl0 + k.cla * aNeg - (alpha - aNeg) * 3.0f; if (CL > -0.3f) CL = -0.3f; stalled = airborne;
    } else CL = k.cl0 + k.cla * alpha;
    float q = k.kL * V * V;

    V3 f = bF, u = bU;
    V3 acc = v3(0, 0, -9.8f);
    if (V > 0.5f) {
        V3 vn = vel * (1.0f / V);
        V3 lift = u - vn * dot(u, vn);
        float ll = len(lift);
        if (ll > 1e-3f) acc = acc + lift * (q * CL / ll);
        float cd = k.cd0 + k.kInd * CL * CL + (brakes && airborne ? (ev == EV_GLIDER ? 0.14f : 0.06f) : 0);
        if (ev == EV_JET && V > 215) cd += (V - 215) * (V - 215) * 1e-4f;   // transonic drag rise
        acc = acc - vn * (q * cd);
    }
    acc = acc + f * (thr * k.tMax * (burner ? 1.6f : 1.0f));
    gNow = q * CL / 9.8f;
    vel = vel + acc * dt;

    drift += windX * dt;
    float lift = thermalLift(pos.x, pos.y, pos.z);
    V3 wind = v3(windX, 0, lift);
    V3 gv = vel + wind;
    vz = gv.z;
    pos = pos + gv * dt;

    // attitude. The stick asks for an angle of attack (back = more, up to just past the stall);
    // the nose rotates to get it, so holding back-stick with enough speed loops all the way round.
    float auth = fclamp(V / (ev == EV_GLIDER ? 9.0f : 30.0f), 0.25f, 1.0f);   // control authority
    // hands off: the glider holds its trimmed glide; the plane holds about 1 g (level flight at this
    // speed when upright, so you sink a little in a bank and need back-stick to turn)
    float base = k.aTrim;
    if (k.tMax > 0 && airborne)
        base = fclamp((9.8f * fclamp(bU.z, 0, 1) / (q + 0.01f) - k.cl0) / k.cla, -4 * DEG, k.aStall - 3 * DEG);
    float aCmd = base;
    if (stickY > 0) aCmd += stickY * (k.aStall + 2 * DEG - base);
    if (stickY < 0) aCmd += stickY * (base + 10 * DEG);
    if (k.gMax > 0 && q > 1) {                                    // the jet's g limiter
        float aG = (k.gMax * 9.8f / q - k.cl0) / k.cla;
        if (aCmd > aG) aCmd = aG;
    }
    float pq = fclamp(4.0f * (aCmd - alpha), -k.pitchRate * 2, k.pitchRate * 2) * auth;
    if (stalled && alpha > 0) pq -= 1.2f * (alpha - k.aStall);
    // jet: feed forward the flight path's own turn rate, so alpha actually reaches what the stick asks
    // for (without it the nose lags far behind at jet speed and a full pull only makes ~4 g)
    if (k.gMax > 0 && airborne && V > 20) pq += (q * CL - 9.8f * bU.z) / V;
    float pp = stickX * k.rollRate * auth;                        // roll about the nose, no limit
    if (!stickX && bU.z > 0.7f) pp -= roll * 0.3f;               // a gentle hands-off level when upright
    float pr = fclamp(3.5f * beta, -1.5f, 1.5f) * fclamp(V / 10, 0, 1);   // weathervane into the airflow
    rotate(pp * dt, pq * dt, pr * dt);

    // ground
    if (pos.z <= 0) {
        pos.z = 0;
        if (airborne) { airborne = false; touchdown(); }
        if (vel.z < 0) vel.z = 0;
        vz = 0;
        float decel = (crashed ? 6.0f : ev == EV_GLIDER ? 5.0f : (1.0f + (brakes ? 3.5f : 0)));
        float s = len(v3(vel.x, vel.y, 0));
        if (s > 0.01f) {
            float ns = s - decel * dt; if (ns < 0) ns = 0;
            vel.x *= ns / s; vel.y *= ns / s;
        }
        roll -= roll * 6 * dt;
        if (fabs_(roll) > 90 * DEG) roll = 0;                      // (upside down only happens in a crash)
        pitch = fclamp(pitch, 0, 14 * DEG);
        if (pitch > 0 && V < 18) pitch -= 6 * DEG * dt;
        if (pitch < 0) pitch = 0;
        basis(yaw, pitch, roll, bF, bR, bU);
    } else if (pos.z > 0.4f && !landed) airborne = true;
    if (landed && pos.z > 1.5f && !crashed && ev == EV_FREE) { landed = false; }   // free flight: touch and go

    if (airborne) checkRings(prev);
    if (airborne && !crashed && hitBuilding(pos)) crash();

    // the camera lags a little, so the craft visibly banks against it; it follows through rolls and loops
    kF = norm(kF + (bF - kF) * 0.3f);
    kU = kU + (bU - kU) * 0.22f;
    V3 kr = norm(cross(kU, kF));
    kU = cross(kF, kr);
}

// ---------------------------------------------------------------------------
// Drawing objects
static void drawRing(const Ring &g, uint8_t col) {
    float R = ringR();
    V3 c = v3(g.x, g.y, g.z), side = v3(fcos(g.hdg), fsin(g.hdg), 0), up = v3(0, 0, 1);
    float zc;
    int cx, cy;
    if (!project(c, cx, cy, zc) || zc > 1800) return;
    int px = 0, py = 0; bool pv = false;
    for (int i = 0; i <= 16; i++) {
        float a = i * (2 * PI_F / 16);
        V3 p = c + side * (fcos(a) * R) + up * (fsin(a) * R);
        int x, y; float z;
        bool v = project(p, x, y, z);
        if (v && pv) {
            if (zc < 250) { gfx_line(px + 1, py + 1, x + 1, y + 1, C_INK); gfx_line(px, py + 1, x, y + 1, col); }
            gfx_line(px, py, x, y, col);
        }
        px = x; py = y; pv = v;
    }
}

static void drawCloud(float x, float y) {
    int sx, sy; float z;
    if (!project(v3(x, y, 430), sx, sy, z) || z > 2600) return;
    int r = (int)(60 * FOC / z);
    if (r < 2) r = 2;
    if (r > 90) r = 90;
    gfx_fillEllipse(sx, sy + r / 3, r + r / 2, r / 2, C_HAZE);
    gfx_fillCircle(sx - r / 2, sy, r * 2 / 3, C_WHITE);
    gfx_fillCircle(sx + r / 2, sy, r * 3 / 4, C_WHITE);
    gfx_fillCircle(sx, sy - r / 3, r * 3 / 4, C_WHITE);
}

static void drawShadow() {
    if (pos.z > 220) return;
    int sx, sy; float z;
    if (!project(v3(pos.x, pos.y, 0), sx, sy, z)) return;
    float span = 3.0f;                    // drawn a bit smaller than life, like the sprite
    int rx = (int)(span * FOC / z);
    if (rx > 16) rx = 16;
    int ry = (int)(rx * (camP.z / (z + 1)));
    if (ry < 1) ry = 1;
    if (ry > rx / 2) ry = rx / 2;
    gfx_fillEllipse(sx, sy, rx, ry, C_GREY_DK);
}

// polygon in half pixels, rotated by (sn, cs) = sin/cos * 256, centred on (ox, oy)
static void poly(const int8_t *pts, uint8_t n, uint8_t col, int ox, int oy, int sn, int cs) {
    int16_t xy[16];
    for (uint8_t i = 0; i < n; i++) {
        int x = pts[i * 2], y = pts[i * 2 + 1];
        xy[i * 2] = (int16_t)(ox * 16 + ((x * cs - y * sn) >> 5));
        xy[i * 2 + 1] = (int16_t)(oy * 16 + ((x * sn + y * cs) >> 5));
    }
    fillConvex(xy, n, col);
}

// Sprites, in half pixels (y down), seen from behind
static const int8_t PL_WING[]  = { -36, -1, 36, -1, 35, 2, -35, 2 };
static const int8_t PL_WTIPL[] = { -36, -1, -30, -1, -30, 2, -35, 2 };
static const int8_t PL_WTIPR[] = { 30, -1, 36, -1, 35, 2, 30, 2 };
static const int8_t PL_BODY[]  = { -4, -4, 4, -4, 5, 4, 2, 7, -2, 7, -5, 4 };
static const int8_t PL_TAIL[]  = { -11, 5, 11, 5, 11, 7, -11, 7 };
static const int8_t PL_FIN[]   = { -1, -10, 1, -10, 1, 6, -1, 6 };
static const int8_t PL_CANOPY[]= { -3, -3, 3, -3, 2, 0, -2, 0 };
static const int8_t GL_WINGL[] = { 0, -4, -32, 4, -28, 8, 0, 3 };
static const int8_t GL_WINGR[] = { 0, -4, 32, 4, 28, 8, 0, 3 };
static const int8_t GL_KEEL[]  = { -1, -3, 1, -3, 1, 3, -1, 3 };
static const int8_t GL_PILOT[] = { -2, 6, 2, 6, 2, 12, -2, 12 };
static const int8_t GL_BAR[]   = { -7, 8, 7, 8, 7, 9, -7, 9 };
static const int8_t JT_WING[]  = { -30, 1, 30, 1, 26, 4, -26, 4 };
static const int8_t JT_STAB[]  = { -12, 4, 12, 4, 11, 6, -11, 6 };
static const int8_t JT_BODY[]  = { -4, -3, 4, -3, 5, 3, 0, 6, -5, 3 };
static const int8_t JT_FIN[]   = { -1, -15, 1, -15, 2, -3, -2, -3 };
static const int8_t JT_CANOPY[]= { -2, -4, 2, -4, 1, -2, -1, -2 };

static void drawCraft() {
    int sx, sy; float z;
    if (!project(pos, sx, sy, z)) return;
    float a = fatan2(dot(bU, camR), dot(bU, camU));   // the craft's bank against the lagging camera
    int sn = (int)(fsin(a) * 256), cs = (int)(fcos(a) * 256);
    if (ev == EV_JET) {
        poly(JT_STAB, 4, C_ASPHALT, sx, sy, sn, cs);
        poly(JT_WING, 4, C_ASPHALT, sx, sy, sn, cs);
        poly(JT_BODY, 5, C_GREY_DK, sx, sy, sn, cs);
        poly(JT_FIN, 4, C_GREY_DK, sx, sy, sn, cs);
        poly(JT_CANOPY, 4, C_SKY_TOP, sx, sy, sn, cs);
        if (burner) { gfx_fillCircle(sx, sy, 2 + (chgame.frameCount & 1), C_GOLD); gfx_fillCircle(sx, sy, 1, C_WHITE); }
        else gfx_fillCircle(sx, sy, 1, C_INK);
    } else if (ev == EV_GLIDER) {
        poly(GL_WINGL, 4, C_RED, sx, sy, sn, cs);
        poly(GL_WINGR, 4, C_GOLD, sx, sy, sn, cs);
        poly(GL_KEEL, 4, C_INK, sx, sy, sn, cs);
        poly(GL_BAR, 4, C_GREY_DK, sx, sy, sn, cs);
        poly(GL_PILOT, 4, C_SKY_TOP, sx, sy, sn, cs);
    } else {
        poly(PL_TAIL, 4, C_WHITE, sx, sy, sn, cs);
        poly(PL_WING, 4, C_WHITE, sx, sy, sn, cs);
        poly(PL_WTIPL, 4, C_RED, sx, sy, sn, cs);
        poly(PL_WTIPR, 4, C_RED, sx, sy, sn, cs);
        poly(PL_BODY, 6, C_RED, sx, sy, sn, cs);
        poly(PL_FIN, 4, C_RED, sx, sy, sn, cs);
        poly(PL_CANOPY, 4, C_SKY_TOP, sx, sy, sn, cs);
        if (thr > 0.05f && (chgame.frameCount & 1)) gfx_circle(sx, sy, 4, C_GREY_DK);  // prop blur
    }
}

// ---------------------------------------------------------------------------
// HUD
static void arrow(int cx, int cy, float a, uint8_t col) {
    static const int8_t TRI[] = { 0, -14, 10, 8, 0, 3, -10, 8 };
    int sn = (int)(fsin(a) * 256), cs = (int)(fcos(a) * 256);
    poly(TRI, 4, C_INK, cx + 1, cy + 1, sn, cs);
    poly(TRI, 4, col, cx, cy, sn, cs);
}

static void hud() {
    char b[12];
    gfx_fillRect(0, 0, 128, 8, C_INK);
    text35(2, 2, "ALT", C_GREY_DK); fmtInt(b, (int32_t)pos.z); text35(15, 2, b, C_WHITE);
    text35(34, 2, "SPD", C_GREY_DK); fmtInt(b, (int32_t)(len(vel) * 1.94f)); text35(47, 2, b, C_WHITE);   // knots
    if (cd) {
        char *p = fmtInt(b, ringsDone); *p++ = '/'; fmtInt(p, ringCount);
        text35(66, 2, "RING", C_GREY_DK); text35(84, 2, b, C_GOLD);
        fmtTime(b, ticks / 30);
        text35(126 - text35Width(b), 2, b, C_WHITE);
    } else {
        text35(66, 2, "THR", C_GREY_DK); fmtInt(b, (int32_t)(thr * 100)); text35(80, 2, b, C_WHITE);
    }
    if (ev != EV_GLIDER) {                       // throttle bar
        int h = (int)(thr * 40);
        gfx_rect(1, 30, 4, 42, C_INK);
        gfx_fillRect(2, 31 + 40 - h, 2, h, burner ? C_RED : C_GOLD);
        if (burner) text35(1, 75, "AB", C_GOLD);
    }
    if (ev == EV_JET) {                          // G meter
        text35(2, 121, "G", C_GREY_DK);
        int32_t g10 = (int32_t)(gNow * 10);
        if (g10 < 0) g10 = -g10;
        char *p = fmtInt(b, g10 / 10); *p++ = '.'; fmtInt(p, g10 % 10);
        text35(8, 121, b, gNow > 6 ? C_RED : C_WHITE);
        text35(24, 121, "CLB", C_GREY_DK);                         // flight-path climb angle, degrees
        fmtInt(b, (int32_t)(fatan2(vz, len(v3(vel.x, vel.y, 0))) / DEG)); text35(38, 121, b, C_WHITE);
    }
    {                                            // vario bar, centre at y 51
        int v = (int)fclamp(vz * 5, -20, 20);
        gfx_rect(123, 30, 4, 42, C_INK);
        gfx_hline(121, 51, 7, C_WHITE);
        if (v > 0) gfx_fillRect(124, 51 - v, 2, v, C_LIME);
        else if (v < 0) gfx_fillRect(124, 52, 2, -v, C_RED);
    }
    // guide arrow: next ring, then the runway or target
    if (!landed && cd && !gateDone) {
        float tx, ty;
        int8_t nx = nextRing();
        if (nx >= 0) { tx = rings[nx].x; ty = rings[nx].y; }
        else if (cd->fin == FIN_RUNWAY) {        // the runway end you're heading for
            tx = rings[ringCount - 1].x < RWY_X0 ? RWY_X0 + 40 : RWY_X1 - 40; ty = (RWY_Y0 + RWY_Y1) / 2; }
        else { tx = TGT_X; ty = TGT_Y; }
        float a = fwrap(fatan2(tx - pos.x, -(ty - pos.y)) - yaw);
        arrow(64, 18, a, nx >= 0 ? C_GOLD : C_LIME);
    }
    if (stalled && (chgame.frameCount & 8)) text35x2s(64 - 20, 96, "STALL", C_RED, C_INK);
    if (msg && msgT) {
        int w = text35Width(msg) * 2;
        text35x2s(64 - w / 2, 40, msg, msg[0] == '+' ? C_GOLD : C_WHITE, C_INK);
    }
}

#if defined(CHSIM) && defined(SKYL_AUTO)
// Simulator-only test pilot: flies the course so the geometry can be checked.
static uint8_t apPhase;
static void autopilot(int8_t &sx, int8_t &sy) {
    if (landed) { sx = sy = 0; return; }
    float tx, ty, tz; int8_t nx = -1;
    for (uint8_t i = 0; i < ringCount; i++) if (!rings[i].done) { nx = i; break; }
    float V = len(vel), vtgt = (ev == EV_GLIDER) ? 10.5f : ev == EV_JET ? 150 : 42;
    bool final_ = false;
    if (nx >= 0) {
        // a carrot on the ring's axis, halfway between us and the ring
        const Ring &g = rings[nx];
        float nxv = fsin(g.hdg), nyv = -fcos(g.hdg);
        float along = (pos.x - g.x) * nxv + (pos.y - g.y) * nyv;
        float lat = fabs_((pos.x - g.x) * -nyv + (pos.y - g.y) * nxv);
        // L1 guidance: aim at a point a fixed distance ahead along the ring's axis line
        float L1 = (ev == EV_GLIDER) ? 45 : ev == EV_JET ? 650 : 150;
        float k = along + L1 < 0 ? along + L1 : 0;
        if (along > -L1 * 0.3f && lat > L1 * 0.5f) k = -2 * L1;          // hopelessly off line: go back out
        tx = g.x + nxv * k; ty = g.y + nyv * k; tz = g.z;
    } else if (ev == EV_JET) { tx = pos.x + fsin(yaw) * 800; ty = pos.y - fcos(yaw) * 800; tz = pos.z;
    } else if (ev == EV_PLANE) {
        float cy = (RWY_Y0 + RWY_Y1) / 2;
        if (apPhase == 0 && pos.x > 650) apPhase = 0;
        if (pos.x < 760 && apPhase == 0) { tx = 760; ty = cy; tz = 45; if (fabs_(pos.x - 760) < 40) apPhase = 1; }
        else { apPhase = 1; tx = pos.x + 200; ty = cy; tz = fclamp((pos.x - 860) * -0.07f, 0, 45); final_ = true; }
        if (apPhase == 0) { tx = 640; ty = cy; tz = 50; if (pos.x < 700 && fabs_(pos.y - cy) < 60) apPhase = 1; }
    } else { tx = TGT_X; ty = TGT_Y; tz = 0; final_ = true; }
    // thermal climbing for the glider
    static int8_t circ = -1;
    if (ev == EV_GLIDER && nx >= 0 && rings[nx].z > pos.z - 5) {
        if (circ < 0) {
            for (uint8_t i = 0; i < NTHERM; i++) {
                float dx = THERMALS[i].x + drift - pos.x, dy = THERMALS[i].y - pos.y;
                if (dx * dx + dy * dy < 40 * 40 && rings[nx].z > pos.z + 10) circ = i;
            }
        }
        if (circ >= 0 && rings[nx].z + 25 < pos.z) circ = -1;
    } else circ = -1;
    float droll, dpitch;
    float dx = tx - pos.x, dy = ty - pos.y, d = fsqrt(dx * dx + dy * dy);
    if (circ >= 0) {
        float cx = THERMALS[circ].x + drift - pos.x, cy = THERMALS[circ].y - pos.y;
        droll = (cx * cx + cy * cy > 30 * 30) ? fclamp(fwrap(fatan2(cx, -cy) - yaw) * 1.8f, -0.5f, 0.5f) : 26 * DEG;
        dpitch = cr->aTrim; }
    else {
        float e = fwrap(fatan2(dx, -dy) - yaw);
        droll = fclamp(e * 1.8f, -cr->rollMax * 0.6f, cr->rollMax * 0.6f);

        float gd = fclamp((tz - pos.z) / (d + 1), -0.18f, 0.15f);
        if (final_ && pos.z < 10) gd = -0.03f;
        dpitch = gd + cr->aTrim + fabs_(roll) * 0.08f;
        if (V < vtgt * 0.8f) dpitch -= 3 * DEG;
    }
    sx = droll > roll + 2 * DEG ? 1 : droll < roll - 2 * DEG ? -1 : 0;
    sy = dpitch > pitch + 0.5f * DEG ? 1 : dpitch < pitch - 0.5f * DEG ? -1 : 0;
    if (ev != EV_GLIDER) thr = fclamp(thr + ((final_ ? 32 : vtgt) - V) * 0.01f, 0, 1);
    if (ev == EV_GLIDER) brakes = final_ && pos.z > d * 0.14f + 8;
}
#endif

// ---------------------------------------------------------------------------
// Game states
enum State : uint8_t { ST_TITLE, ST_FLY, ST_PAUSE, ST_RESULT };
static State st = ST_TITLE;
static uint8_t menu;
static float titleA;
static uint16_t lastScore;
static bool newBest;

static int16_t score() {
    int16_t s = ringsDone * 10;
    if (!crashed && cd) {
        s += landPts;
        float late = ticks / 30.0f - cd->par;
        if (ev == EV_PLANE && landPts) s += (int16_t)fclamp(30 - late * 0.5f, 0, 30);
        if (ev == EV_JET && gateDone) s += (int16_t)fclamp(50 - late * 2, 0, 50);
    }
    return s;
}

static void finish() {
    st = ST_RESULT;
    lastScore = cd ? score() : 0;
    newBest = false;
    if (cd && lastScore > sv.best[ev][course]) { sv.best[ev][course] = lastScore; svDirty = true; newBest = true; }
}

void setup() {
    chgame.boot();
    dbg::begin("SKYL 1.0");
    gfx_begin(GFX_DIV2, GFX_12BPP);
    setPalette(false);
    audio::begin(SOUNDS, sizeof(SOUNDS) / sizeof(SOUNDS[0]));
    save::load(MAGIC, 2, sv);
    makeTown();
    chgame.setFrameRate(30);
}

// Buildings and rings, far to near (painter's order), the nearest MAXOBJ at most.
static void drawObjects(bool withRings) {
    struct Item { float d; uint8_t i; };
    static Item it[72 + 12];
    uint8_t n = 0;
    for (uint8_t i = 0; i < nTown; i++) {
        const Bldg &b = town[i];
        float cx = b.x0 + b.w * 0.5f - camP.x, cy = b.y0 + b.d * 0.5f - camP.y;
        float d = cx * cx + cy * cy, r = b.h > 25 ? 1600.0f : 420.0f;     // past ~400 m the painted roofs stand in
        if (d > r * r) continue;
        float z = cx * camF.x + cy * camF.y + (b.h * 0.5f - camP.z) * camF.z;
        if (z < -b.h - 30) continue;                       // well behind us
        it[n++] = { d, i };
    }
    int8_t next = nextRing();
    if (withRings)
        for (uint8_t i = 0; i < ringCount; i++) {
            if (rings[i].done) continue;
            float dx = rings[i].x - camP.x, dy = rings[i].y - camP.y;
            it[n++] = { dx * dx + dy * dy, (uint8_t)(200 + i) };
        }
    for (uint8_t i = 1; i < n; i++)
        for (uint8_t j = i; j > 0 && it[j].d > it[j - 1].d; j--) { Item t = it[j]; it[j] = it[j - 1]; it[j - 1] = t; }
    const uint8_t MAXOBJ = 30;
    for (uint8_t k = n > MAXOBJ ? n - MAXOBJ : 0; k < n; k++) {
        uint8_t i = it[k].i;
        if (i >= 200) drawRing(rings[i - 200], (i - 200 == next && (chgame.frameCount & 4)) ? C_WHITE : C_GOLD);
        else drawBldg(town[i], it[k].d > 1000.0f * 1000.0f);
    }
}

static void drawTitle() {
    // attract: orbit the airfield
    titleA += 0.006f;
    V3 c = v3(1024 + fsin(titleA) * 420, 1322 - fcos(titleA) * 420, 140);
    V3 f, r, u;
    basis(titleA + PI_F / 2 + 0.35f, -0.22f, 0.12f, f, r, u);
    setCamera(c, f, r, u);
    renderGround(0, GFX_H);
    for (const ThermDef &t : THERMALS) drawCloud(t.x, t.y);
    drawObjects(false);
    text35x2s(64 - 28, 14, "SKYLARK", C_GOLD, C_INK);
    text35s(64 - text35Width("FLIGHT SCHOOL") / 2, 30, "FLIGHT SCHOOL", C_WHITE, C_INK);
    panel(12, 50, 104, 64, 3, C_INK, C_GREY_DK);
    static const char *const ITEMS[] = { "LIGHT PLANE", "HANG GLIDER", "JET", "FREE FLIGHT" };
    char b[8];
    for (uint8_t i = 0; i < 4; i++) {
        int y = 55 + i * 9;
        if (i == menu) gfx_fillRect(16, y - 2, 96, 9, C_GREY_DK);
        text35(20, y, ITEMS[i], i == menu ? C_GOLD : C_WHITE);
        if (i < 3) {
            uint16_t best = sv.best[i][sv.course[i]];
            if (best) { fmtInt(b, best); text35(108 - text35Width(b), y, b, C_LIME); }
        }
    }
    if (menu < 3) {                                      // the course, LEFT/RIGHT to change it
        const char *nm = COURSES[menu][sv.course[menu]].name;
        int w = text35Width(nm);
        text35(64 - w / 2, 93, nm, C_LIME);
        if (sv.course[menu] > 0) text35(64 - w / 2 - 8, 93, "<", C_GOLD);
        if (sv.course[menu] + 1 < COURSE_N[menu]) text35(64 + w / 2 + 4, 93, ">", C_GOLD);
    }
    text35(18, 104, sv.invert ? "SEL: UP=NOSE UP" : "SEL: UP=NOSE DOWN", C_HAZE);
    if (chgame.frameCount & 16) text35s(64 - text35Width("PRESS A") / 2, 118, "PRESS A", C_WHITE, C_INK);
}

static void drawWorld() {
    V3 f, r, u;
    f = kF; u = kU; r = norm(cross(u, f));
    V3 c = pos - f * cr->camD + u * cr->camH;
    setCamera(c, f, r, u);
    renderGround(0, GFX_H);
    for (const ThermDef &t : THERMALS) drawCloud(t.x + drift, t.y);
    drawShadow();
    drawObjects(true);
    drawCraft();
}

// One results row: label on the left, value on the right
static void row(int &y, const char *label, const char *val, uint8_t col) {
    text35(18, y, label, col);
    if (val) text35(110 - text35Width(val), y, val, col);
    y += 9;
}

static void drawResult() {
    gfx_fillRect(12, 20, 104, 92, C_INK);
    gfx_rect(12, 20, 104, 92, C_GOLD);
    char b[12];
    const char *t = cd ? cd->name : "FREE FLIGHT";
    text35(64 - text35Width(t) / 2, 25, t, C_GOLD);
    int y = 38;
    if (!cd) {
        row(y, crashed ? "CRASHED" : "SAFE LANDING", nullptr, crashed ? C_RED : C_LIME);
        fmtTime(b, ticks / 30); row(y, "FLIGHT TIME", b, C_WHITE);
    } else {
        char *p = fmtInt(b, ringsDone); *p++ = '/'; fmtInt(p, ringCount);
        row(y, ev == EV_JET ? "GATES" : "RINGS", b, C_WHITE);
        if (ev == EV_JET) {
            fmtTime(b, ticks / 30);
            row(y, crashed ? "CRASHED" : gateDone ? "TIME" : "DNF", gateDone && !crashed ? b : nullptr, crashed ? C_RED : C_WHITE);
            fmtTime(b, cd->par); row(y, "PAR", b, C_GREY_DK);
        } else {
            fmtInt(b, crashed ? 0 : landPts);
            row(y, crashed ? "CRASHED" : ev == EV_PLANE ? "LANDING" : "TARGET", b, crashed ? C_RED : C_WHITE);
            if (!crashed) {
                p = fmtInt(b, (int32_t)touchSink); *p++ = '.'; fmtInt(p, (int32_t)(touchSink * 10) % 10);
                row(y, "SINK M/S", b, C_GREY_DK);
            }
            if (ev == EV_PLANE && !crashed && landPts) { fmtTime(b, ticks / 30); row(y, "TIME", b, C_WHITE); }
        }
        gfx_hline(18, y, 92, C_GREY_DK); y += 4;
        text35x2s(18, y, "SCORE", C_WHITE, C_GREY_DK); fmtInt(b, lastScore);
        text35x2s(110 - text35Width(b) * 2, y, b, C_GOLD, C_GREY_DK); y += 16;
        if (newBest && (chgame.frameCount & 8)) text35(64 - text35Width("NEW BEST!") / 2, y, "NEW BEST!", C_LIME);
        else { fmtInt(b, sv.best[ev][course]); row(y, "BEST", b, C_GREY_DK); }
    }
    text35(18, 103, "A AGAIN  B MENU", C_HAZE);
}

void loop() {
    dbg::poll();
    if (!chgame.nextFrame()) return;
    chgame.pollButtons();

    if (st == ST_TITLE) {
        if (chgame.justPressed(UP_BUTTON) && menu > 0) { menu--; audio::sfx(SFX_MENU); }
        if (chgame.justPressed(DOWN_BUTTON) && menu < 3) { menu++; audio::sfx(SFX_MENU); }
        if (menu < 3) {
            uint8_t &c = sv.course[menu];
            if (c >= COURSE_N[menu]) c = 0;
            if (chgame.justPressed(LEFT_BUTTON) && c > 0) { c--; svDirty = true; audio::sfx(SFX_MENU); }
            if (chgame.justPressed(RIGHT_BUTTON) && c + 1 < COURSE_N[menu]) { c++; svDirty = true; audio::sfx(SFX_MENU); }
        }
        if (chgame.justPressed(SELECT_BUTTON)) { sv.invert ^= 1; svDirty = true; audio::sfx(SFX_MENU); }
        if (chgame.justPressed(A_BUTTON)) { startEvent((Event)menu); st = ST_FLY; audio::sfx(SFX_MENU); }
    } else if (st == ST_FLY) {
        if (chgame.justPressed(START_BUTTON)) st = ST_PAUSE;
        int8_t sx = chgame.pressed(RIGHT_BUTTON) ? 1 : chgame.pressed(LEFT_BUTTON) ? -1 : 0;
        int8_t sy = chgame.pressed(UP_BUTTON) ? -1 : chgame.pressed(DOWN_BUTTON) ? 1 : 0;  // pull back = nose up
        if (sv.invert) sy = -sy;
#if defined(CHSIM) && defined(SKYL_AUTO)
        autopilot(sx, sy);
#endif
        if (landed) { sx = 0; if (ev != EV_FREE) sy = 0; }
        if (cr->tMax > 0 && !crashed) {
            if (chgame.pressed(A_BUTTON)) thr += 0.02f;
            if (chgame.pressed(B_BUTTON)) thr -= 0.02f;
            thr = fclamp(thr, 0, 1);
        }
        {   // afterburner: hold A with the throttle already at full
            bool ab = ev == EV_JET && airborne && !crashed && thr >= 1 && chgame.pressed(A_BUTTON);
            if (ab && !burner) audio::sfx(SFX_BURN);
            burner = ab;
        }
        brakes = chgame.pressed(B_BUTTON) && (ev == EV_GLIDER || !airborne || (ev == EV_JET && thr <= 0));   // jet: speedbrake at idle
#if defined(CHSIM) && defined(SKYL_AUTO)
        { int8_t a = 0, b = 0; if (ev == EV_GLIDER) autopilot(a, b); else if (!airborne && landed) brakes = true; }
#endif
        if (landed && ev != EV_FREE) thr = 0;
        physics(sx, sy);
        if ((!landed || ev == EV_FREE) && !gateDone) ticks++;
        if (msgT) msgT--;
        if (flashT) flashT--;
        gfx_setFade(flashT ? flashT * 8 : 0, RGB(255, 60, 40));

        if (ev == EV_GLIDER && airborne) {            // variometer
            if (varioT) varioT--;
            else if (vz > 0.25f) { audio::blip((uint16_t)(700 + vz * 260), 45, true); varioT = (uint8_t)fclamp(13 - vz * 3, 3, 13); }
            else if (vz < -1.9f) { audio::blip(240, 160, true); varioT = 18; }
        }
        if (stalled && (chgame.frameCount & 7) == 0 && !audio::playing()) audio::blip(2200, 60);

        if (landed) {
            float s = len(v3(vel.x, vel.y, 0));
            if (s < 0.5f || crashed) endTicks++;
            if (ev == EV_FREE && !crashed && thr > 0.05f) endTicks = 0;      // still taxiing
            if (endTicks > (crashed ? 60 : 45)) finish();
        } else if (gateDone && ++endTicks > 60) finish();
#if defined(CHSIM) && defined(SKYL_TRACE)
        if (chgame.frameCount % 15 == 0)
            fprintf(stderr, "t=%5.1f st=%d x=%6.0f y=%6.0f z=%6.1f V=%5.1f vz=%5.2f pit=%5.1f rol=%5.1f yaw=%6.1f thr=%.2f stall=%d air=%d land=%d crash=%d rings=%d pts=%d\n",
                ticks / 30.0f, st, pos.x, pos.y, pos.z, len(vel), vz, pitch / DEG, roll / DEG, yaw / DEG, thr, stalled, airborne, landed, crashed, ringsDone, landPts);
#endif
        if (pos.z > (ev == EV_JET ? 9000 : 2500) || pos.x < -20000 || pos.x > 22000 || pos.y < -20000 || pos.y > 22000) { crashed = true; finish(); }
    } else if (st == ST_PAUSE) {
        if (chgame.justPressed(START_BUTTON)) st = ST_FLY;
        if (chgame.justPressed(SELECT_BUTTON)) { st = ST_TITLE; setPalette(false); gfx_setFade(0); }
    } else if (st == ST_RESULT) {
        if (chgame.justPressed(A_BUTTON)) { startEvent(ev); st = ST_FLY; }
        if (chgame.justPressed(B_BUTTON)) { st = ST_TITLE; setPalette(false); gfx_setFade(0); }
    }
    audio::update();

    gfx_wait();
    if (svDirty) svDirty = !save::store(MAGIC, 2, sv);
    if (st == ST_TITLE) drawTitle();
    else {
        drawWorld();
        if (st == ST_FLY || st == ST_PAUSE) hud();
        if (st == ST_PAUSE) {
            panel(24, 50, 80, 28, 3, C_INK, C_GOLD);
            text35x2s(64 - 24, 54, "PAUSED", C_WHITE, C_GREY_DK);
            text35(64 - text35Width("SELECT: QUIT") / 2, 69, "SELECT: QUIT", C_HAZE);
        }
        if (st == ST_RESULT) drawResult();
    }
    gfx_flushAsync();
}
