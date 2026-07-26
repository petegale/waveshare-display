#include "display.h"
#include "config.h"
#include <lvgl.h>
#include <stdio.h>
#include <math.h>

static lv_obj_t *s_tank_arc[MAX_DISPLAY_TANKS];
static lv_obj_t *s_tank_label[MAX_DISPLAY_TANKS];
static lv_obj_t *s_tank_title[MAX_DISPLAY_TANKS];

static lv_obj_t *s_batt_soc_label;
static lv_obj_t *s_batt_voltage_label;
static lv_obj_t *s_batt_current_label;

static const char *fluid_name(uint8_t fluid_type) {
    switch (fluid_type) {
        case DISPLAY_FLUID_FUEL:     return "Fuel";
        case DISPLAY_FLUID_WATER:    return "Water";
        case DISPLAY_FLUID_GREY:     return "Grey";
        case DISPLAY_FLUID_BLACK:    return "Black";
        case DISPLAY_FLUID_OIL:      return "Oil";
        case DISPLAY_FLUID_LIVEWELL: return "Live Well";
        default:                     return "Tank";
    }
}

void display_init() {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lv_coord_t arc_size = 180;
    lv_coord_t gap = 40;
    lv_coord_t total_w = MAX_DISPLAY_TANKS * arc_size + (MAX_DISPLAY_TANKS - 1) * gap;
    lv_coord_t start_x = (SCREEN_WIDTH - total_w) / 2;

    for (int i = 0; i < MAX_DISPLAY_TANKS; i++) {
        lv_obj_t *arc = lv_arc_create(scr);
        lv_obj_set_size(arc, arc_size, arc_size);
        lv_obj_set_pos(arc, start_x + i * (arc_size + gap), 60);
        lv_arc_set_rotation(arc, 135);
        lv_arc_set_bg_angles(arc, 0, 270);
        lv_arc_set_range(arc, 0, 100);
        lv_arc_set_value(arc, 0);
        lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        s_tank_arc[i] = arc;

        lv_obj_t *label = lv_label_create(arc);
        lv_obj_center(label);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
        lv_label_set_text(label, "--");
        s_tank_label[i] = label;

        lv_obj_t *title = lv_label_create(scr);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
        lv_obj_align_to(title, arc, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
        lv_label_set_text(title, "Tank");
        s_tank_title[i] = title;
    }

    lv_obj_t *batt_panel = lv_obj_create(scr);
    lv_obj_set_size(batt_panel, SCREEN_WIDTH - 80, 120);
    lv_obj_align(batt_panel, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_flex_flow(batt_panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(batt_panel, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_batt_soc_label = lv_label_create(batt_panel);
    lv_obj_set_style_text_font(s_batt_soc_label, &lv_font_montserrat_24, 0);
    lv_label_set_text(s_batt_soc_label, "SoC: --%");

    s_batt_voltage_label = lv_label_create(batt_panel);
    lv_obj_set_style_text_font(s_batt_voltage_label, &lv_font_montserrat_24, 0);
    lv_label_set_text(s_batt_voltage_label, "-- V");

    s_batt_current_label = lv_label_create(batt_panel);
    lv_obj_set_style_text_font(s_batt_current_label, &lv_font_montserrat_24, 0);
    lv_label_set_text(s_batt_current_label, "-- A");
}

void display_update(const display_state_t &state) {
    char buf[32];

    for (int i = 0; i < MAX_DISPLAY_TANKS; i++) {
        if (i < state.n_tanks && state.tanks[i].level_pct != DISPLAY_LEVEL_NO_DATA) {
            lv_arc_set_value(s_tank_arc[i], state.tanks[i].level_pct);
            snprintf(buf, sizeof(buf), "%d%%", state.tanks[i].level_pct);
            lv_label_set_text(s_tank_label[i], buf);
            lv_label_set_text(s_tank_title[i], fluid_name(state.tanks[i].fluid_type));
        } else {
            lv_arc_set_value(s_tank_arc[i], 0);
            lv_label_set_text(s_tank_label[i], "--");
            lv_label_set_text(s_tank_title[i], "No Data");
        }
    }

    if (state.batt_soc_pct != DISPLAY_BATT_NO_DATA) {
        snprintf(buf, sizeof(buf), "SoC: %d%%", state.batt_soc_pct);
        lv_label_set_text(s_batt_soc_label, buf);

        snprintf(buf, sizeof(buf), "%.2f V", state.batt_voltage_cV / 100.0f);
        lv_label_set_text(s_batt_voltage_label, buf);

        bool charging = state.batt_flags & DISPLAY_BATT_FLAG_CHARGING;
        snprintf(buf, sizeof(buf), "%s%.1f A", charging ? "+" : "-", fabsf(state.batt_current_dA / 10.0f));
        lv_label_set_text(s_batt_current_label, buf);
    } else {
        lv_label_set_text(s_batt_soc_label, "SoC: --%");
        lv_label_set_text(s_batt_voltage_label, "-- V");
        lv_label_set_text(s_batt_current_label, "-- A");
    }
}
