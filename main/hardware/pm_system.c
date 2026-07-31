#include "hardware/pm_system.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "mimi_config.h"

static const char *TAG = "pm_sys";
static mimi_pwr_mode_t s_current_mode = MIMI_PWR_BALANCED;

esp_err_t pm_system_init(void) {
  ESP_LOGI(TAG, "Initializing Power Management");

  /* Default to Balanced */
  return pm_system_set_mode(MIMI_PWR_BALANCED);
}

esp_err_t pm_system_set_mode(mimi_pwr_mode_t mode) {
  esp_pm_config_t pm_config = {0};

  switch (mode) {
    case MIMI_PWR_BALANCED:
      ESP_LOGI(TAG, "Mode: BALANCED (160MHz Max, 80MHz Min)");
      pm_config.max_freq_mhz = 160;
      pm_config.min_freq_mhz = 80;
      pm_config.light_sleep_enable = false; // Disable for stability during network ops
      esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
      break;

  case MIMI_PWR_PERFORMANCE:
    ESP_LOGI(TAG, "Mode: PERFORMANCE (240MHz Max, 160MHz Min)");
    pm_config.max_freq_mhz = 240;
    pm_config.min_freq_mhz = 160;
    pm_config.light_sleep_enable = false;
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM); // Never use WIFI_PS_NONE! It causes brownouts!
    break;
  }

  esp_err_t err = esp_pm_configure(&pm_config);
  if (err == ESP_ERR_NOT_SUPPORTED) {
      ESP_LOGW(TAG, "Power management is disabled in menuconfig, ignoring.");
      s_current_mode = mode;
      return ESP_OK;
  }
  if (err == ESP_OK) {
    s_current_mode = mode;
  } else {
    ESP_LOGE(TAG, "Failed to configure power management: %s", esp_err_to_name(err));
  }
  return err;
}

mimi_pwr_mode_t pm_system_get_mode(void) { return s_current_mode; }

void pm_system_deep_sleep(uint64_t ms) {
  ESP_LOGI(TAG, "Entering Deep Sleep for %llu ms", ms);

  /* Ensure WiFi is stopped for clean shutdown */
  esp_wifi_stop();

  esp_sleep_enable_timer_wakeup(ms * 1000);
  esp_deep_sleep_start();
}
