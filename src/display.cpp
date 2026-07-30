/**
 * display.cpp — LVGL UI for the 4.3" helm display.
 *
 * Carries over the e-paper gauge's design language so the two displays
 * read as one product: three tiles (two tanks + house battery), a black
 * label bar on each, a bottom-up fill, and hairlines at 25/50/75%.
 *
 * Differences from the e-paper, all deliberate:
 *   • Dark theme. This panel is backlit and lives at the helm — white-on-
 *     black preserves night vision where the e-paper's ink-on-paper does
 *     not. Colour is used only where it carries meaning (fill state,
 *     link loss), never decoratively.
 *   • The battery gauge keeps the 50–100% window (a lead-acid bank below
 *     50% is being damaged, so the usable half fills the whole gauge) but
 *     the printed number is always TRUE SoC. Scale marks at 100/75/50
 *     make the non-zero baseline explicit.
 */

#include "display.h"
#include "config.h"
#include "espnow.h"          // hub history queries
#include "remote_protocol.h"  // HIST_METRIC_*, HIST_WINDOW_*
#include <lvgl.h>
#include <stdio.h>
#include <math.h>

// ─── Palette ────────────────────────────────────────────────────────────────
#define COL_BG        lv_color_hex(0x000000)
#define COL_PANEL     lv_color_hex(0x111214)
#define COL_BORDER    lv_color_hex(0x3A3D42)
#define COL_BAR_BG    lv_color_hex(0x1C1E22)
#define COL_TEXT      lv_color_hex(0xF2F2F2)
// Neutral grey, not the tinted 0x8A9099 it used to be. On an RGB-stripe
// panel a thin glyph lights unequal sub-pixels, and a colour that is already
// blue-weighted turns single-pixel stems visibly red on one edge and white on
// the other — which is exactly how the detail-page labels were rendering.
// Equal channels keep the fringing symmetric, and the lighter value keeps it
// legible at arm's length: ~7:1 on the black background.
#define COL_DIM       lv_color_hex(0xB4B4B4)
#define COL_FILL      lv_color_hex(0x4A9EFF)   // tank / healthy battery
#define COL_WARN      lv_color_hex(0xFFB020)   // battery below the window
#define COL_ALARM     lv_color_hex(0xFF4D4D)   // link lost
#define COL_OK        lv_color_hex(0x3ED598)

// ─── Layout ─────────────────────────────────────────────────────────────────
static const lv_coord_t HEADER_H = 56;
static const lv_coord_t TILE_W   = 246;
static const lv_coord_t TILE_H   = 386;
static const lv_coord_t TILE_GAP = 14;
static const lv_coord_t TILE_Y   = HEADER_H + 14;
static const lv_coord_t BAR_H    = 44;   // label bar inside each tile

#define N_TILES 3

typedef struct {
  lv_obj_t* root;
  lv_obj_t* label;      // fluid / "HOUSE" in the black bar
  lv_obj_t* bar;        // the fill
  lv_obj_t* value;      // big percentage
  lv_obj_t* sub;        // current line (battery) — empty for tanks
  lv_obj_t* badge;      // LOW badge (battery only)
} tile_t;

static tile_t   s_tile[N_TILES];
static lv_obj_t* s_clock;
static lv_obj_t* s_link;

// ─── Screens ────────────────────────────────────────────────────────────────
// Two screens rather than show/hide containers: LVGL only renders the
// loaded screen, so the detail page costs nothing while it's closed.
static lv_obj_t* s_mainScr   = nullptr;
static lv_obj_t* s_detailScr = nullptr;

static lv_obj_t*        s_dTitle = nullptr;
static lv_obj_t*        s_loading = nullptr;
static lv_obj_t*        s_dSpan  = nullptr;
static lv_obj_t*        s_chart  = nullptr;
static lv_chart_series_t* s_chartSer = nullptr;
static lv_obj_t* s_winBtn = nullptr;
static lv_obj_t* s_backBtn = nullptr;
static lv_obj_t* s_fwdBtn = nullptr;
static lv_obj_t* s_winLbl = nullptr;

// Which metric the detail page is currently showing, or -1 when closed.
static int s_openTile = -1;

// Last state we rendered — the detail page needs it to redraw on update.
static display_state_t s_last     = {};
static bool            s_haveLast = false;

#define CHART_POINTS 120
// Space the axis tick labels need. LVGL draws them OUTSIDE the chart's
// border box, so this is not interior padding — an earlier version treated it
// as though it were and left the series inset by 60 px of empty frame at each
// end for no reason.
#define AXIS_LABEL_W 60

// The only interior padding: enough that a full-height bar does not merge
// with the frame, and no more. Identical on both charts, which is what keeps
// the line registered with its columns.
#define PLOT_INSET   6

// Chart geometry. The single-line title freed a line of vertical space, and
// the chart is what the page exists for, so it takes it.
#define CHART_H      344
#define CHART_BOTTOM 52

// Width is what remains once BOTH label columns and a margin come out. The
// chart was previously sized from the screen and nudged right, which pushed
// the right-hand label column past the edge of the panel: the rate axis was
// drawn but clipped, which is worse than not drawing it.
#define CHART_MARGIN 16
#define CHART_W      (SCREEN_WIDTH - 2 * (AXIS_LABEL_W + CHART_MARGIN))

// Backing store handed to LVGL via lv_chart_set_ext_y_array. Populating
// the series with lv_chart_set_value_by_id() instead would invalidate the
// whole chart once per point — 120 redraws to draw one line.
static lv_coord_t s_chartPts[CHART_POINTS];
static lv_obj_t* s_rateChart = nullptr;
static lv_chart_series_t* s_rateSer = nullptr;
static lv_coord_t s_ratePts[CHART_POINTS];
static lv_obj_t* s_legend = nullptr;
// Tank capacities in litres from the hub, 0 when unknown. Lets the rate
// trace be litres per hour rather than percent per hour.
static uint16_t s_tankCapacityL[2] = {0, 0};


// Per-tile descriptor: which history metric backs it, and how to print it.
typedef struct {
  const char*   unit;
  float         scale;      // raw → display units
  int           decimals;
  bool          fixedRange;  // percentages pin the Y axis to 0..100
  uint8_t       hubMetric;   // HIST_METRIC_* — what to ask the hub for
} tile_desc_t;

static const tile_desc_t TILE_DESC[N_TILES] = {
  { "%", 1.0f, 0, true, HIST_METRIC_TANK0    },
  { "%", 1.0f, 0, true, HIST_METRIC_TANK1    },
  { "%", 1.0f, 0, true, HIST_METRIC_BATT_SOC },
};

