/*
  CMozGlow_Morse — blink a secret message in real Morse code
  -----------------------------------------------------------
  Great for badges, cosplay and classroom challenges: can your
  friends decode what your jacket is saying?

  Letters A-Z, digits 0-9 and spaces are allowed (up to 63 characters).
*/

#include <CMozGlow.h>

// ══════════════════ CHANGE THESE ══════════════════
const char*    MESSAGE  = "HELLO WORLD";
const uint16_t NUM_LEDS = 1;      // onboard pixel only — perfect for badges
const uint8_t  SPEED    = 6;      // higher = faster Morse
const uint32_t COLOR    = CMozGlow::Color(0, 160, 255);
const uint8_t  LED_PIN  = 3;      // change for other boards
// ══════════════════════════════════════════════════

CMozGlow glow(NUM_LEDS, LED_PIN);

void setup() {
  Serial.begin(115200);

  if (!glow.begin()) {
    Serial.print("CMozGlow couldn't start: ");
    Serial.println(glow.errorText());
    while (true) delay(1000);
  }

  if (!glow.setMorseMessage(MESSAGE)) {
    Serial.print("Bad message: ");
    Serial.println(glow.errorText());   // e.g. punctuation isn't allowed
  }

  glow.setEffect(CMOZ_FX_MORSE);
  glow.setEffectColor(COLOR);
  glow.setEffectSpeed(SPEED);
}

void loop() {
  glow.update();
}
