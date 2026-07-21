/*
  CMozGlow_AutoPilot — give your LEDs their own CPU core (advanced)
  ------------------------------------------------------------------
  The ESP32-S3 has TWO processor cores. Your sketch's loop() runs on
  core 1. With one line —

      glow.autoUpdate(true);

  — CMozGlow starts a background task on core 0 that animates the LEDs
  forever. Your loop() never has to think about them again: sensors,
  Bluetooth, touch, servos... all run completely undisturbed.

  PROOF IT WORKS: this sketch counts loop() laps per second and prints
  the number. Watch it stay huge (hundreds of thousands) while a full
  animation plays. Comment out autoUpdate(true) and uncomment the
  glow.update() line in loop() to compare with manual mode.

  Everything stays thread-safe: you can call setEffect(), setPixel(),
  setBrightness() and friends from loop() any time while AutoPilot
  flies. Call glow.autoUpdate(false) to land it again.
*/

#include <CMozGlow.h>

// ══════════════════ CHANGE THESE ══════════════════
const uint16_t NUM_LEDS = 13;   // onboard + a 12-LED strip on GPIO 3
const uint8_t  SPEED    = 6;
const uint8_t  LED_PIN  = 3;    // change for other boards
// ══════════════════════════════════════════════════

CMozGlow glow(NUM_LEDS, LED_PIN);

uint32_t laps = 0, lastReport = 0;
uint8_t  fx   = CMOZ_FX_AURORA;

void setup() {
  Serial.begin(115200);

  if (!glow.begin()) {
    Serial.print("CMozGlow couldn't start: ");
    Serial.println(glow.errorText());
    while (true) delay(1000);
  }

  glow.setPowerBudgetmA(450);
  glow.setEffect(fx);
  glow.setEffectSpeed(SPEED);

  // ✈️ One line. LEDs move to core 0. loop() is now 100% yours.
  if (!glow.autoUpdate(true)) {
    Serial.print("AutoPilot problem: ");
    Serial.println(glow.errorText());
  }
}

void loop() {
  // glow.update();   // <- NOT needed in AutoPilot mode (try both ways!)

  laps++;                              // pretend this is your sensor code
  uint32_t now = millis();

  if (now - lastReport >= 1000) {      // once a second: report + swap effect
    Serial.print("loop() laps this second: ");
    Serial.print(laps);
    Serial.print("   |   LEDs drawing ~");
    Serial.print(glow.estimatedCurrentmA());
    Serial.println(" mA on their own core");
    laps = 0;
    lastReport = now;
  }

  if ((now / 8000) % 2 == 0 && fx != CMOZ_FX_AURORA) {
    fx = CMOZ_FX_AURORA;  glow.setEffect(fx);   // safe mid-flight!
  } else if ((now / 8000) % 2 == 1 && fx != CMOZ_FX_SEQUIN) {
    fx = CMOZ_FX_SEQUIN;  glow.setEffect(fx);
  }
}
