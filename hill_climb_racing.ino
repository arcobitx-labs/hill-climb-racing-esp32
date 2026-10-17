// SPDX-License-Identifier: MIT
// Copyright (c) 2026 arcobitx-labs - see LICENSE
/*
  HILL CLIMB RACING v2  -  hardware edition
  Board : ESP32 WROOM-32 ("ESP32 Dev Module")      Lib: U8g2

  WIRING
    OLED  VCC->3V3  GND->GND  SDA->GPIO21  SCL->GPIO22
    Touch LEFT  OUT->GPIO32     Touch RIGHT OUT->GPIO33   (TTP223, momentary, active HIGH)

  CONTROLS
    Menus : single tap = previous (left) / next (right)
            LEFT double tap = back, RIGHT double tap = select / buy
    Race  : hold LEFT = brake / reverse / lean forward in air
            hold RIGHT = gas / lean back in air
            hold BOTH for 3 s = pause (a bar fills while you hold)
    Factory reset: hold both pads while powering on for 3 s

  NOTE ON DRAWING: U8g2 uses 8-bit coordinates on 128x64, so every shape is
  culled to x <= 118 before drawing. Do not remove those checks.
*/
#pragma GCC optimize ("O3")
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>

// ======================= USER CONFIG =======================
#define USE_SH1106        0
#define PIN_SDA           21
#define PIN_SCL           22
#define PIN_TOUCH_L       32
#define PIN_TOUCH_R       33
#define TOUCH_ACTIVE      HIGH
#define I2C_SPEED         800000UL
#define TAP_MAX_MS        280
#define DOUBLE_GAP_MS     260
#define PAUSE_HOLD_MS     3000
#define SAVE_INTERVAL_MS  2500

#define NV 8
#define NS 8
#define SC 10.0f                 // pixels per metre
#define PHYS_DT (1.0f/120.0f)
#define FRAME_US 16000          // frame cap ~62 fps
#define MAX_ITEMS 64
#define MAX_PARTS 40

#if USE_SH1106
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);
#else
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);
#endif
Preferences prefs;

// Forward declarations: the Arduino builder auto-generates function prototypes
// ABOVE the struct definitions, so these types must be declared before the first function.
struct Vehicle;
struct Stage;
struct Poly;

// ======================= HELPERS =======================
static inline uint32_t hash32(uint32_t v) {
  v ^= v >> 16; v *= 0x7feb352dU; v ^= v >> 15; v *= 0x846ca68bU; v ^= v >> 16; return v;
}
static inline int fdiv(int a, int b) { int q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }
static inline int fmodp(int a, int b) { int m = a % b; return m < 0 ? m + b : m; }
static inline int wrapi(float v, int span) { float r = fmodf(v, (float)span); if (r < 0) r += span; return (int)r; }

static uint32_t rngState = 12345;
static uint32_t rnd() { rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5; return rngState; }
static inline float frand() { return (float)(rnd() & 1023) * (1.0f / 512.0f) - 1.0f; }   // -1..1

// ======================= STAGES =======================
struct Stage {
  const char* name;
  float g, grip, a0, a1, a2, f0, f1, f2, rough;
  uint16_t price;
  float camBias;
};
static const Stage STAGES[NS] = {
  {"COUNTRYSIDE", 9.81f, 1.00f, 2.0f, 0.9f, 0.3f, 0.15f, 0.31f, 0.70f, 0.00f, 0,     0.0f},
  {"DESERT",      9.81f, 0.85f, 1.5f, 0.8f, 0.3f, 0.12f, 0.23f, 0.60f, 0.00f, 1500,  0.0f},
  {"ARCTIC",      9.81f, 0.45f, 2.4f, 1.0f, 0.2f, 0.10f, 0.20f, 0.50f, 0.00f, 3500,  0.0f},
  {"CAVE",        9.81f, 0.90f, 1.4f, 0.7f, 0.3f, 0.16f, 0.33f, 0.80f, 0.45f, 6000,  0.9f},
  {"CITY",        9.81f, 1.05f, 0.0f, 0.0f, 0.0f, 0.00f, 0.00f, 0.00f, 0.90f, 9000,  0.0f},
  {"MOON",        1.62f, 0.90f, 1.8f, 0.8f, 0.3f, 0.13f, 0.29f, 0.70f, 1.60f, 13000, 0.0f},
  {"MARS",        3.71f, 1.00f, 2.2f, 1.2f, 0.4f, 0.14f, 0.33f, 0.80f, 0.35f, 18000, 0.0f},
  {"VOLCANO",     9.81f, 0.80f, 2.0f, 1.0f, 0.4f, 0.15f, 0.35f, 0.90f, 0.50f, 25000, 0.0f},
};

static float terrainH(uint8_t st, float x) {
  if (x < 6.0f) return 0.0f;
  const Stage& S = STAGES[st];
  float d = fminf(x * (1.0f / 2500.0f), 1.0f);
  float amp = 0.5f + 0.8f * d;
  float r = fminf((x - 6.0f) * (1.0f / 30.0f), 1.0f); r = r * r * (3.0f - 2.0f * r);
  float h;
  if (st == 4) {                                   // city: rooftops joined by ramps
    const float L = 26.0f;
    int i = (int)floorf(x / L);
    float u = x - i * L;
    float h1 = (float)(hash32((uint32_t)i * 977u + 13u) % 4) * S.rough * amp;
    float h0 = (float)(hash32((uint32_t)(i - 1) * 977u + 13u) % 4) * S.rough * amp;
    float t = fminf(u / 6.0f, 1.0f); t = t * t * (3.0f - 2.0f * t);
    h = h0 + (h1 - h0) * t;
    return h * r;
  }
  if (st == 1) h = S.a0 * (1.0f - 2.0f * fabsf(sinf(x * S.f0 + 0.7f)));
  else h = S.a0 * sinf(x * S.f0 + 0.7f);
  h += S.a1 * sinf(x * S.f1 + 2.1f) + S.a2 * sinf(x * S.f2 + 4.3f);
  h *= amp;
  if (st == 3) h += S.rough * fabsf(sinf(x * 1.9f));                       // stalagmite bumps
  else if (st == 5) { float c = sinf(x * 0.19f + 1.3f); if (c > 0.0f) h -= S.rough * powf(c, 8.0f); }
  else if (st == 6) h += S.rough * sinf(x * 2.9f + 1.0f) * sinf(x * 0.45f + 0.3f);
  else if (st == 7) h += S.rough * (2.0f / PI) * asinf(sinf(x * 0.8f));    // jagged triangle ridge
  return h * r;
}
static inline float terrainSlope(uint8_t st, float x) { return (terrainH(st, x + 0.1f) - terrainH(st, x - 0.1f)) * 5.0f; }
static inline float ceilH(uint8_t st, float x) { return terrainH(st, x) + 5.4f + 0.5f * sinf(x * 0.31f); }

// ======================= VEHICLES =======================
struct Poly { uint8_t n; uint8_t col; float p[6][2]; };
struct Vehicle {
  const char* name;
  uint16_t price;
  float mass, fmax, vmax;
  float rr, rf;
  float arx, ary, afx, afy;
  float travel, freq, zeta, mu, fuel;
  uint8_t drive;                 // 0 RWD, 1 FWD, 2 AWD
  float airK;                    // air control multiplier
  float hx, hy, hr;              // head
  float sx, sy, ux, uy, fx, fy;  // seat, hands, foot
  float ex, ey;                  // exhaust (ey < -50: none)
  float arch;                    // wheel arch cutout (1 = none)
  uint8_t knob;                  // 0 smooth, 1 off-road, 2 chunky
  uint8_t nHull; float hull[8][2];
  uint8_t nPoly; Poly poly[6];
  uint8_t nLines; float lines[8][5];   // x0 y0 x1 y1 colour
};

