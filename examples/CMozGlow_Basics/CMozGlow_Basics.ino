/*
  --------------------------------------------------------------------
  Christine Farion  ·  http://www.christinefarion.com
  CMozMaker · Tinker Tailor
  https://www.youtube.com/@CMozMaker
  https://www.tinkertailor.ca

  Tinker Tailor is a small but passionate shop dedicated to the wearable
  technology maker community. We provide a curated selection of tools,
  components, and materials like conductive fabrics, circuit boards, and
  more to help creators bring their innovative ideas to life. With a focus
  on education and creativity, we also offer tutorials, resources, and
  expert advice to empower makers at every skill level.
  At Tinker Tailor we believe in making wearable tech accessible, fun,
  and inspiring for everyone!

  More projects in my book:
  https://www.amazon.ca/Ultimate-Informed-Wearable-Technology-hands/dp/1803230592

  Questions about this build? Ask in Discussions —
  https://github.com/cmoz/YouTube/discussions
  Please include your board, IDE, and the error message.
  --------------------------------------------------------------------
*/

/*
  CMozGlow_Basics — your first sketch for the CMoz ESP32-S3 Mini
  ---------------------------------------------------------------
  Change ONE number below and re-upload. That's it!

  ┌────┬───────────┬──────────────────────────────────────────────┐
  │ #  │ Effect    │ What it looks like                           │
  ├────┼───────────┼──────────────────────────────────────────────┤
  │ 0  │ Solid     │ steady colour                                │
  │ 1  │ Sequin    │ random glints, like sequins under stage light│
  │ 2  │ Catwalk   │ a bold sweep back and forth with a fade train│
  │ 3  │ Loom      │ two colours weaving past each other          │
  │ 4  │ Heartbeat │ realistic lub-dub pulse                      │
  │ 5  │ Firefly   │ soft lights breathing in and out             │
  │ 6  │ Silk      │ a slow sheen rolling along the strip         │
  │ 7  │ Ember     │ warm campfire glow                           │
  │ 8  │ Aurora    │ northern lights 🇨🇦                           │
  │ 9  │ Morse     │ blinks a real Morse-code message             │
  └────┴───────────┴──────────────────────────────────────────────┘

  Wiring reminder: the onboard pixel lives on GPIO 3. Solder/clip your
  strip's DIN to GPIO 3 too — the onboard pixel becomes pixel 0 and your
  strip starts at pixel 1. So NUM_LEDS = 1 + your strip length.
*/

#include <CMozGlow.h>

// ══════════════════ CHANGE THESE ══════════════════
const uint8_t  EFFECT   = 8;    // 0-9, see the table above
const uint16_t NUM_LEDS = 1;    // onboard only = 1. With a 12-LED strip = 13
const uint8_t  SPEED    = 5;    // 1 = dreamy ... 10 = party
const uint32_t COLOR    = CMozGlow::Color(255, 42, 120); // R, G, B
const uint8_t  LED_PIN  = 3;    // GPIO 3 = the CMoz onboard pixel.
                                // Using a different board or your own wiring?
                                // Just change this number.
// ══════════════════════════════════════════════════

CMozGlow glow(NUM_LEDS, LED_PIN);

void setup() {
  Serial.begin(115200);

  if (!glow.begin()) {
    // CMozGlow tells you WHAT went wrong, in plain English:
    Serial.print("CMozGlow couldn't start: ");
    Serial.println(glow.errorText());
    while (true) delay(1000);
  }

  glow.setPowerBudgetmA(450);   // stay safe on USB / a small LiPo
  glow.setBrightness(180);

  glow.setEffect(EFFECT);
  glow.setEffectColor(COLOR);
  glow.setEffectSpeed(SPEED);

  // Feeling fancy? Uncomment the next line and delete update() from loop() —
  // the LEDs get their OWN CPU core (see the CMozGlow_AutoPilot example):
  // glow.autoUpdate(true);

  Serial.print("Running effect: ");
  Serial.println(CMozGlow::effectName(EFFECT));
}

void loop() {
  glow.update();                // non-blocking — your own code fits here too!
}
