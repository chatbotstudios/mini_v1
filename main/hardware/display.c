#include "hardware/display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_random.h"
#include <errno.h>
#include "esp_heap_caps.h"
#include "wifi/wifi_manager.h"
#include "hardware/battery.h"
#include "hardware/shtc3.h"
#include "hardware/pm_system.h"
#include "agent/agent_metrics.h"
#include "esp_wifi.h"

static const char *TAG = "display";

/* Fonts (declared in fonts/ or via LVGL font converter) */
LV_FONT_DECLARE(inter_16);
LV_FONT_DECLARE(inter_24);
LV_FONT_DECLARE(inter_48);

/* Screen objects */
static lv_obj_t *s_splash_screen = NULL;
static lv_obj_t *s_home_screen = NULL;
static lv_obj_t *s_offline_screen = NULL;
static lv_obj_t *s_dashboard_screen = NULL;
static lv_obj_t *s_filesystem_screen = NULL;
static lv_obj_t *s_settings_screen = NULL;

static lv_obj_t *s_welcome_msg_label = NULL;

/* Settings UI variables */
static lv_obj_t *s_settings_list = NULL;
static lv_obj_t *s_brightness_slider = NULL;
static lv_obj_t *s_wifi_switch = NULL;

/* Filesystem UI variables */
static lv_obj_t *s_fs_list = NULL;
static lv_obj_t *s_fs_title = NULL;
static char s_current_fs_path[256] = "/sdcard";
static lv_obj_t *s_dashboard_bg_img = NULL;

static lv_obj_t *s_page_indicator_container = NULL;
static lv_obj_t *s_page_dots[3] = {NULL};
static ui_screen_t s_current_screen = UI_SCREEN_SPLASH;
static bool s_is_thinking = false;

/* Gallery State */
#define MAX_GALLERY_IMAGES 32
static int s_gallery_count = 0;
static int s_gallery_index = 0;
static char *s_gallery_paths[MAX_GALLERY_IMAGES] = {0};

/* PSRAM Preloader for Images (Bypasses concurrent DMA issues) */
static uint8_t *s_preloaded_jpegs[MAX_GALLERY_IMAGES] = {0};
static size_t s_preloaded_sizes[MAX_GALLERY_IMAGES] = {0};

/* File System RAM Cache (Bypasses concurrent DMA issues during UI) */
#define MAX_FS_NODES 256
typedef struct {
    char parent_path[128];
    char name[64];
    bool is_dir;
} fs_node_t;
static fs_node_t s_fs_nodes[MAX_FS_NODES];
static int s_fs_node_count = 0;

static lv_image_dsc_t s_psram_img_dsc = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RAW,
        .flags = 0,
        .w = 0,
        .h = 0,
        .stride = 0,
    },
    .data_size = 0,
    .data = NULL,
};
static lv_obj_t *s_batt_overlay = NULL;

/* Dashboard UI elements */
static lv_obj_t *s_label_wifi = NULL;
static lv_obj_t *s_label_ip = NULL;
static lv_obj_t *s_label_batt = NULL;
static lv_obj_t *s_label_temp = NULL;
static lv_obj_t *s_label_hum = NULL;
static lv_obj_t *s_label_bt = NULL;
static lv_obj_t *s_label_time = NULL;
static lv_obj_t *s_label_uptime = NULL;
static lv_obj_t *s_arc_batt = NULL;


// Hardcoded fallback removed by user request

#include <stdio.h>


#define MAX_WELCOME_MSGS 50
static char *s_dynamic_messages[MAX_WELCOME_MSGS] = {0};
static size_t s_dynamic_msg_count = 0;

esp_err_t ui_load_welcome_messages(void) {
    ESP_LOGI(TAG, "Attempting to open /spiffs/messages/welcome.txt...");
    FILE *f = fopen("/spiffs/messages/welcome.txt", "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open welcome.txt on SPIFFS!");
        // We log the error code using errno
        ESP_LOGE(TAG, "fopen errno: %s", strerror(errno));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Successfully opened /spiffs/messages/welcome.txt. Parsing lines...");
    char line[256];
    while (fgets(line, sizeof(line), f) && s_dynamic_msg_count < MAX_WELCOME_MSGS) {
        line[strcspn(line, "\r\n")] = 0; // trim newline
        if (strlen(line) > 0) {
            s_dynamic_messages[s_dynamic_msg_count++] = strdup(line);
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "Loaded %d welcome messages from SPIFFS", (int)s_dynamic_msg_count);
    ui_show_random_welcome();
    return ESP_OK;
}

/* Forward declarations */
static void create_splash_screen(void);
static void create_home_screen(void);
static void create_page_indicator(void);
static void create_offline_screen(void);
static void create_dashboard_screen(void);
static void create_filesystem_screen(void);
static void create_settings_screen(void);
static void settings_event_cb(lv_event_t *e);
static void ui_gallery_enter(void);
static void load_directory(const char *path);
static void fs_list_btn_cb(lv_event_t *e);

static void splash_lv_timer_cb(lv_timer_t *timer);
static void splash_click_cb(lv_event_t *e);
static void home_icon_click_cb(lv_event_t *e);
static void offline_tap_cb(lv_event_t *e);
static void screen_gesture_cb(lv_event_t *e);
static void scan_gallery_dir(const char *dir_path);
static void ui_gallery_show_image(int index);
static void gallery_enter_timer_cb(lv_timer_t *timer);

/* ==================== PUBLIC API ==================== */

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initializing AMOLED + LVGL display (BSP)...");

    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 1,
            .mirror_y = 1}
    };
    cfg.lv_adapter_cfg.task_stack_size = 16 * 1024; // Increase stack for TJPGD decoding
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    /* Seed random for welcome messages */
    srand(esp_random());

    bsp_display_lock(portMAX_DELAY);
    
    /* Create all screens */
    create_splash_screen();
    create_home_screen();
    create_offline_screen();
    create_dashboard_screen();
    create_filesystem_screen();
    create_settings_screen();
    create_page_indicator();

    /* Start on splash */
    s_current_screen = UI_SCREEN_SPLASH;
    lv_scr_load(s_splash_screen);

    /* Auto transition from splash after 4 seconds using LVGL timer */
    lv_timer_create(splash_lv_timer_cb, 4000, NULL);
    
    bsp_display_unlock();

    ESP_LOGI(TAG, "Multi-screen UI initialized successfully.");
    
    /* Pre-scan the SD card for JPEGs outside of the GUI task 
       to prevent DMA bus starvation during screen transitions! */
    ESP_LOGI(TAG, "Pre-scanning SD card for JPEGs to avoid DMA collisions...");
    scan_gallery_dir("/sdcard");
    ESP_LOGI(TAG, "Found %d JPEGs.", s_gallery_count);

    return ESP_OK;
}

