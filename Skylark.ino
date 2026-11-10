// SKYLARK: a Pilotwings-style flight demake for the CHGame.
//
// Events
//   LIGHT PLANE  fly the ring course over town and lake, land on runway 09.
//   HANG GLIDER  ride thermals up through the rings, land on the target.
//   FREE FLIGHT  take off from the runway at golden hour and go anywhere.
//
// Controls
//   D-pad        stick: LEFT/RIGHT roll, UP/DOWN pitch (UP = nose down,
//                like a real stick; SELECT on the title screen flips it)
//   A / B        plane: throttle up / down (B also brakes on the ground)
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
enum { SFX_CHIME, SFX_CRASH, SFX_LAND, SFX_MENU, SFX_MISS };
const audio::Effect SOUNDS[] = {
    AUDIO_EFFECT(S_CHIME, 2), AUDIO_EFFECT(S_CRASH, 3), AUDIO_EFFECT(S_LAND, 3),
    AUDIO_EFFECT(S_MENU, 1), AUDIO_EFFECT(S_MISS, 2),
};

// ---------------------------------------------------------------------------
// Save
struct SaveData { uint16_t best[2]; uint8_t invert; uint8_t pad; };
const uint32_t MAGIC = save::magic("SKYL");
static SaveData sv = { { 0, 0 }, 0, 0 };
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

static const RingDef PLANE_RINGS[] = {
    { 1600, 1400, 150, 0 },   { 1560, 1000, 165, 350 }, { 1500, 650, 110, 335 },
    { 1260, 430, 150, 290 },  { 900, 360, 175, 265 },   { 560, 420, 130, 235 },
    { 380, 760, 70, 185 },    { 440, 1322, 40, 90 },     // last: the final approach gate
};
static const RingDef GLIDER_RINGS[] = {
    { 1200, 1560, 170, 0 },   { 1200, 1450, 255, 0 },   { 940, 1230, 235, 315 },
    { 800, 1100, 330, 300 },  { 1000, 820, 210, 60 },
};
static const ThermDef THERMALS[] = { { 1200, 1440, 34 }, { 800, 1090, 32 }, { 1480, 820, 26 }, { 560, 1500, 28 } };

static const float RWY_X0 = 864, RWY_X1 = 1184, RWY_Y0 = 1304, RWY_Y1 = 1340;  // metres
static const float TGT_X = 1202, TGT_Y = 922;

static Ring rings[8];
static uint8_t ringCount, ringsDone;

// ---------------------------------------------------------------------------
// Aircraft
static const Craft PLANE  = { 0.0125f, 5.5f, 0.25f, 15 * DEG, 0.05f, 0.045f, 3.6f, 2.0f * DEG, 0.9f, 1.6f, 80 * DEG, 14, 3.0f };
static const Craft GLIDER = { 0.12f, 4.5f, 0.30f, 14 * DEG, 0.05f, 0.05f, 0, 5.6f * DEG, 0.7f, 1.1f, 60 * DEG, 9, 2.0f };

static Event ev;
static const Craft *cr;
static V3 pos, vel;                    // vel is air-relative
static float yaw, pitch, roll, thr;
static V3 bF, bR, bU;                    // body axes: nose, right wing, top. Free to point anywhere.
static V3 kF, kU;                        // the chase camera's lagging forward and up
// yaw/pitch/roll above are derived from the body axes each frame (HUD, scoring)
static bool airborne, stalled, landed, crashed, brakes;
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

