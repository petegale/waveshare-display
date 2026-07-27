#pragma once

/**
 * Waveshare ESP32-S3-Touch-LCD-4.3B pin map.
 *
 * IMPORTANT — this is the "B" variant, which differs from the plain 4.3:
 * the 4.3" panel consumes nearly every GPIO, so the backlight and both
 * reset lines are NOT on ESP32 pins. They hang off a CH422G I2C IO
 * expander sharing the bus with the GT911 touch controller. Driving a
 * bare GPIO for the backlight (as a generic 4.3 map does) leaves the
 * panel dark.
 *
 * Source: Waveshare ESP32-S3-Touch-LCD-4.3B wiki + schematic.
 */

// ─── RGB timing / control ───────────────────────────────────────────────────
#define TFT_DE      5
#define TFT_VSYNC   3
#define TFT_HSYNC   46
#define TFT_PCLK    7

// ─── RGB data lines (RGB565) ────────────────────────────────────────────────
// LovyanGFX bus order is d0..d4 = B0..B4, d5..d10 = G0..G5, d11..d15 = R0..R4.
#define TFT_R0      1
#define TFT_R1      2
#define TFT_R2      42
#define TFT_R3      41
#define TFT_R4      40

#define TFT_G0      39
#define TFT_G1      0
#define TFT_G2      45
#define TFT_G3      48
#define TFT_G4      47
#define TFT_G5      21

#define TFT_B0      14
#define TFT_B1      38
#define TFT_B2      18
#define TFT_B3      17
#define TFT_B4      10

// ─── Panel timing ───────────────────────────────────────────────────────────
// If the image shimmers or sits offset, these are the numbers to nudge.
#define TFT_PCLK_HZ         16000000
#define TFT_HSYNC_PULSE     8
#define TFT_HSYNC_BACK      16
#define TFT_HSYNC_FRONT     16
#define TFT_VSYNC_PULSE     8
#define TFT_VSYNC_BACK      16
#define TFT_VSYNC_FRONT     16

// ─── Shared I2C bus (CH422G expander + GT911 touch) ─────────────────────────
#define I2C_SDA     8
#define I2C_SCL     9
#define TOUCH_INT   4

// GT911 samples its INT line while reset is released to choose its address:
// INT low → 0x5D. We drive that sequence explicitly, but probe both.
#define GT911_ADDR_PRIMARY    0x5D
#define GT911_ADDR_SECONDARY  0x14

// ─── CH422G IO expander ─────────────────────────────────────────────────────
// The chip is addressed by register: a one-byte write to I2C address 0x24
// sets the system config, and to 0x38 sets the 8 output bits.
#define CH422G_ADDR_MODE    0x24
#define CH422G_ADDR_OUTPUT  0x38
#define CH422G_MODE_OUTPUT  0x01    // EXIO0-7 as push-pull outputs

#define CH422G_BIT_TP_RST   (1 << 1)   // EXIO1 — GT911 reset (active low)
#define CH422G_BIT_DISP     (1 << 2)   // EXIO2 — LCD backlight / DISP enable
#define CH422G_BIT_LCD_RST  (1 << 3)   // EXIO3 — LCD reset (active low)
#define CH422G_BIT_SD_CS    (1 << 4)   // EXIO4 — TF card CS (active low, idle high)

#define SCREEN_WIDTH  800
#define SCREEN_HEIGHT 480

// ─── Hub link ───────────────────────────────────────────────────────────────
// Mains-powered display: poll continuously rather than the M5Paper's
// 15-minute deep-sleep cadence. The hub answers every request, so this is
// only bounded by how fresh we want the numbers.
#define POLL_INTERVAL_MS      5000UL
// No state response for this long → show the link as lost.
#define LINK_TIMEOUT_MS       30000UL
#define PROBE_LISTEN_MS       120UL
