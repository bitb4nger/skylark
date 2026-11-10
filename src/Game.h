// Skylark's types, in a header so the IDE's prototype generator sees them first.
#pragma once
#include <stdint.h>

// Palette indices
enum : uint8_t {
    C_SKY_TOP, C_SKY_MID, C_HAZE, C_WHITE, C_GRASS_DK, C_GRASS, C_CROP, C_SOIL,
    C_WATER_DK, C_WATER, C_ASPHALT, C_GREY_DK, C_RED, C_GOLD, C_INK, C_LIME
};

enum Event : uint8_t { EV_PLANE, EV_GLIDER, EV_FREE };

struct RingDef { int16_t x, y, z, hdg; };

struct ThermDef { int16_t x, y; uint8_t w10; };

struct Ring { float x, y, z, hdg; bool done; };

struct Craft {
    float kL, cla, cl0, aStall, cd0, kInd, tMax, aTrim, pitchRate, rollRate, rollMax, camD, camH;
};
