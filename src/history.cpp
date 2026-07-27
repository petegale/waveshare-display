#include "history.h"
#include <Arduino.h>
#include <string.h>

// One ring per metric. int16 covers every metric we store: percentages
// 0..100, and current in deciamps (±3276.7 A). ~2.9 kB per metric.
typedef struct {
  int16_t  ring[HIST_SAMPLES];
  uint16_t head;      // next write position
  uint16_t count;     // valid samples, saturating at HIST_SAMPLES
  int32_t  acc;       // open bucket accumulator
  uint16_t accN;
} series_t;

static series_t s_series[HIST_COUNT];
static uint32_t s_bucketStartMs = 0;

void history_init() {
  memset(s_series, 0, sizeof(s_series));
  for (int m = 0; m < HIST_COUNT; m++)
    for (int i = 0; i < HIST_SAMPLES; i++)
      s_series[m].ring[i] = HIST_NO_DATA;
  s_bucketStartMs = millis();
}

void history_add(hist_metric_t m, int16_t value) {
  if (m < 0 || m >= HIST_COUNT) return;
  s_series[m].acc  += value;
  s_series[m].accN += 1;
}

void history_tick() {
  uint32_t now = millis();
  if (now - s_bucketStartMs < HIST_BUCKET_MS) return;
  s_bucketStartMs = now;

  for (int m = 0; m < HIST_COUNT; m++) {
    series_t& s = s_series[m];
    // A bucket with no readings still advances the ring — otherwise a
    // dropout would compress the time axis and silently misdate history.
    int16_t v = (s.accN > 0) ? (int16_t)(s.acc / s.accN) : HIST_NO_DATA;
    s.ring[s.head] = v;
    s.head = (s.head + 1) % HIST_SAMPLES;
    if (s.count < HIST_SAMPLES) s.count++;
    s.acc  = 0;
    s.accN = 0;
  }
}

// Oldest sample index for a ring that may not be full yet.
static inline uint16_t oldestIdx(const series_t& s) {
  return (s.count < HIST_SAMPLES) ? 0
                                  : (uint16_t)(s.head % HIST_SAMPLES);
}

int history_series(hist_metric_t m, int16_t* out, int n) {
  if (m < 0 || m >= HIST_COUNT || !out || n <= 0) return 0;
  const series_t& s = s_series[m];

  for (int i = 0; i < n; i++) out[i] = HIST_NO_DATA;
  if (s.count == 0) return 0;

  const uint16_t start = oldestIdx(s);
  int valid = 0;

  // Average each output slot's share of the window so a long series
  // doesn't just decimate away its own peaks.
  for (int i = 0; i < n; i++) {
    int lo = (int)((uint32_t)i * s.count / n);
    int hi = (int)((uint32_t)(i + 1) * s.count / n);
    if (hi <= lo) hi = lo + 1;
    if (hi > s.count) hi = s.count;

    int32_t sum = 0; int cnt = 0;
    for (int k = lo; k < hi; k++) {
      int16_t v = s.ring[(start + k) % HIST_SAMPLES];
      if (v == HIST_NO_DATA) continue;
      sum += v; cnt++;
    }
    if (cnt > 0) { out[i] = (int16_t)(sum / cnt); valid++; }
  }
  return valid;
}

bool history_stats(hist_metric_t m, int16_t* minOut, int16_t* maxOut, int16_t* avgOut) {
  if (m < 0 || m >= HIST_COUNT) return false;
  const series_t& s = s_series[m];
  if (s.count == 0) return false;

  const uint16_t start = oldestIdx(s);
  int16_t lo = INT16_MAX, hi = INT16_MIN;
  int32_t sum = 0; int cnt = 0;

  for (uint16_t k = 0; k < s.count; k++) {
    int16_t v = s.ring[(start + k) % HIST_SAMPLES];
    if (v == HIST_NO_DATA) continue;
    if (v < lo) lo = v;
    if (v > hi) hi = v;
    sum += v; cnt++;
  }
  if (cnt == 0) return false;

  if (minOut) *minOut = lo;
  if (maxOut) *maxOut = hi;
  if (avgOut) *avgOut = (int16_t)(sum / cnt);
  return true;
}

uint32_t history_span_minutes(hist_metric_t m) {
  if (m < 0 || m >= HIST_COUNT) return 0;
  return s_series[m].count;   // one sample per minute
}