// Which window the detail page is showing. The hub keeps 24 h / 30 d / 1 y;
// this node's own rings only reach 24 h, and are the fallback when the link
// is down — so the longer windows are only offered when the hub answers.
// Chart series, filled from the hub's response. Sized to the chart so
// refresh_detail() and statsOf() work from exactly what is drawn.
static int16_t s_pts[CHART_POINTS];

// Min / max / mean over the drawn series, gaps excluded. False if there is
// nothing real in it.
static bool statsOf(const int16_t* p, int n, int16_t* mn, int16_t* mx, int16_t* av) {
  int32_t sum = 0; int cnt = 0;
  for (int i = 0; i < n; i++) {
    if (p[i] == HIST_NO_DATA) continue;
    if (!cnt) { *mn = *mx = p[i]; }
    if (p[i] < *mn) *mn = p[i];
    if (p[i] > *mx) *mx = p[i];
    sum += p[i]; cnt++;
  }
  if (!cnt) return false;
  *av = (int16_t)(sum / cnt);
  return true;
}

static uint8_t s_window = HIST_WINDOW_24H;

// One column per next unit down from the window: hours across a day, days
// across a month, months across a year. The hub averages each bucket, so a
// column is a real mean over that unit rather than a sample plucked from it —
// and at 12–30 columns each one is wide enough to read, which 120 was not.
static int points_for_window(uint8_t w) {
  switch (w) {
    case HIST_WINDOW_30D: return 30;   // days
    case HIST_WINDOW_1Y:  return 12;   // months
    default:              return 24;   // hours
  }
}

static const char* bucket_name(uint8_t w) {
  switch (w) {
    case HIST_WINDOW_30D: return "day";
    case HIST_WINDOW_1Y:  return "month";
    default:              return "hour";
  }
}

// Points currently on the charts. Both charts are resized together so the
// rate line stays registered with the columns it belongs to.
static int s_pointCount = 24;

// How many columns back the right-hand edge sits. Reset on opening a page or
// changing window, since an offset in columns means nothing once the column
// width changes under it.
static uint16_t s_offsetBuckets = 0;

// A press moves half a screen, not a whole one. Stepping a full screen
// replaces every column at once and leaves nothing recognisable to navigate
// by; half keeps the features the user was looking at on screen while new
// ones arrive behind them.
static uint16_t page_step() {
  int half = s_pointCount / 2;
  return (uint16_t)(half > 0 ? half : 1);
}

// Show the loading state AND clear the plot. Paging used to put the label
// over the previous page's columns, because nothing redrew the chart until
// the next state broadcast five seconds later — so the old data sat there
// under the word "Loading" looking like the answer.
static void show_loading(const char* msg);
static lv_obj_t* s_probe = nullptr;
// Column currently under the finger, or -1. Read by the draw hook.
static int s_pressedIdx = -1;

// Rate axis labels: the series is stored at ten times scale, so 53 on the
// axis means 5.3 in the world. Rewrite the text rather than change the
// storage — the scaling is what stops fuel burn under 1 %/h quantising to a
// flat line at zero.
static void rate_draw_part(lv_event_t* e) {
  lv_obj_draw_part_dsc_t* dsc = lv_event_get_draw_part_dsc(e);
  if (!dsc || dsc->part != LV_PART_TICKS || !dsc->text) return;
  snprintf(dsc->text, dsc->text_length, "%.1f", (float)dsc->value / 10.0f);
}

static void chart_draw_part(lv_event_t* e) {
  lv_obj_draw_part_dsc_t* dsc = lv_event_get_draw_part_dsc(e);
  if (!dsc || dsc->part != LV_PART_ITEMS) return;
  if (s_pressedIdx < 0 || (int)dsc->id != s_pressedIdx) return;
  if (!dsc->rect_dsc) return;
  dsc->rect_dsc->bg_color = COL_TEXT;      // near-white against the blue
  dsc->rect_dsc->border_color = COL_TEXT;
}

static void fmt_metric(char* buf, size_t n, int tileIdx, int16_t raw);
static void days_to_ymd(uint16_t days, int& y, int& m, int& d);

// When a column happened, as a clock time or a date. "16 hours ago" is only
// useful if you already know what time you are reading it, and on a boat you
// often do not — a date is what you compare against the log book.
//
// Falls back to relative only when the hub has no time source at all, which
// is the one case where an absolute label would be a fabrication.
static const char* MONTHS[12] = { "Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec" };

static void bucket_when(char* out, size_t n, int idx, int count) {
  int back = (count - 1) - idx;
  uint32_t newest = espnow_history_newest_utc();
  uint16_t iv     = espnow_history_interval_s();

  if (newest == 0 || iv == 0) {
    if (back <= 0) { snprintf(out, n, "latest"); return; }
    snprintf(out, n, "%d %s%s back", back, bucket_name(s_window),
             back == 1 ? "" : "s");
    return;
  }

  uint32_t t = newest - (uint32_t)back * (uint32_t)iv;
  uint32_t days = t / 86400UL;
  uint32_t secs = t % 86400UL;
  int y, mo, d;
  days_to_ymd((uint16_t)days, y, mo, d);

  switch (s_window) {
    case HIST_WINDOW_1Y:
      snprintf(out, n, "%s %d", MONTHS[(mo - 1) % 12], y);
      break;
    case HIST_WINDOW_30D:
      snprintf(out, n, "%d %s", d, MONTHS[(mo - 1) % 12]);
      break;
    default:
      snprintf(out, n, "%02u:%02u", (unsigned)(secs / 3600),
               (unsigned)((secs % 3600) / 60));
      break;
  }
}

