/*
Christine (http://www.christinefarion.com)
CMozMaker · Tinker Tailor
https://www.youtube.com/@CMozMaker
https://www.tinkertailor.ca

Tinker Tailor is a small but passionate shop dedicated to the wearable 
technology maker community. We provide a curated selection of tools, components, 
and materials like conductive fabrics, circuit boards, and more to help creators 
bring their innovative ideas to life. With a focus on education and creativity, 
we also offer tutorials, resources, and expert advice to empower makers at every 
skill level. 

At Tinker Tailor, we believe in making wearable tech accessible, fun, and inspiring for everyone!

More projects in my book: https://www.amazon.ca/Ultimate-Informed-Wearable-Technology-hands/dp/1803230592

Questions about this build? Ask in [Discussions](https://github.com/cmoz/YouTube/discussions) 
and include your board, IDE, and error message.
--------------------------------------------------------------------

  CMozGlow — implementation
  --------------------------------------------------------------------
  Architecture (v1.1 — the responsive engine):

    • ASYNC TRANSMIT  show() builds the frame, hands it to the RMT
      peripheral, and returns immediately. The hardware clocks the LEDs
      out on its own. The *next* show() waits for the previous transfer
      first — which at any sane frame rate has long since finished, so
      the wait costs ~nothing. Worst case it's bounded by a timeout.

    • ENCODED LATCH   the WS2812B ≥280 µs reset gap is appended to every
      frame as one extra RMT symbol, so it happens inside the hardware
      transfer. No busy-waiting anywhere in the library.

    • SYMBOL LUT      every possible byte value maps to 8 precomputed
      RMT symbols (a one-time 8 KB table). Building a frame is three
      32-byte copies per pixel instead of 24 conditional constructions.

    • AUTOPILOT       an optional FreeRTOS task pinned to core 0 calls
      update() forever, leaving core 1 (your loop()) completely free.
      A recursive mutex makes every public call thread-safe, whether
      AutoPilot is flying or not.

  Works on arduino-esp32 core 2.x (legacy RMT driver) and 3.x (rmtWrite
  API), selected automatically at compile time.
*/

#include "CMozGlow.h"
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#if __has_include(<esp_arduino_version.h>)
  #include <esp_arduino_version.h>
#endif

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  #define CMOZ_CORE3 1
#else
  #define CMOZ_CORE3 0
  #include "driver/rmt.h"
#endif

// Both cores' RMT symbol types are 32-bit unions with an identical bit
// layout, so the LUT stores raw uint32_t values and we copy them in.
#if CMOZ_CORE3
  typedef rmt_data_t cmoz_rmt_t;
#else
  typedef rmt_item32_t cmoz_rmt_t;
#endif
static_assert(sizeof(cmoz_rmt_t) == sizeof(uint32_t),
              "CMozGlow's symbol LUT assumes 32-bit RMT symbols");

// WS2812B timing at a 10 MHz RMT tick (100 ns per tick)
#define CMOZ_T0H 4   // 0-bit: 400 ns high
#define CMOZ_T0L 8   //        800 ns low
#define CMOZ_T1H 8   // 1-bit: 800 ns high
#define CMOZ_T1L 4   //        400 ns low
#define CMOZ_LATCH_TICKS 3200   // 320 µs low, encoded at the end of each frame

// ─────────────────── one-time shared tables ────────────────────────

static uint8_t  sGamma[256];
static uint32_t sLut[256][8];        // byte value -> 8 RMT symbols (MSB first)
static uint32_t sLatch;              // one long-low symbol = the reset gap
static bool     sTablesBuilt = false;

