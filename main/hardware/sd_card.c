/**
 * @file sd_card.c
 * @brief Resilient MicroSD driver for Waveshare ESP32-S3-Touch-AMOLED-1.75
 *
 * Strategy:
 *   1. Try native SDMMC 1-bit mode (preferred – higher throughput when it works)
 *      - Force CS high (GPIO 41)
 *      - Retry with progressive frequency reduction (10 → 5 → 2 → 1 MHz)
 *   2. If SDMMC fails completely → fall back to SPI mode (very robust on this board)
 *
 * Mount point remains "/sdcard" so the rest of the firmware is unaffected.
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
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "bsp/esp32_s3_touch_amoled_1_75.h"   // for BSP_SD_* macros if available

static const char *TAG = "sd_card";

/* -------------------------------------------------------------------------- */
/*  Pin definitions for Waveshare ESP32-S3-Touch-AMOLED-1.75                 */
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

#define SD_PIN_CS           GPIO_NUM_41          // Always present on this board
#define SD_MOUNT_POINT      "/sdcard"

/* SPI host used for the fallback path (SPI3 is free on this board, SPI2 is used by LCD) */
#define SD_SPI_HOST         SPI3_HOST

/* -------------------------------------------------------------------------- */
/*  Internal state                                                            */
/* -------------------------------------------------------------------------- */
typedef enum {
    SD_MODE_NONE = 0,
    SD_MODE_SDMMC,
    SD_MODE_SPI
} sd_mode_t;

static sdmmc_card_t *s_card = NULL;
static sd_mode_t     s_mode = SD_MODE_NONE;
static spi_host_device_t s_spi_host = SD_SPI_HOST;
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
             (s_card->ocr & (1<<30)) ? "SDHC/SDXC" : "SDSC");
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
/*  SDMMC path (preferred)                                                    */
/* -------------------------------------------------------------------------- */

static esp_err_t try_sdmmc(int max_freq_khz)
{
    ESP_LOGI(TAG, "Trying SDMMC 1-bit @ %d kHz ...", max_freq_khz);

    force_cs_high();
    vTaskDelay(pdMS_TO_TICKS(250));          // power / signal settle

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = max_freq_khz;
    host.flags |= SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot = {
        .clk   = BSP_SD_CLK,
        .cmd   = BSP_SD_CMD,
        .d0    = BSP_SD_D0,
        .d1    = GPIO_NUM_NC,
        .d2    = GPIO_NUM_NC,
        .d3    = GPIO_NUM_NC,                // we drive CS ourselves
        .cd    = SDMMC_SLOT_NO_CD,
        .wp    = SDMMC_SLOT_NO_WP,
        .width = 1,
        .flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP,
    };

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 32,        // important for recursive gallery
        .allocation_unit_size   = 32 * 1024,
        .disk_status_check_enable = true,
    };

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot,
                                            &mount_cfg, &s_card);
    if (ret == ESP_OK) {
        s_mode = SD_MODE_SDMMC;
        ESP_LOGI(TAG, "SDMMC mount succeeded");
    } else {
        ESP_LOGW(TAG, "SDMMC failed: %s", esp_err_to_name(ret));
        // Make sure nothing is left half-mounted
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
    }
    return ret;
}

/* -------------------------------------------------------------------------- */
/*  SPI fallback path                                                         */
/* -------------------------------------------------------------------------- */

static esp_err_t try_spi(void)
{
    ESP_LOGI(TAG, "Falling back to SPI mode (CS=GPIO%d) ...", SD_PIN_CS);

    force_cs_high();
    vTaskDelay(pdMS_TO_TICKS(200));

    // 1. Initialise SPI bus
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = BSP_SD_CMD,       // GPIO 1
        .miso_io_num     = BSP_SD_D0,        // GPIO 3
        .sclk_io_num     = BSP_SD_CLK,       // GPIO 2
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(s_spi_host, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 2. SDSPI device config
    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs   = SD_PIN_CS;
    slot_cfg.host_id   = s_spi_host;
    slot_cfg.gpio_cd   = GPIO_NUM_NC;
    slot_cfg.gpio_wp   = GPIO_NUM_NC;
    slot_cfg.gpio_int  = GPIO_NUM_NC;

    // 3. Host (SPI flavour)
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = s_spi_host;
    // 10–20 MHz is usually fine in SPI mode on this board
    host.max_freq_khz = 16000;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 32,
        .allocation_unit_size   = 32 * 1024,
        .disk_status_check_enable = true,
    };

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg,
                                  &mount_cfg, &s_card);
    if (ret == ESP_OK) {
        s_mode = SD_MODE_SPI;
        ESP_LOGI(TAG, "SPI mount succeeded");
    } else {
        ESP_LOGE(TAG, "SPI mount failed: %s", esp_err_to_name(ret));
        spi_bus_free(s_spi_host);
        s_card = NULL;
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

    ESP_LOGI(TAG, "=== SD Card bring-up (FORCING SPI MODE) ===");

    // Force SPI mode immediately since SDMMC gives false-positive mounts that fail during DMA
    esp_err_t ret = try_spi();

    if (ret == ESP_OK) {
        log_card_info();
        dump_root_dir();
        ESP_LOGI(TAG, "SD card ready (mode = %s)",
                 s_mode == SD_MODE_SDMMC ? "SDMMC" : "SPI");
    } else {
        ESP_LOGE(TAG, "FATAL: could not mount SD card with any method");
    }

    xSemaphoreGive(s_mutex);
    return ret;
}

void sd_card_deinit(void)
{
    if (!s_card) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    ESP_LOGI(TAG, "Unmounting SD card (mode = %s)",
             s_mode == SD_MODE_SDMMC ? "SDMMC" : "SPI");

    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;

    if (s_mode == SD_MODE_SPI) {
        spi_bus_free(s_spi_host);
    }

    s_mode = SD_MODE_NONE;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "SD card unmounted");
}