static const Vehicle VEH[NV] = {
 { "HILL CLIMBER", 0, 1000, 5500, 24, 0.50f, 0.50f, -1.1f, -0.2f, 1.15f, -0.2f, 0.45f, 1.5f, 0.30f, 1.00f, 100, 0, 1.0f,
   -0.3f, 0.95f, 0.27f,  -0.3f, 0.3f, 0.45f, 0.6f, 0.35f, 0.2f,  -1.75f, 0.2f, 1.3f, 1,
   6, {{-1.7f,-0.35f},{1.75f,-0.35f},{1.75f,0.15f},{-1.7f,0.3f},{0.8f,0.85f},{0.0f,-0.35f}},
   1, {{5,1,{{-1.7f,-0.35f},{1.75f,-0.35f},{1.75f,0.15f},{0.55f,0.3f},{-1.7f,0.3f}}}},
   5, {{0.55f,0.3f,0.82f,0.88f,1},{0.82f,0.88f,0.95f,0.88f,1},{-0.75f,0.3f,-0.75f,-0.2f,0},{1.2f,-0.05f,1.6f,-0.05f,0},{-1.7f,0.05f,-1.3f,0.05f,0}} },

 { "MOTOCROSS", 1500, 240, 1900, 28, 0.43f, 0.43f, -0.75f, -0.1f, 0.85f, -0.1f, 0.30f, 2.2f, 0.35f, 1.15f, 80, 0, 1.4f,
   -0.1f, 1.3f, 0.24f,  -0.4f, 0.55f, 0.4f, 0.78f, 0.0f, 0.05f,  -1.0f, 0.28f, 1.0f, 0,
   6, {{-0.85f,0.0f},{0.9f,0.0f},{-0.95f,0.5f},{0.45f,0.8f},{0.0f,-0.18f},{-0.3f,0.62f}},
   2, {{4,1,{{-0.4f,-0.18f},{0.5f,-0.18f},{0.42f,0.32f},{-0.3f,0.36f}}},{4,1,{{-0.95f,0.42f},{0.35f,0.4f},{0.4f,0.62f},{-0.9f,0.5f}}}},
   6, {{0.85f,-0.1f,0.58f,0.72f,1},{0.58f,0.72f,0.35f,0.8f,1},{-0.35f,-0.05f,-0.75f,-0.1f,1},{-0.3f,0.0f,-1.0f,0.28f,1},{-0.4f,0.3f,-0.62f,-0.05f,1},{0.62f,0.5f,0.98f,0.42f,1}} },

 { "TRACTOR", 3500, 1600, 6400, 15, 0.80f, 0.45f, -1.0f, 0.0f, 1.35f, -0.35f, 0.30f, 1.2f, 0.40f, 1.20f, 120, 0, 0.8f,
   -0.6f, 1.25f, 0.27f,  -0.6f, 0.5f, -0.1f, 0.95f, 0.0f, 0.4f,  1.3f, 1.4f, 1.1f, 2,
   6, {{-1.35f,-0.25f},{1.85f,-0.25f},{1.85f,0.45f},{-1.25f,1.5f},{0.0f,1.5f},{0.0f,-0.25f}},
   4, {{5,1,{{-1.35f,-0.25f},{0.0f,-0.25f},{0.0f,0.55f},{-0.7f,0.65f},{-1.35f,0.45f}}},
       {4,1,{{0.0f,-0.25f},{1.85f,-0.25f},{1.85f,0.45f},{0.0f,0.55f}}},
       {4,1,{{-1.3f,0.5f},{0.05f,0.5f},{0.0f,1.5f},{-1.25f,1.5f}}},
       {4,0,{{-1.1f,0.7f},{-0.15f,0.7f},{-0.15f,1.35f},{-1.1f,1.35f}}}},
   3, {{1.3f,0.5f,1.3f,1.4f,1},{1.22f,1.4f,1.4f,1.4f,1},{1.85f,-0.1f,1.5f,-0.1f,0}} },

 { "MONSTER TRUCK", 6000, 2400, 11500, 21, 0.90f, 0.90f, -1.35f, 0.0f, 1.35f, 0.0f, 0.70f, 1.0f, 0.22f, 1.10f, 140, 2, 0.9f,
   0.15f, 0.78f, 0.26f,  0.1f, 0.55f, 0.6f, 0.72f, 0.5f, 0.5f,  -1.2f, 1.15f, 1.15f, 2,
   6, {{-1.95f,-0.2f},{1.95f,-0.2f},{2.0f,0.4f},{-0.5f,1.1f},{0.65f,1.1f},{-1.95f,0.5f}},
   3, {{4,1,{{-1.95f,-0.2f},{1.95f,-0.2f},{2.0f,0.4f},{-1.95f,0.5f}}},
       {4,1,{{-0.75f,0.5f},{0.95f,0.42f},{0.65f,1.1f},{-0.5f,1.1f}}},
       {4,0,{{-0.35f,0.62f},{0.75f,0.56f},{0.55f,0.98f},{-0.3f,0.98f}}}},
   4, {{-1.1f,0.5f,-1.1f,1.15f,1},{-1.3f,0.5f,-1.3f,1.15f,1},{2.0f,0.1f,2.25f,0.0f,1},{-1.0f,-0.2f,-1.0f,0.4f,0}} },

 { "DUNE BUGGY", 9000, 800, 5600, 27, 0.56f, 0.46f, -1.2f, -0.25f, 1.15f, -0.35f, 0.55f, 1.2f, 0.25f, 0.95f, 90, 0, 1.2f,
   -0.1f, 0.95f, 0.27f,  -0.2f, 0.12f, 0.3f, 0.5f, 0.4f, 0.05f,  -1.9f, 0.3f, 1.0f, 1,
   7, {{-1.5f,-0.32f},{1.6f,-0.32f},{-1.45f,0.1f},{-0.5f,1.2f},{0.35f,1.2f},{1.35f,0.0f},{-1.4f,0.5f}},
   2, {{4,1,{{-1.5f,-0.32f},{1.6f,-0.32f},{1.35f,0.0f},{-1.45f,0.1f}}},{4,1,{{-1.45f,0.1f},{-0.7f,0.1f},{-0.65f,0.6f},{-1.4f,0.55f}}}},
   6, {{-0.65f,0.1f,-0.5f,1.2f,1},{-0.5f,1.2f,0.35f,1.2f,1},{0.35f,1.2f,0.55f,0.0f,1},{-0.55f,0.7f,0.45f,0.7f,1},{1.35f,0.0f,1.75f,-0.12f,1},{-1.45f,0.45f,-1.9f,0.3f,1}} },

 { "RALLY CAR", 13000, 1100, 8800, 31, 0.40f, 0.40f, -1.2f, -0.1f, 1.25f, -0.1f, 0.30f, 1.9f, 0.38f, 1.25f, 90, 2, 1.1f,
   -0.15f, 0.65f, 0.25f,  -0.2f, 0.35f, 0.35f, 0.5f, 0.4f, 0.3f,  -1.75f, 0.0f, 1.3f, 1,
   8, {{-1.75f,-0.28f},{1.85f,-0.28f},{1.85f,0.12f},{-1.75f,0.32f},{-0.7f,0.88f},{0.3f,0.88f},{-2.0f,0.7f},{0.0f,-0.28f}},
   3, {{5,1,{{-1.75f,-0.28f},{1.85f,-0.28f},{1.85f,0.12f},{0.95f,0.3f},{-1.75f,0.32f}}},
       {4,1,{{-0.95f,0.3f},{0.75f,0.3f},{0.3f,0.88f},{-0.7f,0.88f}}},
       {4,0,{{-0.78f,0.38f},{0.6f,0.38f},{0.25f,0.8f},{-0.58f,0.8f}}}},
   6, {{-1.7f,0.32f,-1.7f,0.7f,1},{-2.0f,0.7f,-1.4f,0.7f,1},{-1.5f,0.0f,1.7f,0.0f,0},{-0.2f,0.3f,-0.2f,-0.2f,0},{1.8f,0.05f,1.92f,0.05f,1},{1.0f,-0.1f,1.5f,-0.1f,0}} },

 { "RACE CAR", 18000, 850, 8500, 38, 0.40f, 0.40f, -1.25f, -0.12f, 1.4f, -0.12f, 0.20f, 2.6f, 0.40f, 1.40f, 80, 2, 1.0f,
   -0.15f, 0.6f, 0.25f,  -0.2f, 0.38f, 0.5f, 0.45f, 0.8f, 0.15f,  -1.8f, 0.25f, 1.0f, 0,
   7, {{-1.8f,-0.22f},{2.0f,-0.22f},{1.65f,0.08f},{-1.8f,0.3f},{-1.65f,0.82f},{-0.5f,0.85f},{0.0f,-0.22f}},
   2, {{4,1,{{-1.8f,-0.22f},{2.0f,-0.22f},{1.65f,0.08f},{-1.8f,0.38f}}},{4,1,{{-0.4f,0.08f},{1.0f,0.08f},{0.7f,0.3f},{-0.6f,0.38f}}}},
   5, {{-1.65f,0.38f,-1.65f,0.82f,1},{-2.05f,0.82f,-1.25f,0.82f,1},{1.95f,-0.15f,2.35f,-0.15f,1},{-0.55f,0.38f,-0.5f,0.85f,1},{-1.6f,0.1f,1.5f,0.1f,0}} },

 { "BUS", 25000, 4000, 12000, 17, 0.60f, 0.60f, -1.5f, -0.6f, 1.5f, -0.6f, 0.35f, 1.0f, 0.28f, 1.00f, 160, 0, 0.6f,
   1.35f, 0.5f, 0.25f,  1.35f, -0.1f, 1.75f, 0.1f, 1.55f, -0.7f,  -2.25f, -0.5f, 1.2f, 0,
   6, {{-2.2f,-0.9f},{2.3f,-0.9f},{2.3f,0.0f},{1.9f,0.9f},{-2.2f,0.9f},{0.0f,-0.9f}},
   5, {{5,1,{{-2.2f,-0.9f},{2.3f,-0.9f},{2.3f,0.0f},{1.9f,0.9f},{-2.2f,0.9f}}},
       {4,0,{{-2.0f,0.2f},{-1.2f,0.2f},{-1.2f,0.68f},{-2.0f,0.68f}}},
       {4,0,{{-1.0f,0.2f},{-0.2f,0.2f},{-0.2f,0.68f},{-1.0f,0.68f}}},
       {4,0,{{0.0f,0.2f},{0.8f,0.2f},{0.8f,0.68f},{0.0f,0.68f}}},
       {4,0,{{1.0f,0.15f},{1.85f,0.15f},{1.75f,0.75f},{1.0f,0.75f}}}},
   3, {{-2.2f,-0.35f,2.3f,-0.35f,0},{-2.2f,-0.45f,2.3f,-0.45f,0},{2.3f,-0.6f,2.45f,-0.6f,1}} },
};

static const char* UPG_NAME[4] = {"ENGINE", "SUSPENS", "TIRES", "4WD"};
static const uint8_t UPG_MAX[4] = {5, 5, 5, 3};
static const uint16_t UPG_COST[4][5] = {
  {200, 500, 1000, 2000, 4000}, {200, 500, 1000, 2000, 4000},
  {200, 500, 1000, 2000, 4000}, {500, 1500, 3000, 0, 0},
};

// ======================= SAVE =======================
struct SaveData {
  uint32_t magic, coins;
  uint8_t veh, stage, vehOwned, stageOwned;
  uint8_t upg[NV][4];
  uint16_t best[NS];
  uint32_t chk;
};
static SaveData save, lastSaved;
static bool saveDirty = false;
static uint32_t lastSaveMs = 0;
#define SAVE_MAGIC 0x48435232u

static uint32_t fnv(const uint8_t* p, size_t n) { uint32_t h = 2166136261u; while (n--) { h ^= *p++; h *= 16777619u; } return h; }
static void saveDefaults() { memset(&save, 0, sizeof(save)); save.magic = SAVE_MAGIC; save.vehOwned = 1; save.stageOwned = 1; }
static void saveNow() {
  save.chk = fnv((const uint8_t*)&save, sizeof(SaveData) - sizeof(uint32_t));
  if (memcmp(&save, &lastSaved, sizeof(SaveData)) != 0) { prefs.putBytes("save2", &save, sizeof(SaveData)); lastSaved = save; }
  saveDirty = false; lastSaveMs = millis();
}
static void saveLater() { saveDirty = true; }
static void loadSave() {
  SaveData tmp; memset(&tmp, 0, sizeof(tmp));
  size_t n = prefs.getBytes("save2", &tmp, sizeof(SaveData));
  if (n == sizeof(SaveData) && tmp.magic == SAVE_MAGIC &&
      tmp.chk == fnv((const uint8_t*)&tmp, sizeof(SaveData) - sizeof(uint32_t)) && tmp.veh < NV && tmp.stage < NS) save = tmp;
  else saveDefaults();
  lastSaved = save;
}

// ======================= INPUT =======================
struct Pad {
  uint8_t pin; bool down = false; uint32_t tDown = 0, tRel = 0; uint8_t taps = 0; bool ev1 = false, ev2 = false;
  void update(uint32_t now) {
    bool s = (digitalRead(pin) == TOUCH_ACTIVE);
    ev1 = ev2 = false;
    if (s && !down) { down = true; tDown = now; }
    else if (!s && down) {
      down = false;
      if (now - tDown < TAP_MAX_MS) { taps++; tRel = now; if (taps >= 2) { ev2 = true; taps = 0; } }
      else taps = 0;
    }
    if (taps == 1 && !down && now - tRel > DOUBLE_GAP_MS) { ev1 = true; taps = 0; }
  }
};
static Pad L, R;

// ======================= GAME STATE =======================
enum State { S_TITLE, S_MAIN, S_VEH, S_STAGE, S_TUNE, S_PLAY, S_PAUSE, S_CRASH };
static State state = S_TITLE;
static int mainSel = 0, vehCur = 0, stCur = 0, tuneSel = 0, pauseSel = 0, crashSel = 0;

struct Wheel { float comp = 0, prev = 0, spinV = 0, ang = 0, cx = 0, cy = 0, vt = 0; bool contact = false, slip = false; };
struct Car {
  float x = 0, y = 0, th = 0, vx = 0, vy = 0, w = 0, px = 0, py = 0, pth = 0;
  Wheel wh[2];
  float m = 1, I = 1, fmax = 0, vmax = 1, travel = 0.3f, mu = 1, k = 0, cd = 0, fs = 0, airA = 5, thr = 0, airIn = 0, airMax = 3;
  float hdx = 0, hdy = 0, hvx = 0, hvy = 0;      // driver head offset (chassis frame)
};
static Car car;
struct Item { float x, y; uint8_t type; bool on; };   // 0 coin, 1 gold 100, 2 fuel
static Item items[MAX_ITEMS];
struct Part { float x, y, vx, vy, life; uint8_t t; };  // 0 dust 1 snow 2 smoke 3 spark 4 ember
static Part parts[MAX_PARTS];

static float camX = 0, camY = 0;
static int camPX = 0, camPY = 0;
static float fuel = 100, dist = 0, spawnX = 14, lastFuelX = 0, crashT = 0, idleT = 0;
static float airT = 0, rotAcc = 0, wheelieT = 0, popT = 0;
static uint32_t runCoins = 0, bothT = 0, lastUs = 0;
static float accT = 0;
static float renderAlpha = 1.0f, shakeAmp = 0.0f;
static bool crashed = false, newBest = false;
static uint8_t crashReason = 0;
static char popTxt[28];
static char toastTxt[24];
static uint32_t toastUntil = 0;

