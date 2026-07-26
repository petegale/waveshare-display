#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lvgl.h>
#include <Wire.h>
#include <esp_heap_caps.h>

#include "config.h"
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
            cfg.freq_write  = 16000000;

            cfg.hsync_polarity    = 0;
            cfg.hsync_front_porch = 8;
            cfg.hsync_pulse_width = 4;
            cfg.hsync_back_porch  = 8;
            cfg.vsync_polarity    = 0;
            cfg.vsync_front_porch = 8;
            cfg.vsync_pulse_width = 4;
            cfg.vsync_back_porch  = 8;
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

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    lcd.startWrite();
    lcd.setAddrWindow(area->x1, area->y1, w, h);
    lcd.pushPixels((uint16_t *)color_p, w * h, true);
    lcd.endWrite();

    lv_disp_flush_ready(drv);
}

#define GT911_ADDR 0x5D

static bool gt911_read_point(int16_t *x, int16_t *y) {
    Wire.beginTransmission(GT911_ADDR);
    Wire.write(0x81);
    Wire.write(0x4E);
    if (Wire.endTransmission(false) != 0) return false;

    Wire.requestFrom(GT911_ADDR, 1);
    if (Wire.available() < 1) return false;
    uint8_t status = Wire.read();
    if ((status & 0x80) == 0 || (status & 0x0F) == 0) {
        Wire.beginTransmission(GT911_ADDR);
        Wire.write(0x81);
        Wire.write(0x4E);
        Wire.write((uint8_t)0);
        Wire.endTransmission();
        return false;
    }

    Wire.beginTransmission(GT911_ADDR);
    Wire.write(0x81);
    Wire.write(0x50);
    Wire.endTransmission(false);
    Wire.requestFrom(GT911_ADDR, 4);
    if (Wire.available() < 4) return false;

    uint8_t xl = Wire.read();
    uint8_t xh = Wire.read();
    uint8_t yl = Wire.read();
    uint8_t yh = Wire.read();

    *x = (xh << 8) | xl;
    *y = (yh << 8) | yl;

    Wire.beginTransmission(GT911_ADDR);
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

    lcd.init();
    lcd.setBrightness(255);
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    pinMode(TOUCH_INT, INPUT);

    lv_init();

    const size_t buf_pixels = SCREEN_WIDTH * 40;
    buf1 = (lv_color_t *)heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    buf2 = (lv_color_t *)heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_pixels);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_WIDTH;
    disp_drv.ver_res = SCREEN_HEIGHT;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_read_cb;
    lv_indev_drv_register(&indev_drv);

    display_init();
    espnow_init();
}

void loop() {
    lv_timer_handler();
    delay(5);
}
