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
#include "history.h"
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
#define COL_DIM       lv_color_hex(0x8A9099)
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
static lv_obj_t*        s_dNow   = nullptr;
static lv_obj_t*        s_dStats = nullptr;
static lv_obj_t*        s_dSpan  = nullptr;
static lv_obj_t*        s_chart  = nullptr;
static lv_chart_series_t* s_chartSer = nullptr;
static lv_obj_t* s_winBtn = nullptr;
static lv_obj_t* s_winLbl = nullptr;

// Which metric the detail page is currently showing, or -1 when closed.
static int s_openTile = -1;

// Last state we rendered — the detail page needs it to redraw on update.
static display_state_t s_last     = {};
static bool            s_haveLast = false;

#define CHART_POINTS 120

// Backing store handed to LVGL via lv_chart_set_ext_y_array. Populating
// the series with lv_chart_set_value_by_id() instead would invalidate the
// whole chart once per point — 120 redraws to draw one line.
static lv_coord_t s_chartPts[CHART_POINTS];

// Per-tile descriptor: which history metric backs it, and how to print it.
typedef struct {
  hist_metric_t metric;
  const char*   unit;
  float         scale;      // raw → display units
  int           decimals;
  bool          fixedRange;  // percentages pin the Y axis to 0..100
  uint8_t       hubMetric;   // HIST_METRIC_* — what to ask the hub for
} tile_desc_t;

static const tile_desc_t TILE_DESC[N_TILES] = {
  { HIST_TANK0,    "%", 1.0f,  0, true, HIST_METRIC_TANK0    },
  { HIST_TANK1,    "%", 1.0f,  0, true, HIST_METRIC_TANK1    },
  { HIST_BATT_SOC, "%", 1.0f,  0, true, HIST_METRIC_BATT_SOC },
};

// Which window the detail page is showing. The hub keeps 24 h / 30 d / 1 y;
// this node's own rings only reach 24 h, and are the fallback when the link
// is down — so the longer windows are only offered when the hub answers.
static uint8_t s_window = HIST_WINDOW_24H;
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
  lv_obj_align(s_dTitle, LV_ALIGN_TOP_LEFT, 20, 14);

  s_dNow = lv_label_create(s_detailScr);
  lv_obj_set_style_text_font(s_dNow, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(s_dNow, COL_TEXT, 0);
  lv_label_set_text(s_dNow, "--");
  lv_obj_align(s_dNow, LV_ALIGN_TOP_LEFT, 20, 46);

  s_dStats = lv_label_create(s_detailScr);
  lv_obj_set_style_text_font(s_dStats, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(s_dStats, COL_DIM, 0);
  lv_label_set_text(s_dStats, "");
  lv_obj_align(s_dStats, LV_ALIGN_TOP_LEFT, 200, 60);

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
  lv_obj_set_size(s_winBtn, 190, 56);
  lv_obj_align(s_winBtn, LV_ALIGN_TOP_RIGHT, -152, 14);
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

  s_chart = lv_chart_create(s_detailScr);
  lv_obj_set_size(s_chart, SCREEN_WIDTH - 100, 300);
  lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 10, -46);
  lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(s_chart, CHART_POINTS);
  lv_chart_set_div_line_count(s_chart, 5, 6);
  lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
  lv_obj_set_style_bg_color(s_chart, COL_PANEL, 0);
  lv_obj_set_style_border_color(s_chart, COL_BORDER, 0);
  lv_obj_set_style_border_width(s_chart, 2, 0);
  lv_obj_set_style_line_color(s_chart, COL_BORDER, LV_PART_MAIN);
  lv_obj_set_style_size(s_chart, 0, LV_PART_INDICATOR);   // no point dots
  lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  s_chartSer = lv_chart_add_series(s_chart, COL_FILL, LV_CHART_AXIS_PRIMARY_Y);
  for (int i = 0; i < CHART_POINTS; i++) s_chartPts[i] = LV_CHART_POINT_NONE;
  lv_chart_set_ext_y_array(s_chart, s_chartSer, s_chartPts);

  s_dSpan = lv_label_create(s_detailScr);
  lv_obj_set_style_text_font(s_dSpan, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_dSpan, COL_DIM, 0);
  lv_label_set_text(s_dSpan, "");
  lv_obj_align(s_dSpan, LV_ALIGN_BOTTOM_MID, 0, -16);
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
    lv_obj_set_style_text_font(m, &lv_font_montserrat_14, 0);
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
  espnow_history_request(TILE_DESC[tileIdx].hubMetric, s_window, CHART_POINTS);
  refresh_detail();
  lv_scr_load(s_detailScr);
}

