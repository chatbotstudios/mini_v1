#include "hardware/battery.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include <math.h>

#include "driver/gpio.h"

static const char *TAG = "battery";

/* Hardware Settings */
#define BATT_ADC_UNIT           ADC_UNIT_1
#define BATT_ADC_CHAN           ADC_CHANNEL_3 // GPIO 4 on ESP32-S3
#define BATT_ADC_ATTEN          ADC_ATTEN_DB_12 // Full scale 0-3.1V (S3)
#define BATT_DIVIDER_RATIO      2.0f            // 100k/100k divider
#define BATT_ADC_RES            4095.0f         // 12-bit
#define BATT_CTRL_PIN           17

/* Voltage Mapping (Standard LiPo) */
#define V_FULL  4.20f
#define V_EMPTY 3.20f

static adc_oneshot_unit_handle_t s_adc_handle = NULL;

esp_err_t battery_init(void) {
    // ESP_LOGI(TAG, "Initializing battery sensing on GPIO 4 with Control on GPIO 17");
    /* Disabled for AMOLED migration, AXP2101 PMIC handles battery */
    /*
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BATT_CTRL_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level(BATT_CTRL_PIN, 1); // Enable battery divider

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = BATT_ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &s_adc_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = BATT_ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, BATT_ADC_CHAN, &config));
    */

    return ESP_OK;
}

float battery_get_voltage(void) {
    /* Mock voltage for now until AXP2101 PMIC is integrated */
    return 4.0f;
}

int battery_get_percentage(void) {
    float v = battery_get_voltage();
    
    if (v >= V_FULL) return 100;
    if (v <= V_EMPTY) return 0;

    /* Simple linear mapping (can be improved with a curve lookup) */
    int percentage = (int)((v - V_EMPTY) / (V_FULL - V_EMPTY) * 100.0f);
    
    return percentage;
}
