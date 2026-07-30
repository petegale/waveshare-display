#pragma once
#include <stdint.h>
#include "remote_protocol.h"

/**
 * espnow.h — hub link for the always-on helm display.
 *
 * The hub is a request/response server: it only emits display_state_t in
 * reply to a display_request_t from a paired display, and it only talks to
 * a device it has a pairing record for. So this client must:
 *   probe for the hub's channel → receive the LMK → register an encrypted
 *   peer → poll with display_request_t.
 *
 * Unlike the battery-powered M5Paper this node never sleeps, so it holds
 * the channel once found and simply re-probes if the link goes quiet.
 */

enum link_state_t {
  LINK_SEARCHING,   // scanning channels for the hub
  LINK_UP,          // state responses arriving
  LINK_LOST,        // was up, nothing recently
};

void espnow_init();

// Raw ESP-NOW frames that reached the receive callback, and the last length.
// An encrypted-peer mismatch drops frames below the callback with no error
// anywhere, so counting what actually arrives is the only way to tell "the
// hub is silent" from "the hub is answering and we are discarding it".
uint32_t espnow_rx_count();
int      espnow_rx_last_len();

// Drive probing / polling. Call from loop().
void espnow_tick();

link_state_t espnow_link_state();

// Seconds since the last state response (0 if never).
uint32_t espnow_seconds_since_state();

// ─── Hub history ────────────────────────────────────────────────────────────
// The hub keeps far more than this node can: 24 h at one minute, 30 days at
// 15 minutes and a year at the hour, persisted across reboots. The local
// rings in history.h stay as the fallback — they are what the display has
// when the link is down, and they survive nothing but they cost nothing.

// Ask the hub for a series. Fire and forget; the reply lands asynchronously
// and espnow_history_ready() goes true.
void espnow_history_request(uint8_t metricIdx, uint8_t window, uint8_t nPoints);

// True once a response for the most recent request has arrived.
bool espnow_history_ready();

// Copy the last response's points into out[] as real values, oldest first,
// with HIST_NO_DATA for gaps. Returns the number written.
int espnow_history_values(int16_t* out, int maxOut);

// Seconds each returned point represents, for the time axis.
uint16_t espnow_history_interval_s();
