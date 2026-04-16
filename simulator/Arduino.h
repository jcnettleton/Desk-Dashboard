// Minimal Arduino shim for compiling Adafruit GFX on desktop
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>

#include "Print.h"

#define ARDUINO 100

// Flash/PROGMEM — no-ops on desktop
#define PROGMEM
#define F(x) (x)
#define pgm_read_byte(addr) (*(const unsigned char *)(addr))
#define pgm_read_word(addr) (*(const unsigned short *)(addr))
#define pgm_read_dword(addr) (*(const unsigned long *)(addr))

class __FlashStringHelper;

using std::min;
using std::max;
using std::abs;

inline double radians(double deg) { return deg * M_PI / 180.0; }
inline double degrees(double rad) { return rad * 180.0 / M_PI; }

typedef bool boolean;
typedef uint8_t byte;

inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
