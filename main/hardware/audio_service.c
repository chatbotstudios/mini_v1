#include "hardware/audio_service.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "hardware/audio_codec.h"
#include <stdio.h>

static const char *TAG = "audio_service";

#define I2S_BCLK_PIN 15
#define I2S_LRCK_PIN 38
#define I2S_DOUT_PIN 16
#define I2S_MCLK_PIN 14
#define PA_ENABLE_PIN 18

static i2s_chan_handle_t tx_handle = NULL;
static esp_codec_dev_handle_t s_codec_dev = NULL;

esp_err_t audio_service_init(void) {
  /* Configure I2S Bus first so we have tx_handle for codec data_if */
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000), // Matched to boot.raw
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_MONO),
      .gpio_cfg =
          {
              .mclk = I2S_MCLK_PIN,
              .bclk = I2S_BCLK_PIN,
              .ws = I2S_LRCK_PIN,
              .dout = I2S_DOUT_PIN,
              .din = -1,
          },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));

  /* Enable Power Amplifier (Tri-Pin Lock) */
  gpio_config_t pa_conf = {
      .pin_bit_mask = (1ULL << 18) | (1ULL << 42) | (1ULL << 46),
      .mode = GPIO_MODE_OUTPUT,
  };
  gpio_config(&pa_conf);
  gpio_set_level(18, 1);
  gpio_set_level(42, 1);
  gpio_set_level(46, 1);

  /* Start the I2S channel manually so MCLK is provided to the ES8311 before I2C
   * config */
  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

  /* Initialize Codec via I2C and pass tx_handle */
  s_codec_dev = audio_codec_init(tx_handle);
  if (!s_codec_dev) {
    ESP_LOGE(TAG, "Failed to initialize ES8311 Codec");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Audio Service initialized (16kHz Mono)");
  return ESP_OK;
}

esp_err_t audio_service_play_buffer(const uint8_t *data, size_t len) {
  if (!tx_handle)
    return ESP_ERR_INVALID_STATE;

  size_t written = 0;
  return i2s_channel_write(tx_handle, data, len, &written, 1000);
}

esp_err_t audio_service_play_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return ESP_FAIL;

  uint8_t buf[1024];
  while (1) {
    size_t read = fread(buf, 1, sizeof(buf), f);
    if (read == 0)
      break;
    audio_service_play_buffer(buf, read);
  }

  fclose(f);
  return ESP_OK;
}

void audio_service_set_volume(int volume) {
  if (s_codec_dev) {
    esp_codec_dev_set_out_vol(s_codec_dev, (float)volume);
  }
}
