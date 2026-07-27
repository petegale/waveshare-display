#include "ch422g.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>

// Shadow of the output register — the CH422G is write-only for outputs,
// so we have to track the state ourselves to do read-modify-write.
static uint8_t s_out = 0;

static bool writeReg(uint8_t addr, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static void flush() {
  writeReg(CH422G_ADDR_OUTPUT, s_out);
}

bool ch422g_init() {
  // EXIO0-7 → push-pull outputs.
  if (!writeReg(CH422G_ADDR_MODE, CH422G_MODE_OUTPUT)) return false;

  // Power-on state: both resets released (active low, so driven high),
  // TF card deselected, backlight off until the panel is initialised so
  // the user never sees a flash of garbage framebuffer.
  s_out = CH422G_BIT_TP_RST | CH422G_BIT_LCD_RST | CH422G_BIT_SD_CS;
  flush();
  delay(100);   // Waveshare's documented settle time before panel init
  return true;
}

void ch422g_set(uint8_t bits, bool high) {
  if (high) s_out |= bits;
  else      s_out &= (uint8_t)~bits;
  flush();
}

void ch422g_backlight(bool on) {
  ch422g_set(CH422G_BIT_DISP, on);
}

void ch422g_touch_reset() {
  // The GT911 latches its I2C address from the INT pin at the moment
  // reset is released: INT low → 0x5D, INT high → 0x14. Drive INT low
  // for the whole pulse so we land on the primary address.
  pinMode(TOUCH_INT, OUTPUT);
  digitalWrite(TOUCH_INT, LOW);

  ch422g_set(CH422G_BIT_TP_RST, false);
  delay(20);
  ch422g_set(CH422G_BIT_TP_RST, true);
  delay(60);    // controller boots and samples INT

  // Hand INT back to the driver as an input.
  pinMode(TOUCH_INT, INPUT);
}
