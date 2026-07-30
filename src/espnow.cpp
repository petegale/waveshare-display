/**
 * espnow.cpp — see header for the link model.
 *
 * Mirrors the proven sensor-display client, minus the deep-sleep power
 * management: channel probe → probe_response_t (carries the LMK) →
 * encrypted peer → periodic display_request_t.
 *
 * Pairing (hub MAC + LMK) is persisted in NVS so a power cycle doesn't
 * need the user to re-open pairing mode on the hub. A zero LMK means the
 * hub asked for an unencrypted link (e.g. the bench simulator).
 */

#include "espnow.h"
#include "config.h"
#include "config.h"
#include "display.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <atomic>
#include <string.h>

static uint8_t  s_hubMac[6]  = {};
static uint8_t  s_hubLmk[16] = {};
static bool     s_paired     = false;
static uint8_t  s_channel    = 0;

static std::atomic<bool> s_gotProbe{false};
static std::atomic<bool> s_gotState{false};
static std::atomic<bool> s_gotUnpair{false};

static display_state_t s_state = {};

// Last history response, and whether it is newer than the last request.
// One in flight at a time: the UI shows one chart, and a second request
// simply supersedes the first.
static history_response_t s_hist = {};
static volatile bool      s_histReady = false;
static uint8_t            s_histSeq = 0;
static uint32_t s_lastStateMs = 0;
static uint32_t s_lastPollMs  = 0;
static uint8_t  s_seq         = 0;
static bool     s_everUp      = false;

// ─── NVS ────────────────────────────────────────────────────────────────────
static void loadPairing() {
  Preferences p;
  p.begin("display", true);
  s_paired = p.getBool("paired", false);
  if (s_paired) {
    p.getBytes("hubMac", s_hubMac, 6);
    p.getBytes("hubLmk", s_hubLmk, 16);
    s_channel = p.getUChar("chan", 0);
  }
  p.end();
}

static void savePairing() {
  Preferences p;
  p.begin("display", false);
  p.putBool("paired", true);
  p.putBytes("hubMac", s_hubMac, 6);
  p.putBytes("hubLmk", s_hubLmk, 16);
  p.putUChar("chan", s_channel);
  p.end();
}

static void clearPairing() {
  Preferences p;
  p.begin("display", false);
  p.clear();
  p.end();
  // Delete the peer BEFORE forgetting the MAC. Zeroing s_hubMac first left
  // the hub registered as an encrypted peer with nothing left pointing at it:
  // unrecoverable, because every later attempt to clean it up no longer knew
  // which address to remove. The display then discarded every unencrypted
  // probe response the hub sent and reported "Hub not found on any channel"
  // forever, with the hub visibly answering every probe.
  uint8_t zero[6] = {};
  if (memcmp(s_hubMac, zero, 6) != 0 && esp_now_is_peer_exist(s_hubMac)) {
    esp_now_del_peer(s_hubMac);
  }
  s_paired  = false;
  s_channel = 0;
  memset(s_hubMac, 0, 6);
  memset(s_hubLmk, 0, 16);
}

// Drop every non-broadcast peer. Used before a channel sweep: probe responses
// arrive unencrypted, so any encrypted registration left over from a previous
// link silently swallows them. Walking the table rather than trusting
// s_hubMac means this still works when the stored MAC has already been lost.
static void dropUnicastPeers() {
  esp_now_peer_info_t info;
  static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  uint8_t doomed[8][6];
  int n = 0;
  if (esp_now_fetch_peer(true, &info) == ESP_OK) {
    do {
      if (memcmp(info.peer_addr, bcast, 6) != 0 && n < 8)
        memcpy(doomed[n++], info.peer_addr, 6);
    } while (esp_now_fetch_peer(false, &info) == ESP_OK);
  }
  // Delete after walking: removing entries mid-iteration invalidates the walk.
  for (int i = 0; i < n; i++) esp_now_del_peer(doomed[i]);
}

