#pragma once
#include "remote_protocol.h"
#include "espnow.h"

void display_init();

// Push a fresh hub state onto the screen.
void display_update(const display_state_t &state);

// Update the header's link indicator. ageSeconds is how long since the
// last state response (ignored when state == LINK_SEARCHING).
void display_set_link(link_state_t state, uint32_t ageSeconds);
