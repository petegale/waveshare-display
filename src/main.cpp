/**
 * main.cpp — Waveshare ESP32-S3-Touch-LCD-4.3B helm display.
 *
 * Always-on LVGL client for the sensor hub: polls over ESP-NOW every few
 * seconds and renders tanks + house battery. Mains powered (7-36 V screw
 * terminal off the boat's 12 V bus), so no sleep management — unlike the
 * battery M5Paper gauge this is a live panel.
 *
 * Init order matters on the B variant: the CH422G expander has to come up
 * first because it owns the LCD reset, touch reset and backlight. The
 * backlight stays off until LVGL has drawn once, so boot never shows a
 * flash of uninitialised framebuffer.
 */

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lvgl.h>
#include <Wire.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "ch422g.h"
#include "display.h"
#include "espnow.h"

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_RGB _panel;
    lgfx::Bus_RGB _bus;

public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.panel = &_panel;

            cfg.pin_d0  = TFT_B0;
            cfg.pin_d1  = TFT_B1;
            cfg.pin_d2  = TFT_B2;
            cfg.pin_d3  = TFT_B3;
            cfg.pin_d4  = TFT_B4;
            cfg.pin_d5  = TFT_G0;
            cfg.pin_d6  = TFT_G1;
            cfg.pin_d7  = TFT_G2;
            cfg.pin_d8  = TFT_G3;
            cfg.pin_d9  = TFT_G4;
            cfg.pin_d10 = TFT_G5;
            cfg.pin_d11 = TFT_R0;
            cfg.pin_d12 = TFT_R1;
            cfg.pin_d13 = TFT_R2;
            cfg.pin_d14 = TFT_R3;
            cfg.pin_d15 = TFT_R4;

            cfg.pin_henable = TFT_DE;
            cfg.pin_vsync   = TFT_VSYNC;
            cfg.pin_hsync   = TFT_HSYNC;
            cfg.pin_pclk    = TFT_PCLK;
            cfg.freq_write  = TFT_PCLK_HZ;

            cfg.hsync_polarity    = 0;
            cfg.hsync_front_porch = TFT_HSYNC_FRONT;
            cfg.hsync_pulse_width = TFT_HSYNC_PULSE;
            cfg.hsync_back_porch  = TFT_HSYNC_BACK;
            cfg.vsync_polarity    = 0;
            cfg.vsync_front_porch = TFT_VSYNC_FRONT;
            cfg.vsync_pulse_width = TFT_VSYNC_PULSE;
            cfg.vsync_back_porch  = TFT_VSYNC_BACK;
            cfg.pclk_active_neg   = 1;
            cfg.de_idle_high      = 0;
            cfg.pclk_idle_high    = 0;

            _bus.config(cfg);
        }
        {
            auto cfg = _panel.config();
            cfg.memory_width  = SCREEN_WIDTH;
            cfg.memory_height = SCREEN_HEIGHT;
            cfg.panel_width   = SCREEN_WIDTH;
            cfg.panel_height  = SCREEN_HEIGHT;
            _panel.config(cfg);
        }
        _panel.setBus(&_bus);
        setPanel(&_panel);
    }
};

static LGFX lcd;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1;
static lv_color_t *buf2;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

static uint8_t s_touchAddr = GT911_ADDR_PRIMARY;

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    lcd.startWrite();
    lcd.setAddrWindow(area->x1, area->y1, w, h);
    lcd.pushPixels((uint16_t *)color_p, w * h, true);
    lcd.endWrite();

    lv_disp_flush_ready(drv);
}

// ─── GT911 ──────────────────────────────────────────────────────────────────
static bool gt911_probe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