// ─── RX (NimBLE-free; runs on the WiFi task) ────────────────────────────────
static volatile bool s_rxStateLog = false;
static volatile bool s_histLog = false;
static volatile uint32_t s_rxTotal = 0;
static volatile int      s_rxLastLen = -1;

uint32_t espnow_rx_count()    { return s_rxTotal; }
int      espnow_rx_last_len() { return s_rxLastLen; }

static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (!info || !data) return;
  s_rxTotal++;
  s_rxLastLen = len;

  if (len == (int)sizeof(probe_response_t)) {
    const probe_response_t* p = (const probe_response_t*)data;
    if (p->magic[0] != PROBE_MAGIC_0 || p->magic[1] != PROBE_MAGIC_1) return;
    if (p->type != PROBE_TYPE_RESPONSE) return;
    memcpy(s_hubMac, info->src_addr, 6);
    memcpy(s_hubLmk, p->lmk, 16);
    s_channel = p->channel;
    s_gotProbe.store(true);
    return;
  }

  if (len == (int)sizeof(display_state_t)) {
    const display_state_t* s = (const display_state_t*)data;
    if (s->protocol_version != PROTOCOL_VERSION) return;
    if (s->type != DISPLAY_RESP_STATE) return;
    memcpy(&s_state, s, sizeof(s_state));
    s_gotState.store(true);
    s_rxStateLog = true;
    return;
  }

  if (len == (int)sizeof(history_response_t)) {
    const history_response_t* h = (const history_response_t*)data;
    if (h->magic[0] != PROBE_MAGIC_0 || h->magic[1] != PROBE_MAGIC_1) return;
    if (h->type != HIST_MSG_RESPONSE) return;
    memcpy(&s_hist, h, sizeof(s_hist));
    s_histReady = true;
    s_histLog   = true;
    return;
  }

  if (len == (int)sizeof(hub_response_t)) {
    const hub_response_t* h = (const hub_response_t*)data;
    if (h->magic[0] != PROBE_MAGIC_0 || h->magic[1] != PROBE_MAGIC_1) return;
    if (h->flags & HUB_RESP_FLAG_UNPAIR) s_gotUnpair.store(true);
    return;
  }
}

// ─── Peers ──────────────────────────────────────────────────────────────────
static void setPeer(const uint8_t* mac, uint8_t ch, const uint8_t* lmk) {
  if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = ch;
  peer.ifidx   = WIFI_IF_STA;
  bool hasLmk = false;
  if (lmk) for (int i = 0; i < 16; i++) if (lmk[i]) { hasLmk = true; break; }
  peer.encrypt = hasLmk;
  if (hasLmk) memcpy(peer.lmk, lmk, 16);
  esp_now_add_peer(&peer);
}

// ─── Probe ──────────────────────────────────────────────────────────────────
// Sweeps the channel range looking for the hub, starting at the last known
// channel so a re-probe after a dropout is usually a single hop.
//
// One channel per tick rather than a blocking sweep: at 13 channels ×
// PROBE_LISTEN_MS a blocking scan would stall lv_timer_handler() for over
// a second every poll, freezing the UI whenever the hub is unreachable —
// exactly when the user is most likely to be looking at it.
static bool     s_scanning   = false;
static int      s_scanTried  = 0;
static uint8_t  s_scanCh     = 0;
static uint32_t s_scanStepAt = 0;

