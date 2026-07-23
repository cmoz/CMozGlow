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
  CMozGlow_ErrorHandling — see the safety features in action
  -----------------------------------------------------------
  This sketch deliberately makes mistakes so you can watch CMozGlow
  catch them and explain them. Open the Serial Monitor at 115200.

  It also shows the two wearable safety features:
    • statusPixel(true)  — the onboard pixel glows green when all is well
                           and red the moment an error is recorded.
    • setPowerBudgetmA() — the library estimates current draw and dims
                           automatically so your battery stays safe.
*/

#include <CMozGlow.h>

const uint16_t NUM_LEDS = 13;   // onboard + a 12-LED strip on GPIO 3

CMozGlow glow(NUM_LEDS);

void report(const char* what) {
  Serial.print(what);
  Serial.print("  ->  ");
  Serial.println(glow.errorText());
  glow.clearError();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n─── CMozGlow error-handling tour ───");

  if (!glow.begin()) {
    report("begin()");
    while (true) delay(1000);
  }
  report("begin()");                         // -> OK

  glow.statusPixel(true);                    // pixel 0 = live status light

  // Mistake 1: writing past the end of the strip
  glow.setPixel(999, 255, 0, 0);
  report("setPixel(999, ...)");              // -> friendly index error

  // Mistake 2: an effect number that doesn't exist
  glow.setEffect(42);
  report("setEffect(42)");                   // -> lists the valid range

  // Mistake 3: punctuation in a Morse message
  glow.setMorseMessage("HI!");
  report("setMorseMessage(\"HI!\")");        // -> explains what's allowed

  // Power budgeting: ask for blinding white on a tiny budget
  glow.setPowerBudgetmA(120);                // pretend we're on a small LiPo
  glow.fill(CMozGlow::Color(255, 255, 255)); // would draw ~700 mA unlimited!
  glow.show();
  Serial.print("Full white requested. Estimated draw now: ");
  Serial.print(glow.estimatedCurrentmA());
  Serial.println(" mA");
  Serial.print("Auto-dimmed to protect the battery? ");
  Serial.println(glow.powerLimited() ? "YES" : "no");

  // Back to something pretty
  glow.setEffect(CMOZ_FX_SILK);
  glow.setEffectColor(CMozGlow::Color(180, 60, 255));
}

void loop() {
  glow.update();
}
