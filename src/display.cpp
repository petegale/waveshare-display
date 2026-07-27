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
  // Base objects are clickable and scrollable by default, and the theme
  // paints a pressed state — so touching a tile repaints it and reads as
  // a flicker. This is a readout, not a control: nothing here is meant
  // to react to touch (yet), so take both flags off.
  lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  return t;
}

void display_init() {
  lv_obj_t* scr = lv_scr_act();
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
}

void display_update(const display_state_t& state) {
  char buf[40];

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