static void toast(const char* s) { strncpy(toastTxt, s, 23); toastTxt[23] = 0; toastUntil = millis() + 1100; }
static float effTravel(uint8_t v) { return VEH[v].travel * (1.0f + 0.06f * save.upg[v][1]); }
static inline int sxOf(float wx) { return (int)lroundf(wx * SC) - camPX + 64; }
static inline int syOf(float wy) { return 34 - ((int)lroundf(wy * SC) - camPY); }
static void setCam() {
  camPX = (int)floorf(camX * SC); camPY = (int)floorf(camY * SC);
  if (shakeAmp > 0.05f && state == S_PLAY) { camPX += (int)lroundf(frand() * shakeAmp); camPY += (int)lroundf(frand() * shakeAmp); }
}

static void emit(float x, float y, float vx, float vy, float life, uint8_t t) {
  static uint8_t idx = 0;
  for (int n = 0; n < MAX_PARTS; n++) {
    int i = (idx + n) % MAX_PARTS;
    if (parts[i].life <= 0.0f) { parts[i] = Part{x, y, vx, vy, life, t}; idx = (i + 1) % MAX_PARTS; return; }
  }
}
static void updateParts(float dt) {
  for (int i = 0; i < MAX_PARTS; i++) {
    Part& p = parts[i];
    if (p.life <= 0.0f) continue;
    p.life -= dt;
    p.x += p.vx * dt; p.y += p.vy * dt;
    if (p.t == 3) p.vy -= 9.0f * dt;
    else if (p.t == 2) { p.vy += 0.6f * dt; p.vx *= (1.0f - 1.5f * dt); }
    else if (p.t == 4) { p.vx += frand() * 2.0f * dt; }
    else p.vy -= 1.2f * dt;
  }
}
static void drawParts() {
  u8g2.setDrawColor(1);
  for (int i = 0; i < MAX_PARTS; i++) {
    const Part& p = parts[i];
    if (p.life <= 0.0f) continue;
    int sx = sxOf(p.x), sy = syOf(p.y);
    if (sx < 1 || sx > 124 || sy < 9 || sy > 62) continue;
    if (p.t == 2) { u8g2.drawPixel(sx, sy); if (p.life > 0.4f) { u8g2.drawPixel(sx + 1, sy); u8g2.drawPixel(sx, sy - 1); } }
    else if (p.t == 3) { u8g2.drawLine(sx, sy, sx + (int)(p.vx * 0.3f), sy - (int)(p.vy * 0.3f)); }
    else if (p.t == 4) { if ((i + (int)(millis() / 90)) & 1) u8g2.drawPixel(sx, sy); }
    else { u8g2.drawPixel(sx, sy); if (p.life > 0.35f && p.t == 0) u8g2.drawPixel(sx + 1, sy); }
  }
}

static void award(int c, const char* txt) {
  save.coins += c; runCoins += c; saveLater();
  snprintf(popTxt, sizeof(popTxt), "%s +%d", txt, c);
  popT = 1.6f;
}

static void setupCar() {
  const Vehicle& V = VEH[save.veh];
  const uint8_t* u = save.upg[save.veh];
  float e = u[0], s = u[1], t = u[2], w = u[3];
  car.m = V.mass;
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
  for (int i = 0; i < V.nHull; i++) {
    x0 = fminf(x0, V.hull[i][0]); x1 = fmaxf(x1, V.hull[i][0]);
    y0 = fminf(y0, V.hull[i][1]); y1 = fmaxf(y1, V.hull[i][1]);
  }
  y1 = fmaxf(y1, V.hy + V.hr);
  float bw = x1 - x0, bh = y1 - y0 + 0.4f;
  car.I = car.m * (bw * bw + bh * bh) / 12.0f * 1.3f * (1.0f + 0.5f * (V.airK - 1.0f));
  car.fmax = V.fmax * (1.0f + 0.14f * e) * (1.0f + 0.05f * w);
  car.vmax = V.vmax * (1.0f + 0.035f * e);
  car.travel = effTravel(save.veh);
  car.mu = V.mu * (1.0f + 0.07f * t);
  float om = 2.0f * PI * V.freq;
  car.k = (car.m * 0.5f) * om * om;
  float zeta = V.zeta + 0.04f * s;
  car.cd = 2.0f * zeta * sqrtf(car.k * car.m * 0.5f);
  if (V.drive == 0) car.fs = 0.17f * w; else if (V.drive == 1) car.fs = 1.0f - 0.17f * w; else car.fs = 0.5f;
  car.airA = (2.5f + 200.0f / car.m) * V.airK;
  car.airMax = 3.0f * sqrtf(V.airK);
}

// ======================= ITEMS =======================
static void addItem(float x, uint8_t type, float yoff) {
  for (int i = 0; i < MAX_ITEMS; i++)
    if (!items[i].on) { items[i].on = true; items[i].x = x; items[i].type = type; items[i].y = terrainH(save.stage, x) + yoff; return; }
}
static void spawnItems() {
  while (spawnX < car.x + 70.0f) {
    if (spawnX - lastFuelX > 180.0f + (float)(rnd() % 60)) { addItem(spawnX, 2, 1.0f); lastFuelX = spawnX; spawnX += 12.0f; continue; }
    uint32_t r = rnd() % 100;
    if (r < 38) { int n = 3 + rnd() % 6; for (int i = 0; i < n; i++) addItem(spawnX + i * 1.7f, 0, 1.1f); spawnX += n * 1.7f + 8.0f; }
    else if (r < 44) { addItem(spawnX, 1, 1.3f); spawnX += 10.0f; }
    else spawnX += 6.0f + (float)(rnd() % 10);
  }
  for (int i = 0; i < MAX_ITEMS; i++) if (items[i].on && items[i].x < car.x - 30.0f) items[i].on = false;
}
static void collectItems() {
  for (int i = 0; i < MAX_ITEMS; i++) {
    if (!items[i].on) continue;
    float dx = items[i].x - car.x, dy = items[i].y - car.y;
    if (dx * dx + dy * dy < 2.6f) {
      items[i].on = false;
      if (items[i].type == 2) fuel = VEH[save.veh].fuel;
      else { uint32_t v = (items[i].type == 1) ? 100 : 10; save.coins += v; runCoins += v; saveLater(); }
    }
  }
}

// ======================= RUN CONTROL =======================
static void resetTiming() { lastUs = micros(); accT = 0; }
static void startRun() {
  setupCar();
  const Vehicle& V = VEH[save.veh];
  float om0 = 2.0f * PI * V.freq, sc0 = fminf(car.travel, STAGES[save.stage].g / (om0 * om0));   // spawn at static ride height
  float bR = -(V.ary - (car.travel - sc0) - V.rr), bF = -(V.afy - (car.travel - sc0) - V.rf);
  car.x = 2.0f; car.y = terrainH(save.stage, 2.0f) + fmaxf(bR, bF) + 0.03f; car.th = 0; car.vx = car.vy = car.w = 0;
  car.thr = 0; car.airIn = 0; car.hdx = car.hdy = car.hvx = car.hvy = 0;
  car.px = car.x; car.py = car.y; car.pth = car.th; shakeAmp = 0; renderAlpha = 1.0f;
  for (int i = 0; i < 2; i++) car.wh[i] = Wheel();
  fuel = V.fuel; dist = 0; runCoins = 0; crashed = false; newBest = false;
  crashT = 0; idleT = 0; bothT = 0; airT = 0; rotAcc = 0; wheelieT = 0; popT = 0;
  for (int i = 0; i < MAX_ITEMS; i++) items[i].on = false;
  for (int i = 0; i < MAX_PARTS; i++) parts[i].life = 0;
  spawnX = 14.0f; lastFuelX = 0.0f;
  rngState = esp_random() | 1u;
  camX = car.x + 3.0f; camY = car.y;
  state = S_PLAY; resetTiming();
}
static void endRun() {
  int d = (int)dist;
  if (d > save.best[save.stage]) { save.best[save.stage] = (uint16_t)min(d, 65000); newBest = true; }
  saveNow(); crashSel = 0; state = S_CRASH;
}

