// Float helpers (no libm: these are small and plenty accurate for flight).
#pragma once
#include <stdint.h>

static const float PI_F = 3.14159265f;
static const float DEG = PI_F / 180.0f;

static inline float fwrap(float a) {           // to -pi..pi
    while (a > PI_F) a -= 2 * PI_F;
    while (a < -PI_F) a += 2 * PI_F;
    return a;
}
static inline float fsin(float x) {
    x = fwrap(x);
    if (x > PI_F / 2) x = PI_F - x;
    else if (x < -PI_F / 2) x = -PI_F - x;
    float x2 = x * x;                   // Taylor to x^7 on -pi/2..pi/2
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42)));
}
static inline float fcos(float x) { return fsin(x + PI_F / 2); }
static inline float fabs_(float x) { return x < 0 ? -x : x; }
static inline float fclamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float fatan2(float y, float x) {
    float ax = fabs_(x), ay = fabs_(y);
    if (ax < 1e-6f && ay < 1e-6f) return 0;
    float z = ax > ay ? ay / ax : ax / ay;
    float a = z * (PI_F / 4) - z * (z - 1) * (0.2447f + 0.0663f * z);
    if (ay > ax) a = PI_F / 2 - a;
    if (x < 0) a = PI_F - a;
    return y < 0 ? -a : a;
}
static inline float fsqrt(float x) {
    if (x <= 0) return 0;
    union { float f; uint32_t i; } u = { x };
    u.i = 0x1FBD1DF5 + (u.i >> 1);
    float r = u.f;
    r = 0.5f * (r + x / r);
    r = 0.5f * (r + x / r);
    return r;
}

struct V3 { float x, y, z; };
static inline V3 v3(float x, float y, float z) { V3 v = { x, y, z }; return v; }
static inline V3 operator+(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 operator-(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 operator*(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float len(V3 a) { return fsqrt(dot(a, a)); }
static inline V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline V3 norm(V3 a) { float l = len(a); return l > 1e-6f ? a * (1.0f / l) : a; }

