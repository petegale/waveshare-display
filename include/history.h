#pragma once
#include <stdint.h>

/**
 * history.h — on-display time series.
 *
 * Neither the hub nor the protocol carries history: display_state_t is a
 * snapshot. This node is mains-powered and always on, so it accumulates
 * its own series from the 5-second poll.
 *
 * Samples are averaged into one-minute buckets over a 24-hour window —
 * tank levels move over hours, so per-poll resolution would be noise
 * with 288x the storage. RAM only for now: a power cut loses the series.
 * (Persisting to the TF card is the obvious follow-up; the slot is there.)
 */

enum hist_metric_t {
  HIST_TANK0 = 0,
  HIST_TANK1,
  HIST_BATT_SOC,
  HIST_BATT_CURRENT,   // deciamps, signed
  HIST_COUNT
};

#define HIST_BUCKET_MS   60000UL
#define HIST_SAMPLES     1440           // 24 h at one per minute
#define HIST_NO_DATA     INT16_MIN

void history_init();

// Feed the newest reading. Averaged into the open bucket; pass nothing
// for a metric that has no data this round and the gap is preserved.
void history_add(hist_metric_t m, int16_t value);

// Close the current bucket when its minute is up. Call from the loop.
void history_tick();

// Fill out[0..n-1] oldest→newest, downsampled to n points across the
// stored window. Gaps come back as HIST_NO_DATA. Returns the number of
// points that hold real data.
int history_series(hist_metric_t m, int16_t* out, int n);

// Min / max / mean over the stored window. False if there's no data yet.
bool history_stats(hist_metric_t m, int16_t* minOut, int16_t* maxOut, int16_t* avgOut);

// How far back the stored series reaches, in minutes.
uint32_t history_span_minutes(hist_metric_t m);