static void chart_pressed(lv_event_t* e) {
  (void)e;
  if (s_openTile < 0 || !s_probe) return;

  // Column under the finger, from the touch coordinate rather than LVGL's
  // pressed-point. lv_chart_get_pressed_point() latches on the press and does
  // not follow a finger dragged across the chart, so reading a second column
  // meant lifting off and tapping again — the chart is a strip of values and
  // scrubbing along it is the obvious way to read them.
  lv_indev_t* indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t pt;
  lv_indev_get_point(indev, &pt);

  lv_coord_t x0   = s_chart->coords.x1 + PLOT_INSET;
  lv_coord_t plotW = lv_obj_get_width(s_chart) - 2 * PLOT_INSET;
  if (plotW <= 0 || s_pointCount <= 0) return;

  int32_t rel = pt.x - x0;
  if (rel < 0) rel = 0;
  if (rel >= plotW) rel = plotW - 1;
  uint16_t id = (uint16_t)((rel * s_pointCount) / plotW);
  if ((int)id >= s_pointCount) id = (uint16_t)(s_pointCount - 1);
  if (s_pts[id] == HIST_NO_DATA) {
    lv_label_set_text(s_probe, "#B4B4B4 no data for this bucket#");
    lv_obj_clear_flag(s_probe, LV_OBJ_FLAG_HIDDEN);
    if (s_pressedIdx != (int)id) { s_pressedIdx = (int)id; lv_obj_invalidate(s_chart); }
    return;
  }

  char age[32], val[16];
  bucket_when(age, sizeof(age), id, s_pointCount);
  fmt_metric(val, sizeof(val), s_openTile, s_pts[id]);

  // Show the level and, when this bucket has one, its own rate — the figure
  // an average over the window cannot give you.
  char line[96];
  if (s_ratePts[id] != LV_CHART_POINT_NONE) {
    const char* uBase = (s_openTile < 2 && s_tankCapacityL[s_openTile] > 0)
                      ? "L" : "%";
    float r = (float)s_ratePts[id] / 10.0f;
    snprintf(line, sizeof(line), "#F2F2F2 %s#  #4A9EFF %s#   #FFB020 %+.1f %s/%s#",
             age, val, r, uBase, bucket_name(s_window));
  } else {
    snprintf(line, sizeof(line), "#F2F2F2 %s#  #4A9EFF %s#", age, val);
  }
  lv_label_set_text(s_probe, line);
  lv_obj_clear_flag(s_probe, LV_OBJ_FLAG_HIDDEN);
  // Repaint only when the column changes — a finger held still would
  // otherwise invalidate the chart on every press event.
  if (s_pressedIdx != (int)id) { s_pressedIdx = (int)id; lv_obj_invalidate(s_chart); }
}

static void chart_released(lv_event_t* e) {
  (void)e;
  // Momentary: the readout belongs to the finger, and leaving it up after the
  // finger has gone leaves a number on screen with nothing pointing at it.
  if (s_probe) lv_obj_add_flag(s_probe, LV_OBJ_FLAG_HIDDEN);
  if (s_pressedIdx >= 0) { s_pressedIdx = -1; lv_obj_invalidate(s_chart); }
}

static void set_point_count(int n) {
  s_pointCount = n;
  lv_chart_set_point_count(s_chart, n);
  lv_chart_set_point_count(s_rateChart, n);
}
static bool    s_usingHub = false;

static const char* window_name(uint8_t w) {
  switch (w) {
    case HIST_WINDOW_30D: return "30 DAYS";
    case HIST_WINDOW_1Y:  return "1 YEAR";
    default:              return "24 HOURS";
  }
}

static void open_detail(int tileIdx);
static void refresh_detail();
static void cycle_window(lv_event_t* e);
static void chart_pressed(lv_event_t* e);
static void chart_released(lv_event_t* e);
static void chart_draw_part(lv_event_t* e);
static void page_older(lv_event_t* e);
static void page_newer(lv_event_t* e);
static void request_page();
static void rate_draw_part(lv_event_t* e);

// ─── Helpers ────────────────────────────────────────────────────────────────
static const char* fluid_name(uint8_t f) {
  switch (f) {
    case DISPLAY_FLUID_FUEL:     return "FUEL";
    case DISPLAY_FLUID_WATER:    return "WATER";
    case DISPLAY_FLUID_GREY:     return "GREY";
    case DISPLAY_FLUID_BLACK:    return "BLACK";
    case DISPLAY_FLUID_OIL:      return "OIL";
    case DISPLAY_FLUID_LIVEWELL: return "LIVE WELL";
    default:                     return "TANK";
  }
}

// Days since 1970-01-01 → y/m/d (Howard Hinnant's civil_from_days).
static void days_to_ymd(uint16_t days, int& y, int& m, int& d) {
  int32_t z = (int32_t)days + 719468;
  int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097);
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int32_t yr = (int32_t)yoe + era * 400;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint32_t mp = (5 * doy + 2) / 153;
  uint32_t dd = doy - (153 * mp + 2) / 5 + 1;
  uint32_t mm = mp < 10 ? mp + 3 : mp - 9;
  if (mm <= 2) yr += 1;
  y = (int)yr; m = (int)mm; d = (int)dd;
}

static lv_obj_t* make_tile(lv_obj_t* parent, int idx) {
  lv_obj_t* t = lv_obj_create(parent);
  lv_obj_set_size(t, TILE_W, TILE_H);
  lv_obj_set_pos(t, 14 + idx * (TILE_W + TILE_GAP), TILE_Y);
  lv_obj_set_style_bg_color(t, COL_PANEL, 0);
  lv_obj_set_style_border_color(t, COL_BORDER, 0);
  lv_obj_set_style_border_width(t, 2, 0);
  lv_obj_set_style_radius(t, 6, 0);
  lv_obj_set_style_pad_all(t, 0, 0);
  // Scrolling makes no sense on a fixed tile, but clicking does — each
  // tile opens its history page. The pressed state below is deliberate
  // touch feedback (a brighter border), unlike the accidental theme
  // repaint that used to read as flicker.
  lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_border_color(t, COL_FILL, LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(t, COL_PANEL, LV_STATE_PRESSED);
  return t;
}

// ─── Formatting helpers ─────────────────────────────────────────────────────
static void fmt_metric(char* buf, size_t n, int tileIdx, int16_t raw) {
  const tile_desc_t& d = TILE_DESC[tileIdx];
  if (raw == HIST_NO_DATA) { snprintf(buf, n, "--"); return; }
  if (d.decimals == 0) snprintf(buf, n, "%d%s", (int)lroundf(raw * d.scale), d.unit);
  else                 snprintf(buf, n, "%.*f%s", d.decimals, raw * d.scale, d.unit);
}

// LVGL's base object constructor sets LV_OBJ_FLAG_CLICKABLE, and widgets
// like lv_bar inherit it without ever using it. A child like the fill bar
// covers the whole tile body, so it silently swallows taps meant for the
// tile and nothing happens. Make every child transparent to touch and let
// the tile root be the only hit target.
static void children_ignore_touch(lv_obj_t* parent) {
  uint32_t n = lv_obj_get_child_cnt(parent);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t* c = lv_obj_get_child(parent, i);
    if (!c) continue;
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    children_ignore_touch(c);          // labels nested in the label bar
  }
}

static void tile_clicked(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  open_detail(idx);
}

// Screen switches are instant, not animated. A slide forces a full
// 800x480 repaint per animation frame — ~768 kB of pixel traffic each,
// which this panel cannot sustain and which showed up as a stutter. An
// instrument panel wants an immediate response anyway.
static void back_clicked(lv_event_t* e) {
  LV_UNUSED(e);
  s_openTile = -1;
  lv_scr_load(s_mainScr);
}

