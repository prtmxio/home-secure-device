#include <string.h>

#include "screens.h"
#include "images.h"
#include "fonts.h"
#include "actions.h"
#include "vars.h"
#include "styles.h"
#include "ui.h"

objects_t objects;

static const char *screen_names[] = { "Main" };
static const char *object_names[] = {
    "main", "sensor_data_label", "hub_location_label", "sensor_list_label"
};

lv_obj_t *tick_value_change_obj;

/*
 * Layout for 240 x 320 ILI9341 (portrait), background #EE1C25
 *
 *  [0]  Header          — logo (bg deco) + "GLAZIA" + "Hub"   y: 0..67
 *  [1]  Sensor Data     — incoming data + hub location         y: 68..137
 *  [2]  Critical Toggle — centered button                      y: 147..182
 *  [3]  Sensor List     — scrollable sensor status table       y: 192..311
 */

void create_screen_main()
{
    void *flowState = getFlowState(0, 0);
    (void)flowState;

    // ── Root screen ────────────────────────────────────────────────────────────
    lv_obj_t *obj = lv_obj_create(0);
    objects.main = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 240, 320);
    lv_obj_set_style_bg_color(obj, lv_color_make(238, 28, 37),  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj,   LV_OPA_COVER,                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 0,                        LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(obj,  0,                            LV_PART_MAIN | LV_STATE_DEFAULT);

    {
        lv_obj_t *parent_obj = obj;

        // ── Glazia logo — left side of header ────────────────────────────────
        {
            lv_obj_t *img = lv_img_create(parent_obj);
            lv_obj_set_pos(img, 5, 6);
            lv_img_set_src(img, &img_glazia_logo);
            lv_img_set_zoom(img, 40);       // 40/256 * 350px ≈ 55px square
            lv_img_set_pivot(img, 0, 0);    // scale from top-left, not center
        }

        // ── "GLAZIA" title — right side ───────────────────────────────────────
        {
            lv_obj_t *lbl = lv_label_create(parent_obj);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_26,  LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(lbl, lv_color_white(),        LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(lbl, "GLAZIA");
            lv_obj_align(lbl, LV_ALIGN_TOP_RIGHT, -8, 8);
        }

        // ── "Hub" subtitle — right side ───────────────────────────────────────
        {
            lv_obj_t *lbl = lv_label_create(parent_obj);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14,   LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(lbl, lv_color_make(255,180,180), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(lbl, "Hub");
            lv_obj_align(lbl, LV_ALIGN_TOP_RIGHT, -8, 42);
        }

        // ── Sensor Data Panel ─────────────────────────────────────────────────
        {
            lv_obj_t *panel = lv_obj_create(parent_obj);
            lv_obj_set_pos(panel, 8, 68);
            lv_obj_set_size(panel, 224, 70);
            lv_obj_set_style_bg_color(panel, lv_color_make(20, 20, 20),    LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(panel, LV_OPA_COVER,                   LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(panel, lv_color_make(238,28,37), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(panel, 1,                        LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(panel, 6,                              LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_all(panel, 8,                             LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

            // Incoming sensor data — updated by STATUS: command
            {
                lv_obj_t *lbl = lv_label_create(panel);
                objects.sensor_data_label = lbl;
                lv_label_set_text(lbl, "Waiting for data...");
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
                lv_obj_set_width(lbl, 208);
                lv_obj_set_style_text_color(lbl, lv_color_white(),          LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10,     LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
            }

            // Sensor location — updated by SENSOR_LOC: command
            // Pinned to panel bottom so sensor_data wrapping never causes overlap
            {
                lv_obj_t *lbl = lv_label_create(panel);
                objects.hub_location_label = lbl;
                lv_label_set_text(lbl, "Sensor: --");
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
                lv_obj_set_width(lbl, 208);
                lv_obj_set_style_text_color(lbl, lv_color_make(200,200,200), LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10,      LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_align(lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
            }
        }

        // ── Critical Toggle — ON while hub is powered ─────────────────────────
        {
            lv_obj_t *sw = lv_switch_create(parent_obj);
            lv_obj_set_size(sw, 52, 26);
            lv_obj_align(sw, LV_ALIGN_TOP_MID, 0, 148);
            lv_obj_add_state(sw, LV_STATE_CHECKED);         // hub on = always ON
            lv_obj_clear_flag(sw, LV_OBJ_FLAG_CLICKABLE);

            // Indicator (the sliding fill): blue when ON, light-grey when OFF
            lv_obj_set_style_bg_color(sw, lv_color_make(30, 144, 255),
                                      LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER,
                                    LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_set_style_bg_color(sw, lv_color_make(210, 210, 210),
                                      LV_PART_INDICATOR | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER,
                                    LV_PART_INDICATOR | LV_STATE_DEFAULT);
            // Knob: always white
            lv_obj_set_style_bg_color(sw, lv_color_white(),
                                      LV_PART_KNOB | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER,
                                    LV_PART_KNOB | LV_STATE_DEFAULT);

            // Label below
            {
                lv_obj_t *lbl = lv_label_create(parent_obj);
                lv_label_set_text(lbl, "Critical Toggle");
                lv_obj_set_style_text_color(lbl, lv_color_white(),      LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 178);
            }
        }

        // ── Sensor List Panel — scrollable ────────────────────────────────────
        {
            lv_obj_t *panel = lv_obj_create(parent_obj);
            lv_obj_set_pos(panel, 8, 192);
            lv_obj_set_size(panel, 224, 120);
            lv_obj_set_style_bg_color(panel, lv_color_make(20, 20, 20),    LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(panel, LV_OPA_COVER,                   LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(panel, lv_color_make(238,28,37), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(panel, 1,                        LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(panel, 6,                              LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_all(panel, 8,                             LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_scroll_dir(panel, LV_DIR_VER);

            // Sensor list: "Name : #00FF00 Online#" / "#FF0000 Offline#"
            {
                lv_obj_t *lbl = lv_label_create(panel);
                objects.sensor_list_label = lbl;
                lv_label_set_text(lbl, "No sensors paired yet.\nPress hub button to pair.");
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
                lv_label_set_recolor(lbl, true);
                lv_obj_set_width(lbl, 208);
                lv_obj_set_style_text_color(lbl, lv_color_make(200,200,200), LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10,      LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 4);
            }
        }
    }

    tick_screen_main();
}

void tick_screen_main()
{
    void *flowState = getFlowState(0, 0);
    (void)flowState;
}

typedef void (*tick_screen_func_t)();
tick_screen_func_t tick_screen_funcs[] = {
    tick_screen_main,
};

void tick_screen(int screen_index)
{
    tick_screen_funcs[screen_index]();
}

void tick_screen_by_id(enum ScreensEnum screenId)
{
    tick_screen_funcs[screenId - 1]();
}

//
// Styles
//

static const char *style_names[] = { "local_style" };

extern void add_style(lv_obj_t *obj, int32_t styleIndex);
extern void remove_style(lv_obj_t *obj, int32_t styleIndex);

//
// Fonts actually used by this screen
//

ext_font_desc_t fonts[] = {
#if LV_FONT_MONTSERRAT_10
    { "MONTSERRAT_10", &lv_font_montserrat_10 },
#endif
#if LV_FONT_MONTSERRAT_12
    { "MONTSERRAT_12", &lv_font_montserrat_12 },
#endif
#if LV_FONT_MONTSERRAT_14
    { "MONTSERRAT_14", &lv_font_montserrat_14 },
#endif
#if LV_FONT_MONTSERRAT_26
    { "MONTSERRAT_26", &lv_font_montserrat_26 },
#endif
};

void create_screens()
{
    eez_flow_init_styles(add_style, remove_style);
    eez_flow_init_style_names(style_names, sizeof(style_names) / sizeof(const char *));

    eez_flow_init_fonts(fonts, sizeof(fonts) / sizeof(ext_font_desc_t));

    lv_disp_t *dispp = lv_disp_get_default();
    lv_theme_t *theme = lv_theme_default_init(dispp,
        lv_palette_main(LV_PALETTE_RED),
        lv_palette_main(LV_PALETTE_GREY),
        true, LV_FONT_DEFAULT);
    lv_disp_set_theme(dispp, theme);

    eez_flow_init_screen_names(screen_names, sizeof(screen_names) / sizeof(const char *));
    eez_flow_init_object_names(object_names, sizeof(object_names) / sizeof(const char *));

    create_screen_main();
}
