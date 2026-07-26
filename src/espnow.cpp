#include "espnow.h"
#include "display_state.h"
#include "display.h"
#include <WiFi.h>
#include <esp_now.h>
#include <string.h>

static display_state_t s_display_state;

static void on_espnow_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len != sizeof(display_state_t)) return;
    if (data[1] != DISPLAY_RESP_STATE) return;

    memcpy(&s_display_state, data, sizeof(display_state_t));
    display_update(s_display_state);
}

void espnow_init() {
    WiFi.mode(WIFI_STA);

    if (esp_now_init() != ESP_OK) {
        return;
    }

    esp_now_register_recv_cb(on_espnow_recv);
}