// ======================= PHYSICS =======================
static void physicsStep(float dt, bool gas, bool brake) {
  const Vehicle& V = VEH[save.veh];
  const uint8_t st = save.stage;
  const Stage& S = STAGES[st];
  car.px = car.x; car.py = car.y; car.pth = car.th;
  const float c = cosf(car.th), s = sinf(car.th);
  float Fx = 0, Fy = -car.m * S.g, Tq = 0;
  auto applyF = [&](float px, float py, float fx, float fy) { Fx += fx; Fy += fy; Tq += (px - car.x) * fy - (py - car.y) * fx; };

  if (crashed) { gas = false; brake = false; }
  bool canGas = gas && fuel > 0.0f;
  car.thr += ((canGas ? 1.0f : 0.0f) - car.thr) * fminf(1.0f, dt * 5.0f);
  float vFwd = car.vx * c + car.vy * s;
  float thw = remainderf(car.th, 2.0f * PI);
  float pitchCut = (thw > 0.5f) ? constrain(1.0f - (thw - 0.5f) / 0.5f, 0.0f, 1.0f) : 1.0f;   // ease off when the nose gets too high
  float gScale = 0.45f + 0.55f * fminf(1.0f, S.g / 9.81f);                                         // less torque in low gravity
  float drive = car.fmax * car.thr * pitchCut * gScale * (1.0f - vFwd / car.vmax);
  if (drive < 0) drive = 0;
  if (drive > car.fmax) drive = car.fmax;
  int grounded = 0;

  for (int i = 0; i < 2; i++) {
    float lx = i ? V.afx : V.arx, ly = i ? V.afy : V.ary, r = i ? V.rf : V.rr;
    float awx = car.x + lx * c - ly * s, awy = car.y + lx * s + ly * c;
    float wx = awx + s * car.travel, wy = awy - c * car.travel;
    float sl = terrainSlope(st, wx);
    float hh = terrainH(st, wx);
    float inv = 1.0f / sqrtf(1.0f + sl * sl);
    float nx = -sl * inv, ny = inv, tx = inv, ty = sl * inv;
    float pen = r - (wy - hh) * ny;
    Wheel& W = car.wh[i];
    float share = i ? car.fs : (1.0f - car.fs);
    if (pen > 0.0f) {
      float al = -s * nx + c * ny; if (al < 0.4f) al = 0.4f;
      float comp = pen / al, extra = 0;
      if (comp > car.travel) { comp = car.travel; extra = pen - comp * al; if (extra < 0) extra = 0; }
      if (!W.contact) { W.prev = comp; if (car.vy < -3.5f && !crashed) shakeAmp = fmaxf(shakeAmp, fminf(2.5f, -car.vy * 0.18f)); }
      float cdot = (comp - W.prev) / dt; W.prev = comp;
      float fsN = car.k * comp + car.cd * cdot + car.k * 8.0f * extra;
      if (fsN < 0) fsN = 0;
      applyF(awx, awy, nx * fsN, ny * fsN);                      // wheel pushes along the ground normal

      float pvx = car.vx - car.w * (awy - car.y), pvy = car.vy + car.w * (awx - car.x);
      float vt = pvx * tx + pvy * ty;
      float Ftmax = car.mu * S.grip * (fsN + 0.25f * car.m * (S.g + 3.0f));
      float ft;
      if (brake) {
        if (vt > 0.5f) ft = -fminf(vt * car.m * 2.0f, Ftmax * 0.7f);
        else ft = -0.15f * car.fmax * fmaxf(0.0f, 1.0f + fminf(0.0f, vFwd) / 6.0f);
      } else if (car.thr > 0.02f) ft = drive * share;
      else { float Fr = 0.10f * fsN; ft = -constrain(vt * car.m * 0.5f, -Fr, Fr); }
      ft = constrain(ft, -Ftmax, Ftmax);
      float wcx = awx + s * (car.travel - comp), wcy = awy - c * (car.travel - comp);
      applyF(wcx, wcy, tx * ft, ty * ft);        // traction acts at the wheel => real squat / wheelie torque

      float spin = vt / r;
      bool slip = (car.thr > 0.5f && !brake && ft >= Ftmax * 0.97f);
      if (slip) spin = fmaxf(spin, 14.0f);
      if (brake) spin *= 0.15f;
      W.spinV = spin; W.slip = slip; W.vt = vt;
      W.contact = true; W.comp = comp; grounded++;
      W.cx = wcx - nx * r; W.cy = wcy - ny * r;
    } else {
      W.contact = false; W.slip = false;
      W.comp -= 2.5f * dt; if (W.comp < 0) W.comp = 0;
      float target = canGas ? 30.0f : (brake ? 0.0f : W.spinV);
      W.spinV += (target - W.spinV) * fminf(1.0f, dt * 3.0f);
    }
    W.ang += W.spinV * dt;
  }

  // hull vs terrain (also roof contact = crash)
  float kc = car.m * 1500.0f, cc = car.m * 55.0f;
  for (int i = 0; i < V.nHull; i++) {
    float lx = V.hull[i][0], ly = V.hull[i][1];
    float px = car.x + lx * c - ly * s, py = car.y + lx * s + ly * c;
    float pen = terrainH(st, px) - py;
    if (pen > 0.0f) {
      float sl = terrainSlope(st, px);
      float inv = 1.0f / sqrtf(1.0f + sl * sl);
      float nx = -sl * inv, ny = inv, tx = inv, ty = sl * inv;
      float pvx = car.vx - car.w * (py - car.y), pvy = car.vy + car.w * (px - car.x);
      float vn = pvx * nx + pvy * ny, vt = pvx * tx + pvy * ty;
      float Fn = kc * pen - cc * vn; if (Fn < 0) Fn = 0;
      float Ff = -constrain(vt * car.m * 3.0f, -0.6f * Fn, 0.6f * Fn);
      applyF(px, py, nx * Fn + tx * Ff, ny * Fn + ty * Ff);
      if (fabsf(vt) > 2.5f && (rnd() % 6) == 0) emit(px, py, frand() * 2.0f, 1.5f + fabsf(frand()) * 2.0f, 0.35f, 3);
      if (!crashed && pen > 0.02f && ly > V.hy * 0.55f) { crashed = true; crashReason = 0; crashT = 0; }
    }
  }

  { float spd = sqrtf(car.vx * car.vx + car.vy * car.vy); Fx -= car.m * 0.0032f * car.vx * spd; Fy -= car.m * 0.0032f * car.vy * spd; }   // air drag
  // pilot input: smooth ramp -> speed-limited air lean / ground pitch authority
  float inp = (canGas ? 1.0f : 0.0f) - (brake ? 1.0f : 0.0f);
  if (crashed) inp = 0.0f;
  car.airIn += (inp - car.airIn) * fminf(1.0f, dt * 5.0f);
  if (grounded == 0) {
    if ((car.airIn > 0.0f && car.w < car.airMax) || (car.airIn < 0.0f && car.w > -car.airMax)) car.w += car.airIn * car.airA * dt;
    car.w *= (1.0f - 0.10f * dt);
  } else {
    Tq += car.airIn * ((grounded == 1) ? 2.0f : 0.35f) * car.I;
    car.w *= (1.0f - 1.0f * dt);
  }

  // driver head: spring-damper driven by felt acceleration
  {
    float cax = Fx / car.m, cay = Fy / car.m + S.g;
    float lax = cax * c + cay * s, lay = -cax * s + cay * c;
    float gain = 0.6f * (1.0f + 0.4f * (V.airK - 1.0f));
    float hk = crashed ? 15.0f : 110.0f, hc = crashed ? 2.0f : 10.5f;
    float ax = -gain * lax - hk * car.hdx - hc * car.hvx;
    float ay = -gain * (lay - S.g) - hk * car.hdy - hc * car.hvy;
    car.hvx += ax * dt; car.hvy += ay * dt;
    car.hdx += car.hvx * dt; car.hdy += car.hvy * dt;
    float lim = crashed ? 0.9f : 0.32f;
    car.hdx = constrain(car.hdx, -lim, lim); car.hdy = constrain(car.hdy, -lim * 0.8f, lim * 0.8f);
  }

  car.vx += Fx / car.m * dt; car.vy += Fy / car.m * dt; car.w += Tq / car.I * dt;
  car.w *= (1.0f - 0.15f * dt);
  car.vx = constrain(car.vx, -90.0f, 90.0f); car.vy = constrain(car.vy, -90.0f, 90.0f); car.w = constrain(car.w, -8.0f, 8.0f);
  car.x += car.vx * dt; car.y += car.vy * dt; car.th += car.w * dt;

  if (st == 3) {   // cave ceiling
    float ce = ceilH(st, car.x);
    if (car.y + 1.35f > ce) { car.y = ce - 1.35f; if (car.vy > 0) car.vy *= -0.3f; }
  }

  if (!crashed) {
    fuel -= dt * (canGas ? 3.2f : 0.6f); if (fuel < 0) fuel = 0;
    float cc2 = cosf(car.th), ss2 = sinf(car.th);
    float hl = V.hx + car.hdx, hv = V.hy + car.hdy;
    float hxw = car.x + hl * cc2 - hv * ss2, hyw = car.y + hl * ss2 + hv * cc2;
    if (hyw - terrainH(st, hxw) < V.hr) { crashed = true; crashReason = 0; crashT = 0; }
    if (fuel <= 0.0f) {
      if (fabsf(car.vx) + fabsf(car.vy) < 0.5f) idleT += dt; else idleT = 0;
      if (idleT > 1.6f) { crashed = true; crashReason = 1; crashT = 0; }
    }
    // stunts
    if (grounded == 0) { airT += dt; rotAcc += car.w * dt; }
    else {
      if (airT > 0.35f && !crashed) {
        int flips = (int)((fabsf(rotAcc) + 1.2f) / (2.0f * PI));
        char b[24];
        if (flips > 0) {
          if (flips > 1) snprintf(b, sizeof(b), "%dx %s", flips, rotAcc > 0 ? "BACKFLIP" : "FRONTFLIP");
          else snprintf(b, sizeof(b), "%s", rotAcc > 0 ? "BACKFLIP" : "FRONTFLIP");
          award(100 * flips, b);
        } else if (airT > 1.3f) award(40, "AIR TIME");
      }
      airT = 0; rotAcc = 0;
    }
    if (grounded == 1 && car.wh[0].contact && car.th > 0.35f) wheelieT += dt;
    else { if (wheelieT > 1.5f && !crashed) award(min(100, (int)(wheelieT * 12.0f)), "WHEELIE"); wheelieT = 0; }
  } else crashT += dt;

  if (car.x - 2.0f > dist) dist = car.x - 2.0f;
}