void CMozGlow::buildTables() {
  for (int i = 0; i < 256; i++)
    sGamma[i] = (uint8_t)(powf(i / 255.0f, 2.6f) * 255.0f + 0.5f);

  cmoz_rmt_t s;
  for (int v = 0; v < 256; v++) {
    for (int bit = 7; bit >= 0; bit--) {
      bool one = v & (1 << bit);
      s.level0 = 1; s.duration0 = one ? CMOZ_T1H : CMOZ_T0H;
      s.level1 = 0; s.duration1 = one ? CMOZ_T1L : CMOZ_T0L;
      sLut[v][7 - bit] = s.val;
    }
  }
  s.level0 = 0; s.duration0 = CMOZ_LATCH_TICKS / 2;
  s.level1 = 0; s.duration1 = CMOZ_LATCH_TICKS / 2;
  sLatch = s.val;
}

// ─────────────────── tiny RAII lock guard ──────────────────────────
// Recursive, so effects calling setPixel() under update() nest safely,
// and a null mutex (pre-begin) degrades to a no-op instead of a crash.

namespace {
  struct CMozLock {
    explicit CMozLock(void* m) : _m((SemaphoreHandle_t)m) {
      if (_m) xSemaphoreTakeRecursive(_m, portMAX_DELAY);
    }
    ~CMozLock() { if (_m) xSemaphoreGiveRecursive(_m); }
    SemaphoreHandle_t _m;
  };
}

// ─────────────────────────── ctor / dtor ───────────────────────────

CMozGlow::CMozGlow(uint16_t count, uint8_t pin) : _pin(pin), _count(count) {
  for (auto &f : _flies) f.alive = false;
}

CMozGlow::~CMozGlow() { end(); }

// ─────────────────────────── lifecycle ─────────────────────────────

/*
  Which GPIOs can actually drive LEDs? This differs per chip, so we check
  against the real target rather than assuming the CMoz board. Pins that
  are input-only, non-existent, or wired to flash/PSRAM are rejected —
  those are the ones that silently produce a dark strip and a puzzled maker.
*/
bool CMozGlow::pinIsValid(uint8_t pin) const {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  // ESP32-S3: 0-21 and 33-48. 22-25 don't exist; 26-32 are flash/PSRAM.
  return (pin <= 21) || (pin >= 33 && pin <= 48);
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
  // ESP32-S2: 0-21 and 33-46. 22-25 don't exist; 26-32 are flash/PSRAM.
  return (pin <= 21) || (pin >= 33 && pin <= 46);
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  // ESP32-C3: 0-10 and 18-21. 11-17 are flash.
  return (pin <= 10) || (pin >= 18 && pin <= 21);
#elif defined(CONFIG_IDF_TARGET_ESP32)
  // Classic ESP32: 0-19, 21-23, 25-27, 32-33 can output.
  // 20 and 24 don't exist; 6-11 are flash; 34-39 are INPUT ONLY.
  if (pin >= 6 && pin <= 11) return false;      // SPI flash — never
  if (pin == 20 || pin == 24) return false;     // not bonded out
  if (pin >= 34) return false;                  // input-only pins
  return (pin <= 19) || (pin >= 21 && pin <= 23)
      || (pin >= 25 && pin <= 27) || (pin >= 32 && pin <= 33);
#else
  // Unknown ESP32 variant — allow anything the core will accept.
  return pin < 64;
#endif
}

bool CMozGlow::begin() {
  if (_begun) end();                  // calling begin() twice is fine
  _err = CMOZ_OK;

  if (_count < 1 || _count > CMOZ_MAX_LEDS) { _err = CMOZ_ERR_BAD_COUNT; return false; }
  if (!pinIsValid(_pin))                    { _err = CMOZ_ERR_BAD_PIN;   return false; }

  if (!_mutex) _mutex = xSemaphoreCreateRecursiveMutex();
  if (!_mutex) { _err = CMOZ_ERR_ALLOC; return false; }

  _pix  = (uint8_t*)calloc(_count, 3);
  _heat = (uint8_t*)calloc(_count, 1);
  _rmt  = calloc((size_t)_count * 24 + 1, sizeof(cmoz_rmt_t)); // +1 = latch
  if (!_pix || !_heat || !_rmt) { end(); _err = CMOZ_ERR_ALLOC; return false; }

  if (!sTablesBuilt) { buildTables(); sTablesBuilt = true; }

  if (!rmtBegin()) { end(); _err = CMOZ_ERR_RMT; return false; }

  _begun = true;
  _txPending = false;
  _t0 = millis();
  clear();
  show();                             // start dark & latched
  return true;
}

