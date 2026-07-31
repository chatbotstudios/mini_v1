/**
 * @file sd_card.c
 * @brief Native SDMMC 1-bit driver for Waveshare ESP32-S3-Touch-AMOLED-1.75
 *
 * Strictly uses the hardware-accelerated SDMMC peripheral (no SPI fallback).
 * Display lock in the gallery is expected to prevent DMA collisions with the
 * QSPI AMOLED.
 */

#include "hardware/sd_card.h"

#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "bsp/esp32_s3_touch_amoled_1_75.h"

static const char *TAG = "sd_card";

/* -------------------------------------------------------------------------- */
/*  Pin definitions (Waveshare ESP32-S3-Touch-AMOLED-1.75)                    */
/* -------------------------------------------------------------------------- */
#ifndef BSP_SD_CLK
#define BSP_SD_CLK          GPIO_NUM_2
#endif
#ifndef BSP_SD_CMD
#define BSP_SD_CMD          GPIO_NUM_1
#endif
#ifndef BSP_SD_D0
#define BSP_SD_D0           GPIO_NUM_3
#endif

#define SD_PIN_CS           GPIO_NUM_41
#define SD_MOUNT_POINT      "/sdcard"

/* -------------------------------------------------------------------------- */
/*  Internal state                                                            */
/* -------------------------------------------------------------------------- */
static sdmmc_card_t *s_card = NULL;
static SemaphoreHandle_t s_mutex = NULL;

/* -------------------------------------------------------------------------- */
/*  Helpers                                                                   */
/* -------------------------------------------------------------------------- */

static void force_cs_high(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << SD_PIN_CS,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(SD_PIN_CS, 1);
}

static void log_card_info(void)
{
    if (!s_card) return;
    ESP_LOGI(TAG, "Card name   : %s", s_card->cid.name);
    ESP_LOGI(TAG, "Card type   : %s",
             (s_card->ocr & (1U << 30)) ? "SDHC/SDXC" : "SDSC");
    ESP_LOGI(TAG, "Speed       : %s",
             (s_card->csd.tr_speed > 25000000) ? "High Speed" : "Default Speed");
    ESP_LOGI(TAG, "Size        : %llu MB",
             ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024));
}

static void dump_root_dir(void)
{
    DIR *d = opendir(SD_MOUNT_POINT);
    if (!d) {
        ESP_LOGE(TAG, "Failed to open %s", SD_MOUNT_POINT);
        return;
    }
    ESP_LOGI(TAG, "Contents of %s:", SD_MOUNT_POINT);
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(d)) != NULL && count < 20) {
        ESP_LOGI(TAG, "  %s", entry->d_name);
        count++;
    }
    if (count == 20) ESP_LOGI(TAG, "  ... (truncated)");
    closedir(d);
}

/* -------------------------------------------------------------------------- */
/*  SDMMC 1-bit mount with progressive frequency fallback                     */
/* -------------------------------------------------------------------------- */

static esp_err_t try_sdmmc(int max_freq_khz)
{
    ESP_LOGI(TAG, "Trying SDMMC 1-bit @ %d kHz ...", max_freq_khz);

    force_cs_high();
    vTaskDelay(pdMS_TO_TICKS(300));          // power + signal settle

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = max_freq_khz;
    host.flags |= SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot = {
        .clk   = BSP_SD_CLK,
        .cmd   = BSP_SD_CMD,
        .d0    = BSP_SD_D0,
        .d1    = GPIO_NUM_NC,
        .d2    = GPIO_NUM_NC,
        .d3    = GPIO_NUM_NC,                // we force CS high ourselves
        .cd    = SDMMC_SLOT_NO_CD,
        .wp    = SDMMC_SLOT_NO_WP,
        .width = 1,
        .flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP,
    };

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed   = false,
        .max_files                = 32,      // gallery needs headroom
        .allocation_unit_size     = 16 * 1024,
        .disk_status_check_enable = false,   // status polling is a common source of 0x107
    };

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot,
                                            &mount_cfg, &s_card);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SDMMC mount succeeded @ %d kHz", max_freq_khz);
    } else {
        ESP_LOGW(TAG, "SDMMC @ %d kHz failed: %s", max_freq_khz, esp_err_to_name(ret));
        // Clean up any partial state
        if (s_card) {
            esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
            s_card = NULL;
        }
    }
    return ret;
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */

esp_err_t sd_card_init(void)
{
    if (s_card) {
        ESP_LOGW(TAG, "SD card already mounted");
        return ESP_OK;
    }

    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    ESP_LOGI(TAG, "=== SD Card bring-up (native SDMMC 1-bit only) ===");

    // Progressive frequency attempts (reliability first)
    const int freqs[] = { 10000, 5000, 2000, 1000 };  // kHz
    esp_err_t ret = ESP_FAIL;

    for (size_t i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
        ret = try_sdmmc(freqs[i]);
        if (ret == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    if (ret == ESP_OK) {
        // Quick health check – read sector 0
        uint8_t *buf = heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (buf) {
            vTaskDelay(pdMS_TO_TICKS(30));
            esp_err_t hr = sdmmc_read_sectors(s_card, buf, 0, 1);
            free(buf);
            if (hr != ESP_OK) {
                ESP_LOGE(TAG, "Post-mount sector-0 read failed: %s", esp_err_to_name(hr));
                esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
                s_card = NULL;
                ret = hr;
            } else {
                ESP_LOGI(TAG, "Health-check OK");
            }
        }

        if (ret == ESP_OK) {
            log_card_info();
            dump_root_dir();
            ESP_LOGI(TAG, "SD card ready (native SDMMC 1-bit)");
        }
    } else {
        ESP_LOGE(TAG, "FATAL: could not mount SD card with native SDMMC");
    }

    xSemaphoreGive(s_mutex);
    return ret;
}

void sd_card_deinit(void)
{
    if (!s_card) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    ESP_LOGI(TAG, "Unmounting SD card (SDMMC)");
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;

    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "SD card unmounted");
}