static bool gt911_read_point(int16_t *x, int16_t *y) {
    Wire.beginTransmission(s_touchAddr);
    Wire.write(0x81);
    Wire.write(0x4E);
    if (Wire.endTransmission(false) != 0) return false;

    Wire.requestFrom((int)s_touchAddr, 1);
    if (Wire.available() < 1) return false;
    uint8_t status = Wire.read();
    if ((status & 0x80) == 0 || (status & 0x0F) == 0) {
        Wire.beginTransmission(s_touchAddr);
        Wire.write(0x81);
        Wire.write(0x4E);
        Wire.write((uint8_t)0);
        Wire.endTransmission();
        return false;
    }

    Wire.beginTransmission(s_touchAddr);
    Wire.write(0x81);
    Wire.write(0x50);
    Wire.endTransmission(false);
    Wire.requestFrom((int)s_touchAddr, 4);
    if (Wire.available() < 4) return false;

    uint8_t xl = Wire.read();
    uint8_t xh = Wire.read();
    uint8_t yl = Wire.read();
    uint8_t yh = Wire.read();

    *x = (xh << 8) | xl;
    *y = (yh << 8) | yl;

    Wire.beginTransmission(s_touchAddr);
    Wire.write(0x81);
    Wire.write(0x4E);
    Wire.write((uint8_t)0);
    Wire.endTransmission();

    return true;
}

static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    int16_t x, y;
    if (gt911_read_point(&x, &y)) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = x;
        data->point.y = y;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n— waveshare-display boot —");

    // 1. Shared I2C bus, then the expander that owns the reset/backlight
    //    lines. Everything else depends on this.
    Wire.begin(I2C_SDA, I2C_SCL);
    // 400 kHz: the touch controller is polled from the LVGL input
    // callback, so a slow bus turns into CPU time stolen from rendering.
    Wire.setClock(400000);
    if (!ch422g_init()) {
        Serial.println("CH422G not responding — check I2C (backlight will stay off)");
    }

    // 2. Touch out of reset (also selects its I2C address).
    ch422g_touch_reset();
    if (!gt911_probe(s_touchAddr)) {
        uint8_t alt = GT911_ADDR_SECONDARY;
        if (gt911_probe(alt)) s_touchAddr = alt;
        else Serial.println("GT911 not found at 0x5D or 0x14");
    }
    Serial.printf("Touch at 0x%02X\n", s_touchAddr);

    // 3. Panel.
    lcd.init();

    // 4. LVGL.
    //
    // Draw buffers go in INTERNAL DMA RAM, not PSRAM. The RGB panel
    // streams its framebuffer out of PSRAM continuously; putting the
    // draw buffers there too makes every redraw contend with the panel
    // refresh for the same bus, starving the line buffer and showing up
    // as intermittent flicker.
    //
    // 32 lines is the most we can take from internal RAM while leaving
    // comfortable headroom for the WiFi stack. Each flush carries fixed
    // overhead, so bigger chunks mean a full-screen repaint costs 15
    // flushes rather than 24.
    lv_init();
    const size_t buf_pixels = SCREEN_WIDTH * 32;
    const size_t buf_bytes  = buf_pixels * sizeof(lv_color_t);
    buf1 = (lv_color_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    buf2 = (lv_color_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf1 || !buf2) {
        // Fall back to PSRAM rather than refusing to boot — a flickery
        // display still beats a blank one.
        Serial.println("Internal draw buffers unavailable — falling back to PSRAM");
        if (buf1) heap_caps_free(buf1);
        if (buf2) heap_caps_free(buf2);
        buf1 = (lv_color_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
        buf2 = (lv_color_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_pixels);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = SCREEN_WIDTH;
    disp_drv.ver_res  = SCREEN_HEIGHT;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_read_cb;
    lv_indev_drv_register(&indev_drv);

    // 5. Build the UI and draw it once *before* the backlight comes on,
    //    so the first thing the user sees is the finished screen.
    display_init();
    lv_timer_handler();
    ch422g_backlight(true);

    espnow_init();
    Serial.println("Setup complete");
}

void loop() {
    lv_timer_handler();
    espnow_tick();
    // 1 ms rather than 5: during a full-screen repaint the renderer wants
    // every slice of CPU it can get, and both ticks below are cheap
    // no-ops between their intervals.
    delay(1);
}