static void ui_bg_opa_anim_cb(void * var, int32_t v)
{
    lv_obj_set_style_image_opa((lv_obj_t *)var, v, 0);
}

void ui_update_page_indicator(ui_screen_t screen) {
    if (screen == UI_SCREEN_SPLASH || screen >= UI_SCREEN_COUNT || !s_page_indicator_container) {
        if (s_page_indicator_container) {
            lv_obj_add_flag(s_page_indicator_container, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    
    lv_obj_clear_flag(s_page_indicator_container, LV_OBJ_FLAG_HIDDEN);
    
    int active_idx = -1;
    if (screen == UI_SCREEN_OFFLINE) active_idx = 0;
    else if (screen == UI_SCREEN_HOME) active_idx = 1;
    else if (screen == UI_SCREEN_DASHBOARD) active_idx = 2;
    
    for (int i = 0; i < 3; i++) {
        if (!s_page_dots[i]) continue;
        if (i == active_idx) {
            lv_obj_set_style_bg_opa(s_page_dots[i], 255, 0);
            lv_obj_set_width(s_page_dots[i], 16);
        } else {
            lv_obj_set_style_bg_opa(s_page_dots[i], 100, 0);
            lv_obj_set_width(s_page_dots[i], 8);
        }
    }
}

void ui_switch_to_screen_anim(ui_screen_t screen, lv_scr_load_anim_t anim_type)
{
    if (screen >= UI_SCREEN_COUNT) return;

    bsp_display_lock(portMAX_DELAY);

    // Memory optimization: unload background images when leaving screens
    if (s_current_screen == UI_SCREEN_DASHBOARD && screen != UI_SCREEN_DASHBOARD) {
        // Free gallery image memory when leaving gallery
        if (s_dashboard_bg_img) {
            lv_obj_del(s_dashboard_bg_img);
            s_dashboard_bg_img = NULL;
        }
        if (s_batt_overlay) {
            lv_obj_del(s_batt_overlay);
            s_batt_overlay = NULL;
        }
    } else if (screen == UI_SCREEN_DASHBOARD && s_current_screen != UI_SCREEN_DASHBOARD) {
        ui_gallery_enter();
    }
    
    if (screen == UI_SCREEN_FILESYSTEM && s_current_screen != UI_SCREEN_FILESYSTEM) {
        load_directory("/sdcard");
    }

    lv_obj_t *target = NULL;
    switch (screen) {
        case UI_SCREEN_SPLASH:   target = s_splash_screen; break;
        case UI_SCREEN_HOME:     target = s_home_screen; break;
        case UI_SCREEN_OFFLINE:  target = s_offline_screen; break;
        case UI_SCREEN_DASHBOARD: target = s_dashboard_screen; break;
        case UI_SCREEN_FILESYSTEM: target = s_filesystem_screen; break;
        case UI_SCREEN_SETTINGS: target = s_settings_screen; break;
        default: break;
    }

    if (target) {
        lv_scr_load_anim(target, anim_type, 300, 0, false);
        s_current_screen = screen;
        ui_update_page_indicator(screen);
        ESP_LOGI(TAG, "Switched to screen %d", screen);
    }

    bsp_display_unlock();
}

void ui_switch_to_screen(ui_screen_t screen)
{
    ui_switch_to_screen_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON);
}

ui_screen_t ui_get_current_screen(void)
{
    return s_current_screen;
}

void ui_show_random_welcome(void)
{
    if (!s_offline_screen) return;

    bsp_display_lock(portMAX_DELAY);

    if (s_welcome_msg_label) {
        if (s_dynamic_msg_count > 0) {
            int idx = rand() % s_dynamic_msg_count;
            lv_label_set_text(s_welcome_msg_label, s_dynamic_messages[idx]);
        } else {
            lv_label_set_text(s_welcome_msg_label, "SPIFFS Error:\nwelcome.txt not found");
        }
    }
    
    bsp_display_unlock();
}

void ui_update_wifi_status(bool connected, const char *ip)
{
    if (!s_label_wifi || !s_label_ip) return;

    bsp_display_lock(portMAX_DELAY);

    char buf[64];
    if (connected && ip) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %s", "CONNECTED");
        lv_label_set_text(s_label_wifi, buf);
        snprintf(buf, sizeof(buf), "IP: %s", ip);
        lv_label_set_text(s_label_ip, buf);
    } else {
        lv_label_set_text(s_label_wifi, LV_SYMBOL_WIFI " OFFLINE");
        lv_label_set_text(s_label_ip, "IP: N/A");
    }

    bsp_display_unlock();
}

void display_update_dashboard(const char *ssid, const char *ip, float voltage,
                              int batt_pct, float temp, float hum, bool bt_on,
                              int pwr_mode, const char *uptime_str, bool thinking)
{
    bsp_display_lock(portMAX_DELAY);

    /* If splash is still visible, remove it */
    if (s_splash_screen && lv_obj_is_valid(s_splash_screen)) {
        lv_obj_del(s_splash_screen);
        s_splash_screen = NULL;
    }

    s_is_thinking = thinking;

    char buf[80];

    /* WiFi */
    bool has_wifi = ssid && strcmp(ssid, "N/A") != 0 && strcmp(ssid, "") != 0;
    snprintf(buf, sizeof(buf), "%s %s", LV_SYMBOL_WIFI, has_wifi ? ssid : "OFFLINE");
    if (s_label_wifi) lv_label_set_text(s_label_wifi, buf);

    snprintf(buf, sizeof(buf), "IP: %s", ip ? ip : "N/A");
    if (s_label_ip) lv_label_set_text(s_label_ip, buf);

    /* Battery */
    if (s_label_batt && s_arc_batt) {
        const char *batt_icon = LV_SYMBOL_BATTERY_FULL;
        if (batt_pct <= 10) batt_icon = LV_SYMBOL_BATTERY_EMPTY;
        else if (batt_pct <= 30) batt_icon = LV_SYMBOL_BATTERY_1;
        else if (batt_pct <= 60) batt_icon = LV_SYMBOL_BATTERY_2;
        else if (batt_pct <= 85) batt_icon = LV_SYMBOL_BATTERY_3;

        snprintf(buf, sizeof(buf), "%s %d%% (%.2fV)", batt_icon, batt_pct, voltage);
        lv_label_set_text(s_label_batt, buf);
        lv_arc_set_value(s_arc_batt, batt_pct);
    }

    /* Sensors with emojis removed to avoid missing glyph boxes */
    if (s_label_temp && temp > -100.0f) {
        snprintf(buf, sizeof(buf), "T: %.1fC", temp);
        lv_label_set_text(s_label_temp, buf);
    }
    if (s_label_hum && hum > 0) {
        snprintf(buf, sizeof(buf), "H: %.0f%%", hum);
        lv_label_set_text(s_label_hum, buf);
    }

    /* Bluetooth */
    if (s_label_bt) {
        snprintf(buf, sizeof(buf), "%s %s", LV_SYMBOL_BLUETOOTH, bt_on ? "ON" : "OFF");
        lv_label_set_text(s_label_bt, buf);
    }

    /* Time & Uptime */
    if (s_label_time) lv_label_set_text(s_label_time, uptime_str ? uptime_str : "??:??");
    if (s_label_uptime) lv_label_set_text(s_label_uptime, uptime_str ? uptime_str : "Uptime: --");

    bsp_display_unlock();
}

static lv_obj_t *s_msg_overlay = NULL;
static lv_obj_t *s_msg_label = NULL;

void display_show_message(const char *msg)
{
    bsp_display_lock(portMAX_DELAY);

    if (!s_msg_overlay) {
        s_msg_overlay = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_msg_overlay);
        lv_obj_set_size(s_msg_overlay, 466, 466);
        lv_obj_set_style_bg_color(s_msg_overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_msg_overlay, LV_OPA_80, 0);

        lv_obj_t *box = lv_obj_create(s_msg_overlay);
        lv_obj_set_size(box, 400, 300);
        lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(box, lv_color_black(), 0);
        lv_obj_set_style_border_color(box, lv_color_hex(0x00FFCC), 0);
        lv_obj_set_style_border_width(box, 4, 0);
        lv_obj_set_style_radius(box, 20, 0);

        s_msg_label = lv_label_create(box);
        lv_obj_set_width(s_msg_label, 360);
        lv_label_set_long_mode(s_msg_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(s_msg_label, &inter_24, 0);
        lv_obj_set_style_text_color(s_msg_label, lv_color_white(), 0);
        lv_obj_align(s_msg_label, LV_ALIGN_CENTER, 0, 0);
    }
    lv_label_set_text(s_msg_label, msg);

    bsp_display_unlock();
}

void display_clear_message(void)
{
    bsp_display_lock(portMAX_DELAY);
    if (s_msg_overlay) {
        lv_obj_del(s_msg_overlay);
        s_msg_overlay = NULL;
        s_msg_label = NULL;
    }
    bsp_display_unlock();
}

/* ==================== FILESYSTEM VIEWER ==================== */

static char s_fs_paths[50][512];

static void load_directory(const char *path) {
    if (!s_fs_list || !s_fs_title) return;
    
    // Update path label
    lv_label_set_text(s_fs_title, path);
    strncpy(s_current_fs_path, path, sizeof(s_current_fs_path)-1);
    
    // Clear list
    lv_obj_clean(s_fs_list);
    
    // If not root, add ".." button
    if (strcmp(path, "/sdcard") != 0 && strcmp(path, "/sdcard/") != 0) {
        lv_obj_t *btn = lv_list_add_btn(s_fs_list, LV_SYMBOL_UP, " .. (Go Up)");
        lv_obj_set_style_text_font(btn, &inter_24, 0);
        lv_obj_add_event_cb(btn, fs_list_btn_cb, LV_EVENT_CLICKED, strdup(".."));
    }
    
    int count = 0;
    // Iterate RAM cache instead of querying SD card live
    for (int i = 0; i < s_fs_node_count && count < 50; i++) {
        if (strcmp(s_fs_nodes[i].parent_path, path) == 0) {
            
            snprintf(s_fs_paths[count], 512, "%s/%s", path, s_fs_nodes[i].name);
            char *fullpath = s_fs_paths[count];
            
            const char *icon = s_fs_nodes[i].is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE;
            lv_obj_t *btn = lv_list_add_btn(s_fs_list, icon, s_fs_nodes[i].name);
            lv_obj_set_style_text_font(btn, &inter_24, 0);
            lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), 0); // Ensure visible text
            lv_obj_add_event_cb(btn, fs_list_btn_cb, LV_EVENT_CLICKED, fullpath);
            
            count++;
        }
    }
}

