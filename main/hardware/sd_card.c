#include "hardware/sd_card.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include <string.h>

static const char *TAG = "sd_card";

esp_err_t sd_card_init(void) {
  ESP_LOGI(TAG, "Initializing SD card using Waveshare BSP...");

  /* Wait for SD Card to stabilize after power-on */
  vTaskDelay(pdMS_TO_TICKS(200));

  esp_err_t ret = bsp_sdcard_mount();
  
  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE(TAG, "Failed to mount filesystem. Formatting may be required.");
    } else {
      ESP_LOGE(TAG, "Failed to initialize the card (%s).", esp_err_to_name(ret));
    }
    return ret;
  }

  ESP_LOGI(TAG, "SD Card mounted successfully!");
  
  if (bsp_sdcard != NULL) {
      sdmmc_card_print_info(stdout, bsp_sdcard);
  }

  /* DEBUG: Print SD Card root directory */
  #include <dirent.h>
  DIR *d = opendir("/sdcard");
  if (d) {
    ESP_LOGI(TAG, "Contents of /sdcard:");
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
      ESP_LOGI(TAG, " - %s", dir->d_name);
    }
    closedir(d);
  } else {
    ESP_LOGE(TAG, "Failed to open /sdcard directory!");
  }

  return ESP_OK;
}

void sd_card_deinit(void) {
  bsp_sdcard_unmount();
  ESP_LOGI(TAG, "Card unmounted");
}