void CMozGlow::end() {
  // Land AutoPilot first. If it somehow can't land, DO NOT free the
  // buffers a live task may still be touching — leaked memory beats a
  // crash in someone's costume, every time.
  if (!autoUpdate(false)) return;         // _err already set to CMOZ_ERR_TASK
  if (_begun) rmtWaitDone(txTimeoutMs()); // never free a buffer mid-transfer!
  _begun = false;
  if (_pix)  { free(_pix);  _pix  = nullptr; }
  if (_heat) { free(_heat); _heat = nullptr; }
  if (_rmt)  { free(_rmt);  _rmt  = nullptr; }
  if (_mutex) { vSemaphoreDelete((SemaphoreHandle_t)_mutex); _mutex = nullptr; }
}

// ─────────────────────────── RMT layer ─────────────────────────────

// How long could one frame possibly take on the wire? 30 µs per LED
// plus the encoded latch, with generous headroom for interrupt jitter.
uint32_t CMozGlow::txTimeoutMs() const {
  return ((uint32_t)_count * 30 + CMOZ_LATCH_TICKS / 10) / 1000 + 5;
}

bool CMozGlow::rmtBegin() {
#if CMOZ_CORE3
  return rmtInit(_pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000); // 10 MHz tick
#else
  rmt_config_t cfg = RMT_DEFAULT_CONFIG_TX((gpio_num_t)_pin, RMT_CHANNEL_0);
  cfg.clk_div = 8;                                   // 80 MHz / 8 = 10 MHz tick
  if (rmt_config(&cfg) != ESP_OK) return false;
  if (rmt_driver_install(cfg.channel, 0, 0) != ESP_OK) return false;
  return true;
#endif
}

// Fire the frame and return immediately — the RMT hardware takes over.
bool CMozGlow::rmtSendAsync(size_t symbols) {
#if CMOZ_CORE3
  if (!rmtWriteAsync(_pin, (rmt_data_t*)_rmt, symbols)) return false;
#else
  if (rmt_write_items(RMT_CHANNEL_0, (rmt_item32_t*)_rmt, symbols, false) != ESP_OK)
    return false;
#endif
  _txPending = true;
  return true;
}

// Wait (bounded!) for the previous frame to leave the wire. Because the
// latch gap is encoded into the frame, "done" also means "latched".
bool CMozGlow::rmtWaitDone(uint32_t timeoutMs) {
  if (!_txPending) return true;
#if CMOZ_CORE3
  uint32_t t0 = millis();
  while (!rmtTransmitCompleted(_pin)) {
    if (millis() - t0 > timeoutMs) return false;
    delayMicroseconds(25);
  }
#else
  if (rmt_wait_tx_done(RMT_CHANNEL_0, pdMS_TO_TICKS(timeoutMs)) != ESP_OK)
    return false;
#endif
  _txPending = false;
  return true;
}

// ─────────────────────────── error text ────────────────────────────

