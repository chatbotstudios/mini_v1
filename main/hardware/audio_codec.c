#include "hardware/audio_codec.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "audio_codec";

extern i2c_master_bus_handle_t bus_handle;

esp_codec_dev_handle_t audio_codec_init(i2s_chan_handle_t tx_handle) {
    if (!bus_handle) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return NULL;
    }

    /* Initialize I2C Control Interface */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 0,
        .addr = 0x18, // The ES8311 address on this board
        .bus_handle = bus_handle,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!i2c_ctrl) {
        ESP_LOGE(TAG, "Failed to create I2C control interface");
        return NULL;
    }

    /* Configure ES8311 */
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = i2c_ctrl,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1, // PA is handled manually via Tri-Pin Lock
        .use_mclk = true,
    };
    const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);
    if (!es8311_if) {
        ESP_LOGE(TAG, "Failed to create ES8311 interface");
        return NULL;
    }

    /* Create I2S Data Interface */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = 0,
        .rx_handle = NULL,
        .tx_handle = tx_handle,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data_if) {
        ESP_LOGE(TAG, "Failed to create I2S data interface");
        return NULL;
    }

    /* Create Codec Device */
    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = es8311_if,
        .data_if = data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
    };
    esp_codec_dev_handle_t codec = esp_codec_dev_new(&dev_cfg);
    if (!codec) {
        ESP_LOGE(TAG, "Failed to create esp_codec_dev instance");
        return NULL;
    }

    /* Default Open config (Will be overridden by file parameters if needed) */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = 16000,
    };
    esp_codec_dev_open(codec, &fs);
    esp_codec_dev_set_out_vol(codec, 100);

    ESP_LOGI(TAG, "ES8311 Codec Deep Initialized via esp_codec_dev (16kHz Ready)");
    return codec;
}
