/**
 * espnow.cpp — see header for the link model.
 *
 * The link logic lives in protocol/espnow_client.h and is shared with every
 * other device on the hub. This file is the executor: it performs the radio
 * operations the state machine asks for, and feeds it events. The ordering
 * rules that used to be duplicated (and independently broken) in each client
 * — channel before peer, drop peers before sweeping, delete peer before
 * forgetting the MAC — are now decided there and covered by host tests.
 *
 * What stays here is genuinely display-specific: a 5 s poll cadence, mains
 * power, the state/history message handling, and the NVS pairing record.
 */

#include "espnow.h"
#include "config.h"
#include "display.h"
#include "espnow_client.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <atomic>
#include <string.h>

static espnow_client_t s_client;

static std::atomic<bool> s_gotState{false};
static std::atomic<bool> s_gotUnpair{false};

// A probe response is handed to the state machine from the tick, not from the
// RX callback: the callback runs on the WiFi task and the client is not
// thread-safe. The callback only stashes and flags.
static std::atomic<bool> s_gotProbe{false};
static uint8_t s_probeMac[6];
static uint8_t s_probeLmk[16];
static uint8_t s_probeCh;

// Send results, likewise drained in the tick rather than applied in the
// callback. Consecutive failures are one of the two independent ways the
// client notices a dead link.
static std::atomic<uint32_t> s_txOk{0};
static std::atomic<uint32_t> s_txFail{0};

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
  bool paired = p.getBool("paired", false);
  if (paired) {
    uint8_t mac[6] = {}, lmk[16] = {};
    p.getBytes("hubMac", mac, 6);
    p.getBytes("hubLmk", lmk, 16);
    uint8_t ch = p.getUChar("chan", 0);
    // Seeds which channel to sweep first and who to expect. It does not claim
    // the link is up — that still has to be proven by a probe exchange.
    espnow_client_restore(&s_client, mac, lmk, ch);
    Serial.printf("Restored pairing from NVS (ch%d)\n", ch);
  }
  p.end();
}

static void savePairing() {
  Preferences p;
  p.begin("display", false);
  p.putBool("paired", true);
  p.putBytes("hubMac", s_client.hubMac, 6);
  p.putBytes("hubLmk", s_client.hubLmk, 16);
  p.putUChar("chan", s_client.channel);
  p.end();
}

// Drop every non-broadcast peer. Walking the table rather than trusting a
// stored MAC means this still works when the address has already been lost.
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

// ─── RX (runs on the WiFi task) ─────────────────────────────────────────────
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
    memcpy(s_probeMac, info->src_addr, 6);
    memcpy(s_probeLmk, p->lmk, 16);
    s_probeCh = p->channel;
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