static void startEvent(Event e) {
    ev = e;
    cr = (e == EV_GLIDER) ? &GLIDER : &PLANE;
    setPalette(e == EV_FREE);
    ringCount = ringsDone = 0;
    const RingDef *rd = nullptr;
    if (e == EV_PLANE) { rd = PLANE_RINGS; ringCount = sizeof(PLANE_RINGS) / sizeof(RingDef); }
    if (e == EV_GLIDER) { rd = GLIDER_RINGS; ringCount = sizeof(GLIDER_RINGS) / sizeof(RingDef); }
    for (uint8_t i = 0; i < ringCount; i++)
        rings[i] = { (float)rd[i].x, (float)rd[i].y, (float)rd[i].z, rd[i].hdg * DEG, false };
    roll = 0; brakes = false; stalled = landed = crashed = false;
    ticks = endTicks = 0; msg = nullptr; flashT = 0; touchScore = landPts = 0;
    windX = 0; drift = 0;
    if (e == EV_PLANE) {
        pos = v3(1600, 1720, 160); yaw = 0; vel = v3(0, -42, 0); thr = 0.36f; pitch = cr->aTrim;   // trimmed
        airborne = true;
    } else if (e == EV_GLIDER) {
        pos = v3(1200, 1720, 185); yaw = 0; vel = v3(0, -10.5f, 0); thr = 0; pitch = cr->aTrim;
        airborne = true; windX = 1.0f;
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
    if (SKYL_TEST == 4 && e == EV_PLANE) { pos = v3(1300, 1700, 220); roll = 90 * DEG; yaw = 340 * DEG; vel = v3(-15, -60, 0); }
#endif
    basis(yaw, pitch, roll, bF, bR, bU);
    kF = bF; kU = bU;
    gfx_setFade(0);
}

static void say(const char *m, uint8_t t = 45) { msg = m; msgT = t; }

static void touchdown() {
    float sink = -vz;
    bool onRwy = pos.x > RWY_X0 && pos.x < RWY_X1 && pos.y > RWY_Y0 - 6 && pos.y < RWY_Y1 + 6;
    float maxSink = (ev == EV_GLIDER) ? 3.2f : 3.4f;
    float maxRoll = (ev == EV_GLIDER) ? 25 * DEG : 14 * DEG;
    touchSink = sink;
    if (sink > maxSink || fabs_(roll) > maxRoll || pitch < -8 * DEG || (ev == EV_PLANE && sink > 1.5f && !onRwy)) {
        crashed = true; landed = true; flashT = 24;
        audio::sfx(SFX_CRASH);
        say("CRASH!", 90);
        vel = vel * 0.15f;
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

static void checkRings(V3 prev) {
    for (uint8_t i = 0; i < ringCount; i++) {
        Ring &g = rings[i];
        if (g.done) continue;
        V3 n = v3(fsin(g.hdg), -fcos(g.hdg), 0), c = v3(g.x, g.y, g.z);
        float a = dot(prev - c, n), b = dot(pos - c, n);
        if ((a < 0) == (b < 0)) continue;
        float t = a / (a - b);
        V3 hit = prev + (pos - prev) * t;
        float d = len(hit - c);
        float R = (ev == EV_GLIDER) ? 8.0f : 10.0f;
        if (d < R) { g.done = true; ringsDone++; audio::sfx(SFX_CHIME); say("+10", 30); }
        else if (d < R * 2.2f) { audio::sfx(SFX_MISS); say("MISSED", 30); }
    }
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
        float cd = k.cd0 + k.kInd * CL * CL + (brakes && ev == EV_GLIDER ? 0.14f : 0);
        acc = acc - vn * (q * cd);
    }
    acc = acc + f * (thr * k.tMax);
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
    float pq = fclamp(4.0f * (aCmd - alpha), -k.pitchRate * 2, k.pitchRate * 2) * auth;
    if (stalled && alpha > 0) pq -= 1.2f * (alpha - k.aStall);
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

    // the camera lags a little, so the craft visibly banks against it; it follows through rolls and loops
    kF = norm(kF + (bF - kF) * 0.3f);
    kU = kU + (bU - kU) * 0.22f;
    V3 kr = norm(cross(kU, kF));
    kU = cross(kF, kr);
}

// ---------------------------------------------------------------------------
// Drawing objects
static void drawRing(const Ring &g, uint8_t col) {
    float R = (ev == EV_GLIDER) ? 8.0f : 10.0f;
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

static void drawCraft() {
    int sx, sy; float z;
    if (!project(pos, sx, sy, z)) return;
    float a = fatan2(dot(bU, camR), dot(bU, camU));   // the craft's bank against the lagging camera
    int sn = (int)(fsin(a) * 256), cs = (int)(fcos(a) * 256);
    if (ev == EV_GLIDER) {
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
    if (ev != EV_FREE) {
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
        gfx_fillRect(2, 31 + 40 - h, 2, h, C_GOLD);
    }
    {                                            // vario bar, centre at y 51
        int v = (int)fclamp(vz * 5, -20, 20);
        gfx_rect(123, 30, 4, 42, C_INK);
        gfx_hline(121, 51, 7, C_WHITE);
        if (v > 0) gfx_fillRect(124, 51 - v, 2, v, C_LIME);
        else if (v < 0) gfx_fillRect(124, 52, 2, -v, C_RED);
    }
    // guide arrow: next ring, then the runway or target
    if (!landed && ev != EV_FREE) {
        float tx, ty;
        int8_t nx = -1;
        for (uint8_t i = 0; i < ringCount; i++) if (!rings[i].done) { nx = i; break; }
        if (nx >= 0) { tx = rings[nx].x; ty = rings[nx].y; }
        else if (ev == EV_PLANE) { tx = RWY_X0 + 40; ty = (RWY_Y0 + RWY_Y1) / 2; }
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
    float V = len(vel), vtgt = (ev == EV_GLIDER) ? 10.5f : 42;
    bool final_ = false;
    if (nx >= 0) {
        // a carrot on the ring's axis, halfway between us and the ring
        const Ring &g = rings[nx];
        float nxv = fsin(g.hdg), nyv = -fcos(g.hdg);
        float along = (pos.x - g.x) * nxv + (pos.y - g.y) * nyv;
        float lat = fabs_((pos.x - g.x) * -nyv + (pos.y - g.y) * nxv);
        float k = along < 0 ? along * 0.5f : 0;
        float setup = (ev == EV_GLIDER) ? 60 : 260;
        if (along > -setup * 0.8f && lat > ((ev == EV_GLIDER) ? 6 : 12)) k = -setup;   // go round again
        tx = g.x + nxv * k; ty = g.y + nyv * k; tz = g.z;
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
            for (uint8_t i = 0; i < 4; i++) {
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
    if (ev == EV_PLANE) thr = fclamp(thr + ((final_ ? 32 : vtgt) - V) * 0.01f, 0, 1);
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
    if (!crashed) {
        s += landPts;
        if (ev == EV_PLANE && landPts) s += (int16_t)fclamp(30 - ((int)(ticks / 30) - 120) * 0.5f, 0, 30);
    }
    return s;
}

static void finish() {
    st = ST_RESULT;
    lastScore = (ev == EV_FREE) ? 0 : score();
    newBest = false;
    if (ev != EV_FREE && lastScore > sv.best[ev]) { sv.best[ev] = lastScore; svDirty = true; newBest = true; }
}

void setup() {
    chgame.boot();
    dbg::begin("SKYL 1.0");
    gfx_begin(GFX_DIV2, GFX_12BPP);
    setPalette(false);
    audio::begin(SOUNDS, sizeof(SOUNDS) / sizeof(SOUNDS[0]));
    save::load(MAGIC, 1, sv);
    chgame.setFrameRate(30);
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
    text35x2s(64 - 28, 14, "SKYLARK", C_GOLD, C_INK);
    text35s(64 - text35Width("FLIGHT SCHOOL") / 2, 30, "FLIGHT SCHOOL", C_WHITE, C_INK);
    panel(14, 62, 100, 50, 3, C_INK, C_GREY_DK);
    static const char *const ITEMS[] = { "LIGHT PLANE", "HANG GLIDER", "FREE FLIGHT" };
    char b[8];
    for (uint8_t i = 0; i < 3; i++) {
        int y = 67 + i * 11;
        if (i == menu) gfx_fillRect(18, y - 2, 92, 9, C_GREY_DK);
        text35(22, y, ITEMS[i], i == menu ? C_GOLD : C_WHITE);
        if (i < 2 && sv.best[i]) { fmtInt(b, sv.best[i]); text35(106 - text35Width(b), y, b, C_LIME); }
    }
    text35(18, 102, sv.invert ? "SEL: UP=NOSE UP" : "SEL: UP=NOSE DOWN", C_HAZE);
    if (chgame.frameCount & 16) text35s(64 - text35Width("PRESS A") / 2, 117, "PRESS A", C_WHITE, C_INK);
}

static void drawWorld() {
    V3 f, r, u;
    f = kF; u = kU; r = norm(cross(u, f));
    V3 c = pos - f * cr->camD + u * cr->camH;
    setCamera(c, f, r, u);
    renderGround(0, GFX_H);
    for (const ThermDef &t : THERMALS) drawCloud(t.x + drift, t.y);
    // far rings first
    uint8_t order[8]; float dist[8];
    for (uint8_t i = 0; i < ringCount; i++) {
        order[i] = i;
        float dx = rings[i].x - camP.x, dy = rings[i].y - camP.y;
        dist[i] = dx * dx + dy * dy;
    }
    for (uint8_t i = 1; i < ringCount; i++)
        for (uint8_t j = i; j > 0 && dist[order[j]] > dist[order[j - 1]]; j--) { uint8_t t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    int8_t next = -1;
    for (uint8_t i = 0; i < ringCount; i++) if (!rings[i].done) { next = i; break; }
    for (uint8_t i = 0; i < ringCount; i++) {
        const Ring &g = rings[order[i]];
        if (!g.done) drawRing(g, (order[i] == next && (chgame.frameCount & 4)) ? C_WHITE : C_GOLD);
    }
    drawShadow();
    drawCraft();
}

static void drawResult() {
    gfx_fillRect(12, 20, 104, 92, C_INK);
    gfx_rect(12, 20, 104, 92, C_GOLD);
    char b[12];
    const char *t = ev == EV_PLANE ? "LIGHT PLANE" : ev == EV_GLIDER ? "HANG GLIDER" : "FREE FLIGHT";
    text35(64 - text35Width(t) / 2, 25, t, C_GOLD);
    int y = 38;
    if (ev != EV_FREE) {
        text35(18, y, "RINGS", C_WHITE); char *p = fmtInt(b, ringsDone); *p++ = '/'; fmtInt(p, ringCount);
        text35(110 - text35Width(b), y, b, C_WHITE); y += 9;
        text35(18, y, crashed ? "CRASHED" : ev == EV_PLANE ? "LANDING" : "TARGET", crashed ? C_RED : C_WHITE);
        fmtInt(b, crashed ? 0 : landPts); text35(110 - text35Width(b), y, b, C_WHITE); y += 9;
        if (!crashed) {
            text35(18, y, "SINK M/S", C_GREY_DK); p = fmtInt(b, (int32_t)touchSink); *p++ = '.';
            fmtInt(p, (int32_t)(touchSink * 10) % 10); text35(110 - text35Width(b), y, b, C_GREY_DK); y += 9;
        }
        if (ev == EV_PLANE && !crashed && landPts) {
            text35(18, y, "TIME", C_WHITE); fmtTime(b, ticks / 30); text35(110 - text35Width(b), y, b, C_WHITE); y += 9;
        }
        gfx_hline(18, y, 92, C_GREY_DK); y += 4;
        text35x2s(18, y, "SCORE", C_WHITE, C_GREY_DK); fmtInt(b, lastScore);
        text35x2s(110 - text35Width(b) * 2, y, b, C_GOLD, C_GREY_DK); y += 16;
        if (newBest && (chgame.frameCount & 8)) text35(64 - text35Width("NEW BEST!") / 2, y, "NEW BEST!", C_LIME);
        else { text35(18, y, "BEST", C_GREY_DK); fmtInt(b, sv.best[ev]); text35(110 - text35Width(b), y, b, C_GREY_DK); }
    } else {
        text35(18, y, crashed ? "CRASHED" : "SAFE LANDING", crashed ? C_RED : C_LIME); y += 10;
        text35(18, y, "FLIGHT TIME", C_WHITE); fmtTime(b, ticks / 30); text35(110 - text35Width(b), y, b, C_WHITE);
    }
    text35(18, 103, "A AGAIN  B MENU", C_HAZE);
}

void loop() {
    dbg::poll();
    if (!chgame.nextFrame()) return;
    chgame.pollButtons();

    if (st == ST_TITLE) {
        if (chgame.justPressed(UP_BUTTON) && menu > 0) { menu--; audio::sfx(SFX_MENU); }
        if (chgame.justPressed(DOWN_BUTTON) && menu < 2) { menu++; audio::sfx(SFX_MENU); }
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
        brakes = chgame.pressed(B_BUTTON) && (ev == EV_GLIDER || !airborne);
#if defined(CHSIM) && defined(SKYL_AUTO)
        { int8_t a = 0, b = 0; if (ev == EV_GLIDER) autopilot(a, b); else if (!airborne && landed) brakes = true; }
#endif
        if (landed && ev != EV_FREE) thr = 0;
        physics(sx, sy);
        if (!landed || ev == EV_FREE) ticks++;
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
        }
#if defined(CHSIM) && defined(SKYL_TRACE)
        if (chgame.frameCount % 15 == 0)
            fprintf(stderr, "t=%5.1f st=%d x=%6.0f y=%6.0f z=%6.1f V=%5.1f vz=%5.2f pit=%5.1f rol=%5.1f yaw=%6.1f thr=%.2f stall=%d air=%d land=%d crash=%d rings=%d pts=%d\n",
                ticks / 30.0f, st, pos.x, pos.y, pos.z, len(vel), vz, pitch / DEG, roll / DEG, yaw / DEG, thr, stalled, airborne, landed, crashed, ringsDone, landPts);
#endif
        if (pos.z > 2500 || pos.x < -20000 || pos.x > 22000 || pos.y < -20000 || pos.y > 22000) { crashed = true; finish(); }
    } else if (st == ST_PAUSE) {
        if (chgame.justPressed(START_BUTTON)) st = ST_FLY;
        if (chgame.justPressed(SELECT_BUTTON)) { st = ST_TITLE; setPalette(false); gfx_setFade(0); }
    } else if (st == ST_RESULT) {
        if (chgame.justPressed(A_BUTTON)) { startEvent(ev); st = ST_FLY; }
        if (chgame.justPressed(B_BUTTON)) { st = ST_TITLE; setPalette(false); gfx_setFade(0); }
    }
    audio::update();

    gfx_wait();
    if (svDirty) svDirty = !save::store(MAGIC, 1, sv);
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