static void scanNextChannel() {
  static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  const int nCh = WIFI_CHANNEL_MAX - WIFI_CHANNEL_MIN + 1;

  if (s_scanTried >= nCh) {           // full sweep with no answer
    s_scanning  = false;
    s_scanTried = 0;
    Serial.println("Hub not found on any channel");
    return;
  }

  s_scanCh = (s_scanTried == 0)
             ? (s_channel ? s_channel : DEFAULT_WIFI_CHANNEL)
             : WIFI_CHANNEL_MIN + ((s_scanCh - WIFI_CHANNEL_MIN + 1) % nCh);
  s_scanTried++;

  esp_wifi_set_channel(s_scanCh, WIFI_SECOND_CHAN_NONE);
  setPeer(bcast, s_scanCh, nullptr);

  channel_probe_t pkt = {};
  pkt.magic[0] = PROBE_MAGIC_0;
  pkt.magic[1] = PROBE_MAGIC_1;
  pkt.type     = PROBE_TYPE_REQUEST;
  pkt.channel  = 0;
  esp_now_send(bcast, (const uint8_t*)&pkt, sizeof(pkt));

  s_scanStepAt = millis();
}

// Called from the tick when the RX callback has flagged a probe response.
static void lockOntoHub() {
  esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
  setPeer(s_hubMac, s_channel, s_hubLmk);
  s_paired    = true;
  s_scanning  = false;
  s_scanTried = 0;
  savePairing();
  Serial.printf("Hub found on ch%d (%02X:%02X:%02X:%02X:%02X:%02X)\n",
                s_channel, s_hubMac[0], s_hubMac[1], s_hubMac[2],
                s_hubMac[3], s_hubMac[4], s_hubMac[5]);
}

static void sendRequest() {
  display_request_t req = {};
  req.protocol_version = PROTOCOL_VERSION;
  req.type             = DISPLAY_REQ_STATE;
  req.battery_pct      = 100;    // mains powered
  req.flags            = 0;
  req.sequence         = s_seq++;
  req.reserved         = 0;
  esp_now_send(s_hubMac, (const uint8_t*)&req, sizeof(req));
}

// ─── Public ─────────────────────────────────────────────────────────────────
void espnow_init() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  static const uint8_t pmk[16] = ESPNOW_PMK;
  esp_now_set_pmk(pmk);
  esp_now_register_recv_cb(onRecv);

  loadPairing();
  if (s_paired) {
    if (s_channel) esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
    setPeer(s_hubMac, s_channel, s_hubLmk);
    Serial.printf("Restored pairing from NVS (ch%d)\n", s_channel);
  }
}

void espnow_tick() {
  uint32_t now = millis();

  if (s_gotUnpair.exchange(false)) {
    Serial.println("UNPAIR received — clearing pairing");
    clearPairing();
  }

  if (s_histLog) {
    s_histLog = false;
    Serial.printf("HIST RX metric=%u window=%u points=%u vmin=%d vmax=%d\n",
                  s_hist.metric_idx, s_hist.window, s_hist.n_points,
                  s_hist.v_min, s_hist.v_max);
  }

  // One-shot self-test: ask for a chart ten seconds after the link comes up,
  // without waiting for anyone to touch the screen. The request/response path
  // had gone several rounds unverified purely because a capture window and a
  // finger tap never coincided. Costs one 7-byte frame per boot and touches
  // no UI state.
#ifdef HIST_SELFTEST
  static bool selfTestDone = false;
  if (!selfTestDone && s_everUp && (now - s_lastStateMs) < 10000 &&
      now > 10000) {
    selfTestDone = true;
    Serial.println("HIST selftest: requesting tank0 / 24h");
    espnow_history_request(HIST_METRIC_TANK0, HIST_WINDOW_24H, 120);
  }
#endif

  if (s_gotState.exchange(false)) {
    s_lastStateMs = now;
    s_everUp      = true;
    if (s_rxStateLog) {
      s_rxStateLog = false;
      // What actually arrived, decoded on this side. The hub logs what it
      // believes it sent; this is the only way to tell the two apart.
      Serial.printf("STATE n_tanks=%u t0=%u t1=%u batt=%u ver=%u\n",
                    s_state.n_tanks, s_state.tanks[0].level_pct,
                    s_state.tanks[1].level_pct, s_state.batt_soc_pct,
                    s_state.protocol_version);
    }
    display_update(s_state);
  }

  // Refresh the link banner every tick — cheap, and it keeps the "last
  // update" age ticking up while the hub is quiet.
  display_set_link(espnow_link_state(), espnow_seconds_since_state());

  // A probe answer can land at any time — including mid-sweep.
  if (s_gotProbe.exchange(false)) {
    lockOntoHub();
    sendRequest();
    s_lastPollMs = now;
    return;
  }

  // Mid-sweep: step to the next channel once this one has had its listen
  // window, and don't poll until the sweep resolves.
  if (s_scanning) {
    if (now - s_scanStepAt >= PROBE_LISTEN_MS) scanNextChannel();
    return;
  }

  if (now - s_lastPollMs < POLL_INTERVAL_MS) return;
  s_lastPollMs = now;

  // Unpaired, or the link has been quiet long enough that the hub may
  // have hopped channels → start a sweep instead of polling into the void.
  const bool quiet = (s_lastStateMs == 0) ||
                     (now - s_lastStateMs > LINK_TIMEOUT_MS);
  if (!s_paired || quiet) {
    // Drop the hub's encrypted peer registration before sweeping. The hub
    // sends probe responses UNENCRYPTED — the response carries the LMK, so it
    // cannot be encrypted with it — and ESP-NOW discards an unencrypted frame
    // from a peer registered as encrypted, below the receive callback, with
    // no error at either end. Without this the display could pair once and
    // then never recover from any link loss: the hub answered every probe and
    // the display dropped every answer, reporting "Hub not found on any
    // channel" forever. The sensor node has carried this same fix for a while;
    // this client was written without it.
    dropUnicastPeers();
    s_scanning  = true;
    s_scanTried = 0;
    scanNextChannel();
    return;
  }

  sendRequest();
}