static void fs_list_btn_cb(lv_event_t *e) {
    char *path = (char *)lv_event_get_user_data(e);
    if (!path) return;
    
    if (strcmp(path, "..") == 0) {
        // Go up one directory
        char *last_slash = strrchr(s_current_fs_path, '/');
        if (last_slash && last_slash != s_current_fs_path) {
            *last_slash = '\0';
            load_directory(s_current_fs_path);
        } else if (last_slash == s_current_fs_path) {
            load_directory("/sdcard");
        }
    } else {
        bool is_dir = false;
        // Check RAM cache instead of live stat()
        for (int i = 0; i < s_fs_node_count; i++) {
            char fullpath[192];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", s_fs_nodes[i].parent_path, s_fs_nodes[i].name);
            if (strcmp(fullpath, path) == 0) {
                is_dir = s_fs_nodes[i].is_dir;
                break;
            }
        }
        
        if (is_dir) {
            load_directory(path);
        } else {
            ESP_LOGI(TAG, "Selected file: %s", path);
            if (strstr(path, ".jpg") || strstr(path, ".jpeg") || strstr(path, ".png") ||
                strstr(path, ".JPG") || strstr(path, ".JPEG") || strstr(path, ".PNG")) {
                for (int i = 0; i < s_gallery_count; i++) {
                    if (strcmp(s_gallery_paths[i], path) == 0) {
                        ui_gallery_show_image(i);
                        ui_switch_to_screen(UI_SCREEN_DASHBOARD);
                        break;
                    }
                }
            }
        }
    }
}