static void onSent(const wifi_tx_info_t* info, esp_now_send_status_t status) {
  // Broadcast probes always "succeed" and say nothing about the link; only
  // unicast to the hub is evidence either way.
  static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  if (!info || memcmp(info->des_addr, bcast, 6) == 0) return;
  if (status == ESP_NOW_SEND_SUCCESS) s_txOk.fetch_add(1);
  else                                s_txFail.fetch_add(1);
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

static void sendProbe() {
  static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  channel_probe_t pkt = {};
  pkt.magic[0] = PROBE_MAGIC_0;
  pkt.magic[1] = PROBE_MAGIC_1;
  pkt.type     = PROBE_TYPE_REQUEST;
  pkt.channel  = 0;
  esp_now_send(bcast, (const uint8_t*)&pkt, sizeof(pkt));
}

static void sendRequest() {
  display_request_t req = {};
  req.protocol_version = PROTOCOL_VERSION;
  req.type             = DISPLAY_REQ_STATE;
  req.battery_pct      = 100;    // mains powered
  req.flags            = 0;
  req.sequence         = s_seq++;
  req.reserved         = 0;
  esp_now_send(s_client.hubMac, (const uint8_t*)&req, sizeof(req));
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
  esp_now_register_send_cb(onSent);

  // Transport policy — the part that is legitimately this device's own.
  // A mains-powered display can afford to re-probe promptly; it has no sleep
  // budget to protect and the user is looking at it.
  espnow_cfg_t cfg = {};
  cfg.chMin         = WIFI_CHANNEL_MIN;
  cfg.chMax         = WIFI_CHANNEL_MAX;
  cfg.listenMs      = PROBE_LISTEN_MS;
  cfg.txFailLimit   = 5;
  cfg.linkTimeoutMs = LINK_TIMEOUT_MS;
  espnow_client_init(&s_client, &cfg);

  loadPairing();
}

void espnow_tick() {
  uint32_t now = millis();
  espnow_client_set_time(&s_client, now);

  // Drain the callbacks into the state machine.
  for (uint32_t n = s_txOk.exchange(0);   n; n--) espnow_client_on_tx_result(&s_client, true);
  for (uint32_t n = s_txFail.exchange(0); n; n--) espnow_client_on_tx_result(&s_client, false);

  if (s_gotUnpair.exchange(false)) {
    Serial.println("UNPAIR received — clearing pairing");
    Preferences p;
    p.begin("display", false);
    p.clear();
    p.end();
    s_lastStateMs = 0;
    s_everUp      = false;
    // The peer deletion itself is sequenced by the client, which will not let
    // the MAC be forgotten until the registration is gone.
    espnow_client_on_unpair(&s_client);
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
  // Which metric to ask for is a build flag: a hub with no tanks configured
  // answers HIST_METRIC_TANK0 with "no such metric", so a hardcoded tank made
  // the self-test silently untestable on exactly the bench it was meant for.
  #ifndef HIST_SELFTEST_METRIC
  #define HIST_SELFTEST_METRIC HIST_METRIC_TANK0
  #endif
  static bool selfTestDone = false;
  if (!selfTestDone && s_everUp && (now - s_lastStateMs) < 10000 &&
      now > 10000) {
    selfTestDone = true;
    Serial.printf("HIST selftest: requesting metric %d / 24h\n",
                  (int)HIST_SELFTEST_METRIC);
    espnow_history_request(HIST_SELFTEST_METRIC, HIST_WINDOW_24H, 120, 0);
  }
#endif

  if (s_gotState.exchange(false)) {
    s_lastStateMs = now;
    s_everUp      = true;
    espnow_client_on_hub_frame(&s_client);
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
    espnow_client_on_probe_reply(&s_client, s_probeMac, s_probeLmk, s_probeCh);
  }

  // The poll cadence is this device's own policy; everything else about when
  // to transmit, sweep or give up belongs to the shared client.
  const bool wantSend = (now - s_lastPollMs) >= POLL_INTERVAL_MS;

  switch (espnow_client_tick(&s_client, wantSend)) {
  case ESPNOW_ACT_DROP_PEERS:
    dropUnicastPeers();
    break;
  case ESPNOW_ACT_SET_CHANNEL:
    esp_wifi_set_channel(s_client.act_channel, WIFI_SECOND_CHAN_NONE);
    break;
  case ESPNOW_ACT_ADD_BCAST_PEER: {
    static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    setPeer(bcast, s_client.act_channel, nullptr);
    break;
  }
  case ESPNOW_ACT_SEND_PROBE:
    sendProbe();
    break;
  case ESPNOW_ACT_ADD_HUB_PEER:
    setPeer(s_client.act_mac, s_client.act_channel, s_client.act_lmk);
    savePairing();
    Serial.printf("Hub found on ch%d (%02X:%02X:%02X:%02X:%02X:%02X)\n",
                  s_client.act_channel,
                  s_client.act_mac[0], s_client.act_mac[1], s_client.act_mac[2],
                  s_client.act_mac[3], s_client.act_mac[4], s_client.act_mac[5]);
    // Poll immediately rather than waiting out the cadence — the user is
    // watching a "searching" banner.
    sendRequest();
    s_lastPollMs = now;
    break;
  case ESPNOW_ACT_DEL_HUB_PEER:
    if (esp_now_is_peer_exist(s_client.act_mac))
      esp_now_del_peer(s_client.act_mac);
    break;
  case ESPNOW_ACT_SEND_DATA:
    sendRequest();
    s_lastPollMs = now;
    break;
  case ESPNOW_ACT_REPORT_LOST:
    Serial.printf("Hub not found on any channel (rx=%lu tx=%lu fail=%lu sweeps=%lu)\n",
                  (unsigned long)s_client.rxFrames,
                  (unsigned long)s_client.txAttempts,
                  (unsigned long)s_client.txFailures,
                  (unsigned long)s_client.sweeps);
    break;
  case ESPNOW_ACT_NONE:
    break;
  }
}

link_state_t espnow_link_state() {
  // The client's own notion of "up" is about the peer registration; the banner
  // is about whether data is arriving, which is what the user cares about.
  if (espnow_client_link(&s_client) == ESPNOW_LINK_UP &&
      s_lastStateMs != 0 && (millis() - s_lastStateMs) <= LINK_TIMEOUT_MS)
    return LINK_UP;
  return s_everUp ? LINK_LOST : LINK_SEARCHING;
}

uint32_t espnow_seconds_since_state() {
  if (s_lastStateMs == 0) return 0;
  return (millis() - s_lastStateMs) / 1000UL;
}


// ─── Hub history ────────────────────────────────────────────────────────────
void espnow_history_request(uint8_t metricIdx, uint8_t window, uint8_t nPoints,
                            uint16_t offsetBuckets) {
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
  req.offset_buckets = offsetBuckets;
  esp_now_send(s_client.hubMac, (const uint8_t*)&req, sizeof(req));
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