// ======================= TEXTURES =======================
static const uint8_t BAY[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
static inline bool gray(int wx, int wy, int lvl) { return BAY[wy & 3][wx & 3] < lvl; }

static bool texPix(uint8_t st, int wx, int wy, int d) {
  switch (st) {
    case 0: {                                           // countryside: grass over soil and stones
      if (d <= 1) return true;
      if (d < 6) return gray(wx, wy, 4);
      int lv = 5 + (d - 6) / 7; if (lv > 10) lv = 10;
      bool on = gray(wx, wy, lv);
      int lx = wx & 7, ly = wy & 7;
      if (lx >= 2 && lx <= 5 && ly >= 2 && ly <= 4 && hash32((uint32_t)(fdiv(wx, 8) * 131 + fdiv(wy, 8) * 977)) % 9 == 0) on = true;
      return on;
    }
    case 1: {                                           // desert: sand strata
      if (d <= 0) return true;
      if (d < 4) return gray(wx, wy, 2);
      bool on = gray(wx, wy, d > 30 ? 5 : 3);
      if (fmodp(wy + fmodp(fdiv(wx, 32), 4), 11) == 0 && (wx & 7) < 6) on = true;
      int lx = wx & 7, ly = wy & 7;
      if (lx >= 2 && lx <= 4 && ly >= 3 && ly <= 4 && hash32((uint32_t)(fdiv(wx, 8) * 71 + fdiv(wy, 8) * 313)) % 19 == 0) on = true;
      return on;
    }
    case 2: {                                           // arctic: thick snow, ice glints
      if (d <= 2) return true;
      if (d < 7) return gray(wx, wy, 13);
      bool on = gray(wx, wy, d > 25 ? 7 : 4);
      if (fmodp(wx + wy * 2, 13) == 0) on = true;
      if (d > 9 && fmodp(wx * 2 - wy, 17) == 0) on = !on;
      return on;
    }
    case 3: {                                           // cave: dark cracked rock
      if (d <= 0) return true;
      bool on = gray(wx, wy, d > 14 ? 10 : 7);
      if (fmodp(wx * 3 + wy * 5, 23) == 0) on = false;
      int lx = fmodp(wx, 6), ly = fmodp(wy, 6);
      if (lx >= 1 && lx <= 3 && ly >= 2 && ly <= 3 && hash32((uint32_t)(fdiv(wx, 6) * 53 + fdiv(wy, 6) * 211)) % 13 == 0) on = true;
      return on;
    }
    case 4: {                                           // city: roof edge, shadow, brick, windows
      if (d <= 1) return true;
      if (d <= 3) return false;
      int lx = fmodp(wx, 10), ly = fmodp(wy, 12);
      if (lx >= 2 && lx <= 7 && ly >= 3 && ly <= 8) {
        if (lx == 2 || lx == 7 || ly == 3 || ly == 8) return true;
        return (hash32((uint32_t)(fdiv(wx, 10) * 31 + fdiv(wy, 12) * 977)) % 3) != 0;
      }
      return gray(wx, wy, 3);
    }
    case 5: {                                           // moon: grey regolith
      if (d <= 0) return true;
      bool on = gray(wx, wy, d < 6 ? 3 : (d < 24 ? 6 : 8));
      if ((wx & 3) == 1 && (wy & 3) == 1 && hash32((uint32_t)(fdiv(wx, 4) * 73 + fdiv(wy, 4) * 19)) % 11 == 0) on = true;
      return on;
    }
    case 6: {                                           // mars: rocky strata and boulders
      if (d <= 1) return true;
      bool on = gray(wx, wy, d < 8 ? 5 : (d < 28 ? 7 : 10));
      if (fmodp(wx + wy * 2, 9) == 0) on = true;
      int lx = wx & 7, ly = wy & 7;
      if (lx >= 1 && lx <= 5 && ly >= 2 && ly <= 4 && hash32((uint32_t)(fdiv(wx, 8) * 41 + fdiv(wy, 8) * 593)) % 8 == 0) on = true;
      return on;
    }
    default: {                                          // volcano: dark rock with lava veins
      if (d <= 0) return true;
      bool on = gray(wx, wy, d < 8 ? 5 : 2);
      if (fmodp(wx * 2 + wy * 3, 29) < 2) on = true;
      if (fmodp(wx * 3 - wy * 2, 37) < 1) on = true;
      return on;
    }
  }
}

// per-column height cache: only newly scrolled-in columns cost a terrainH() call
static int32_t colTag[256];
static int16_t colHP[256], ceilHP[256];
static uint8_t colSt = 255;
static inline int terrainHP(uint8_t st, int wxp) {
  if (colSt != st) { for (int k = 0; k < 256; k++) colTag[k] = 0x7fffffff; colSt = st; }
  int i = wxp & 255;
  if (colTag[i] != wxp) {
    colTag[i] = wxp;
    float x = wxp * (1.0f / SC);
    colHP[i] = (int16_t)lroundf(terrainH(st, x) * SC);
    if (st == 3) ceilHP[i] = (int16_t)lroundf(ceilH(st, x) * SC);
  }
  return colHP[i];
}

static void drawTerrain(uint8_t st) {
  uint8_t* buf = u8g2.getBufferPtr();
  int prevY = -999;
  for (int px = 0; px < 128; px++) {
    int wxp = px - 64 + camPX;
    int y = 34 - (terrainHP(st, wxp) - camPY);
    if (y > 63) { prevY = 64; continue; }
    if (y < 0) y = 0;
    if (prevY == -999) prevY = y;
    int top = min(y, prevY);
    int fullEnd = (abs(y - prevY) >= 2) ? max(y, prevY) : y;
    for (int yy = top; yy < 64; yy++) {
      int d = yy - y; if (d < 0) d = 0;
      bool on = (yy <= fullEnd) ? true : texPix(st, wxp, camPY + 34 - yy, d);
      if (on) buf[(yy >> 3) * 128 + px] |= (uint8_t)(1 << (yy & 7));
    }
    if (st == 0 && y > 9 && (((uint32_t)wxp * 2654435761u) >> 29) < 3) buf[((y - 1) >> 3) * 128 + px] |= (uint8_t)(1 << ((y - 1) & 7));   // grass blades
    prevY = y;
  }
}

static void drawCeiling(uint8_t st) {
  uint8_t* buf = u8g2.getBufferPtr();
  for (int px = 0; px < 128; px++) {
    int wxp = px - 64 + camPX;
    terrainHP(st, wxp);
    int yc = 34 - (ceilHP[wxp & 255] - camPY);
    int cell = fdiv(wxp, 24), u = fmodp(wxp, 24);
    int len = 4 + (int)(hash32((uint32_t)cell) % 9), depth = 0;
    int dd = abs(u - 5);
    if (u < 10 && dd < 5) depth = len * (5 - dd) / 5;
    int yb = yc + depth;
    if (yb > 63) yb = 63;
    for (int yy = 0; yy <= yb; yy++) {
      bool on = (yy == yb) ? true : gray(wxp, camPY + 34 - yy, 7 + (yb - yy < 3 ? 3 : 0));
      if (on) buf[(yy >> 3) * 128 + px] |= (uint8_t)(1 << (yy & 7));
    }
  }
}

// ======================= SCENERY =======================
static void ridge(float par, float freq, float base, float amp, bool solid) {
  int prev = -1000;
  for (int px = 0; px < 128; px += 2) {
    float u = (px + camPX * par) * freq;
    int y = (int)(base - amp * sinf(u) - 0.5f * amp * sinf(u * 2.3f + 1.0f) - camPY * 0.1f);
    y = constrain(y, 9, 63);
    if (solid && prev > -1000) u8g2.drawLine(px - 2, prev, px, y); else u8g2.drawPixel(px, y);
    prev = y;
  }
}
static void mountain(int cx, int halfW, int topY, int baseY) {
  for (int px = 0; px < 128; px++) {
    int d = abs(px - cx);
    if (d > halfW) continue;
    int y = baseY - (baseY - topY) * (halfW - d) / halfW - (int)(camPY * 0.1f);
    if (y >= 9 && y < 64) u8g2.drawPixel(px, y);
  }
}
static void drawBackground(uint8_t st) {
  u8g2.setDrawColor(1);
  float t = millis() * 0.001f;
  switch (st) {
    case 0:
      u8g2.drawCircle(104, 17, 5);
      for (int k = 0; k < 8; k++) { float a = k * 0.785f; u8g2.drawPixel(104 + (int)(8 * cosf(a)), 17 + (int)(8 * sinf(a))); }
      for (int i = 0; i < 3; i++) {
        int x = wrapi(i * 70 - camPX * 0.08f, 180) - 25, y = 14 + i * 6;
        if (x > -14 && x < 108) { u8g2.drawHLine(x, y, 14); u8g2.drawHLine(x + 3, y - 1, 8); u8g2.drawHLine(x + 5, y - 2, 4); }
      }
      ridge(0.15f, 0.045f, 36, 5, false); ridge(0.3f, 0.07f, 42, 3, false);
      break;
    case 1:
      u8g2.drawDisc(100, 18, 6); u8g2.drawCircle(100, 18, 9);
      mountain(wrapi(150 - camPX * 0.12f, 260) - 40, 22, 28, 44);
      ridge(0.25f, 0.03f, 38, 6, false);
      break;
    case 2:
      for (int i = 0; i < 12; i++) u8g2.drawPixel((i * 53) % 128, 10 + (i * 29) % 18);
      for (int px = 0; px < 128; px += 3) u8g2.drawPixel(px, (int)(15 + 4 * sinf((px + camPX * 0.05f) * 0.08f + t * 0.5f)));
      for (int px = 0; px < 128; px += 2) {
        float u = (px + camPX * 0.15f) * 0.09f;
        int y = (int)(44.0f - fabsf(fmodf(u, 2.0f) - 1.0f) * 18.0f - camPY * 0.1f);
        if (y >= 9 && y < 64) u8g2.drawPixel(px, y);
      }
      for (int i = 0; i < 24; i++) {
        int x = (int)(i * 37 + t * 8.0f + 3.0f * sinf(t + i)) % 128, y = 9 + (int)(i * 23 + t * 22.0f) % 54;
        u8g2.drawPixel(x, y);
      }
      break;
    case 3:
      for (int i = 0; i < 10; i++) {
        int x = wrapi(i * 41 - camPX * 0.2f, 128), y = 14 + (i * 17) % 30;
        if (((millis() / 200) + i) % 5 != 0) u8g2.drawPixel(x, y);
      }
      break;
    case 4: {
      u8g2.drawCircle(104, 16, 4);
      for (int i = 0; i < 10; i++) u8g2.drawPixel((i * 61) % 128, 10 + (i * 13) % 14);
      int off = (int)(camPX * 0.2f), base = fdiv(off, 16);
      for (int k = 0; k < 10; k++) {
        int idx = base + k, x = idx * 16 - off + 64 - 64;
        int h = 10 + (int)(hash32((uint32_t)idx) % 22);
        int y = (int)(46 - camPY * 0.1f) - h;
        if (x < -12 || x > 110 || y < 9) continue;
        u8g2.drawFrame(x, y, 12, h);
        uint32_t hh = hash32((uint32_t)idx * 7u);
        for (int w = 0; w < 4; w++) if ((hh >> w) & 1) u8g2.drawPixel(x + 3 + (w & 1) * 4, y + 4 + (w >> 1) * 5);
      }
      break;
    }
    case 5:
      for (int i = 0; i < 18; i++) u8g2.drawPixel(wrapi(i * 53 - camPX * 0.04f, 128), 10 + (i * 29) % 28);
      u8g2.drawCircle(100, 17, 6); u8g2.drawDisc(98, 15, 2);
      ridge(0.2f, 0.05f, 38, 5, false); ridge(0.35f, 0.09f, 43, 3, false);
      break;
    case 6:
      for (int i = 0; i < 10; i++) u8g2.drawPixel(wrapi(i * 71 - camPX * 0.04f, 128), 10 + (i * 31) % 24);
      u8g2.drawDisc(92, 14, 2); u8g2.drawDisc(112, 22, 1);
      ridge(0.15f, 0.035f, 34, 7, false); ridge(0.3f, 0.06f, 41, 4, false);
      break;
    default: {
      int cx = wrapi(130 - camPX * 0.1f, 260) - 50;
      mountain(cx, 42, 26, 52);
      for (int i = 0; i < 10; i++) {
        int y = 24 - i * 2; if (y < 9) break;
        int x = cx + (int)(3.0f * sinf(t * 1.5f + i * 0.7f)) + i / 2;
        if (x >= 0 && x < 128) u8g2.drawPixel(x, y);
      }
      for (int i = 0; i < 14; i++) {
        int x = (i * 47 + (int)(t * 6.0f)) % 128, y = 62 - ((int)(t * 14.0f) + i * 9) % 50;
        u8g2.drawPixel(x, y);
      }
      break;
    }
  }
}

static void drawDecor(uint8_t st) {
  u8g2.setDrawColor(1);
  float x0 = (camPX - 64) / SC, x1 = (camPX + 64) / SC;
  int i0 = (int)floorf(x0 / 6.0f) - 2, i1 = (int)floorf(x1 / 6.0f) + 2;
  for (int i = i0; i <= i1; i++) {
    uint32_t h = hash32((uint32_t)(i * 7919 + st * 131));
    float x = i * 6.0f + (h & 7) * 0.4f;
    if (x < 12.0f) continue;
    if (fabsf(terrainSlope(st, x)) > 0.45f) continue;
    int sx = sxOf(x), sy = syOf(terrainH(st, x));
    if (sx < -14 || sx > 118 || sy < 12 || sy > 70) continue;
    int type = (h >> 8) % 6;
    switch (st) {
      case 0:
        if (type < 2) { u8g2.drawVLine(sx, sy - 8, 8); u8g2.drawCircle(sx, sy - 11, 4); u8g2.drawCircle(sx, sy - 11, 2); }
        else if (type == 2) u8g2.drawDisc(sx, sy - 2, 3);
        break;
      case 1:
        if (type < 2) { u8g2.drawVLine(sx, sy - 9, 9); u8g2.drawHLine(sx - 3, sy - 6, 3); u8g2.drawVLine(sx - 3, sy - 8, 3); u8g2.drawHLine(sx, sy - 4, 3); u8g2.drawVLine(sx + 3, sy - 6, 3); }
        else if (type == 2) u8g2.drawDisc(sx, sy - 1, 2);
        break;
      case 2:
        if (type < 3) { u8g2.drawVLine(sx, sy - 3, 3); u8g2.drawTriangle(sx - 4, sy - 3, sx + 4, sy - 3, sx, sy - 10); u8g2.drawTriangle(sx - 3, sy - 8, sx + 3, sy - 8, sx, sy - 14); }
        break;
      case 3:
        if (type < 3) u8g2.drawTriangle(sx - 3, sy, sx + 3, sy, sx, sy - 6 - type * 2);
        else if (type == 3) { u8g2.drawLine(sx, sy, sx + 2, sy - 5); u8g2.drawLine(sx + 2, sy - 5, sx + 4, sy); u8g2.drawPixel(sx + 2, sy - 2); }
        break;
      case 4:
        if (type == 0) { u8g2.drawVLine(sx, sy - 12, 12); if ((millis() / 400) & 1) u8g2.drawPixel(sx, sy - 13); }
        else if (type == 1) { u8g2.drawFrame(sx - 4, sy - 4, 8, 4); u8g2.drawPixel(sx, sy - 2); }
        else if (type == 2) { u8g2.drawFrame(sx - 8, sy - 14, 16, 8); u8g2.drawVLine(sx - 5, sy - 6, 6); u8g2.drawVLine(sx + 5, sy - 6, 6); u8g2.drawHLine(sx - 5, sy - 10, 10); }
        break;
      case 5:
        if (type < 2) u8g2.drawDisc(sx, sy - 1, 2);
        else if (type == 2) { u8g2.drawVLine(sx, sy - 10, 10); u8g2.drawBox(sx + 1, sy - 10, 5, 3); }
        break;
      case 6:
        if (type < 2) u8g2.drawTriangle(sx - 3, sy, sx + 3, sy, sx, sy - 9 - type * 3);
        else if (type == 2) u8g2.drawDisc(sx, sy - 2, 3);
        break;
      default:
        if (type < 3) u8g2.drawTriangle(sx - 3, sy, sx + 3, sy, sx, sy - 10);
        else if (type == 3) { int f = (int)((millis() / 80 + i) % 5); u8g2.drawVLine(sx, sy - 3 - f, 3 + f); u8g2.drawPixel(sx - 1, sy - 2); u8g2.drawPixel(sx + 1, sy - 2); }
        break;
    }
  }
  // distance signs every 100 m
  int m0 = ((int)floorf(x0 / 100.0f)) * 100;
  for (int m = m0; m <= (int)x1 + 100; m += 100) {
    if (m < 100) continue;
    int sx = sxOf((float)m), sy = syOf(terrainH(st, (float)m));
    if (sx < -14 || sx > 118 || sy < 24 || sy > 70) continue;
    char b[12]; snprintf(b, sizeof(b), "%d", m);
    u8g2.drawVLine(sx, sy - 14, 14);
    u8g2.setDrawColor(0); u8g2.drawBox(sx - 9, sy - 23, 19, 9);
    u8g2.setDrawColor(1); u8g2.drawFrame(sx - 9, sy - 23, 19, 9);
    u8g2.setFont(u8g2_font_4x6_tr); u8g2.drawStr(sx - 1 - u8g2.getStrWidth(b) / 2, sy - 16, b);
  }
}

static void drawScenery(uint8_t st) {
  drawBackground(st);
  if (st == 3) drawCeiling(st);
  drawTerrain(st);
  drawDecor(st);
}

static void drawItems() {
  u8g2.setDrawColor(1);
  float t = millis() * 0.006f;
  for (int i = 0; i < MAX_ITEMS; i++) {
    if (!items[i].on) continue;
    int sx = sxOf(items[i].x), sy = syOf(items[i].y);
    if (sx < 4 || sx > 118 || sy < 10 || sy > 60) continue;
    if (items[i].type == 0) {
      int rx = 1 + (int)(2.0f * fabsf(cosf(t + items[i].x)));
      u8g2.drawFilledEllipse(sx, sy, rx, 3); u8g2.setDrawColor(0); u8g2.drawPixel(sx, sy); u8g2.setDrawColor(1);
    } else if (items[i].type == 1) {
      u8g2.drawBox(sx - 4, sy - 3, 9, 6); u8g2.setDrawColor(0); u8g2.drawFrame(sx - 3, sy - 2, 7, 4); u8g2.drawPixel(sx, sy - 1); u8g2.setDrawColor(1);
    } else {
      u8g2.drawBox(sx - 3, sy - 4, 7, 8); u8g2.drawHLine(sx - 1, sy - 6, 3); u8g2.drawPixel(sx + 3, sy - 5);
      u8g2.setDrawColor(0); u8g2.drawPixel(sx, sy - 1); u8g2.drawPixel(sx, sy + 1); u8g2.drawHLine(sx - 1, sy - 2, 3); u8g2.setDrawColor(1);
    }
  }
}

// ======================= VEHICLE DRAWING =======================
static void fillPoly(const int* px, const int* py, int n, uint8_t mode, int ax, int ay) {
  uint8_t* buf = u8g2.getBufferPtr();
  int miny = 999, maxy = -999;
  for (int i = 0; i < n; i++) { if (py[i] < miny) miny = py[i]; if (py[i] > maxy) maxy = py[i]; }
  if (miny < 0) miny = 0;
  if (maxy > 63) maxy = 63;
  for (int y = miny; y <= maxy; y++) {
    int xl = 1000, xr = -1000;
    for (int i = 0; i < n; i++) {
      int j = (i + 1) % n;
      int y0 = py[i], y1 = py[j], x0 = px[i], x1 = px[j];
      if (y0 == y1) { if (y == y0) { xl = min(xl, min(x0, x1)); xr = max(xr, max(x0, x1)); } continue; }
      if ((y < y0 && y < y1) || (y > y0 && y > y1)) continue;
      int x = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
      xl = min(xl, x); xr = max(xr, x);
    }
    if (xl > xr) continue;
    if (xl < 0) xl = 0;
    if (xr > 127) xr = 127;
    uint8_t bit = (uint8_t)(1 << (y & 7));
    uint8_t* row = buf + (y >> 3) * 128;
    for (int x = xl; x <= xr; x++) {
      bool on = (mode == 1) || (mode == 2 && gray(x - ax + 512, y - ay + 512, 12));
      if (on) row[x] |= bit; else row[x] &= (uint8_t)~bit;
    }
  }
}

static void drawVehicle(const Vehicle& V, float trav, float ox, float oy, float th, float sc,
                        float cR, float cF, float aR, float aF, float hdx, float hdy, bool dead) {
  const float c = cosf(th), s = sinf(th);
  auto P = [&](float lx, float ly, int& x, int& y) { x = (int)lroundf(ox + (lx * c - ly * s) * sc); y = (int)lroundf(oy - (lx * s + ly * c) * sc); };
  int px[6], py[6];
  int pox = (int)lroundf(ox), poy = (int)lroundf(oy);
  for (int k = 0; k < V.nPoly; k++) {                              // body panels: dithered metal + crisp outline
    const Poly& pl = V.poly[k];
    for (int i = 0; i < pl.n; i++) P(pl.p[i][0], pl.p[i][1], px[i], py[i]);
    fillPoly(px, py, pl.n, pl.col ? 2 : 0, pox, poy);
    u8g2.setDrawColor(pl.col ? 1 : 0);
    for (int i = 0; i < pl.n; i++) { int j = (i + 1) % pl.n; u8g2.drawLine(px[i], py[i], px[j], py[j]); }
  }
  int wcx[2], wcy[2], rpx[2], anx[2], any[2];
  for (int i = 0; i < 2; i++) {
    float lx = i ? V.afx : V.arx, ly = i ? V.afy : V.ary, r = i ? V.rf : V.rr, comp = i ? cF : cR;
    P(lx, ly, anx[i], any[i]);
    P(lx, ly - (trav - comp), wcx[i], wcy[i]);
    rpx[i] = (int)lroundf(r * sc); if (rpx[i] < 2) rpx[i] = 2;
  }
  if (V.arch > 1.01f) {
    u8g2.setDrawColor(0);
    for (int i = 0; i < 2; i++) u8g2.drawDisc(wcx[i], wcy[i], (int)lroundf(rpx[i] * V.arch));
  }
  for (int k = 0; k < V.nLines; k++) {
    int x0, y0, x1, y1;
    P(V.lines[k][0], V.lines[k][1], x0, y0); P(V.lines[k][2], V.lines[k][3], x1, y1);
    u8g2.setDrawColor(V.lines[k][4] > 0.5f ? 1 : 0);
    u8g2.drawLine(x0, y0, x1, y1);
  }
  u8g2.setDrawColor(1);
  for (int i = 0; i < 2; i++) {                                   // suspension struts
    u8g2.drawLine(anx[i], any[i], wcx[i], wcy[i]);
    if (trav >= 0.5f) u8g2.drawLine(anx[i] + 1, any[i], wcx[i] + 1, wcy[i]);
  }
  float hxl = V.hx + hdx, hyl = V.hy + hdy;                       // driver
  int hx, hy, sx, sy, ux, uy, fx_, fy_, shx, shy;
  P(hxl, hyl, hx, hy); P(V.sx, V.sy, sx, sy); P(V.ux, V.uy, ux, uy); P(V.fx, V.fy, fx_, fy_);
  P(V.sx + (hxl - V.sx) * 0.78f, V.sy + (hyl - V.sy) * 0.78f, shx, shy);
  u8g2.drawLine(sx, sy, shx, shy); u8g2.drawLine(sx + 1, sy, shx + 1, shy);
  u8g2.drawLine(shx, shy, ux, uy);
  u8g2.drawLine(sx, sy, fx_, fy_);
  int hr = (int)lroundf(V.hr * sc); if (hr < 2) hr = 2;
  u8g2.setDrawColor(0); u8g2.drawDisc(hx, hy, hr);
  u8g2.setDrawColor(1); u8g2.drawCircle(hx, hy, hr);
  u8g2.drawPixel(hx + 1, hy - 1);
  if (dead) { u8g2.drawPixel(hx - 1, hy - 1); u8g2.drawPixel(hx + 1, hy + 1); u8g2.drawPixel(hx + 1, hy - 1); u8g2.drawPixel(hx - 1, hy + 1); }
  for (int i = 0; i < 2; i++) {                                   // wheels
    float ang = i ? aF : aR;
    int rp = rpx[i];
    u8g2.setDrawColor(0); u8g2.drawDisc(wcx[i], wcy[i], rp);
    u8g2.setDrawColor(1); u8g2.drawCircle(wcx[i], wcy[i], rp);
    if (rp >= 5) u8g2.drawCircle(wcx[i], wcy[i], rp - 1);
    if (rp >= 6) u8g2.drawCircle(wcx[i], wcy[i], rp / 2);
    int cnt = V.knob == 1 ? 8 : (V.knob == 2 ? 12 : 0);
    for (int k = 0; k < cnt; k++) {
      float a = ang + k * (6.2832f / cnt);
      int xa = wcx[i] + (int)lroundf((rp - 1) * cosf(a)), ya = wcy[i] + (int)lroundf((rp - 1) * sinf(a));
      int xb = wcx[i] + (int)lroundf((rp + 1) * cosf(a)), yb = wcy[i] + (int)lroundf((rp + 1) * sinf(a));
      u8g2.drawLine(xa, ya, xb, yb);
    }
    float sr = (float)(rp - 2);
    for (int k = 0; k < 2; k++) {
      float a = ang + k * 1.5708f;
      int dx = (int)lroundf(sr * cosf(a)), dy = (int)lroundf(sr * sinf(a));
      u8g2.drawLine(wcx[i] - dx, wcy[i] - dy, wcx[i] + dx, wcy[i] + dy);
    }
  }
}

static void carPose(float& ix, float& iy, float& ith) {
  float a = renderAlpha;
  ix = car.px + (car.x - car.px) * a; iy = car.py + (car.y - car.py) * a; ith = car.pth + (car.th - car.pth) * a;
}
static void drawCarWorld() {
  const Vehicle& V = VEH[save.veh];
  float ix, iy, ith; carPose(ix, iy, ith);
  float ox = ix * SC - camPX + 64.0f, oy = 34.0f - (iy * SC - camPY);
  drawVehicle(V, effTravel(save.veh), ox, oy, ith, SC, car.wh[0].comp, car.wh[1].comp,
              car.wh[0].ang, car.wh[1].ang, car.hdx, car.hdy, crashed);
}
static void drawSpeedLines() {
  uint32_t slot = millis() / 40;
  int len = 6 + (int)(car.vx * 0.5f);
  for (int i = 0; i < 4; i++) {
    uint32_t h = hash32(slot * 7u + i);
    int y = 12 + (int)(h % 36), x = (int)((h >> 8) % 90);
    if (x + len < 118) u8g2.drawHLine(x, y, len);
  }
}
static void drawBeam() {                                          // headlight cone for the cave
  const Vehicle& V = VEH[save.veh];
  float ix, iy, ith; carPose(ix, iy, ith);
  float fx0 = ix + (V.afx + 1.0f) * cosf(ith) - 0.1f * sinf(ith), fy0 = iy + (V.afx + 1.0f) * sinf(ith) + 0.1f * cosf(ith);
  float ox = fx0 * SC - camPX + 64.0f, oy = 34.0f - (fy0 * SC - camPY);
  for (int r = -1; r <= 1; r++) {
    float a = ith + r * 0.2f - 0.08f;
    for (int k = 4; k < 46; k += 4) {
      int x = (int)(ox + cosf(a) * k), y = (int)(oy - sinf(a) * k);
      if (x > 0 && x < 118 && y > 9 && y < 63) u8g2.drawPixel(x, y);
    }
  }
}
static void drawWorld(uint8_t st) {
  setCam();
  drawScenery(st);
  drawParts();
  drawItems();
  drawCarWorld();
  if (state == S_PLAY && !crashed) {
    if (st == 3) drawBeam();
    if (car.vx > 15.0f) drawSpeedLines();
  }
}

static void vehBounds(const Vehicle& V, float& x0, float& x1, float& y0, float& y1) {
  x0 = fminf(V.arx - V.rr, V.afx - V.rf); x1 = fmaxf(V.arx + V.rr, V.afx + V.rf);
  y0 = 1e9f; y1 = -1e9f;
  for (int i = 0; i < V.nHull; i++) {
    x0 = fminf(x0, V.hull[i][0]); x1 = fmaxf(x1, V.hull[i][0]);
    y0 = fminf(y0, V.hull[i][1]); y1 = fmaxf(y1, V.hull[i][1]);
  }
  y1 = fmaxf(y1, V.hy + V.hr);
  y0 = fminf(y0, V.ary - V.travel * 0.87f - V.rr); y0 = fminf(y0, V.afy - V.travel * 0.87f - V.rf);
}
static void drawVehiclePreview(uint8_t idx, int cx, int cy, float maxW, float maxH) {
  const Vehicle& V = VEH[idx];
  float x0, x1, y0, y1; vehBounds(V, x0, x1, y0, y1);
  float sc = fminf(maxW / (x1 - x0), maxH / (y1 - y0));
  float ox = cx - 0.5f * (x0 + x1) * sc, oy = cy + 0.5f * (y0 + y1) * sc;
  float a = millis() * 0.004f;
  float trav = V.travel * (1.0f + 0.06f * save.upg[idx][1]);
  float bob = 0.02f * sinf(millis() * 0.006f);
  drawVehicle(V, trav, ox, oy, 0.0f, sc, 0.3f * V.travel + bob, 0.3f * V.travel - bob, a, a, 0.0f, 0.02f * sinf(millis() * 0.009f), false);
}

// ======================= SCREENS =======================
static void txtC(const char* s, int y) { int w = u8g2.getStrWidth(s); u8g2.drawStr((128 - w) / 2, y, s); }
static void txtR(const char* s, int xr, int y) { int w = u8g2.getStrWidth(s); u8g2.drawStr(xr - w, y, s); }
static void drawToast() {
  if (millis() >= toastUntil) return;
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.setDrawColor(1); u8g2.drawBox(0, 55, 128, 9);
  u8g2.setDrawColor(0); txtC(toastTxt, 62); u8g2.setDrawColor(1);
}
static void coinHud() { char b[16]; snprintf(b, sizeof(b), "$%lu", (unsigned long)save.coins); u8g2.setFont(u8g2_font_5x7_tr); txtR(b, 127, 7); }

static void drawHud() {
  u8g2.setDrawColor(0); u8g2.drawBox(0, 0, 128, 8);
  u8g2.setDrawColor(1);
  u8g2.drawFrame(0, 1, 5, 6); u8g2.drawPixel(5, 2); u8g2.drawPixel(2, 3); u8g2.drawPixel(2, 4);   // can icon
  u8g2.drawFrame(7, 1, 32, 6);
  float cap = VEH[save.veh].fuel;
  int fw = (int)(30.0f * fuel / cap);
  bool blink = (fuel / cap < 0.2f) && ((millis() / 250) & 1);
  if (fw > 0 && !blink) u8g2.drawBox(8, 2, fw, 4);
  u8g2.setFont(u8g2_font_4x6_tr);
  char b[20];
  snprintf(b, sizeof(b), "%dm", (int)dist); txtC(b, 6);
  snprintf(b, sizeof(b), "%lu", (unsigned long)save.coins); txtR(b, 127, 6);
  u8g2.drawDisc(127 - u8g2.getStrWidth(b) - 5, 4, 2);
  if (popT > 0.0f) {
    u8g2.setFont(u8g2_font_5x7_tr);
    int w = u8g2.getStrWidth(popTxt);
    u8g2.setDrawColor(0); u8g2.drawBox((128 - w) / 2 - 2, 10, w + 4, 10);
    u8g2.setDrawColor(1); u8g2.drawFrame((128 - w) / 2 - 2, 10, w + 4, 10);
    u8g2.drawStr((128 - w) / 2, 18, popTxt);
  }
  if (bothT != 0 && !crashed) {
    int bw = (int)(60.0f * fminf(1.0f, (float)(millis() - bothT) / PAUSE_HOLD_MS));
    u8g2.setFont(u8g2_font_4x6_tr);
    u8g2.setDrawColor(0); u8g2.drawBox(30, 22, 68, 14); u8g2.setDrawColor(1);
    txtC("HOLD TO PAUSE", 28); u8g2.drawFrame(32, 30, 64, 5); u8g2.drawBox(34, 31, bw, 3);
  }
}

static void drawTitle() {
  float t = millis() * 0.001f;
  const Vehicle& V = VEH[save.veh];
  float x = 20.0f + t * 7.0f;
  float hr = terrainH(0, x - 1.1f), hf = terrainH(0, x + 1.1f);
  car.x = x; car.th = atan2f(hf - hr, 2.2f); car.y = 0.5f * (hr + hf) + 1.05f + 0.03f * sinf(t * 9.0f);
  car.wh[0].comp = car.wh[1].comp = 0.3f * V.travel;
  car.wh[0].ang = car.wh[1].ang = x / V.rr;
  car.hdx = 0.04f * sinf(t * 7.0f); car.hdy = 0.03f * sinf(t * 11.0f);
  car.px = car.x; car.py = car.y; car.pth = car.th; renderAlpha = 1.0f;
  camX = car.x + 3.0f; camY = car.y;
  crashed = false;
  for (int i = 0; i < MAX_ITEMS; i++) items[i].on = false;
  for (int i = 0; i < MAX_PARTS; i++) parts[i].life = 0;
  drawWorld(0);
  u8g2.setDrawColor(0); u8g2.drawBox(0, 0, 128, 27); u8g2.drawBox(0, 55, 128, 9);
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_helvB12_tr); txtC("HILL CLIMB", 14);
  u8g2.setFont(u8g2_font_6x10_tr); txtC("R A C I N G", 25);
  if ((millis() / 500) & 1) { u8g2.setFont(u8g2_font_4x6_tr); txtC("RIGHT DOUBLE TAP TO START", 62); }
}

