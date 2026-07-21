/*
  CMozGlow — the friendly WS2812B library for the CMoz ESP32-S3 Mini
  ------------------------------------------------------------------
  Made for wearables & fashion tech by TinkerTailor.ca / CMozMaker.

  Why CMozGlow is different:
    • Real error handling  — every call reports what went wrong, in English.
    • Power budgeting      — tell it your battery limit in mA and it will
                             automatically dim to stay safe. Wearable-first!
    • Status pixel         — the onboard pixel (index 0, GPIO 3) can act as a
                             live status light: green = happy, red = error.
    • Non-blocking effects — set an effect once, call update() in loop().
                             No delay() anywhere, your buttons keep working.
    • AutoPilot mode       — advanced: give the LEDs their own CPU core.
                             glow.autoUpdate(true) and never think about
                             update() again. Thread-safe throughout.
    • Async LED engine     — frames are handed to the RMT peripheral and
                             sent in hardware while your code carries on.
                             show() returns in ~100 µs even on long strips.
    • Fashion-tech effects — Sequin, Catwalk, Loom, Silk, Aurora, Heartbeat,
                             Firefly, Ember and a Morse-code messenger.
    • Gamma correction     — colours look the way you mixed them, by default.

  Hardware notes:
    • The CMoz ESP32-S3 Mini has ONE onboard WS2812B on GPIO 3.
    • External strips share GPIO 3, wired after the onboard pixel.
      So: NUM_LEDS = 1 (onboard) + however many are on your strip.

  MIT License — remix, teach, sell, enjoy.
*/

#ifndef CMOZ_GLOW_H
#define CMOZ_GLOW_H

#include <Arduino.h>

#if !defined(ESP32)
#error "CMozGlow is written for the ESP32 family (designed on the CMoz ESP32-S3 Mini)."
#endif

// ───────────────────────── Board defaults ─────────────────────────
#define CMOZ_DEFAULT_PIN     3      // onboard WS2812B data pin on the CMoz ESP32-S3 Mini
#define CMOZ_MAX_LEDS        2000   // sanity ceiling
#define CMOZ_MAX_FIREFLIES   8
#define CMOZ_AUTOPILOT_CORE  0      // Arduino loop() lives on core 1; LEDs get core 0
#define CMOZ_AUTOPILOT_STACK 4096

// ───────────────────────── Error codes ────────────────────────────
enum CMozError : uint8_t {
  CMOZ_OK = 0,          // all good
  CMOZ_ERR_NOT_BEGUN,   // you forgot to call begin() (or it failed)
  CMOZ_ERR_BAD_PIN,     // that GPIO can't drive LEDs on the ESP32-S3
  CMOZ_ERR_BAD_COUNT,   // LED count must be 1..CMOZ_MAX_LEDS
  CMOZ_ERR_ALLOC,       // out of memory (very long strip?)
  CMOZ_ERR_RMT,         // the RMT LED peripheral refused to start or timed out
  CMOZ_ERR_INDEX,       // pixel index past the end of the strip
  CMOZ_ERR_BAD_EFFECT,  // effect id doesn't exist — see CMOZ_FX_COUNT
  CMOZ_ERR_BAD_ARG,     // an argument was out of range
  CMOZ_ERR_TASK         // the AutoPilot task couldn't start or stop
};

// ───────────────────────── Effect IDs ──────────────────────────────
enum CMozEffect : uint8_t {
  CMOZ_FX_SOLID = 0,   // steady colour
  CMOZ_FX_SEQUIN,      // random glints, like sequins catching the light
  CMOZ_FX_CATWALK,     // bold sweep with a fading train, back and forth
  CMOZ_FX_LOOM,        // two threads weaving past each other
  CMOZ_FX_HEARTBEAT,   // realistic lub-dub double pulse
  CMOZ_FX_FIREFLY,     // soft fireflies blinking in and out
  CMOZ_FX_SILK,        // slow shimmering sheen rolling down the strip
  CMOZ_FX_EMBER,       // warm campfire glow
  CMOZ_FX_AURORA,      // northern lights (a little Canada in every project)
  CMOZ_FX_MORSE,       // blinks a real Morse-code message!
  CMOZ_FX_COUNT        // number of effects (not an effect itself)
};

class CMozGlow {
public:
  // count = total pixels INCLUDING the onboard one. pin defaults to GPIO 3.
  explicit CMozGlow(uint16_t count = 1, uint8_t pin = CMOZ_DEFAULT_PIN);
  ~CMozGlow();

  // ── Lifecycle ────────────────────────────────────────────────────
  bool begin();                       // returns false on failure; check errorText()
  void end();                         // stops AutoPilot, waits for the LEDs, frees memory