const char* CMozGlow::errorText() const {
  switch (_err) {
    case CMOZ_OK:              return "OK — everything is fine";
    case CMOZ_ERR_NOT_BEGUN:   return "begin() was never called (or it failed) — call glow.begin() in setup()";
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    case CMOZ_ERR_BAD_PIN:     return "that GPIO can't drive LEDs — on the ESP32-S3 use 0-21 or 33-48 (the CMoz onboard pixel is on 3)";
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
    case CMOZ_ERR_BAD_PIN:     return "that GPIO can't drive LEDs — on the ESP32-S2 use 0-21 or 33-46";
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
    case CMOZ_ERR_BAD_PIN:     return "that GPIO can't drive LEDs — on the ESP32-C3 use 0-10 or 18-21";
#elif defined(CONFIG_IDF_TARGET_ESP32)
    case CMOZ_ERR_BAD_PIN:     return "that GPIO can't drive LEDs — on the classic ESP32 use 0-5, 12-19, 21-23, 25-27, 32 or 33 (6-11 are the flash pins, 34-39 are input-only)";
#else
    case CMOZ_ERR_BAD_PIN:     return "that GPIO can't drive LEDs on this board — check your board's pinout for a usable output pin";
#endif
    case CMOZ_ERR_BAD_COUNT:   return "LED count must be between 1 and 2000 — remember to include the onboard pixel";
    case CMOZ_ERR_ALLOC:       return "not enough memory for that many LEDs";
    case CMOZ_ERR_RMT:         return "the RMT LED driver failed or timed out — is another library using it?";
    case CMOZ_ERR_INDEX:       return "pixel index is past the end of the strip — indexes run 0 to numPixels()-1";
    case CMOZ_ERR_BAD_EFFECT:  return "no effect with that number — valid effects are 0 to 9";
    case CMOZ_ERR_BAD_ARG:     return "an argument was out of range — check the docs for valid values";
    case CMOZ_ERR_TASK:        return "AutoPilot couldn't start or stop its background task";
  }
  return "unknown error";
}

void CMozGlow::statusPixel(bool enable) { _statusOn = enable; }

// ─────────────────────────── pixels ────────────────────────────────

bool CMozGlow::setPixel(uint16_t i, uint8_t r, uint8_t g, uint8_t b) {
  CMozLock lk(_mutex);
  if (!_begun)     { _err = CMOZ_ERR_NOT_BEGUN; return false; }
  if (i >= _count) { _err = CMOZ_ERR_INDEX;     return false; }
  uint8_t* p = _pix + (uint32_t)i * 3;
  p[0] = r; p[1] = g; p[2] = b;
  return true;
}

