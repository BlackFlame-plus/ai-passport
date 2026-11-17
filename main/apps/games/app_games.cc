// Games hub: one launcher card, three vertical rows, no gameplay changes.
#include "apps/games/app_games.h"
#include "apps/games/games_menu.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "fonts/app_fonts.h"
#include "launcher/app_registry.h"
#include "ui_pixel.h"

namespace {

constexpr int kRowY0 = 48;
constexpr int kRowH = 52;
constexpr int kRowGap = 8;

// Same-session selection only; start() must not reset this.
size_t s_selected = 0;

lv_obj_t *s_screen = nullptr;
lv_obj_t *s_rows[GAMES_MENU_COUNT] = {};
lv_obj_t *s_labels[GAMES_MENU_COUNT] = {};
lv_obj_t *s_footer = nullptr;
ui_pixel_battery_t s_battery{};

void refresh() {
    if (!s_screen) return;
    if (s_selected >= GAMES_MENU_COUNT) s_selected = 0;
    ui_pixel_battery_set(&s_battery, bsp_battery_soc());
    const games_menu_entry_t *entries = games_menu_entries();
    for (size_t i = 0; i < GAMES_MENU_COUNT; ++i) {
        const bool on = i == s_selected;
        ui_pixel_bg_color(s_rows[i], on ? UI_ACCENT : UI_BG);
        ui_pixel_text_color(s_labels[i], on ? UI_ON_ACCENT : UI_TEXT);
        ui_pixel_label_set_text(s_labels[i], entries[i].title);
    }
    ui_pixel_label_set_text(s_footer, "上/下选择 · 确认进入\n长按确认返回");
}

void start() {
    if (!bsp_lvgl_lock(1000)) return;
    if (s_selected >= GAMES_MENU_COUNT) s_selected = 0;

    s_screen = lv_obj_create(nullptr);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    ui_pixel_background(s_screen);

    lv_obj_t *top = ui_pixel_top_bar(s_screen);
    lv_obj_t *title = ui_pixel_label(top, "游戏", &buddy_font_16, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, 0);
    s_battery = ui_pixel_battery_create(top, UI_STATUS_BATTERY_X, 6, UI_TEXT, &lv_font_montserrat_14);

    const games_menu_entry_t *entries = games_menu_entries();
    for (size_t i = 0; i < GAMES_MENU_COUNT; ++i) {
        const int y = kRowY0 + static_cast<int>(i) * (kRowH + kRowGap);
        s_rows[i] = lv_obj_create(s_screen);
        lv_obj_set_size(s_rows[i], 216, kRowH);
        lv_obj_set_pos(s_rows[i], 12, y);
        lv_obj_set_style_radius(s_rows[i], 0, 0);
        lv_obj_set_style_border_width(s_rows[i], 0, 0);
        lv_obj_set_style_pad_hor(s_rows[i], 12, 0);
        lv_obj_set_style_pad_ver(s_rows[i], 0, 0);
        lv_obj_set_style_bg_opa(s_rows[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_SCROLLABLE);

        s_labels[i] = ui_pixel_label(s_rows[i], entries[i].title, &buddy_font_16, UI_TEXT);
        lv_obj_align(s_labels[i], LV_ALIGN_LEFT_MID, 0, 0);
        lv_label_set_long_mode(s_labels[i], LV_LABEL_LONG_CLIP);
    }

    s_footer = ui_pixel_footer(s_screen, "", "", &buddy_font_16);
    lv_obj_set_height(s_footer, 42);
    lv_label_set_long_mode(s_footer, LV_LABEL_LONG_WRAP);

    refresh();
    lv_screen_load(s_screen);
    bsp_lvgl_unlock();
}

void stop() {
    if (!bsp_lvgl_lock(1000)) return;
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = nullptr;
    }
    for (size_t i = 0; i < GAMES_MENU_COUNT; ++i) {
        s_rows[i] = nullptr;
        s_labels[i] = nullptr;
    }
    s_footer = nullptr;
    bsp_lvgl_unlock();
}

void on_key(uint8_t btn, app_btn_event_t event, uint16_t) {
    if (event != BTN_EVT_CLICK) return;
    if (btn == BSP_BTN_UP) {
        if (!bsp_lvgl_lock(500)) return;
        s_selected = games_menu_prev(s_selected);
        refresh();
        bsp_lvgl_unlock();
        return;
    }
    if (btn == BSP_BTN_DOWN) {
        if (!bsp_lvgl_lock(500)) return;
        s_selected = games_menu_next(s_selected);
        refresh();
        bsp_lvgl_unlock();
        return;
    }
    if (btn == BSP_BTN_OK) {
        app_registry_switch_to(games_menu_id_at(s_selected));
    }
}

bool on_back() { return false; }

} // namespace

const passport_app_t g_games_app = {
    .id = APP_ID_GAMES,
    .name = "游戏",
    .en_name = "GAMES",
    .desc = "狗叫、跳一跳与方块\n从这里进入",
    .tag = "本地游戏",
    .theme_color = 0xEEA438,
    .init = nullptr,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .prepare_stop = nullptr,
    .on_back = on_back,
};