// ─── Detail screen ──────────────────────────────────────────────────────────
static void build_detail_screen() {
  s_detailScr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s_detailScr, COL_BG, 0);
  lv_obj_clear_flag(s_detailScr, LV_OBJ_FLAG_SCROLLABLE);

  s_dTitle = lv_label_create(s_detailScr);
  lv_obj_set_style_text_font(s_dTitle, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(s_dTitle, COL_TEXT, 0);
  lv_label_set_text(s_dTitle, "");
  lv_obj_align(s_dTitle, LV_ALIGN_TOP_LEFT, 20, 30);

  // Current level removed from this header. It was positioned with
  // lv_obj_align_to(), which resolves once — and it ran while the title was
  // still empty, so it anchored to a zero-width label and never moved when
  // the name arrived, printing the two on top of each other. The value is on
  // the tile the user just came from, and every column here is a value.

  // s_dStats removed: a single summary number beside a chart the user can
  // now interrogate directly was one more thing to reconcile, not less.

  // Back button — generously sized, this gets used with wet hands.
  lv_obj_t* back = lv_btn_create(s_detailScr);
  lv_obj_set_size(back, 120, 56);
  lv_obj_align(back, LV_ALIGN_TOP_RIGHT, -20, 14);
  lv_obj_set_style_bg_color(back, COL_PANEL, 0);
  lv_obj_set_style_border_color(back, COL_BORDER, 0);
  lv_obj_set_style_border_width(back, 2, 0);
  lv_obj_set_style_border_color(back, COL_FILL, LV_STATE_PRESSED);
  lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);
  lv_obj_t* bl = lv_label_create(back);
  lv_obj_set_style_text_font(bl, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(bl, COL_TEXT, 0);
  lv_label_set_text(bl, "BACK");
  lv_obj_center(bl);
  children_ignore_touch(back);   // same trap: the label must not eat the tap

  // Window selector, left of BACK. Same construction and the same
  // children_ignore_touch trap.
  s_winBtn = lv_btn_create(s_detailScr);
  lv_obj_set_size(s_winBtn, 170, 56);
  lv_obj_align(s_winBtn, LV_ALIGN_TOP_RIGHT, -214, 14);
  lv_obj_set_style_bg_color(s_winBtn, COL_PANEL, 0);
  lv_obj_set_style_border_color(s_winBtn, COL_BORDER, 0);
  lv_obj_set_style_border_width(s_winBtn, 2, 0);
  lv_obj_set_style_border_color(s_winBtn, COL_FILL, LV_STATE_PRESSED);
  lv_obj_add_event_cb(s_winBtn, cycle_window, LV_EVENT_CLICKED, NULL);
  s_winLbl = lv_label_create(s_winBtn);
  lv_obj_set_style_text_font(s_winLbl, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(s_winLbl, COL_TEXT, 0);
  lv_label_set_text(s_winLbl, window_name(s_window));
  lv_obj_center(s_winLbl);
  children_ignore_touch(s_winBtn);

  // ◀ and ▶ flank the window button, so "which span" and "which period"
  // read as one control rather than three scattered ones.
  struct { lv_obj_t** btn; const char* txt; lv_coord_t x; lv_event_cb_t cb; }
  // Positions are stepped across the top bar with a gap between each, rather
  // than guessed: the forward button previously sat on top of the window
  // button, so half of one control was covered by another.
  //   BACK 660..780 | > 594..652 | window 416..586 | < 350..408
  pagers[2] = {
    { &s_backBtn, LV_SYMBOL_LEFT,  -392, page_older },
    { &s_fwdBtn,  LV_SYMBOL_RIGHT, -148, page_newer },
  };
  for (int i = 0; i < 2; i++) {
    lv_obj_t* b = lv_btn_create(s_detailScr);
    *pagers[i].btn = b;
    lv_obj_set_size(b, 58, 56);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, pagers[i].x, 14);
    lv_obj_set_style_bg_color(b, COL_PANEL, 0);
    lv_obj_set_style_border_color(b, COL_BORDER, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, COL_FILL, LV_STATE_PRESSED);
    // Visibly unavailable rather than silently inert.
    lv_obj_set_style_bg_opa(b, LV_OPA_30, LV_STATE_DISABLED);
    lv_obj_set_style_border_opa(b, LV_OPA_30, LV_STATE_DISABLED);
    lv_obj_add_event_cb(b, pagers[i].cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_label_set_text(l, pagers[i].txt);
    lv_obj_center(l);
    children_ignore_touch(b);
  }
  lv_obj_add_state(s_fwdBtn, LV_STATE_DISABLED);   // starts at the live edge

  s_chart = lv_chart_create(s_detailScr);
  lv_obj_set_size(s_chart, CHART_W, CHART_H);
  lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, -CHART_BOTTOM);
  lv_chart_set_type(s_chart, LV_CHART_TYPE_BAR);
  lv_chart_set_point_count(s_chart, CHART_POINTS);
  lv_chart_set_div_line_count(s_chart, 5, 6);
  // Numbered Y axis: the grid alone gave no way to read a bar as a value.
  lv_chart_set_axis_tick(s_chart, LV_CHART_AXIS_PRIMARY_Y, 6, 0, 5, 1, true, 60);
  lv_obj_set_style_text_font(s_chart, &lv_font_montserrat_20, LV_PART_TICKS);
  lv_obj_set_style_text_color(s_chart, COL_DIM, LV_PART_TICKS);
  // Both charts carry the SAME padding on both sides. Tick labels are drawn
  // in the padding, so padding is what fixes the plot area — and an earlier
  // attempt gave these two mirror-image padding (left on one, right on the
  // other), which put their plots in different places and made the line sit
  // further from its columns than before there were any axes at all.
  lv_obj_set_style_pad_left(s_chart,  PLOT_INSET, LV_PART_MAIN);
  lv_obj_set_style_pad_right(s_chart, PLOT_INSET, LV_PART_MAIN);
  lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
  lv_obj_set_style_bg_color(s_chart, COL_PANEL, 0);
  lv_obj_set_style_border_color(s_chart, COL_BORDER, 0);
  lv_obj_set_style_border_width(s_chart, 2, 0);
  lv_obj_set_style_line_color(s_chart, COL_BORDER, LV_PART_MAIN);
  lv_obj_set_style_size(s_chart, 0, LV_PART_INDICATOR);   // no point dots
  // Clickable so a column can be interrogated. Scrolling stays off — a drag
  // is a read, not a pan.
  lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_chart, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_chart, chart_pressed, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(s_chart, chart_pressed, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(s_chart, chart_released, LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(s_chart, chart_released, LV_EVENT_PRESS_LOST, NULL);
  // A horizontal drag would otherwise be interpreted as a scroll gesture and
  // bubble to the screen, cancelling the press mid-scrub.
  lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_SCROLL_CHAIN);
  // Recolour the pressed bar as it is drawn. LVGL has no per-point colour for
  // a bar series, so the draw hook is the only way to give the column itself
  // a pressed state — and the column is what the finger is on, so the column
  // is what should respond. A crosshair over the top would be a different
  // thing sitting in front of the answer.
  lv_obj_add_event_cb(s_chart, chart_draw_part, LV_EVENT_DRAW_PART_BEGIN, NULL);
  s_chartSer = lv_chart_add_series(s_chart, COL_FILL, LV_CHART_AXIS_PRIMARY_Y);
  for (int i = 0; i < CHART_POINTS; i++) s_chartPts[i] = LV_CHART_POINT_NONE;
  lv_chart_set_ext_y_array(s_chart, s_chartSer, s_chartPts);

  // Rate of change rides on a second chart stacked exactly on the first.
  // LVGL sets chart type per chart, not per series, so bars and a line cannot
  // share one — and the two need independent Y ranges anyway: a tank's level
  // and its litres-per-hour have nothing in common but the time axis. This is
  // the same split Victron uses for voltage against current.
  s_rateChart = lv_chart_create(s_detailScr);
  lv_obj_set_size(s_rateChart, CHART_W, CHART_H);
  lv_obj_align(s_rateChart, LV_ALIGN_BOTTOM_MID, 0, -CHART_BOTTOM);
  lv_chart_set_type(s_rateChart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(s_rateChart, CHART_POINTS);
  lv_chart_set_div_line_count(s_rateChart, 0, 0);
  lv_obj_set_style_bg_opa(s_rateChart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_rateChart, 0, 0);
  lv_obj_set_style_size(s_rateChart, 0, LV_PART_INDICATOR);
  lv_obj_set_style_line_width(s_rateChart, 3, LV_PART_ITEMS);
  lv_obj_clear_flag(s_rateChart, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  // Rate reads against its own axis on the right — the same split Victron
  // uses, and the only way a level in percent and a rate in litres per hour
  // can share a plot without one of them being unreadable.
  lv_chart_set_axis_tick(s_rateChart, LV_CHART_AXIS_SECONDARY_Y, 6, 0, 5, 1,
                         true, AXIS_LABEL_W);
  lv_obj_set_style_text_font(s_rateChart, &lv_font_montserrat_20, LV_PART_TICKS);
  lv_obj_set_style_text_color(s_rateChart, COL_WARN, LV_PART_TICKS);
  lv_obj_set_style_pad_left(s_rateChart,  PLOT_INSET, LV_PART_MAIN);
  lv_obj_set_style_pad_right(s_rateChart, PLOT_INSET, LV_PART_MAIN);
  // Tick text is rewritten in the draw hook: the series is held at ten times
  // scale so sub-unit rates survive an integer chart, and the axis must show
  // the real figure rather than that scaling artefact.
  lv_obj_add_event_cb(s_rateChart, rate_draw_part, LV_EVENT_DRAW_PART_BEGIN, NULL);
  s_rateSer = lv_chart_add_series(s_rateChart, COL_WARN, LV_CHART_AXIS_SECONDARY_Y);
  for (int i = 0; i < CHART_POINTS; i++) s_ratePts[i] = LV_CHART_POINT_NONE;
  lv_chart_set_ext_y_array(s_rateChart, s_rateSer, s_ratePts);

  // A single status, centred on the plot. There were two before — a
  // "collecting" beside the title and a "waiting for the hub" along the
  // bottom — which said the same thing twice, in two places, neither of them
  // where the user was looking for the data.
  s_loading = lv_label_create(s_detailScr);
  lv_obj_set_style_text_font(s_loading, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(s_loading, COL_DIM, 0);
  lv_obj_align(s_loading, LV_ALIGN_CENTER, 0, 20);
  lv_label_set_text(s_loading, "Loading");

  // Touch readout. Averages answer "typically"; this answers "that column",
  // which is the question actually being asked of a chart whose interesting
  // days are the ones that differ from the average.
  s_probe = lv_label_create(s_detailScr);
  lv_obj_set_style_text_font(s_probe, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(s_probe, COL_TEXT, 0);
  lv_obj_set_style_bg_color(s_probe, COL_PANEL, 0);
  lv_obj_set_style_bg_opa(s_probe, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s_probe, COL_FILL, 0);
  lv_obj_set_style_border_width(s_probe, 2, 0);
  lv_obj_set_style_pad_all(s_probe, 8, 0);
  lv_obj_align(s_probe, LV_ALIGN_TOP_MID, 0, 96);
  lv_obj_add_flag(s_probe, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_recolor(s_probe, true);

  // Legend, so the two colours are not a guess.
  s_legend = lv_label_create(s_detailScr);
  lv_obj_set_style_text_font(s_legend, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(s_legend, COL_DIM, 0);
  lv_obj_align(s_legend, LV_ALIGN_BOTTOM_MID, 0, -14);
  lv_label_set_text(s_legend, "");

  // s_dSpan removed: it sat at BOTTOM_MID under a legend anchored
  // BOTTOM_LEFT, and a legend long enough to be useful ran straight through
  // it. The window button already names the window, so the only thing worth
  // keeping was the link status, which now rides on the legend line.
  s_dSpan = lv_label_create(s_detailScr);
  lv_obj_set_style_text_font(s_dSpan, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(s_dSpan, COL_DIM, 0);
  lv_label_set_text(s_dSpan, "");
  lv_obj_add_flag(s_dSpan, LV_OBJ_FLAG_HIDDEN);
}

void display_init() {
  s_mainScr = lv_obj_create(NULL);
  lv_obj_t* scr = s_mainScr;
  lv_obj_set_style_bg_color(scr, COL_BG, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // ── Header ──
  lv_obj_t* hdr = lv_obj_create(scr);
  lv_obj_set_size(hdr, SCREEN_WIDTH, HEADER_H);
  lv_obj_set_pos(hdr, 0, 0);
  lv_obj_set_style_bg_color(hdr, COL_BG, 0);
  lv_obj_set_style_border_width(hdr, 0, 0);
  lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_color(hdr, COL_BORDER, 0);
  lv_obj_set_style_pad_all(hdr, 0, 0);
  lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  s_clock = lv_label_create(hdr);
  lv_obj_set_style_text_font(s_clock, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(s_clock, COL_TEXT, 0);
  lv_label_set_text(s_clock, "--:-- UTC");
  lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, 16, 0);

  s_link = lv_label_create(hdr);
  lv_obj_set_style_text_font(s_link, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(s_link, COL_DIM, 0);
  lv_label_set_text(s_link, "SEARCHING");
  lv_obj_align(s_link, LV_ALIGN_RIGHT_MID, -16, 0);

  // ── Tiles ──
  for (int i = 0; i < N_TILES; i++) {
    tile_t& t = s_tile[i];
    t.root = make_tile(scr, i);

    // Label bar — inverted block, like the e-paper's solid header.
    lv_obj_t* bar = lv_obj_create(t.root);
    lv_obj_set_size(bar, TILE_W - 4, BAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, COL_BORDER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    t.label = lv_label_create(bar);
    lv_obj_set_style_text_font(t.label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(t.label, COL_TEXT, 0);
    lv_label_set_text(t.label, i == 2 ? "HOUSE" : "TANK");
    lv_obj_center(t.label);

    // Fill bar — bottom-up, sitting behind the number.
    t.bar = lv_bar_create(t.root);
    lv_obj_set_size(t.bar, TILE_W - 4, TILE_H - BAR_H - 4);
    lv_obj_set_pos(t.bar, 0, BAR_H);
    lv_bar_set_range(t.bar, 0, 100);
    lv_bar_set_value(t.bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(t.bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_radius(t.bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(t.bar, COL_FILL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(t.bar, LV_OPA_40, LV_PART_INDICATOR);
    lv_obj_set_style_radius(t.bar, 0, LV_PART_INDICATOR);

    t.value = lv_label_create(t.root);
    lv_obj_set_style_text_font(t.value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(t.value, COL_TEXT, 0);
    lv_label_set_text(t.value, "--");
    lv_obj_align(t.value, LV_ALIGN_CENTER, 0, i == 2 ? -6 : 12);

    t.sub = lv_label_create(t.root);
    lv_obj_set_style_text_font(t.sub, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t.sub, COL_TEXT, 0);
    lv_label_set_text(t.sub, "");
    lv_obj_align(t.sub, LV_ALIGN_CENTER, 0, 48);

    t.badge = lv_label_create(t.root);
    lv_obj_set_style_text_font(t.badge, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t.badge, COL_WARN, 0);
    lv_obj_set_style_border_color(t.badge, COL_WARN, 0);
    lv_obj_set_style_border_width(t.badge, 2, 0);
    lv_obj_set_style_pad_all(t.badge, 4, 0);
    lv_obj_set_style_radius(t.badge, 3, 0);
    lv_label_set_text(t.badge, "LOW");
    lv_obj_align(t.badge, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_add_flag(t.badge, LV_OBJ_FLAG_HIDDEN);

    // Must come after every child exists, so nothing is left able to
    // intercept a tap intended for the tile.
    children_ignore_touch(t.root);
    lv_obj_add_event_cb(t.root, tile_clicked, LV_EVENT_CLICKED,
                        (void*)(intptr_t)i);
  }

  // Scale marks for the battery tile (100 / 75 / 50) — the visual cue
  // that its gauge does not start at zero.
  const char* marks[3] = { "100", "75", "50" };
  const lv_coord_t frac[3] = { 0, 50, 100 };   // % down the bar area
  for (int i = 0; i < 3; i++) {
    lv_obj_t* m = lv_label_create(s_tile[2].root);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(m, COL_DIM, 0);
    lv_label_set_text(m, marks[i]);
    lv_coord_t barArea = TILE_H - BAR_H - 4;
    lv_coord_t y = BAR_H + 4 + (barArea - 22) * frac[i] / 100;
    lv_obj_set_pos(m, TILE_W - 44, y);
  }
  // These marks were added after the tile loop, so re-run the sweep or
  // they'd be left able to swallow taps on the battery tile.
  children_ignore_touch(s_tile[2].root);

  build_detail_screen();
  lv_scr_load(s_mainScr);
}

// ─── Detail page population ─────────────────────────────────────────────────
static void open_detail(int tileIdx) {
  if (tileIdx < 0 || tileIdx >= N_TILES) return;
  s_openTile = tileIdx;
  // Ask the hub as the page opens. The reply is asynchronous, so the first
  // paint shows the local series and the hub's supersedes it a moment later
  // — better than a blank chart while a round trip completes.
  set_point_count(points_for_window(s_window));
  if (s_loading) {
    lv_label_set_text(s_loading, "Loading");
    lv_obj_clear_flag(s_loading, LV_OBJ_FLAG_HIDDEN);
  }
  s_offsetBuckets = 0;
  espnow_history_request(TILE_DESC[tileIdx].hubMetric, s_window,
                         (uint8_t)s_pointCount, s_offsetBuckets);
  refresh_detail();
  lv_scr_load(s_detailScr);
}

static void show_loading(const char* msg) {
  if (!s_loading) return;
  lv_obj_set_style_text_color(s_loading, COL_DIM, 0);
  lv_label_set_text(s_loading, msg);
  lv_obj_clear_flag(s_loading, LV_OBJ_FLAG_HIDDEN);
  if (s_chart)     lv_obj_add_flag(s_chart, LV_OBJ_FLAG_HIDDEN);
  if (s_rateChart) lv_obj_add_flag(s_rateChart, LV_OBJ_FLAG_HIDDEN);
  if (s_legend)    lv_label_set_text(s_legend, "");
  if (s_probe)     lv_obj_add_flag(s_probe, LV_OBJ_FLAG_HIDDEN);
}

static void request_page() {
  if (s_openTile < 0) return;
  show_loading("Loading");
  espnow_history_request(TILE_DESC[s_openTile].hubMetric, s_window,
                         (uint8_t)s_pointCount, s_offsetBuckets);
  // FORWARD is meaningless at the live edge, and a button that does nothing
  // is worse than one that is visibly unavailable.
  if (s_fwdBtn) {
    if (s_offsetBuckets == 0) lv_obj_add_state(s_fwdBtn, LV_STATE_DISABLED);
    else                      lv_obj_clear_state(s_fwdBtn, LV_STATE_DISABLED);
  }
}

// Paging replaces pinch-zoom: a drag across this chart already means "read
// the values", so overloading it with a pan would make both worse. Whole
// screenfuls also keep every page on the same bucket boundaries.
static void page_older(lv_event_t* e) {
  (void)e;
  uint32_t next = (uint32_t)s_offsetBuckets + page_step();
  s_offsetBuckets = (next > 60000) ? 60000 : (uint16_t)next;
  request_page();
}

static void page_newer(lv_event_t* e) {
  (void)e;
  uint16_t step = page_step();
  s_offsetBuckets = (s_offsetBuckets > step) ? (uint16_t)(s_offsetBuckets - step) : 0;
  request_page();
}

// Cycle 24 h → 30 d → 1 y. One button rather than three: the detail page is
// already full, and the label always says which window is showing.
static void cycle_window(lv_event_t* e) {
  (void)e;
  s_window = (s_window == HIST_WINDOW_24H) ? HIST_WINDOW_30D
           : (s_window == HIST_WINDOW_30D) ? HIST_WINDOW_1Y
                                           : HIST_WINDOW_24H;
  set_point_count(points_for_window(s_window));
  s_offsetBuckets = 0;
  if (s_openTile >= 0)
    espnow_history_request(TILE_DESC[s_openTile].hubMetric, s_window,
                           (uint8_t)s_pointCount, s_offsetBuckets);
  if (s_winLbl) lv_label_set_text(s_winLbl, window_name(s_window));
}

static void refresh_detail() {
  if (s_openTile < 0) return;

  // Re-ask while the page is open and still empty. The request is a single
  // unacknowledged ESP-NOW frame: lose it — or send it in the moment the link
  // is re-probing — and the chart stays blank for as long as the page is up,
  // with nothing on screen to say a reply was ever expected. This runs off
  // the state poll, so it retries about every five seconds at no extra cost.
  if (!espnow_history_ready()) {
    espnow_history_request(TILE_DESC[s_openTile].hubMetric, s_window,
                           (uint8_t)s_pointCount, s_offsetBuckets);
  }
  const int idx = s_openTile;
  const tile_desc_t& d = TILE_DESC[idx];
  char buf[96];

  // Title mirrors the tile's own label so the two screens agree.
  lv_label_set_text(s_dTitle, lv_label_get_text(s_tile[idx].label));


  // Stats over whatever is on screen. Computed from the hub series below
  // rather than kept separately, so the numbers always describe the chart
  // the user is actually looking at.
  // Deliberately not min/max/avg of the level. Max is "however full it was
  // when last filled" and the average level of a tank that gets refilled is a
  // number with no physical meaning — both are already visible in the bars.
  // What the bars do not state is the figure the chart exists to answer:
  // how much per day. That is computed below, once the series is in hand.

  // Series comes from the hub, and only from the hub. This node kept its own
  // rings for a while, which meant two different answers to the same question
  // depending on which one happened to have data — and the local one reset on
  // every power cut. The hub's series is persisted, longer, and the same data
  // the PWA shows. When the link is down there is no history, and the page
  // says so rather than quietly showing a shorter different series.
  int valid = 0;
  s_usingHub = false;
  for (int i = 0; i < s_pointCount; i++) s_pts[i] = HIST_NO_DATA;
  if (espnow_history_ready()) {
    int n = espnow_history_values(s_pts, s_pointCount);
    if (n > 0) {
      // Right-align a short series so the newest point stays at the right
      // edge rather than the chart appearing to run backwards in time.
      if (n < s_pointCount) {
        int shift = s_pointCount - n;
        for (int i = s_pointCount - 1; i >= shift; i--) s_pts[i] = s_pts[i - shift];
        for (int i = 0; i < shift; i++) s_pts[i] = HIST_NO_DATA;
      }
      for (int i = 0; i < s_pointCount; i++)
        if (s_pts[i] != HIST_NO_DATA) valid++;
      s_usingHub = true;
    }
  }
  int16_t* pts = s_pts;

  if (d.fixedRange) {
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
  } else if (valid > 0) {
    int16_t lo = INT16_MAX, hi = INT16_MIN;
    for (int i = 0; i < CHART_POINTS; i++) {
      if (pts[i] == HIST_NO_DATA) continue;
      if (pts[i] < lo) lo = pts[i];
      if (pts[i] > hi) hi = pts[i];
    }
    if (hi <= lo) hi = lo + 1;
    int16_t pad = (hi - lo) / 10 + 1;
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, lo - pad, hi + pad);
  }

  // ── Rate of change ──────────────────────────────────────────────────────
  // The question a tank chart is actually asked is "how fast is it going
  // down", which the absolute trace answers only by eye. Differencing the
  // series and scaling to per-hour answers it directly.
  //
  // Scaled ×10 so a tenth of a unit per hour survives the integer chart:
  // fuel burn is often under 1 %/h and would otherwise quantise to a flat
  // line at zero. The axis labels divide it back out.
  // Each column now spans a whole bucket, so the step between columns is the
  // bucket width, not the tier's sample interval.
  const float bucketH = (s_window == HIST_WINDOW_1Y)  ? (24.0f * 30.0f)
                      : (s_window == HIST_WINDOW_30D) ? 24.0f
                                                      : 1.0f;
  const float perHour = 1.0f / bucketH;
  // Litres per hour when the hub has told us the tank's size — percent per
  // hour cannot answer "how much fuel", which is the whole point.
  const float toUnits = (idx < 2 && s_tankCapacityL[idx] > 0)
                      ? (float)s_tankCapacityL[idx] / 100.0f : 1.0f;

  int16_t rateLo = 0, rateHi = 0;
  bool haveRate = false;
  for (int i = 0; i < s_pointCount; i++) {
    if (i == 0 || pts[i] == HIST_NO_DATA || pts[i - 1] == HIST_NO_DATA) {
      s_ratePts[i] = LV_CHART_POINT_NONE;
      continue;
    }
    float r = (float)(pts[i] - pts[i - 1]) * perHour * toUnits * 10.0f;
    if (r >  3000) r =  3000;      // keep a spike from flattening everything
    if (r < -3000) r = -3000;
    int16_t ri = (int16_t)r;
    s_ratePts[i] = ri;
    if (!haveRate) { rateLo = rateHi = ri; haveRate = true; }
    if (ri < rateLo) rateLo = ri;
    if (ri > rateHi) rateHi = ri;
  }
  if (haveRate) {
    // Symmetric about zero so "filling" and "emptying" read the same way and
    // the zero crossing sits on a fixed line rather than wandering.
    int16_t mag = (rateHi > -rateLo ? rateHi : -rateLo);
    if (mag < 10) mag = 10;
    lv_chart_set_range(s_rateChart, LV_CHART_AXIS_SECONDARY_Y, -mag, mag);
    // Per bucket, not per hour — the rate is scaled to the column width, so
    // labelling it /h on a day-per-column view stated the wrong quantity
    // while showing the right number.
    const char* rBase = (idx < 2 && s_tankCapacityL[idx] > 0) ? "L" : "%";
    char rUnit[16];
    snprintf(rUnit, sizeof(rUnit), "%s/%s", rBase, bucket_name(s_window));
    char lbuf[72];
    snprintf(lbuf, sizeof(lbuf),
             "#4A9EFF bars# level %% per %s    #FFB020 line# rate %s",
             bucket_name(s_window), rUnit);
    lv_label_set_recolor(s_legend, true);
    lv_label_set_text(s_legend, lbuf);
  } else {
    lv_label_set_text(s_legend, "");
  }
  // Stats worth having: the lowest the level reached, and the mean rate over
  // the window — "how much water do we use per day", stated rather than
  // estimated off a line.
  lv_chart_refresh(s_rateChart);

  // Write straight into the array LVGL is already pointing at, then
  // invalidate once.
  for (int i = 0; i < s_pointCount; i++) {
    s_chartPts[i] = (pts[i] == HIST_NO_DATA) ? LV_CHART_POINT_NONE
                                             : (lv_coord_t)pts[i];
  }
  lv_chart_refresh(s_chart);

  // Time span covered, and where it came from — a chart that silently falls
  // back to a shorter local series would otherwise look like data loss.
  // Charts are hidden outright while there is nothing to draw, rather than
  // left as an empty frame with a word floating over it — an axis with no
  // series still reads as a chart, so the message looked like an overlay on
  // real data instead of a replacement for it.
  const bool linkUp  = (espnow_link_state() == LINK_UP);
  const bool replied = espnow_history_ready();
  if (valid > 0) {
    lv_obj_add_flag(s_loading, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_rateChart, LV_OBJ_FLAG_HIDDEN);
  } else {
    // "Loading" only while a reply is genuinely outstanding. A page earlier
    // than the record comes back full of gaps, and saying "Loading" forever
    // in that case claims work is in progress that finished long ago.
    const char* msg = !linkUp  ? "No hub connection"
                    : !replied ? "Loading"
                               : "No data for this period";
    lv_obj_set_style_text_color(s_loading, linkUp ? COL_DIM : COL_ALARM, 0);
    lv_label_set_text(s_loading, msg);
    lv_obj_clear_flag(s_loading, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_chart, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rateChart, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_legend, "");
  }
}

void display_update(const display_state_t& state) {
  char buf[40];

  s_last     = state;
  s_haveLast = true;
  for (int i = 0; i < 2; i++)
    s_tankCapacityL[i] = (i < state.n_tanks) ? state.tanks[i].capacity_l : 0;

  // ── Header clock ──
  if (state.utc_days == DISPLAY_UTC_DAYS_NONE) {
    uint32_t h = state.hub_uptime_s / 3600;
    uint32_t m = (state.hub_uptime_s % 3600) / 60;
    snprintf(buf, sizeof(buf), "HUB UP %luh %02lum",
             (unsigned long)h, (unsigned long)m);
  } else {
    int y, mo, d;
    days_to_ymd(state.utc_days, y, mo, d);
    int h = (int)(state.utc_seconds / 3600);
    int m = (int)((state.utc_seconds - h * 3600) / 60);
    snprintf(buf, sizeof(buf), "%02d:%02d UTC  %02d/%02d", h, m, d, mo);
  }
  lv_label_set_text(s_clock, buf);

  // ── Tank tiles ──
  for (int i = 0; i < 2; i++) {
    tile_t& t = s_tile[i];
    const bool has = (i < state.n_tanks) &&
                     (state.tanks[i].level_pct != DISPLAY_LEVEL_NO_DATA);
    if (has) {
      lv_label_set_text(t.label, fluid_name(state.tanks[i].fluid_type));
      lv_bar_set_value(t.bar, state.tanks[i].level_pct, LV_ANIM_ON);
      snprintf(buf, sizeof(buf), "%d%%", state.tanks[i].level_pct);
      lv_label_set_text(t.value, buf);
    } else {
      lv_label_set_text(t.label, (i < state.n_tanks)
                        ? fluid_name(state.tanks[i].fluid_type) : "TANK");
      lv_bar_set_value(t.bar, 0, LV_ANIM_OFF);
      lv_label_set_text(t.value, "--");
    }
  }

  // ── House battery ──
  tile_t& b = s_tile[2];
  if (state.batt_soc_pct != DISPLAY_BATT_NO_DATA) {
    const uint8_t soc = state.batt_soc_pct;
    const bool charging = (state.batt_flags & DISPLAY_BATT_FLAG_CHARGING) != 0;

    // Visual fill maps the usable 50–100% window; the number stays true SoC.
    int fill = (soc > 50) ? (soc - 50) * 2 : 0;
    lv_bar_set_value(b.bar, fill, LV_ANIM_ON);
    lv_obj_set_style_bg_color(b.bar, soc < 50 ? COL_WARN : COL_FILL,
                              LV_PART_INDICATOR);

    snprintf(buf, sizeof(buf), "%d%%", soc);
    lv_label_set_text(b.value, buf);

    const float amps = state.batt_current_dA / 10.0f;
    snprintf(buf, sizeof(buf), "%s%.1f A  %.2f V",
             charging ? "+" : "-", fabsf(amps),
             state.batt_voltage_cV / 100.0f);
    lv_label_set_text(b.sub, buf);
    lv_obj_set_style_text_color(b.sub, charging ? COL_OK : COL_TEXT, 0);

    if (soc < 50) lv_obj_clear_flag(b.badge, LV_OBJ_FLAG_HIDDEN);
    else          lv_obj_add_flag(b.badge, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_bar_set_value(b.bar, 0, LV_ANIM_OFF);
    lv_label_set_text(b.value, "--");
    lv_label_set_text(b.sub, "no monitor");
    lv_obj_set_style_text_color(b.sub, COL_DIM, 0);
    lv_obj_add_flag(b.badge, LV_OBJ_FLAG_HIDDEN);
  }

  // Keep an open history page live rather than frozen at its open time.
  if (s_openTile >= 0) refresh_detail();
}

void display_set_link(link_state_t st, uint32_t ageSeconds) {
  // Called from every loop iteration (~5 ms). lv_label_set_text always
  // invalidates, so writing unconditionally would redraw the header
  // hundreds of times a second and starve the rest of the UI. Only touch
  // the label when the rendered text would actually differ — the age is
  // only shown at second resolution, so that's the comparison granularity.
  static link_state_t s_lastSt  = (link_state_t)-1;
  static uint32_t     s_lastAge = UINT32_MAX;
  if (st == s_lastSt && ageSeconds == s_lastAge) return;
  s_lastSt  = st;
  s_lastAge = ageSeconds;

  char buf[32];
  switch (st) {
    case LINK_UP:
      // Sub-poll ages are noise; just show the link is healthy.
      if (ageSeconds <= (POLL_INTERVAL_MS / 1000UL) * 2) {
        lv_label_set_text(s_link, "HUB OK");
      } else {
        snprintf(buf, sizeof(buf), "HUB OK  %lus", (unsigned long)ageSeconds);
        lv_label_set_text(s_link, buf);
      }
      lv_obj_set_style_text_color(s_link, COL_OK, 0);
      break;
    case LINK_LOST:
      snprintf(buf, sizeof(buf), "NO HUB  %lus", (unsigned long)ageSeconds);
      lv_label_set_text(s_link, buf);
      lv_obj_set_style_text_color(s_link, COL_ALARM, 0);
      break;
    case LINK_SEARCHING:
    default:
      lv_label_set_text(s_link, "SEARCHING");
      lv_obj_set_style_text_color(s_link, COL_DIM, 0);
      break;
  }
}
