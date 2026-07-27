#pragma once
#include <stdint.h>

/**
 * ch422g.h — CH422G I2C IO expander (Waveshare 4.3B).
 *
 * Owns the LCD backlight, LCD reset and touch reset lines. Wire.begin()
 * must have run first — this shares the touch controller's bus.
 */

// Put EXIO0-7 into output mode and drive the board's power-on state:
// resets released, TF card deselected, backlight off. Returns false if
// the expander doesn't ACK (wrong board / bus not up).
bool ch422g_init();

// Set or clear one or more CH422G_BIT_* lines.
void ch422g_set(uint8_t bits, bool high);

// Backlight / DISP enable.
void ch422g_backlight(bool on);

// Pulse the GT911 reset line low then high, holding TOUCH_INT low
// throughout so the controller latches address 0x5D.
void ch422g_touch_reset();
