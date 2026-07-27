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

// Drive probing / polling. Call from loop().
void espnow_tick();

link_state_t espnow_link_state();

// Seconds since the last state response (0 if never).
uint32_t espnow_seconds_since_state();
