#include "hardware/sd_card.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "sd_card";

#define SD_PIN_D0  3
#define SD_PIN_CMD 1
#define SD_PIN_CLK 2

static sdmmc_card_t *s_card = NULL;

esp_err_t sd_card_init(void) {
  ESP_LOGI(TAG, "Initializing SD card over 1-line SDMMC...");

  /* Wait for SD Card to stabilize after power-on */
  vTaskDelay(pdMS_TO_TICKS(200));

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 10,
      .allocation_unit_size = 16 * 1024
  };

  ESP_LOGI(TAG, "Initializing SDMMC host");

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  /* Reduce frequency to 5MHz to improve signal integrity on 1-line SDMMC */
  host.max_freq_khz = 5000;
  /* Increase timeout to prevent 0x107 (ESP_ERR_TIMEOUT) when Wi-Fi heavily uses the bus */
  host.command_timeout_ms = 1500;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 1; /* 1-line SD mode */
  slot_config.clk = SD_PIN_CLK;
  slot_config.cmd = SD_PIN_CMD;
  slot_config.d0 = SD_PIN_D0;
  
  /* CRITICAL: Explicitly set unused pins to -1 to prevent them from defaulting 
     to GPIOs 47/48, which would steal the pins from the I2C SHTC3 sensor! */
  slot_config.d1 = -1;
  slot_config.d2 = -1;
  slot_config.d3 = -1;
  slot_config.d4 = -1;
  slot_config.d5 = -1;
  slot_config.d6 = -1;
  slot_config.d7 = -1;
  slot_config.cd = -1;
  slot_config.wp = -1;

  /* Flags to enable internal pull-ups on the SD lines */
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  ESP_LOGI(TAG, "Mounting FAT filesystem...");
  
  /* Suppress ugly internal errors if the user simply didn't insert a card */
  esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
  esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
  esp_log_level_set("sdmmc_req", ESP_LOG_NONE);

  esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &s_card);

  esp_log_level_set("sdmmc_common", ESP_LOG_INFO);
  esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_INFO);
  esp_log_level_set("sdmmc_req", ESP_LOG_INFO);

  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGW(TAG, "Failed to mount filesystem.");
    } else {
      ESP_LOGW(TAG, "Failed to initialize the card (%s).", esp_err_to_name(ret));
    }
    return ret;
  }

  ESP_LOGI(TAG, "SD Card initialized successfully!");
  sdmmc_card_print_info(stdout, s_card);

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

  /* DEBUG: Print SD Card messages directory */
  DIR *d2 = opendir("/sdcard/messages");
  if (d2) {
    ESP_LOGI(TAG, "Contents of /sdcard/messages:");
    struct dirent *dir2;
    while ((dir2 = readdir(d2)) != NULL) {
      ESP_LOGI(TAG, " - %s", dir2->d_name);
    }
    closedir(d2);
  } else {
    ESP_LOGE(TAG, "Failed to open /sdcard/messages directory!");
  }

  return ESP_OK;
}

void sd_card_deinit(void) {
  if (s_card != NULL) {
    esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
    ESP_LOGI(TAG, "Card unmounted");
    s_card = NULL;
  }
}