static void drawMain() {
  u8g2.setFont(u8g2_font_5x7_tr); u8g2.drawStr(2, 7, "GARAGE"); coinHud(); u8g2.drawHLine(0, 9, 128);
  static const char* names[4] = {"PLAY", "VEHICLE", "STAGE", "TUNING"};
  u8g2.setFont(u8g2_font_6x10_tr);
  for (int i = 0; i < 4; i++) {
    int y = 22 + i * 12;
    if (i == mainSel) { u8g2.drawBox(0, y - 9, 56, 11); u8g2.setDrawColor(0); }
    u8g2.drawStr(5, y, names[i]); u8g2.setDrawColor(1);
  }
  drawVehiclePreview(save.veh, 93, 33, 58, 30);
  u8g2.setFont(u8g2_font_4x6_tr); txtR(STAGES[save.stage].name, 127, 63);
}

static void statBar(int x, int y, const char* lab, float v) {
  u8g2.setFont(u8g2_font_4x6_tr); u8g2.drawStr(x, y + 5, lab);
  u8g2.drawFrame(x + 14, y, 14, 6);
  int w = (int)(12.0f * constrain(v, 0.0f, 1.0f)); if (w > 0) u8g2.drawBox(x + 15, y + 1, w, 4);
}
static void drawVehScreen() {
  const Vehicle& V = VEH[vehCur];
  u8g2.setFont(u8g2_font_5x7_tr); u8g2.drawStr(0, 7, "<"); txtC(V.name, 7); coinHud(); u8g2.drawHLine(0, 9, 128);
  drawVehiclePreview(vehCur, 46, 32, 70, 36);
  statBar(98, 16, "SPD", V.vmax / 40.0f);
  statBar(98, 25, "ACC", (V.fmax / V.mass) / 10.0f);
  statBar(98, 34, "GRP", V.mu / 1.5f);
  statBar(98, 43, "SUS", V.travel / 0.8f);
  u8g2.setFont(u8g2_font_5x7_tr);
  char b[28];
  if (save.vehOwned & (1 << vehCur)) txtC(vehCur == save.veh ? "EQUIPPED" : "R-DBL TAP: EQUIP", 62);
  else { snprintf(b, sizeof(b), "LOCKED  $%u", V.price); txtC(b, 62); }
}