// Cycle 24 h → 30 d → 1 y. One button rather than three: the detail page is
// already full, and the label always says which window is showing.
static void cycle_window(lv_event_t* e) {
  (void)e;
  s_window = (s_window == HIST_WINDOW_24H) ? HIST_WINDOW_30D
           : (s_window == HIST_WINDOW_30D) ? HIST_WINDOW_1Y
                                           : HIST_WINDOW_24H;
  if (s_openTile >= 0)
    espnow_history_request(TILE_DESC[s_openTile].hubMetric, s_window, CHART_POINTS);
  if (s_winLbl) lv_label_set_text(s_winLbl, window_name(s_window));
}

static void refresh_detail() {
  if (s_openTile < 0) return;
  const int idx = s_openTile;
  const tile_desc_t& d = TILE_DESC[idx];
  char buf[96];

  // Title mirrors the tile's own label so the two screens agree.
  lv_label_set_text(s_dTitle, lv_label_get_text(s_tile[idx].label));

  // Current value, straight from the last state rather than the series,
  // so it matches the tile exactly even mid-bucket.
  int16_t nowRaw = HIST_NO_DATA;
  if (s_haveLast) {
    if (idx < 2) {
      if (idx < s_last.n_tanks &&
          s_last.tanks[idx].level_pct != DISPLAY_LEVEL_NO_DATA)
        nowRaw = s_last.tanks[idx].level_pct;
    } else if (s_last.batt_soc_pct != DISPLAY_BATT_NO_DATA) {
      nowRaw = s_last.batt_soc_pct;
    }
  }
  fmt_metric(buf, sizeof(buf), idx, nowRaw);
  lv_label_set_text(s_dNow, buf);

  // Stats over the stored window.
  int16_t mn, mx, av;
  if (history_stats(d.metric, &mn, &mx, &av)) {
    char sMn[16], sMx[16], sAv[16];
    fmt_metric(sMn, sizeof(sMn), idx, mn);
    fmt_metric(sMx, sizeof(sMx), idx, mx);
    fmt_metric(sAv, sizeof(sAv), idx, av);
    snprintf(buf, sizeof(buf), "min %s   max %s   avg %s", sMn, sMx, sAv);
  } else {
    snprintf(buf, sizeof(buf), "collecting…");
  }
  lv_label_set_text(s_dStats, buf);

  // Series: hub first, local rings as the fallback. The hub's series is
  // longer, survives reboots, and is the same data the PWA shows; the local
  // one is what this node still has when the link is down.
  static int16_t pts[CHART_POINTS];
  int valid = 0;
  s_usingHub = false;
  if (espnow_history_ready()) {
    int n = espnow_history_values(pts, CHART_POINTS);
    if (n > 0) {
      // Right-align a short series so the newest point stays at the right
      // edge rather than the chart appearing to run backwards in time.
      if (n < CHART_POINTS) {
        int shift = CHART_POINTS - n;
        for (int i = CHART_POINTS - 1; i >= shift; i--) pts[i] = pts[i - shift];
        for (int i = 0; i < shift; i++) pts[i] = HIST_NO_DATA;
      }
      for (int i = 0; i < CHART_POINTS; i++)
        if (pts[i] != HIST_NO_DATA) valid++;
      s_usingHub = true;
    }
  }
  if (!s_usingHub) valid = history_series(d.metric, pts, CHART_POINTS);

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

  // Write straight into the array LVGL is already pointing at, then
  // invalidate once.
  for (int i = 0; i < CHART_POINTS; i++) {
    s_chartPts[i] = (pts[i] == HIST_NO_DATA) ? LV_CHART_POINT_NONE
                                             : (lv_coord_t)pts[i];
  }
  lv_chart_refresh(s_chart);

  // Time span covered, and where it came from — a chart that silently falls
  // back to a shorter local series would otherwise look like data loss.
  if (s_usingHub) {
    snprintf(buf, sizeof(buf), "%s · from hub", window_name(s_window));
  } else {
    uint32_t mins = history_span_minutes(d.metric);
    if (mins < 2)        snprintf(buf, sizeof(buf), "no history yet — one sample per minute");
    else if (mins < 120) snprintf(buf, sizeof(buf), "last %lu minutes (local)", (unsigned long)mins);
    else                 snprintf(buf, sizeof(buf), "last %.1f hours (local)", mins / 60.0f);
  }
  lv_label_set_text(s_dSpan, buf);
}

void display_update(const display_state_t& state) {
  char buf[40];

  s_last     = state;
  s_haveLast = true;

  // Feed the series. Metrics with no data this round are simply not fed,
  // so the bucket records a gap rather than a fabricated zero.
  for (int i = 0; i < 2; i++) {
    if (i < state.n_tanks && state.tanks[i].level_pct != DISPLAY_LEVEL_NO_DATA)
      history_add(i == 0 ? HIST_TANK0 : HIST_TANK1, state.tanks[i].level_pct);
  }
  if (state.batt_soc_pct != DISPLAY_BATT_NO_DATA) {
    history_add(HIST_BATT_SOC, state.batt_soc_pct);
    history_add(HIST_BATT_CURRENT, state.batt_current_dA);
  }

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
