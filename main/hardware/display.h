#ifndef MIMI_DISPLAY_H
#define MIMI_DISPLAY_H

#include "esp_err.h"
#include <stdbool.h>

esp_err_t display_init(void);

// For the dashboard
void display_update_dashboard(const char *ssid, const char *ip, float voltage,
                              int batt_pct, float temp, float hum, bool bt_on,
                              int pwr_mode, const char *uptime_str, bool thinking);

#endif // MIMI_DISPLAY_H