static void drawStageScreen() {
  camX = 40.0f + fmodf(millis() * 0.004f, 400.0f);
  camY = terrainH(stCur, camX) + 1.5f;
  setCam();
  drawScenery(stCur);
  u8g2.setDrawColor(0); u8g2.drawBox(0, 0, 128, 10); u8g2.drawBox(0, 50, 128, 14);
  u8g2.setDrawColor(1); u8g2.drawHLine(0, 9, 128); u8g2.drawHLine(0, 50, 128);
  u8g2.setFont(u8g2_font_5x7_tr); u8g2.drawStr(0, 7, "<"); txtC(STAGES[stCur].name, 7); coinHud();
  u8g2.setFont(u8g2_font_4x6_tr);
  char b[40];
  if (save.stageOwned & (1 << stCur)) txtC(stCur == save.stage ? "EQUIPPED" : "R-DBL TAP: SELECT", 57);
  else { snprintf(b, sizeof(b), "LOCKED  $%u", STAGES[stCur].price); txtC(b, 57); }
  snprintf(b, sizeof(b), "BEST %um  G %.1f  GRIP %d%%", save.best[stCur], STAGES[stCur].g, (int)(STAGES[stCur].grip * 100));
  txtC(b, 63);
}

static void drawTuneScreen() {
  u8g2.setFont(u8g2_font_5x7_tr); u8g2.drawStr(0, 7, "TUNE:"); u8g2.drawStr(30, 7, VEH[save.veh].name);
  char b[16]; snprintf(b, sizeof(b), "$%lu", (unsigned long)save.coins); txtR(b, 127, 7);
  u8g2.drawHLine(0, 9, 128);
  for (int i = 0; i < 4; i++) {
    int y = 20 + i * 12; bool sel = (i == tuneSel);
    if (sel) u8g2.drawBox(0, y - 8, 128, 11);
    u8g2.setDrawColor(sel ? 0 : 1);
    u8g2.drawStr(3, y, UPG_NAME[i]);
    uint8_t lv = save.upg[save.veh][i];
    for (int k = 0; k < UPG_MAX[i]; k++) { if (k < lv) u8g2.drawBox(46 + k * 8, y - 6, 6, 6); else u8g2.drawFrame(46 + k * 8, y - 6, 6, 6); }
    if (lv >= UPG_MAX[i]) txtR("MAX", 125, y); else { snprintf(b, sizeof(b), "$%u", UPG_COST[i][lv]); txtR(b, 125, y); }
    u8g2.setDrawColor(1);
  }
}