static void create_filesystem_screen(void) {
    s_filesystem_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_filesystem_screen, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(s_filesystem_screen, LV_OPA_COVER, 0);
    
    // Title bar
    s_fs_title = lv_label_create(s_filesystem_screen);
    lv_label_set_text(s_fs_title, "/sdcard");
    lv_obj_set_style_text_font(s_fs_title, &inter_24, 0);
    lv_obj_set_style_text_color(s_fs_title, lv_color_hex(0x00FFCC), 0);
    lv_obj_align(s_fs_title, LV_ALIGN_TOP_MID, 0, 10);
    
    // File list
    s_fs_list = lv_list_create(s_filesystem_screen);
    lv_obj_set_size(s_fs_list, 440, 400);
    lv_obj_align(s_fs_list, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(s_fs_list, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_color(s_fs_list, lv_color_hex(0x333333), 0);
    
    lv_obj_add_event_cb(s_filesystem_screen, screen_gesture_cb, LV_EVENT_GESTURE, NULL);
}

/* ==================== SETTINGS VIEWER ==================== */

static void settings_event_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    const char *action = (const char *)lv_event_get_user_data(e);
    
    if (code == LV_EVENT_VALUE_CHANGED) {
        if (strcmp(action, "wifi") == 0) {
            bool is_on = lv_obj_has_state(obj, LV_STATE_CHECKED);
            ESP_LOGI(TAG, "Settings: WiFi toggled %s", is_on ? "ON" : "OFF");
            if (!is_on) {
                esp_wifi_stop();
            } else {
                esp_wifi_start();
                esp_wifi_connect();
            }
        } else if (strcmp(action, "brightness") == 0) {
            int val = lv_slider_get_value(obj);
            ESP_LOGI(TAG, "Settings: Brightness set to %d", val);
            bsp_display_brightness_set(val);
        }
    } else if (code == LV_EVENT_CLICKED) {
        if (strcmp(action, "reboot") == 0) {
            ESP_LOGW(TAG, "Settings: Rebooting system...");
            esp_restart();
        }
    }
}

static void create_settings_screen(void) {
    s_settings_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_settings_screen, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(s_settings_screen, LV_OPA_COVER, 0);
    
    // Title
    lv_obj_t *title = lv_label_create(s_settings_screen);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &inter_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FFCC), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);
    
    // Settings List
    s_settings_list = lv_list_create(s_settings_screen);
    lv_obj_set_size(s_settings_list, 420, 370);
    lv_obj_align(s_settings_list, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(s_settings_list, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_color(s_settings_list, lv_color_hex(0x333333), 0);
    
    // Network Section
    lv_list_add_text(s_settings_list, "Network");
    
    // WiFi Toggle
    lv_obj_t *wifi_container = lv_list_add_btn(s_settings_list, LV_SYMBOL_WIFI, "WiFi Power");
    lv_obj_set_style_text_font(wifi_container, &inter_24, 0);
    lv_obj_set_style_text_color(wifi_container, lv_color_hex(0xFFFFFF), 0);
    s_wifi_switch = lv_switch_create(wifi_container);
    
    // Style: Main background (OFF state)
    lv_obj_set_style_bg_color(s_wifi_switch, lv_color_hex(0x9E9E9E), LV_PART_MAIN | LV_STATE_DEFAULT);
    // Style: Main background (ON state)
    lv_obj_set_style_bg_color(s_wifi_switch, lv_color_hex(0x9B51E0), LV_PART_MAIN | LV_STATE_CHECKED);
    // Style: Indicator (ON state)
    lv_obj_set_style_bg_color(s_wifi_switch, lv_color_hex(0x6200EE), LV_PART_INDICATOR | LV_STATE_CHECKED);
    // Style: Knob
    lv_obj_set_style_bg_color(s_wifi_switch, lv_color_hex(0xFFFFFF), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_wifi_switch, lv_color_hex(0x6200EE), LV_PART_KNOB | LV_STATE_CHECKED);

    lv_obj_add_state(s_wifi_switch, LV_STATE_CHECKED); // Assume ON by default
    lv_obj_align(s_wifi_switch, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_add_event_cb(s_wifi_switch, settings_event_cb, LV_EVENT_VALUE_CHANGED, "wifi");
    
    // Display Section
    lv_list_add_text(s_settings_list, "Display");
    
    // Brightness Slider
    lv_obj_t *bright_container = lv_list_add_btn(s_settings_list, LV_SYMBOL_SETTINGS, "Brightness");
    lv_obj_set_style_text_font(bright_container, &inter_24, 0);
    lv_obj_set_style_text_color(bright_container, lv_color_hex(0xFFFFFF), 0);
    s_brightness_slider = lv_slider_create(bright_container);
    lv_slider_set_range(s_brightness_slider, 10, 100);
    lv_slider_set_value(s_brightness_slider, 100, LV_ANIM_OFF);
    lv_obj_set_width(s_brightness_slider, 150);
    lv_obj_align(s_brightness_slider, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_add_event_cb(s_brightness_slider, settings_event_cb, LV_EVENT_VALUE_CHANGED, "brightness");
    
    // System Section
    lv_list_add_text(s_settings_list, "System");
    
    // Reboot Button
    lv_obj_t *reboot_btn = lv_list_add_btn(s_settings_list, LV_SYMBOL_POWER, "Reboot Device");
    lv_obj_set_style_text_font(reboot_btn, &inter_24, 0);
    lv_obj_set_style_text_color(reboot_btn, lv_color_hex(0xFF5555), 0);
    lv_obj_add_event_cb(reboot_btn, settings_event_cb, LV_EVENT_CLICKED, "reboot");
    
    // Allow swiping back down
    lv_obj_add_event_cb(s_settings_screen, screen_gesture_cb, LV_EVENT_GESTURE, NULL);
}

/* ==================== SCREEN CREATION ==================== */

static void create_splash_screen(void)
{
    s_splash_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_splash_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_splash_screen, LV_OPA_COVER, 0);

    /* Big title */
    lv_obj_t *title = lv_label_create(s_splash_screen);
    lv_label_set_text(title, "MIMI");
    lv_obj_set_style_text_font(title, &inter_48, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FFCC), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);

    /* Subtitle */
    lv_obj_t *subtitle = lv_label_create(s_splash_screen);
    lv_label_set_text(subtitle, "Gemini AI Agent");
    lv_obj_set_style_text_font(subtitle, &inter_24, 0);
    lv_obj_set_style_text_color(subtitle, lv_color_white(), 0);
    lv_obj_align(subtitle, LV_ALIGN_CENTER, 0, 0);

    /* Tagline */
    lv_obj_t *tag = lv_label_create(s_splash_screen);
    lv_label_set_text(tag, "Touch to continue  •  Swipe for more");
    lv_obj_set_style_text_font(tag, &inter_16, 0);
    lv_obj_set_style_text_color(tag, lv_color_hex(0x888888), 0);
    lv_obj_align(tag, LV_ALIGN_CENTER, 0, 50);

    /* Make splash clickable to skip */
    lv_obj_add_flag(s_splash_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_splash_screen, splash_click_cb, LV_EVENT_CLICKED, NULL);
}

static void create_page_indicator(void) {
    s_page_indicator_container = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_page_indicator_container, 100, 20);
    lv_obj_align(s_page_indicator_container, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_opa(s_page_indicator_container, 0, 0); 
    lv_obj_set_style_border_width(s_page_indicator_container, 0, 0);
    lv_obj_set_flex_flow(s_page_indicator_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_page_indicator_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_page_indicator_container, 8, 0);
    
    for (int i = 0; i < 3; i++) {
        s_page_dots[i] = lv_obj_create(s_page_indicator_container);
        lv_obj_set_size(s_page_dots[i], 8, 8);
        lv_obj_set_style_radius(s_page_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_page_dots[i], lv_color_white(), 0);
        lv_obj_set_style_border_width(s_page_dots[i], 0, 0);
        lv_obj_set_style_bg_opa(s_page_dots[i], 100, 0);
    }
    
    lv_obj_add_flag(s_page_indicator_container, LV_OBJ_FLAG_HIDDEN);
}

static void create_home_screen(void)
{
    s_home_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_home_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_home_screen, LV_OPA_COVER, 0);
    
    /* 1. Chat Icon (Left) */
    lv_obj_t *chat_icon = lv_image_create(s_home_screen);
    lv_image_set_src(chat_icon, "S:/spiffs/icons/chat_icon.png");
    lv_obj_align(chat_icon, LV_ALIGN_CENTER, -110, -20);
    lv_obj_add_flag(chat_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(chat_icon, home_icon_click_cb, LV_EVENT_CLICKED, (void*)UI_SCREEN_OFFLINE);

    lv_obj_t *chat_label = lv_label_create(s_home_screen);
    lv_label_set_text(chat_label, "Chat");
    lv_obj_set_style_text_font(chat_label, &inter_16, 0);
    lv_obj_set_style_text_color(chat_label, lv_color_white(), 0);
    lv_obj_align_to(chat_label, chat_icon, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    /* 2. Gallery Icon (Center) */
    lv_obj_t *gallery_icon = lv_image_create(s_home_screen);
    lv_image_set_src(gallery_icon, "S:/spiffs/icons/gallery_icon.png");
    lv_obj_align(gallery_icon, LV_ALIGN_CENTER, 0, -20);
    lv_obj_add_flag(gallery_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(gallery_icon, home_icon_click_cb, LV_EVENT_CLICKED, (void*)UI_SCREEN_DASHBOARD);
    
    lv_obj_t *gallery_label = lv_label_create(s_home_screen);
    lv_label_set_text(gallery_label, "Gallery");
    lv_obj_set_style_text_font(gallery_label, &inter_16, 0);
    lv_obj_set_style_text_color(gallery_label, lv_color_white(), 0);
    lv_obj_align_to(gallery_label, gallery_icon, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    /* 3. SD Card Icon (Right) */
    lv_obj_t *sd_icon = lv_image_create(s_home_screen);
    lv_image_set_src(sd_icon, "S:/spiffs/icons/folder_icon.png");
    lv_obj_align(sd_icon, LV_ALIGN_CENTER, 110, -20);
    lv_obj_add_flag(sd_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sd_icon, home_icon_click_cb, LV_EVENT_CLICKED, (void*)UI_SCREEN_FILESYSTEM);
    
    lv_obj_t *sd_label = lv_label_create(s_home_screen);
    lv_label_set_text(sd_label, "Files");
    lv_obj_set_style_text_font(sd_label, &inter_16, 0);
    lv_obj_set_style_text_color(sd_label, lv_color_white(), 0);
    lv_obj_align_to(sd_label, sd_icon, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    
    lv_obj_add_event_cb(s_home_screen, screen_gesture_cb, LV_EVENT_GESTURE, NULL);
}

static void create_offline_screen(void)
{
    s_offline_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_offline_screen, lv_color_black(), 0);

    /* Title */
    lv_obj_t *title = lv_label_create(s_offline_screen);
    lv_label_set_text(title, "OFFLINE MODE");
    lv_obj_set_style_text_font(title, &inter_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FFCC), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    /* Big random welcome message */
    s_welcome_msg_label = lv_label_create(s_offline_screen);
    if (s_dynamic_msg_count > 0) {
        lv_label_set_text(s_welcome_msg_label, s_dynamic_messages[0]);
    } else {
        lv_label_set_text(s_welcome_msg_label, "SPIFFS Error:\nwelcome.txt not found");
    }
    lv_obj_set_style_text_font(s_welcome_msg_label, &inter_24, 0);
    lv_obj_set_style_text_color(s_welcome_msg_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_welcome_msg_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_welcome_msg_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_welcome_msg_label, 380);
    lv_obj_align(s_welcome_msg_label, LV_ALIGN_CENTER, 0, -20);

    /* Hint */
    lv_obj_t *hint = lv_label_create(s_offline_screen);
    lv_label_set_text(hint, "Tap anywhere to refresh message  •  Swipe → Dashboard");
    lv_obj_set_style_text_font(hint, &inter_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -30);

    /* Tap anywhere on screen to get new random message */
    lv_obj_add_flag(s_offline_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_offline_screen, offline_tap_cb, LV_EVENT_CLICKED, NULL);

    /* Gesture Support */
    lv_obj_add_event_cb(s_offline_screen, screen_gesture_cb, LV_EVENT_GESTURE, NULL);
}

static void create_dashboard_screen(void)
{
    s_dashboard_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_dashboard_screen, lv_color_black(), 0);
    lv_obj_add_event_cb(s_dashboard_screen, screen_gesture_cb, LV_EVENT_GESTURE, NULL);
}

static void scan_gallery_dir(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue; // Skip hidden/parent dirs

        char *fullpath = malloc(512);
        if (!fullpath) continue;
        
        snprintf(fullpath, 512, "%s/%s", dir_path, ent->d_name);
        ESP_LOGI(TAG, "Scanner checking: %s (d_type=%d)", fullpath, ent->d_type);
        
        bool is_dir = (ent->d_type == DT_DIR);
        if (ent->d_type == DT_UNKNOWN) {
            struct stat st;
            if (stat(fullpath, &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
            }
        }

        // Add to RAM Cache
        if (s_fs_node_count < MAX_FS_NODES) {
            strncpy(s_fs_nodes[s_fs_node_count].parent_path, dir_path, 127);
            strncpy(s_fs_nodes[s_fs_node_count].name, ent->d_name, 63);
            s_fs_nodes[s_fs_node_count].is_dir = is_dir;
            s_fs_node_count++;
        }

        if (is_dir) {
            scan_gallery_dir(fullpath);
        } else {
            if (strstr(ent->d_name, ".jpg") || strstr(ent->d_name, ".jpeg") ||
                strstr(ent->d_name, ".JPG") || strstr(ent->d_name, ".JPEG") ||
                strstr(ent->d_name, ".png") || strstr(ent->d_name, ".PNG")) {
                
                if (s_gallery_count < MAX_GALLERY_IMAGES) {
                    FILE *f = fopen(fullpath, "rb");
                    if (f) {
                        fseek(f, 0, SEEK_END);
                        size_t file_size = ftell(f);
                        fseek(f, 0, SEEK_SET);

                        if (file_size > 0) {
                            uint8_t *psram_buf = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
                            if (psram_buf) {
                                if (fread(psram_buf, 1, file_size, f) == file_size) {
                                    s_preloaded_jpegs[s_gallery_count] = psram_buf;
                                    s_preloaded_sizes[s_gallery_count] = file_size;
                                    s_gallery_paths[s_gallery_count] = strdup(fullpath);
                                    s_gallery_count++;
                                    ESP_LOGI(TAG, "=> PRELOADED to PSRAM: %s (%zu bytes)", fullpath, file_size);
                                } else {
                                    free(psram_buf);
                                    ESP_LOGE(TAG, "Failed to read %s", fullpath);
                                }
                            }
                        }
                        fclose(f);
                    }
                }
            }
        }
        free(fullpath);
    }
    closedir(dir);
}

static void ui_gallery_show_image(int index) {
    if (s_gallery_count == 0) return;
    if (index < 0) index = s_gallery_count - 1;
    if (index >= s_gallery_count) index = 0;
    s_gallery_index = index;

    static char path[128];
    snprintf(path, sizeof(path), "%s", s_gallery_paths[s_gallery_index]);
    ESP_LOGI(TAG, "Gallery Image [%d/%d]: %s", s_gallery_index + 1, s_gallery_count, path);

    /* --- PSRAM Preloader --- */
    // Image is already preloaded in PSRAM, so we don't need to delay for the SD card!
    

    if (!s_dashboard_bg_img) {
        s_dashboard_bg_img = lv_image_create(s_dashboard_screen);
        lv_obj_move_to_index(s_dashboard_bg_img, 0);
        lv_obj_center(s_dashboard_bg_img);
    }
    
    // Smooth crossfade
    lv_obj_set_style_image_opa(s_dashboard_bg_img, 0, 0);
    
    if (s_preloaded_jpegs[s_gallery_index]) {
        s_psram_img_dsc.data = s_preloaded_jpegs[s_gallery_index];
        s_psram_img_dsc.data_size = s_preloaded_sizes[s_gallery_index];
        lv_image_set_src(s_dashboard_bg_img, &s_psram_img_dsc);
    }
    
    // Resume LVGL drawing
    

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_dashboard_bg_img);
    lv_anim_set_exec_cb(&a, ui_bg_opa_anim_cb);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_time(&a, 300);
    lv_anim_start(&a);
}

static void gallery_enter_timer_cb(lv_timer_t *timer)
{
    if (s_gallery_count > 0) {
        ui_gallery_show_image(s_gallery_index);
    }
    lv_timer_del(timer);
}

static void ui_gallery_enter(void)
{
    if (s_gallery_count > 0) {
        /* Delay JPEG load by 400ms to allow LVGL screen transition to finish.
           This prevents the LCD DMA from starving the SDMMC DMA! */
        lv_timer_create(gallery_enter_timer_cb, 400, NULL);
    } else {
        lv_obj_t *fallback = lv_label_create(s_dashboard_screen);
        lv_label_set_text(fallback, "No JPEGs found on SD Card!");
        lv_obj_set_style_text_color(fallback, lv_color_white(), 0);
        lv_obj_align(fallback, LV_ALIGN_CENTER, 0, 0);
    }
}

/* ==================== CALLBACKS ==================== */

static void splash_lv_timer_cb(lv_timer_t *timer)
{
    if (s_current_screen == UI_SCREEN_SPLASH) {
        ui_switch_to_screen_anim(UI_SCREEN_HOME, LV_SCR_LOAD_ANIM_FADE_ON);
    }
    // Delete timer so it only fires once
    lv_timer_del(timer);
}

static void splash_click_cb(lv_event_t *e)
{
    ui_switch_to_screen(UI_SCREEN_HOME);
}

static void home_icon_click_cb(lv_event_t *e)
{
    ui_screen_t target = (ui_screen_t)(uintptr_t)lv_event_get_user_data(e);
    if (target == UI_SCREEN_OFFLINE) {
        ui_switch_to_screen_anim(target, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    } else if (target == UI_SCREEN_DASHBOARD) {
        ui_switch_to_screen_anim(target, LV_SCR_LOAD_ANIM_MOVE_LEFT);
    } else if (target == UI_SCREEN_FILESYSTEM) {
        ui_switch_to_screen_anim(target, LV_SCR_LOAD_ANIM_MOVE_TOP);
    }
}

static void offline_tap_cb(lv_event_t *e)
{
    ui_show_random_welcome();
}

static ui_screen_t s_previous_screen = UI_SCREEN_HOME;

static void screen_gesture_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    
    if (dir == LV_DIR_TOP && s_current_screen != UI_SCREEN_SETTINGS && s_current_screen != UI_SCREEN_SPLASH) {
        s_previous_screen = s_current_screen;
        ui_switch_to_screen_anim(UI_SCREEN_SETTINGS, LV_SCR_LOAD_ANIM_MOVE_TOP);
        return;
    }
    
    if (s_current_screen == UI_SCREEN_SETTINGS) {
        if (dir == LV_DIR_BOTTOM) {
            ui_switch_to_screen_anim(s_previous_screen, LV_SCR_LOAD_ANIM_MOVE_BOTTOM);
        }
        return;
    }
    
    if (s_current_screen == UI_SCREEN_HOME) {
        if (dir == LV_DIR_RIGHT) {
            ui_switch_to_screen_anim(UI_SCREEN_OFFLINE, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
        } else if (dir == LV_DIR_LEFT) {
            ui_switch_to_screen_anim(UI_SCREEN_DASHBOARD, LV_SCR_LOAD_ANIM_MOVE_LEFT);
        }
    } else if (s_current_screen == UI_SCREEN_OFFLINE) {
        if (dir == LV_DIR_LEFT) {
            ui_switch_to_screen_anim(UI_SCREEN_HOME, LV_SCR_LOAD_ANIM_MOVE_LEFT);
        }
    } else if (s_current_screen == UI_SCREEN_DASHBOARD) {
        if (dir == LV_DIR_BOTTOM) {
            ui_switch_to_screen_anim(UI_SCREEN_HOME, LV_SCR_LOAD_ANIM_MOVE_BOTTOM);
        } else if (dir == LV_DIR_LEFT) {
            ui_gallery_show_image(s_gallery_index + 1);
        } else if (dir == LV_DIR_RIGHT) { 
            ui_gallery_show_image(s_gallery_index - 1);
        }
    } else if (s_current_screen == UI_SCREEN_FILESYSTEM) {
        if (dir == LV_DIR_BOTTOM) {
            ui_switch_to_screen_anim(UI_SCREEN_HOME, LV_SCR_LOAD_ANIM_MOVE_BOTTOM);
        }
    }
}

/* Simple periodic UI task (call from app_main or a dedicated task) */
/* Integration Note: Create this task with 4096 stack size and priority 5:
   xTaskCreate(ui_task, "ui_task", 4096, NULL, 5, NULL); */
void ui_task(void *arg)
{
    static int tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        tick++;

        /* 1. Update Clock (Every second) */
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        
        char time_str[16];
        if (timeinfo.tm_year > (2020 - 1900)) {
            strftime(time_str, sizeof(time_str), "%H:%M", &timeinfo);
        } else {
            strcpy(time_str, "--:--");
        }

        /* Fetch hardware data OUTSIDE the LVGL lock */
        bool do_hardware_poll = (tick % 10 == 0);
        bool wifi_conn = false;
        const char *ip = "0.0.0.0";
        int batt_pct = 0;
        float voltage = 0.0f;
        shtc3_data_t sd = {0};
        bool shtc3_ok = false;
        char up_str[32] = {0};

        agent_metrics_get_uptime_str(up_str, sizeof(up_str));

        if (do_hardware_poll) {
            wifi_conn = wifi_manager_is_connected();
            ip = wifi_conn ? wifi_manager_get_ip() : "0.0.0.0";
            batt_pct = battery_get_percentage();
            voltage = battery_get_voltage();
            shtc3_ok = (shtc3_read(&sd) == ESP_OK);
        }

        bsp_display_lock(portMAX_DELAY);

        if (s_current_screen == UI_SCREEN_DASHBOARD) {
            /* Update Time & Thinking indicator */
            if (s_label_time) {
                if (s_is_thinking && (tick % 2 == 0)) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%s " LV_SYMBOL_EYE_OPEN, time_str);
                    lv_label_set_text(s_label_time, buf);
                } else {
                    lv_label_set_text(s_label_time, time_str);
                }
            }

            /* Update uptime (Every second) */
            if (s_label_uptime) {
                lv_label_set_text(s_label_uptime, up_str);
            }

            /* Refresh Wi-Fi / Sensors (Every 10 seconds to save I2C/SPI overhead) */
            if (do_hardware_poll) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%s %s", LV_SYMBOL_WIFI, wifi_conn ? "CONNECTED" : "OFFLINE");
                if (s_label_wifi) lv_label_set_text(s_label_wifi, buf);
                snprintf(buf, sizeof(buf), "IP: %s", ip);
                if (s_label_ip) lv_label_set_text(s_label_ip, buf);

                if (s_label_batt && s_arc_batt) {
                    const char *batt_icon = LV_SYMBOL_BATTERY_FULL;
                    if (batt_pct <= 10) batt_icon = LV_SYMBOL_BATTERY_EMPTY;
                    else if (batt_pct <= 30) batt_icon = LV_SYMBOL_BATTERY_1;
                    else if (batt_pct <= 60) batt_icon = LV_SYMBOL_BATTERY_2;
                    else if (batt_pct <= 85) batt_icon = LV_SYMBOL_BATTERY_3;
                    snprintf(buf, sizeof(buf), "%s %d%% (%.2fV)", batt_icon, batt_pct, voltage);
                    lv_label_set_text(s_label_batt, buf);
                    lv_arc_set_value(s_arc_batt, batt_pct);
                }

                if (shtc3_ok) {
                    if (s_label_temp) {
                        snprintf(buf, sizeof(buf), "T: %.1fC", sd.temperature);
                        lv_label_set_text(s_label_temp, buf);
                    }
                    if (s_label_hum) {
                        snprintf(buf, sizeof(buf), "H: %.0f%%", sd.humidity);
                        lv_label_set_text(s_label_hum, buf);
                    }
                } else {
                    ESP_LOGD(TAG, "Failed to read SHTC3 sensor");
                }
            }
        }
        bsp_display_unlock();
    }
}