bool CMozGlow::setPixel(uint16_t i, uint32_t c) {
  return setPixel(i, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

uint32_t CMozGlow::getPixel(uint16_t i) {
  CMozLock lk(_mutex);
  if (!_begun)     { _err = CMOZ_ERR_NOT_BEGUN; return 0; }
  if (i >= _count) { _err = CMOZ_ERR_INDEX;     return 0; }
  uint8_t* p = _pix + (uint32_t)i * 3;
  return Color(p[0], p[1], p[2]);
}

void CMozGlow::fill(uint32_t c) {
  CMozLock lk(_mutex);
  if (!_begun) { _err = CMOZ_ERR_NOT_BEGUN; return; }
  for (uint16_t i = 0; i < _count; i++) setPixel(i, c);
}

void CMozGlow::clear() { fill(0); }

void CMozGlow::setBrightness(uint8_t b) { _brightness = b; }

void CMozGlow::setPowerBudgetmA(uint16_t mA) { _budgetmA = mA; }

uint16_t CMozGlow::estimatedCurrentmA() { return _lastEstmA; }

// ─────────────────────────── show() ────────────────────────────────
/*
  1. Wait for the previous frame's hardware transfer (normally instant).
  2. Estimate current draw at the requested brightness; if it exceeds the
     power budget, fold an extra dimming factor into the scale.
  3. Build a 256-entry "scaled + gamma'd" byte table for this frame, then
     assemble RMT symbols with three LUT copies per pixel.
  4. Append the encoded latch symbol and fire asynchronously.
  Approximation: ~20 mA per fully-lit colour channel + ~1 mA idle per chip.
*/
bool CMozGlow::show() {
  CMozLock lk(_mutex);
  if (!_begun) { _err = CMOZ_ERR_NOT_BEGUN; return false; }

  if (!rmtWaitDone(txTimeoutMs())) { _err = CMOZ_ERR_RMT; return false; }

  uint16_t bScale = (uint16_t)_brightness + 1;       // 1..256

  // pass 1 — power estimate at requested brightness
  uint32_t sum = 0;
  const uint32_t bytes = (uint32_t)_count * 3;
  for (uint32_t n = 0; n < bytes; n++)
    sum += ((uint16_t)_pix[n] * bScale) >> 8;
  uint32_t est = _count + (sum * 20) / 255;          // mA
  _lastEstmA = (est > 65535) ? 65535 : (uint16_t)est;

  uint16_t pScale = 256;
  _powerLimited = false;
  if (_budgetmA && est > _budgetmA) {
    uint32_t idle = _count;                          // idle draw doesn't dim
    uint32_t room = (_budgetmA > idle) ? (_budgetmA - idle) : 0;
    uint32_t used = est - idle;
    pScale = (uint16_t)((room * 256) / (used ? used : 1));
    if (pScale > 256) pScale = 256;
    _powerLimited = true;
    _lastEstmA = _budgetmA;
  }

  // per-frame byte table: brightness × power budget × gamma, in one lookup
  const uint32_t scale = ((uint32_t)bScale * pScale) >> 8;
  uint8_t xf[256];
  for (uint16_t v = 0; v < 256; v++) {
    uint8_t s = (uint8_t)((v * scale) >> 8);
    xf[v] = _gammaOn ? sGamma[s] : s;
  }

  // pass 2 — assemble symbols (WS2812B wants G,R,B order, MSB first)
  uint32_t* out = (uint32_t*)_rmt;
  const uint8_t* p = _pix;
  for (uint16_t i = 0; i < _count; i++, p += 3) {
    uint8_t r = p[0], g = p[1], b = p[2];

    if (_statusOn && i == 0) {                       // status overlay: bypasses
      if (_err == CMOZ_OK) { r = 0;  g = 24; b = 4; }// brightness & budget so it
      else                 { r = 40; g = 0;  b = 0; }// stays visible when dimmed
      memcpy(out,      sLut[sGamma[g]], 32);
      memcpy(out + 8,  sLut[sGamma[r]], 32);
      memcpy(out + 16, sLut[sGamma[b]], 32);
    } else {
      memcpy(out,      sLut[xf[g]], 32);
      memcpy(out + 8,  sLut[xf[r]], 32);
      memcpy(out + 16, sLut[xf[b]], 32);
    }
    out += 24;
  }
  *out = sLatch;                                     // reset gap, in hardware

  if (!rmtSendAsync((size_t)_count * 24 + 1)) { _err = CMOZ_ERR_RMT; return false; }
  return true;
}

// ─────────────────────────── effects engine ────────────────────────

bool CMozGlow::setEffect(uint8_t id) {
  CMozLock lk(_mutex);
  if (id >= CMOZ_FX_COUNT) { _err = CMOZ_ERR_BAD_EFFECT; return false; }
  _fx = id;
  _t0 = millis();
  _step = 0;
  _nextFrameAt = 0;
  _mChar = _mElem = 0; _mOn = false; _mNextAt = 0;
  for (auto &f : _flies) f.alive = false;
  if (_begun) { clear(); memset(_heat, 0, _count); }
  return true;
}

bool CMozGlow::setEffectSpeed(uint8_t s) {
  if (s < 1 || s > 10) { _err = CMOZ_ERR_BAD_ARG; return false; }
  _speed = s;
  return true;
}

bool CMozGlow::setMorseMessage(const char* msg) {
  if (!msg || !*msg || strlen(msg) > 63) { _err = CMOZ_ERR_BAD_ARG; return false; }
  for (const char* c = msg; *c; c++) {
    char u = toupper(*c);
    if (!(u == ' ' || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9'))) {
      _err = CMOZ_ERR_BAD_ARG; return false;   // letters, digits, spaces only
    }
  }
  CMozLock lk(_mutex);
  strncpy(_mMsg, msg, 63); _mMsg[63] = 0;
  _mChar = _mElem = 0; _mOn = false; _mNextAt = 0;
  return true;
}

const char* CMozGlow::effectName(uint8_t id) {
  static const char* names[CMOZ_FX_COUNT] = {
    "Solid", "Sequin", "Catwalk", "Loom", "Heartbeat",
    "Firefly", "Silk", "Ember", "Aurora", "Morse"
  };
  return (id < CMOZ_FX_COUNT) ? names[id] : "?";
}

uint16_t CMozGlow::frameInterval() const {
  static const uint16_t base[CMOZ_FX_COUNT] =
  //  Solid Sequin Catwalk Loom Heart Fly Silk Ember Aurora Morse
    { 200,  45,    35,     120, 20,   30, 30,  45,   35,    10 };
  uint32_t iv = (uint32_t)base[_fx] * (13 - _speed) / 8;  // speed 1..10
  return (iv < 5) ? 5 : (uint16_t)iv;
}

bool CMozGlow::update() {
  CMozLock lk(_mutex);
  if (!_begun) { _err = CMOZ_ERR_NOT_BEGUN; return false; }
  uint32_t now = millis();
  if (now < _nextFrameAt) return false;
  _nextFrameAt = now + frameInterval();

  switch (_fx) {
    case CMOZ_FX_SOLID:     fxSolid();     break;
    case CMOZ_FX_SEQUIN:    fxSequin();    break;
    case CMOZ_FX_CATWALK:   fxCatwalk();   break;
    case CMOZ_FX_LOOM:      fxLoom();      break;
    case CMOZ_FX_HEARTBEAT: fxHeartbeat(); break;
    case CMOZ_FX_FIREFLY:   fxFirefly();   break;
    case CMOZ_FX_SILK:      fxSilk();      break;
    case CMOZ_FX_EMBER:     fxEmber();     break;
    case CMOZ_FX_AURORA:    fxAurora();    break;
    case CMOZ_FX_MORSE:     fxMorse();     break;
  }
  _step++;
  return show();
}

// ─────────────────────────── AutoPilot ─────────────────────────────
/*
  The LEDs get their own CPU core. Arduino's loop() runs on core 1, so
  the AutoPilot task is pinned to core 0. It renders on schedule and
  sleeps between frames — it never spins. Every public API takes the
  same recursive mutex, so your sketch can keep calling setEffect(),
  setPixel(), setBrightness() etc. while AutoPilot flies.
*/

void CMozGlow::taskThunk(void* self) {
  static_cast<CMozGlow*>(self)->taskLoop();
}

void CMozGlow::taskLoop() {
  while (_autoRun) {
    update();                              // takes the mutex internally
    uint32_t now  = millis();
    uint32_t next = _nextFrameAt;          // benign unlocked read
    uint32_t wait = (next > now) ? (next - now) : 1;
    if (wait > 20) wait = 20;              // stay responsive to landing
    vTaskDelay(pdMS_TO_TICKS(wait));
  }
  _task = nullptr;                         // announce clean exit…
  vTaskDelete(NULL);                       // …then vanish
}

bool CMozGlow::autoUpdate(bool enable) {
  if (enable) {
    if (_task)   return true;              // already flying
    if (!_begun) { _err = CMOZ_ERR_NOT_BEGUN; return false; }

    _autoRun = true;
    TaskHandle_t h = nullptr;
    if (xTaskCreatePinnedToCore(taskThunk, "CMozGlow", CMOZ_AUTOPILOT_STACK,
                                this, 1, &h, CMOZ_AUTOPILOT_CORE) != pdPASS) {
      _autoRun = false;
      _err = CMOZ_ERR_TASK;
      return false;
    }
    _task = h;
    return true;
  }

  // land: signal, then wait for the task to confirm it's gone. We never
  // force-delete — a task killed while holding the mutex would deadlock
  // every later call, which is far worse than reporting a timeout.
  if (!_task) return true;
  _autoRun = false;
  uint32_t t0 = millis();
  while (_task && (millis() - t0) < 500) delay(1);
  if (_task) { _err = CMOZ_ERR_TASK; return false; }
  return true;
}

void CMozGlow::fadeAll(uint8_t keep256) {
  for (uint32_t n = 0; n < (uint32_t)_count * 3; n++)
    _pix[n] = (uint8_t)(((uint16_t)_pix[n] * keep256) >> 8);
}

// ─────────────────────────── colour helper ─────────────────────────

uint32_t CMozGlow::Wheel(uint8_t pos) {
  pos = 255 - pos;
  if (pos < 85)  return Color(255 - pos * 3, 0, pos * 3);
  if (pos < 170) { pos -= 85;  return Color(0, pos * 3, 255 - pos * 3); }
  pos -= 170;    return Color(pos * 3, 255 - pos * 3, 0);
}

// ─────────────────────────── the effects ───────────────────────────
// All renderers run under the mutex (update() holds it) and only touch
// _pix via setPixel/fill, so bounds are always checked.

void CMozGlow::fxSolid() { fill(_fxColor); }

// Sequin — everything slowly dims while random pixels flash to a bright
// glint (colour blended toward white), like sequins catching stage light.
void CMozGlow::fxSequin() {
  fadeAll(225);
  uint8_t sparks = 1 + _count / 24;
  uint8_t r = (_fxColor >> 16) & 0xFF, g = (_fxColor >> 8) & 0xFF, b = _fxColor & 0xFF;
  for (uint8_t s = 0; s < sparks; s++) {
    if (random(100) < 45) {
      uint16_t i = random(_count);
      setPixel(i, r | 0xB0, g | 0xB0, b | 0xB0);   // glint = colour pushed to white
    }
  }
}

// Catwalk — a bold head sweeps end-to-end and back with a fading train.
void CMozGlow::fxCatwalk() {
  fadeAll(200);
  uint16_t span = (_count > 1) ? (_count - 1) * 2 : 1;
  uint16_t s = _step % span;
  uint16_t head = (s < _count) ? s : span - s;
  setPixel(head, _fxColor);
  if (head + 1 < _count) setPixel(head + 1,
      (_fxColor >> 1) & 0x7F7F7F);     // half-bright shoulder
}

// Loom — two "threads" weave across each other, swapping over time.
void CMozGlow::fxLoom() {
  uint32_t a = _fxColor;
  uint32_t b = Color((a) & 0xFF, (a >> 16) & 0xFF, (a >> 8) & 0xFF); // rotated hue
  for (uint16_t i = 0; i < _count; i++)
    setPixel(i, (((i + _step) / 3) & 1) ? a : b);
}

// Heartbeat — a real lub-DUB: strong beat, short gap, softer beat, rest.
void CMozGlow::fxHeartbeat() {
  uint32_t cycle = 1500;
  uint32_t t = (millis() - _t0) % cycle;
  uint16_t e = 0;
  if      (t < 150)              e = 255 - (t * 255) / 150;          // lub
  else if (t >= 250 && t < 420)  e = 170 - ((t - 250) * 170) / 170;  // dub
  uint8_t r = (((_fxColor >> 16) & 0xFF) * e) >> 8;
  uint8_t g = (((_fxColor >> 8)  & 0xFF) * e) >> 8;
  uint8_t b = (((_fxColor)       & 0xFF) * e) >> 8;
  for (uint16_t i = 0; i < _count; i++) setPixel(i, r, g, b);
}

// Firefly — a handful of soft lights breathe in and out at random spots.
void CMozGlow::fxFirefly() {
  clear();
  for (auto &f : _flies) {
    if (!f.alive && random(100) < 6) {
      f.alive = true; f.pos = random(_count);
      f.phase = 0;    f.rate = 2 + random(5);
    }
    if (f.alive) {
      f.phase += f.rate * 40;
      if (f.phase >= 32768) { f.alive = false; continue; }
      uint16_t ph = f.phase >> 6;                    // 0..511
      uint8_t  e  = (ph < 256) ? ph : 511 - ph;      // triangle envelope
      uint8_t r = (((_fxColor >> 16) & 0xFF) * e) >> 8;
      uint8_t g = (((_fxColor >> 8)  & 0xFF) * e) >> 8;
      uint8_t b = (((_fxColor)       & 0xFF) * e) >> 8;
      setPixel(f.pos, r, g, b);
    }
  }
}

// Silk — a slow, smooth sheen rolls along the strip like light on fabric.
void CMozGlow::fxSilk() {
  float t = (millis() - _t0) * 0.0025f;
  for (uint16_t i = 0; i < _count; i++) {
    float s = 0.62f + 0.38f * sinf(i * 0.45f + t);   // never fully dark
    uint8_t r = (uint8_t)(((_fxColor >> 16) & 0xFF) * s);
    uint8_t g = (uint8_t)(((_fxColor >> 8)  & 0xFF) * s);
    uint8_t b = (uint8_t)(((_fxColor)       & 0xFF) * s);
    setPixel(i, r, g, b);
  }
}

// Ember — every pixel does a gentle random walk through campfire colours.
void CMozGlow::fxEmber() {
  for (uint16_t i = 0; i < _count; i++) {
    int16_t h = _heat[i] + random(-14, 15);
    if (h < 40)  h = 40;
    if (h > 255) h = 255;
    _heat[i] = (uint8_t)h;
    setPixel(i, h, (h * 78) >> 8, (h > 200) ? (h - 200) / 4 : 0);
  }
}

// Aurora — drifting curtains of green, teal and violet. Oh, Canada.
void CMozGlow::fxAurora() {
  float t = (millis() - _t0) * 0.001f;
  for (uint16_t i = 0; i < _count; i++) {
    float w1 = sinf(i * 0.30f + t * 1.1f);
    float w2 = sinf(i * 0.13f - t * 0.7f + 1.7f);
    float v  = (w1 + w2) * 0.25f + 0.5f;             // 0..1
    uint8_t g = (uint8_t)(30 + 170 * v);
    uint8_t b = (uint8_t)(20 + 120 * (1.0f - v) * v * 4.0f * 0.6f);
    uint8_t r = (v < 0.25f) ? (uint8_t)(90 * (0.25f - v) * 4.0f) : 0; // violet lows
    setPixel(i, r, g, b);
  }
}

// Morse — the whole strip blinks a real message. Dot = 1 unit, dash = 3.
void CMozGlow::fxMorse() {
  static const char* CODE[36] = {
    ".-","-...","-.-.","-..",".","..-.","--.","....","..",".---",      // A-J
    "-.-",".-..","--","-.","---",".--.","--.-",".-.","...","-",        // K-T
    "..-","...-",".--","-..-","-.--","--..",                           // U-Z
    "-----",".----","..---","...--","....-",".....",                   // 0-5
    "-....","--...","---..","----."                                    // 6-9
  };
  uint32_t now = millis();
  if (now < _mNextAt) return;

  uint16_t unit = 60 + (10 - _speed) * 30;           // ms per Morse unit

  char c = _mMsg[_mChar];
  if (c == 0) {                                      // end of message
    fill(0); _mOn = false; _mChar = 0; _mElem = 0;
    _mNextAt = now + unit * 10;                      // pause, then repeat
    return;
  }
  c = toupper(c);
  if (c == ' ') {                                    // word gap = 7 units
    fill(0); _mChar++; _mElem = 0;
    _mNextAt = now + unit * 7;
    return;
  }
  const char* pat = (c >= 'A') ? CODE[c - 'A'] : CODE[26 + (c - '0')];

  if (!_mOn) {                                       // turn element on
    char e = pat[_mElem];
    fill(_fxColor);
    _mOn = true;
    _mNextAt = now + unit * ((e == '-') ? 3 : 1);
  } else {                                           // element off / gaps
    fill(0);
    _mOn = false;
    _mElem++;
    if (pat[_mElem] == 0) { _mElem = 0; _mChar++; _mNextAt = now + unit * 3; }
    else                  { _mNextAt = now + unit; }
  }
}