static void drawCrash() {
  u8g2.setFont(u8g2_font_helvB10_tr); txtC(crashReason == 1 ? "OUT OF FUEL" : "CRASHED!", 13);
  u8g2.setFont(u8g2_font_6x10_tr);
  char b[28];
  snprintf(b, sizeof(b), "DISTANCE %dm", (int)dist); txtC(b, 25);
  if (newBest) txtC("NEW BEST!", 36); else { snprintf(b, sizeof(b), "BEST %um", save.best[save.stage]); txtC(b, 36); }
  snprintf(b, sizeof(b), "COINS +%lu", (unsigned long)runCoins); txtC(b, 47);
  static const char* bt[2] = {"RETRY", "GARAGE"};
  for (int i = 0; i < 2; i++) {
    int x = 4 + i * 64;
    if (i == crashSel) { u8g2.drawBox(x, 51, 56, 12); u8g2.setDrawColor(0); } else u8g2.drawFrame(x, 51, 56, 12);
    int w = u8g2.getStrWidth(bt[i]); u8g2.drawStr(x + (56 - w) / 2, 61, bt[i]); u8g2.setDrawColor(1);
  }
}

static void drawPause() {
  u8g2.setDrawColor(0); u8g2.drawBox(22, 12, 84, 44);
  u8g2.setDrawColor(1); u8g2.drawFrame(22, 12, 84, 44);
  u8g2.setFont(u8g2_font_6x10_tr);
  static const char* pn[3] = {"RESUME", "RESTART", "QUIT"};
  for (int i = 0; i < 3; i++) {
    int y = 25 + i * 13;
    if (i == pauseSel) { u8g2.drawBox(26, y - 9, 76, 11); u8g2.setDrawColor(0); }
    int w = u8g2.getStrWidth(pn[i]); u8g2.drawStr(64 - w / 2, y, pn[i]); u8g2.setDrawColor(1);
  }
}

// ======================= LOGIC =======================
static void updateMenus() {
  int d = (L.ev1 ? -1 : 0) + (R.ev1 ? 1 : 0);
  bool back = L.ev2, sel = R.ev2;
  switch (state) {
    case S_TITLE: if (sel) { state = S_MAIN; mainSel = 0; } break;
    case S_MAIN:
      if (d) mainSel = (mainSel + 4 + d) % 4;
      if (back) state = S_TITLE;
      if (sel) {
        if (mainSel == 0) startRun();
        else if (mainSel == 1) { vehCur = save.veh; state = S_VEH; }
        else if (mainSel == 2) { stCur = save.stage; state = S_STAGE; }
        else { tuneSel = 0; state = S_TUNE; }
      }
      break;
    case S_VEH:
      if (d) vehCur = (vehCur + NV + d) % NV;
      if (back) state = S_MAIN;
      if (sel) {
        if (save.vehOwned & (1 << vehCur)) { save.veh = vehCur; saveNow(); toast("EQUIPPED"); }
        else if (save.coins >= VEH[vehCur].price) {
          save.coins -= VEH[vehCur].price; save.vehOwned |= (1 << vehCur); save.veh = vehCur; saveNow(); toast("UNLOCKED!");
        } else toast("NOT ENOUGH COINS");
      }
      break;
    case S_STAGE:
      if (d) stCur = (stCur + NS + d) % NS;
      if (back) state = S_MAIN;
      if (sel) {
        if (save.stageOwned & (1 << stCur)) { save.stage = stCur; saveNow(); toast("STAGE SET"); }
        else if (save.coins >= STAGES[stCur].price) {
          save.coins -= STAGES[stCur].price; save.stageOwned |= (1 << stCur); save.stage = stCur; saveNow(); toast("UNLOCKED!");
        } else toast("NOT ENOUGH COINS");
      }
      break;
    case S_TUNE:
      if (d) tuneSel = (tuneSel + 4 + d) % 4;
      if (back) state = S_MAIN;
      if (sel) {
        uint8_t& lv = save.upg[save.veh][tuneSel];
        if (lv >= UPG_MAX[tuneSel]) toast("MAX LEVEL");
        else {
          uint16_t cost = UPG_COST[tuneSel][lv];
          if (save.coins >= cost) { save.coins -= cost; lv++; saveNow(); toast("UPGRADED!"); } else toast("NOT ENOUGH COINS");
        }
      }
      break;
    case S_PAUSE:
      if (d) pauseSel = (pauseSel + 3 + d) % 3;
      if (back || (sel && pauseSel == 0)) { state = S_PLAY; resetTiming(); bothT = 0; }
      else if (sel && pauseSel == 1) startRun();
      else if (sel && pauseSel == 2) { saveNow(); state = S_MAIN; }
      break;
    case S_CRASH:
      if (d) crashSel = 1 - crashSel;
      if (back) state = S_MAIN;
      if (sel) { if (crashSel == 0) startRun(); else state = S_MAIN; }
      break;
    default: break;
  }
}

static void updatePlay(uint32_t nowMs) {
  const Vehicle& V = VEH[save.veh];
  bool gas = R.down, brake = L.down;
  if (!crashed && L.down && R.down) {
    if (bothT == 0) bothT = nowMs;
    else if (nowMs - bothT > PAUSE_HOLD_MS) { bothT = 0; pauseSel = 0; saveNow(); state = S_PAUSE; return; }
  } else bothT = 0;

  uint32_t nu = micros();
  float dtf = (nu - lastUs) * 1e-6f; lastUs = nu;
  if (dtf > 0.05f) dtf = 0.05f;
  accT += dtf;
  while (accT >= PHYS_DT) { physicsStep(PHYS_DT, gas, brake); accT -= PHYS_DT; }
  renderAlpha = accT / PHYS_DT;

  if (!crashed) collectItems();
  spawnItems();
  if (popT > 0) popT -= dtf;
  shakeAmp *= fmaxf(0.0f, 1.0f - 9.0f * dtf);

  const uint8_t st = save.stage;
  for (int i = 0; i < 2; i++) {
    Wheel& W = car.wh[i];
    if (!W.contact) continue;
    float sp = fabsf(W.vt);
    int n = 0;
    if (W.slip || (brake && sp > 2.5f)) n = 2;
    else if (sp > 5.0f && (int)(rnd() % 100) < 20 + (int)sp) n = 1;
    uint8_t type = (st == 2) ? 1 : (st == 7 ? 3 : 0);
    for (int k = 0; k < n; k++)
      emit(W.cx, W.cy + 0.1f, -W.vt * 0.25f + frand() * 1.2f, 0.8f + fabsf(frand()) * 1.8f, 0.55f, type);
  }
  if (V.ey > -50.0f && gas && fuel > 0.0f && !crashed && (rnd() % 100) < 45) {
    float c = cosf(car.th), s = sinf(car.th);
    emit(car.x + V.ex * c - V.ey * s, car.y + V.ex * s + V.ey * c, -car.vx * 0.15f - 0.6f, 0.8f + fabsf(frand()), 0.9f, 2);
  }
  if (st == 7 && (rnd() % 100) < 25) {
    float ex = car.x - 6.0f + (float)(rnd() % 120) * 0.1f;
    emit(ex, terrainH(7, ex) + 0.2f, frand() * 0.5f, 1.5f + fabsf(frand()) * 2.0f, 1.3f, 4);
  }
  updateParts(dtf);

  float ix, iy, ith; carPose(ix, iy, ith);
  float tx = ix + 3.0f + constrain(car.vx * 0.12f, -1.0f, 3.0f);
  camX += (tx - camX) * fminf(1.0f, dtf * 6.0f);
  float ty = iy + STAGES[st].camBias;
  camY += (ty - camY) * fminf(1.0f, dtf * 3.5f);
  if (ty - camY > 2.0f) camY = ty - 2.0f;
  if (ty - camY < -1.6f) camY = ty + 1.6f;

  if (crashed && crashT > 1.0f) endRun();
}

// ======================= ARDUINO =======================
// Frame transmission runs on core 0 while core 1 draws the next frame (double buffered).
static uint8_t txBuf[1024];
static SemaphoreHandle_t txDone;
static TaskHandle_t txTask;
static void txLoop(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    u8x8_t* u8x8 = u8g2.getU8x8();
    for (int ty = 0; ty < 8; ty++) u8x8_DrawTile(u8x8, 0, ty, 16, txBuf + ty * 128);
    xSemaphoreGive(txDone);
  }
}
static void presentFrame() {
  xSemaphoreTake(txDone, portMAX_DELAY);
  memcpy(txBuf, u8g2.getBufferPtr(), 1024);
  xTaskNotifyGive(txTask);
}

void setup() {
  pinMode(PIN_TOUCH_L, INPUT); pinMode(PIN_TOUCH_R, INPUT);
  L.pin = PIN_TOUCH_L; R.pin = PIN_TOUCH_R;
  Wire.begin(PIN_SDA, PIN_SCL);
  u8g2.begin();
  u8g2.setBusClock(I2C_SPEED);
  prefs.begin("hcr", false);
  loadSave();
  rngState = esp_random() | 1u;
  delay(80);
  if (digitalRead(PIN_TOUCH_L) == TOUCH_ACTIVE && digitalRead(PIN_TOUCH_R) == TOUCH_ACTIVE) {
    uint32_t t0 = millis();
    while (digitalRead(PIN_TOUCH_L) == TOUCH_ACTIVE && digitalRead(PIN_TOUCH_R) == TOUCH_ACTIVE) {
      if (millis() - t0 > 3000) {
        saveDefaults(); memset(&lastSaved, 0xFF, sizeof(lastSaved)); saveNow();
        u8g2.clearBuffer(); u8g2.setFont(u8g2_font_6x10_tr); txtC("SAVE ERASED", 34); u8g2.sendBuffer();
        delay(1500); break;
      }
      delay(10);
    }
  }
  txDone = xSemaphoreCreateBinary();
  xSemaphoreGive(txDone);
  xTaskCreatePinnedToCore(txLoop, "oled", 4096, NULL, 2, &txTask, 0);
  setupCar();
  state = S_TITLE;
}

void loop() {
  uint32_t t0 = micros();
  uint32_t nowMs = millis();
  L.update(nowMs); R.update(nowMs);
  if (state == S_PLAY) updatePlay(nowMs); else updateMenus();
  if (state != S_PLAY && state != S_PAUSE) renderAlpha = 1.0f;

  u8g2.clearBuffer();
  switch (state) {
    case S_TITLE: drawTitle(); break;
    case S_MAIN:  drawMain(); break;
    case S_VEH:   drawVehScreen(); break;
    case S_STAGE: drawStageScreen(); break;
    case S_TUNE:  drawTuneScreen(); break;
    case S_PLAY:  drawWorld(save.stage); drawHud(); break;
    case S_PAUSE: drawWorld(save.stage); drawHud(); drawPause(); break;
    case S_CRASH: drawCrash(); break;
  }
  drawToast();
  presentFrame();
  if (saveDirty && millis() - lastSaveMs > SAVE_INTERVAL_MS) saveNow();
  uint32_t el = micros() - t0;
  if (el < FRAME_US) delayMicroseconds(FRAME_US - el);
}