  // ── Error handling (our thing!) ─────────────────────────────────
  CMozError    lastError() const { return _err; }
  const char*  errorText() const;     // human-readable description of lastError()
  void         clearError()      { _err = CMOZ_OK; }
  void         statusPixel(bool enable); // pixel 0 becomes a live status light

  // ── Pixels ──────────────────────────────────────────────────────
  bool  setPixel(uint16_t i, uint8_t r, uint8_t g, uint8_t b);
  bool  setPixel(uint16_t i, uint32_t color);
  uint32_t getPixel(uint16_t i);      // returns 0 and sets error if out of range
  void  fill(uint32_t color);
  void  clear();
  bool  show();                       // hand the frame to hardware (returns in ~µs)

  uint16_t numPixels() const { return _count; }

  // ── Brightness / colour quality ─────────────────────────────────
  void  setBrightness(uint8_t b);     // 0..255, non-destructive
  uint8_t getBrightness() const { return _brightness; }
  void  setGamma(bool on)  { _gammaOn = on; }   // on by default

  // ── Power budgeting for wearables ───────────────────────────────
  void      setPowerBudgetmA(uint16_t mA);  // 0 = unlimited
  uint16_t  estimatedCurrentmA();           // what the last show() drew (approx)
  bool      powerLimited() const { return _powerLimited; } // did we auto-dim?

  // ── Effects engine (non-blocking) ───────────────────────────────
  bool  setEffect(uint8_t id);
  void  setEffectColor(uint32_t c) { _fxColor = c; }
  bool  setEffectSpeed(uint8_t s);    // 1 (dreamy) .. 10 (party)
  bool  setMorseMessage(const char* msg); // A-Z, 0-9, spaces; up to 63 chars
  bool  update();                     // call every loop(); true when a frame was drawn
  static const char* effectName(uint8_t id);

  // ── AutoPilot (advanced): the LEDs get their own CPU core ──────
  // autoUpdate(true) starts a background task pinned to core 0 that calls
  // update() for you, forever. Your loop() stays 100% free for sensors,
  // Bluetooth, touch — anything. All CMozGlow calls remain safe to use
  // from your sketch while AutoPilot flies. autoUpdate(false) lands it.
  bool  autoUpdate(bool enable);
  bool  autoRunning() const { return _task != nullptr; }

  // ── Colour helpers ──────────────────────────────────────────────
  static uint32_t Color(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  }
  static uint32_t Wheel(uint8_t pos); // 0..255 around the rainbow

private:
  // config / state
  uint8_t   _pin;
  uint16_t  _count;
  bool      _begun        = false;
  volatile CMozError _err = CMOZ_OK;
  bool      _statusOn     = false;

  uint8_t   _brightness   = 255;
  bool      _gammaOn      = true;
  uint16_t  _budgetmA     = 0;
  uint16_t  _lastEstmA    = 0;
  bool      _powerLimited = false;

  // buffers
  uint8_t*  _pix   = nullptr;   // RGB, 3 bytes per pixel
  uint8_t*  _heat  = nullptr;   // per-pixel scratch for Ember
  void*     _rmt   = nullptr;   // RMT symbol buffer (data + encoded latch)
  bool      _txPending = false; // a frame is in flight on the RMT hardware

  // concurrency (opaque handles keep this header dependency-free)
  void*          _mutex = nullptr;   // recursive mutex guarding all state
  void* volatile _task  = nullptr;   // AutoPilot task handle
  volatile bool  _autoRun = false;

  // effects
  uint8_t   _fx      = CMOZ_FX_SOLID;
  uint32_t  _fxColor = 0x00FF2A78; // TinkerTailor pink, why not
  uint8_t   _speed   = 5;
  uint32_t  _nextFrameAt = 0;
  uint32_t  _t0 = 0;            // effect epoch
  uint16_t  _step = 0;

  struct Firefly { uint16_t pos; uint16_t phase; uint8_t rate; bool alive; };
  Firefly   _flies[CMOZ_MAX_FIREFLIES];

  // morse
  char      _mMsg[64] = "HELLO";
  uint8_t   _mChar = 0, _mElem = 0;
  bool      _mOn = false;
  uint32_t  _mNextAt = 0;

  // internals
  bool  pinIsValid(uint8_t pin) const;
  bool  rmtBegin();
  bool  rmtSendAsync(size_t symbols);
  bool  rmtWaitDone(uint32_t timeoutMs);
  uint32_t txTimeoutMs() const;
  uint16_t frameInterval() const;
  void  fadeAll(uint8_t keep256);
  static void buildTables();          // gamma + byte→symbol LUT, built once
  static void taskThunk(void* self);
  void  taskLoop();

  // effect renderers
  void fxSolid();    void fxSequin();  void fxCatwalk(); void fxLoom();
  void fxHeartbeat();void fxFirefly(); void fxSilk();    void fxEmber();
  void fxAurora();   void fxMorse();
};

#endif // CMOZ_GLOW_H
