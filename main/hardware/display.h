#ifndef MIMI_DISPLAY_H
#define MIMI_DISPLAY_H

#include "esp_err.h"
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_SCREEN_SPLASH = 0,
    UI_SCREEN_OFFLINE,
    UI_SCREEN_DASHBOARD,
    UI_SCREEN_COUNT
} ui_screen_t;

esp_err_t display_init(void);
esp_err_t ui_load_welcome_messages(void);

/* Update functions */
void display_update_dashboard(const char *ssid, const char *ip, float voltage,
                              int batt_pct, float temp, float hum, bool bt_on,
                              int pwr_mode, const char *uptime_str, bool thinking);

void display_show_message(const char *msg);
void display_clear_message(void);

/* Multi-screen management */
void ui_switch_to_screen(ui_screen_t screen);
void ui_switch_to_screen_anim(ui_screen_t screen, lv_scr_load_anim_t anim_type);
ui_screen_t ui_get_current_screen(void);

/* Specific screen helpers */
void ui_show_random_welcome(void);   // For offline screen
void ui_update_wifi_status(bool connected, const char *ip);

/* Call this from main loop or a task to refresh dynamic elements */
void ui_task(void *arg);

#ifdef __cplusplus
}
#endif

#endif // MIMI_DISPLAY_H