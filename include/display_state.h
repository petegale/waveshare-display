#pragma once
#include <stdint.h>

// Mirrors display_state_t from sensor_hub/protocol/remote_protocol.h (v0.07).
// Hub -> display, unicast ESP-NOW, 30 bytes. Keep in sync with the hub repo.

#define DISPLAY_REQ_STATE       0x01
#define DISPLAY_RESP_STATE      0x02

#define MAX_DISPLAY_TANKS       2

#define DISPLAY_FLUID_FUEL      0x00
#define DISPLAY_FLUID_WATER     0x01
#define DISPLAY_FLUID_GREY      0x02
#define DISPLAY_FLUID_BLACK     0x03
#define DISPLAY_FLUID_OIL       0x04
#define DISPLAY_FLUID_LIVEWELL  0x05

#define DISPLAY_LEVEL_NO_DATA   0xFF
#define DISPLAY_UTC_DAYS_NONE   0xFFFF

#define DISPLAY_BATT_NO_DATA        0xFF
#define DISPLAY_BATT_FLAG_CHARGING  0x01

typedef struct __attribute__((packed)) {
    uint8_t     protocol_version;
    uint8_t     type;
    uint8_t     battery_pct;
    uint8_t     flags;
    uint8_t     sequence;
    uint8_t     reserved;
} display_request_t;

typedef struct __attribute__((packed)) {
    uint8_t     protocol_version;
    uint8_t     type;
    uint8_t     n_tanks;
    uint8_t     flags;
    uint32_t    hub_uptime_s;
    uint16_t    utc_days;
    float       utc_seconds;
    struct {
        uint8_t fluid_type;
        uint8_t level_pct;
        uint8_t reserved[2];
    } tanks[MAX_DISPLAY_TANKS];
    uint8_t     batt_soc_pct;
    uint8_t     batt_flags;
    int16_t     batt_current_dA;
    uint16_t    batt_voltage_cV;
    uint8_t     reserved[2];
} display_state_t;

#ifdef __cplusplus
static_assert(sizeof(display_state_t) == 30, "display_state_t must stay 30 bytes to match hub wire format");
#endif
