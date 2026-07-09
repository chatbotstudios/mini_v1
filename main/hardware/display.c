#include "hardware/display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "display";

static lv_obj_t *s_label_wifi;
static lv_obj_t *s_label_ip;
static lv_obj_t *s_label_batt;
static lv_obj_t *s_label_temp;
static lv_obj_t *s_label_hum;
static lv_obj_t *s_label_bt;
static lv_obj_t *s_label_time;
static lv_obj_t *s_label_uptime;
static lv_obj_t *s_arc_batt;

static void create_dashboard_ui(void) {
    bsp_display_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);

    /* Header */
    lv_obj_t *label_title = lv_label_create(scr);
    lv_label_set_text(label_title, "MIMI");
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 10);

    /* WiFi Info */
    s_label_wifi = lv_label_create(scr);
    lv_label_set_text(s_label_wifi, LV_SYMBOL_WIFI " OFFLINE");
    lv_obj_align(s_label_wifi, LV_ALIGN_TOP_LEFT, 10, 40);

    s_label_ip = lv_label_create(scr);
    lv_label_set_text(s_label_ip, "IP: N/A");
    lv_obj_align(s_label_ip, LV_ALIGN_TOP_LEFT, 10, 60);

    /* Battery Info */
    s_label_batt = lv_label_create(scr);
    lv_label_set_text(s_label_batt, LV_SYMBOL_BATTERY_EMPTY " 0% (0.00V)");
    lv_obj_align(s_label_batt, LV_ALIGN_TOP_LEFT, 10, 90);

    s_arc_batt = lv_arc_create(scr);
    lv_obj_set_size(s_arc_batt, 60, 60);
    lv_arc_set_rotation(s_arc_batt, 270);
    lv_arc_set_bg_angles(s_arc_batt, 0, 360);
    lv_obj_align(s_arc_batt, LV_ALIGN_TOP_RIGHT, -10, 40);

    /* Sensors */
    s_label_temp = lv_label_create(scr);
    lv_label_set_text(s_label_temp, "Temp: N/A");
    lv_obj_align(s_label_temp, LV_ALIGN_TOP_LEFT, 10, 120);

    s_label_hum = lv_label_create(scr);
    lv_label_set_text(s_label_hum, "Hum: N/A");
    lv_obj_align(s_label_hum, LV_ALIGN_TOP_LEFT, 10, 140);

    /* Connectivity */
    s_label_bt = lv_label_create(scr);
    lv_label_set_text(s_label_bt, LV_SYMBOL_BLUETOOTH " OFF");
    lv_obj_align(s_label_bt, LV_ALIGN_TOP_LEFT, 10, 170);

    /* Bottom Status */
    s_label_time = lv_label_create(scr);
    lv_label_set_text(s_label_time, "00:00");
    lv_obj_align(s_label_time, LV_ALIGN_BOTTOM_MID, 0, -30);

    s_label_uptime = lv_label_create(scr);
    lv_label_set_text(s_label_uptime, "Uptime: 0m");
    lv_obj_align(s_label_uptime, LV_ALIGN_BOTTOM_MID, 0, -10);

    bsp_display_unlock();
}

esp_err_t display_init(void) {
    ESP_LOGI(TAG, "Initializing AMOLED display and LVGL...");

    /* Start the display. This configures QSPI, LVGL, and touch */
    bsp_display_start();

    /* Backlight is managed by BSP, generally we can turn it on */
    bsp_display_backlight_on();

    /* Create UI */
    create_dashboard_ui();

    ESP_LOGI(TAG, "Display initialized successfully.");
    return ESP_OK;
}

void display_update_dashboard(const char *ssid, const char *ip, float voltage,
                              int batt_pct, float temp, float hum, bool bt_on,
                              int pwr_mode, const char *uptime_str, bool thinking) {
    bsp_display_lock(0);

    char buf[64];

    /* WiFi */
    bool has_wifi = ssid && strcmp(ssid, "N/A") != 0;
    snprintf(buf, sizeof(buf), "%s %s", LV_SYMBOL_WIFI, has_wifi ? ssid : "OFFLINE");
    lv_label_set_text(s_label_wifi, buf);
    snprintf(buf, sizeof(buf), "IP: %s", ip ? ip : "N/A");
    lv_label_set_text(s_label_ip, buf);

    /* Battery */
    const char *batt_icon = LV_SYMBOL_BATTERY_FULL;
    if (batt_pct <= 25) batt_icon = LV_SYMBOL_BATTERY_1;
    else if (batt_pct <= 50) batt_icon = LV_SYMBOL_BATTERY_2;
    else if (batt_pct <= 75) batt_icon = LV_SYMBOL_BATTERY_3;
    else if (batt_pct <= 5) batt_icon = LV_SYMBOL_BATTERY_EMPTY;
    
    snprintf(buf, sizeof(buf), "%s %d%% (%.2fV)", batt_icon, batt_pct, voltage);
    lv_label_set_text(s_label_batt, buf);
    
    lv_arc_set_value(s_arc_batt, batt_pct);

    /* Sensors */
    if (temp != 0.0f) {
        snprintf(buf, sizeof(buf), "Temp: %.1f C", temp);
        lv_label_set_text(s_label_temp, buf);
    }
    if (hum != 0.0f) {
        snprintf(buf, sizeof(buf), "Hum: %.1f %%", hum);
        lv_label_set_text(s_label_hum, buf);
    }

    /* BT */
    snprintf(buf, sizeof(buf), "%s %s", LV_SYMBOL_BLUETOOTH, bt_on ? "ON" : "OFF");
    lv_label_set_text(s_label_bt, buf);

    /* Time & Uptime */
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year > (2020 - 1900)) {
        strftime(buf, sizeof(buf), "%H:%M  %d.%m.%y", &timeinfo);
        lv_label_set_text(s_label_time, buf);
    }

    snprintf(buf, sizeof(buf), "Uptime: %s", uptime_str ? uptime_str : "0m");
    lv_label_set_text(s_label_uptime, buf);

    bsp_display_unlock();
}
