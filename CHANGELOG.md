# Changelog

## 1.2.1 — 2026-07-21
### Changed
- Documentation clarified: any valid GPIO pin can be used as the data pin on your own
  ESP32 board — the capability already existed but wasn't explained.

## 1.2.0 — 2026-07-21
### Added
- **Multi-board support.** CMozGlow now runs on ESP32-S3, ESP32-S2, ESP32-C3 and the
  classic ESP32. Pin validation is chip-aware, with a permissive fallback for future variants.
- `LED_PIN` constant surfaced at the top of every example, so changing the data pin is
  as easy as changing the effect number.
- README section: "Using a different pin — or a different board", with a per-chip pin table.

### Fixed
- **Pin validation rejected valid pins on non-S3 boards.** The check was hardcoded to the
  ESP32-S3 pinout, so `begin()` refused perfectly usable pins (e.g. GPIO 25–27) on the
  classic ESP32 and gave S3-specific advice. Now correct per chip, and the
  `CMOZ_ERR_BAD_PIN` message names the pins that work on *your* chip.
- Classic ESP32 input-only pins (34–39) and flash pins (6–11) are now correctly rejected —
  they physically cannot drive LEDs.

### Changed
- Brand domain corrected to tinkertailor.ca throughout.
- Documentation: Arduino IDE board setup (USB CDC On Boot), and a fix for the Windows
  ctags "cannot open temporary file" error.

## 1.1.0
### Added
- **AutoPilot mode** — `autoUpdate(true)` renders LEDs on a FreeRTOS task pinned to core 0,
  leaving `loop()` completely free. Thread-safe via a recursive mutex; lands cleanly and
  never force-kills a task. New `CMOZ_ERR_TASK` error code.
### Changed
- **Async RMT transmit.** `show()` hands the frame to hardware and returns (~100 µs even on
  long strips) instead of blocking ~30 µs per LED.
- **Latch encoded in hardware** — the ≥280 µs reset gap is appended as an RMT symbol.
  No busy-waiting anywhere in the library.
- **Byte-to-symbol lookup table** plus a per-frame scale+gamma table, for much faster
  frame assembly.

## 1.0.0
- Initial release: ten effects, plain-English error handling, power budgeting,
  status pixel, gamma correction, non-blocking `update()`.