link_state_t espnow_link_state() {
  if (s_lastStateMs == 0) return LINK_SEARCHING;
  if (millis() - s_lastStateMs > LINK_TIMEOUT_MS)
    return s_everUp ? LINK_LOST : LINK_SEARCHING;
  return LINK_UP;
}

uint32_t espnow_seconds_since_state() {
  if (s_lastStateMs == 0) return 0;
  return (millis() - s_lastStateMs) / 1000UL;
}


// ─── Hub history ────────────────────────────────────────────────────────────
void espnow_history_request(uint8_t metricIdx, uint8_t window, uint8_t nPoints,
                            uint16_t pageBack) {
  if (espnow_link_state() != LINK_UP) {
    Serial.printf("HIST TX skipped — link not up (state=%d)\n",
                  (int)espnow_link_state());
    return;
  }
  Serial.printf("HIST TX metric=%u window=%u points=%u\n",
                metricIdx, window, nPoints);
  s_histReady = false;
  history_request_t req = {};
  req.magic[0]   = PROBE_MAGIC_0;
  req.magic[1]   = PROBE_MAGIC_1;
  req.type       = HIST_MSG_REQUEST;
  req.metric_idx = metricIdx;
  req.window     = window;
  req.n_points   = nPoints;
  req.sequence   = ++s_histSeq;
  req.page_back  = pageBack;
  esp_now_send(s_hubMac, (const uint8_t*)&req, sizeof(req));
}

bool espnow_history_ready() { return s_histReady; }

uint16_t espnow_history_interval_s() { return s_hist.interval_s; }
uint32_t espnow_history_newest_utc()  { return s_hist.newest_utc_s; }

int espnow_history_values(int16_t* out, int maxOut) {
  if (!s_histReady || !out) return 0;
  int n = s_hist.n_points;
  if (n > maxOut) n = maxOut;
  for (int i = 0; i < n; i++) {
    uint8_t p = s_hist.points[i];
    // The hub sends normalised bytes against the min/max in the header;
    // 0xFF is the gap marker and must not be scaled into a real value.
    out[i] = (p == HISTORY_POINT_GAP)
           ? HIST_NO_DATA
           : history_point_to_value(p, s_hist.v_min, s_hist.v_max);
  }
  return n;
}
