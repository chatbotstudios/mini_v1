#include "hardware/epaper.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "epaper";

#define EPD_WIDTH 200
#define EPD_HEIGHT 200
#define EPD_BUFFER_SIZE (EPD_WIDTH * EPD_HEIGHT / 8)

/* ESP32-S3-ePaper-1.54 Board Pins */
#define EPD_PWR_PIN 6
#define EPD_PIN_BUSY 8
#define EPD_PIN_RST 9
#define EPD_PIN_DC 10
#define EPD_PIN_CS 11
#define EPD_PIN_SCK 12
#define EPD_PIN_DIN 13

static spi_device_handle_t s_spi = NULL;
static uint8_t *s_framebuffer = NULL;
static bool s_inverted = false;

/* Simple 8x8 font */
static const uint8_t FONT8x8[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // space
    0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00, // !
    0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // "
    0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00, // #
    0x08, 0x3E, 0x09, 0x1E, 0x28, 0x3E, 0x08, 0x00, // $
    0x61, 0x62, 0x04, 0x08, 0x10, 0x46, 0x86, 0x00, // %
    0x1C, 0x22, 0x22, 0x1C, 0x2A, 0x22, 0x1D, 0x00, // &
    0x18, 0x18, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, // '
    0x0C, 0x10, 0x20, 0x20, 0x20, 0x10, 0x0C, 0x00, // (
    0x30, 0x08, 0x04, 0x04, 0x04, 0x08, 0x30, 0x00, // )
    0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00, // *
    0x00, 0x10, 0x10, 0x7C, 0x10, 0x10, 0x00, 0x00, // +
    0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x08, // ,
    0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00, // -
    0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, // .
    0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x00, // /
    0x3C, 0x46, 0x4A, 0x52, 0x62, 0x3C, 0x00, 0x00, // 0
    0x18, 0x28, 0x08, 0x08, 0x08, 0x3E, 0x00, 0x00, // 1
    0x3C, 0x42, 0x02, 0x3C, 0x40, 0x7E, 0x00, 0x00, // 2
    0x3C, 0x42, 0x0C, 0x02, 0x42, 0x3C, 0x00, 0x00, // 3
    0x08, 0x18, 0x28, 0x48, 0x7E, 0x08, 0x00, 0x00, // 4
    0x7E, 0x40, 0x7C, 0x02, 0x42, 0x3C, 0x00, 0x00, // 5
    0x3C, 0x40, 0x7C, 0x42, 0x42, 0x3C, 0x00, 0x00, // 6
    0x7E, 0x02, 0x04, 0x08, 0x10, 0x10, 0x00, 0x00, // 7
    0x3C, 0x42, 0x3C, 0x42, 0x42, 0x3C, 0x00, 0x00, // 8
    0x3C, 0x42, 0x3E, 0x02, 0x02, 0x3C, 0x00, 0x00, // 9
    0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00, // :
    0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x08, 0x00, // ;
    0x0C, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0C, 0x00, // <
    0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00, // =
    0x30, 0x18, 0x0C, 0x06, 0x0C, 0x18, 0x30, 0x00, // >
    0x3C, 0x42, 0x02, 0x0C, 0x18, 0x00, 0x18, 0x00, // ?
    0x3C, 0x42, 0x5A, 0x5A, 0x5A, 0x40, 0x3C, 0x00, // @
    0x18, 0x24, 0x42, 0x7E, 0x42, 0x42, 0x42, 0x00, // A
    0x7C, 0x42, 0x7C, 0x42, 0x42, 0x7C, 0x00, 0x00, // B
    0x3C, 0x42, 0x40, 0x40, 0x42, 0x3C, 0x00, 0x00, // C
    0x78, 0x44, 0x42, 0x42, 0x44, 0x78, 0x00, 0x00, // D
    0x7E, 0x40, 0x7C, 0x40, 0x40, 0x7E, 0x00, 0x00, // E
    0x7E, 0x40, 0x7C, 0x40, 0x40, 0x40, 0x00, 0x00, // F
    0x3C, 0x42, 0x40, 0x4E, 0x42, 0x3C, 0x00, 0x00, // G
    0x42, 0x42, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x00, // H
    0x3E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x3E, 0x00, // I
    0x1C, 0x04, 0x04, 0x04, 0x44, 0x44, 0x38, 0x00, // J
    0x42, 0x44, 0x48, 0x70, 0x48, 0x44, 0x42, 0x00, // K
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x7E, 0x00, // L
    0x42, 0x66, 0x5A, 0x42, 0x42, 0x42, 0x42, 0x00, // M
    0x42, 0x62, 0x52, 0x4A, 0x46, 0x42, 0x42, 0x00, // N
    0x3C, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3C, 0x00, // O
    0x7C, 0x42, 0x42, 0x7C, 0x40, 0x40, 0x40, 0x00, // P
    0x3C, 0x42, 0x42, 0x42, 0x4A, 0x44, 0x3A, 0x00, // Q
    0x7C, 0x42, 0x42, 0x7C, 0x48, 0x44, 0x42, 0x00, // R
    0x3C, 0x42, 0x40, 0x3C, 0x02, 0x42, 0x3C, 0x00, // S
    0x7E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x00, // T
    0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3C, 0x00, // U
    0x42, 0x42, 0x42, 0x42, 0x42, 0x24, 0x18, 0x00, // V
    0x42, 0x42, 0x42, 0x5A, 0x5A, 0x66, 0x42, 0x00, // W
    0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x42, 0x00, // X
    0x42, 0x42, 0x24, 0x18, 0x18, 0x18, 0x18, 0x00, // Y
    0x7E, 0x02, 0x04, 0x08, 0x10, 0x20, 0x7E, 0x00, // Z
};

static const uint8_t ICON_BATT_25[32] = {
    0x03, 0xC0, 0x03, 0xC0, 0x1F, 0xF8, 0x10, 0x08, 0x10, 0x08, 0x10,
    0x08, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08,
    0x1F, 0xF8, 0x1F, 0xF8, 0x1F, 0xF8, 0x1F, 0xF8, 0x00, 0x00};

static const uint8_t ICON_BATT_50[32] = {
    0x03, 0xC0, 0x03, 0xC0, 0x1F, 0xF8, 0x10, 0x08, 0x10, 0x08, 0x10,
    0x08, 0x10, 0x08, 0x10, 0x08, 0x1F, 0xF8, 0x1F, 0xF8, 0x1F, 0xF8,
    0x1F, 0xF8, 0x1F, 0xF8, 0x1F, 0xF8, 0x1F, 0xF8, 0x00, 0x00};

static const uint8_t ICON_BATT_75[32] = {
    0x03, 0xC0, 0x03, 0xC0, 0x1F, 0xF8, 0x10, 0x08, 0x10, 0x08, 0x1F,
    0xF8, 0x1F, 0xF8, 0x1F, 0xF8, 0x1F, 0xF8, 0x1F, 0xF8, 0x1F, 0xF8,
    0x1F, 0xF8, 0x1F, 0xF8, 0x1F, 0xF8, 0x1F, 0xF8, 0x00, 0x00};

static const uint8_t ICON_BATT_100[32] = {
    0x03, 0xC0, 0x03, 0xC0, 0x1F, 0xF8, 0x1F, 0xF8, 0x1F, 0xF8, 0x1F,
    0xF8, 0x1F, 0xF8, 0x1F, 0xF8, 0x1F, 0xF8, 0x1F, 0xF8, 0x1F, 0xF8,
    0x1F, 0xF8, 0x1F, 0xF8, 0x1F, 0xF8, 0x1F, 0xF8, 0x00, 0x00};

static const uint8_t ICON_TEMP[32] = {
    0x03, 0x80, 0x04, 0x40, 0x1C, 0x40, 0x04, 0x40, 0x1C, 0x40, 0x04,
    0x40, 0x1C, 0x40, 0x04, 0x40, 0x07, 0xC0, 0x07, 0xC0, 0x0F, 0xE0,
    0x1F, 0xF0, 0x3F, 0xF8, 0x3F, 0xF8, 0x1F, 0xF0, 0x0F, 0xE0};

static const uint8_t ICON_DROP[32] = {
    0x01, 0x80, 0x02, 0x40, 0x04, 0x20, 0x08, 0x10, 0x10, 0x08, 0x20,
    0x04, 0x20, 0x04, 0x40, 0x02, 0x40, 0x02, 0x40, 0x02, 0x40, 0x02,
    0x20, 0x04, 0x10, 0x08, 0x08, 0x10, 0x07, 0xE0, 0x00, 0x00};

static const uint8_t ICON_MAIL[32] = {
    0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x80, 0x01, 0xC0, 0x03, 0xA0,
    0x05, 0x90, 0x09, 0x88, 0x11, 0x84, 0x21, 0x82, 0x41, 0x81, 0x81,
    0x80, 0x01, 0x80, 0x01, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};

static const uint8_t ICON_WIFI[32] = {
    0x00, 0x00, 0x00, 0x00, 0x0F, 0xF0, 0x30, 0x0C, 0x40, 0x02, 0x07,
    0xE0, 0x18, 0x18, 0x20, 0x04, 0x03, 0xC0, 0x0C, 0x30, 0x10, 0x08,
    0x00, 0x00, 0x01, 0x80, 0x03, 0xC0, 0x01, 0x80, 0x00, 0x00};

static const uint8_t ICON_BT[32] = {
    0x01, 0x80, 0x01, 0xC0, 0x01, 0xA0, 0x01, 0x90, 0x09, 0xA0, 0x05,
    0xC0, 0x03, 0x80, 0x03, 0x80, 0x05, 0xC0, 0x09, 0xA0, 0x01, 0x90,
    0x01, 0xA0, 0x01, 0xC0, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00};

static void epd_send_command(uint8_t cmd) {
  gpio_set_level(EPD_PIN_CS, 0);
  gpio_set_level(EPD_PIN_DC, 0);
  spi_transaction_t t = {.length = 8, .tx_buffer = &cmd};
  spi_device_polling_transmit(s_spi, &t);
  gpio_set_level(EPD_PIN_CS, 1);
}

static void epd_send_data(uint8_t data) {
  gpio_set_level(EPD_PIN_CS, 0);
  gpio_set_level(EPD_PIN_DC, 1);
  spi_transaction_t t = {.length = 8, .tx_buffer = &data};
  spi_device_polling_transmit(s_spi, &t);
  gpio_set_level(EPD_PIN_CS, 1);
}

static void epd_send_data_bulk(const uint8_t *data, uint32_t len) {
  uint32_t max_chunk = 4000;
  uint32_t offset = 0;

  while (offset < len) {
    uint32_t chunk_size =
        (len - offset) > max_chunk ? max_chunk : (len - offset);
    gpio_set_level(EPD_PIN_CS, 0);
    gpio_set_level(EPD_PIN_DC, 1);
    spi_transaction_t t = {.length = chunk_size * 8,
                           .tx_buffer = data + offset};
    spi_device_polling_transmit(s_spi, &t);
    gpio_set_level(EPD_PIN_CS, 1);
    offset += chunk_size;
  }
}

static void epd_wait_busy(void) {
  while (gpio_get_level(EPD_PIN_BUSY) == 1) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static void epd_reset(void) {
  gpio_set_level(EPD_PIN_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(200));
  gpio_set_level(EPD_PIN_RST, 0);
  vTaskDelay(pdMS_TO_TICKS(10));
  gpio_set_level(EPD_PIN_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(200));
}

static void epd_set_window(uint16_t x_start, uint16_t x_end, uint16_t y_start,
                           uint16_t y_end) {
  epd_send_command(0x44); /* Set RAM X address start/end */
  epd_send_data((x_start >> 3) & 0xFF);
  epd_send_data((x_end >> 3) & 0xFF);
  epd_send_command(0x45); /* Set RAM Y address start/end */
  epd_send_data(y_start & 0xFF);
  epd_send_data((y_start >> 8) & 0xFF);
  epd_send_data(y_end & 0xFF);
  epd_send_data((y_end >> 8) & 0xFF);
}

static void epd_set_cursor(uint16_t x, uint16_t y) {
  epd_send_command(0x4E); /* Set RAM X address counter */
  epd_send_data((x >> 3) & 0xFF);
  epd_send_command(0x4F); /* Set RAM Y address counter */
  epd_send_data(y & 0xFF);
  epd_send_data((y >> 8) & 0xFF);
}

static void epd_update(void) {
  epd_send_command(0x22); /* Display Update Control 2 */
  epd_send_data(0xF7);
  epd_send_command(0x20); /* Master Activation */
  epd_wait_busy();
}

static void draw_pixel(int x, int y, uint8_t black) {
  if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT)
    return;
  int idx = (x / 8) + (y * (EPD_WIDTH / 8));

  bool is_black = s_inverted ? !black : black;

  if (is_black)
    s_framebuffer[idx] &= ~(1 << (7 - (x % 8)));
  else
    s_framebuffer[idx] |= (1 << (7 - (x % 8)));
}

void epaper_fill_rect(int x, int y, int w, int h, uint8_t color) {
  for (int i = y; i < y + h; i++) {
    for (int j = x; j < x + w; j++) {
      draw_pixel(j, i, color);
    }
  }
}

void epaper_draw_rect(int x, int y, int w, int h, uint8_t color) {
  for (int j = x; j < x + w; j++) {
    draw_pixel(j, y, color);
    draw_pixel(j, y + h - 1, color);
  }
  for (int i = y; i < y + h; i++) {
    draw_pixel(x, i, color);
    draw_pixel(x + w - 1, i, color);
  }
}

esp_err_t epaper_init(void) {
  /* Initialize board power */
  gpio_config_t pwr_conf = {
      .pin_bit_mask = (1ULL << EPD_PWR_PIN),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&pwr_conf);

  /* Hard reset peripheral power rail to clear hung I2C devices */
  gpio_set_level(EPD_PWR_PIN, 1); // Power OFF
  vTaskDelay(pdMS_TO_TICKS(50));
  gpio_set_level(EPD_PWR_PIN, 0); // Power ON
  vTaskDelay(pdMS_TO_TICKS(100));

  /* SPI and GPIO Init */
  gpio_config_t io_conf = {
      .pin_bit_mask =
          (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_RST) | (1ULL << EPD_PIN_CS),
      .mode = GPIO_MODE_OUTPUT,
  };
  gpio_config(&io_conf);
  io_conf.pin_bit_mask = (1ULL << EPD_PIN_BUSY);
  io_conf.mode = GPIO_MODE_INPUT;
  gpio_config(&io_conf);

  gpio_set_level(EPD_PIN_CS, 1);

  spi_bus_config_t buscfg = {.miso_io_num = -1,
                             .mosi_io_num = EPD_PIN_DIN,
                             .sclk_io_num = EPD_PIN_SCK,
                             .quadwp_io_num = -1,
                             .quadhd_io_num = -1,
                             .max_transfer_sz = 4096};
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

  spi_device_interface_config_t devcfg = {
      .clock_speed_hz = 2 * 1000 * 1000,
      .mode = 0,
      .spics_io_num = -1,
      .queue_size = 7,
  };
  ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi));

  s_framebuffer = heap_caps_malloc(EPD_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
  memset(s_framebuffer, 0xFF, EPD_BUFFER_SIZE);

  epd_reset();
  epd_wait_busy();
  epd_send_command(0x12); /* SWRESET */
  epd_wait_busy();

  epd_send_command(0x01); /* Driver output control */
  epd_send_data((EPD_HEIGHT - 1) & 0xFF);
  epd_send_data((EPD_HEIGHT - 1) >> 8);
  epd_send_data(0x00);

  epd_send_command(0x11); /* Data entry mode */
  epd_send_data(0x03);

  ESP_LOGI(TAG, "ePaper initialized.");
  return ESP_OK;
}

void epaper_clear(void) {
  memset(s_framebuffer, 0xFF, EPD_BUFFER_SIZE);
  epd_set_window(0, EPD_WIDTH - 1, 0, EPD_HEIGHT - 1);
  epd_set_cursor(0, 0);

  epd_send_command(0x24);
  epd_send_data_bulk(s_framebuffer, EPD_BUFFER_SIZE);

  epd_set_cursor(0, 0);
  epd_send_command(0x26);
  epd_send_data_bulk(s_framebuffer, EPD_BUFFER_SIZE);

  epd_update();
}

void epaper_draw_text(const char *text) {
  epaper_draw_text_ext(text, 10, 10, 1, false);
  epaper_update_screen();
}

void epaper_draw_text_ext(const char *text, int start_x, int start_y, int scale,
                          bool invert) {
  if (scale < 1)
    scale = 1;
  int x = start_x, y = start_y;

  while (*text) {
    char c = *text;
    if (c >= 'a' && c <= 'z') {
        c -= 32; // Convert to uppercase to prevent font glitching
    }
    if (c >= 32 && c <= 126) {
      const uint8_t *glyph = &FONT8x8[(c - 32) * 8];
      for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
          if (glyph[i] & (1 << (7 - j))) {
            for (int dy = 0; dy < scale; dy++) {
              for (int dx = 0; dx < scale; dx++) {
                draw_pixel(x + (j * scale) + dx, y + (i * scale) + dy,
                           invert ? 0 : 1);
              }
            }
          }
        }
      }
      x += 8 * scale;
      if (x > EPD_WIDTH - (8 * scale)) {
        x = start_x;
        y += 12 * scale; // 12 for line spacing
      }
    } else if (*text == '\n') {
      x = start_x;
      y += 12 * scale; // line spacing
    }
    text++;
  }
}

void epaper_update_screen(void) {
  epd_set_window(0, EPD_WIDTH - 1, 0, EPD_HEIGHT - 1);
  epd_set_cursor(0, 0);

  epd_send_command(0x24);
  epd_send_data_bulk(s_framebuffer, EPD_BUFFER_SIZE);

  epd_set_cursor(0, 0);
  epd_send_command(0x26);
  epd_send_data_bulk(s_framebuffer, EPD_BUFFER_SIZE);

  epd_update();
}

void epaper_draw_icon(int start_x, int start_y, const uint8_t *icon,
                      bool strike) {
  for (int i = 0; i < 16; i++) {
    uint16_t row = (icon[i * 2] << 8) | icon[i * 2 + 1];
    if (strike) {
      // Draw a diagonal line from bottom-left to top-right
      // row 0 has bit (1<<15), row 1 has (1<<14)...
      // A slash would be bit (1 << (i))
      row |= (1 << (15 - i));
      // Make it thicker
      if (i < 15)
        row |= (1 << (14 - i));
    }
    for (int j = 0; j < 16; j++) {
      if (row & (1 << (15 - j))) {
        draw_pixel(start_x + j, start_y + i, 1);
      }
    }
  }
}

uint8_t *epaper_get_buffer(void) { return s_framebuffer; }

void epaper_sleep(void) {
  epd_send_command(0x10);
  epd_send_data(0x01);
}

void epaper_show_dashboard(const char *ssid, const char *ip, float voltage,
                            int batt_pct, float temp, float hum, bool bt_on,
                            int pwr_mode, const char *uptime_str, bool thinking) {
  static int refresh_counter = 0;
  if (++refresh_counter >= 10) {
    epaper_full_refresh();
    refresh_counter = 0;
  }

  memset(s_framebuffer, 0xFF, EPD_BUFFER_SIZE); /* Clear buffer white */

  // 1. Draw frame borders
  epaper_draw_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, 1);
  epaper_draw_rect(2, 2, EPD_WIDTH - 4, EPD_HEIGHT - 4, 1);

  // 2. Top Header Bar
  epaper_fill_rect(2, 2, EPD_WIDTH - 4, 24, 1);

  // Use scale 2 for a bold, enlarged header. "MIMI" is 4 chars * 16px = 64px
  // wide. Center X = (200 - 64) / 2 = 68. Center Y in 24px box = 6.
  epaper_draw_text_ext("MIMI", 68, 6, 2, true);

  // 3. Network
  char buf[64];
  bool has_wifi = ssid && strcmp(ssid, "N/A") != 0;
  epaper_draw_icon(12, 32, ICON_WIFI, !has_wifi);
  snprintf(buf, sizeof(buf), "%.14s", has_wifi ? ssid : "OFFLINE");
  epaper_draw_text_ext(buf, 34, 32, 1, false);

  snprintf(buf, sizeof(buf), "IP: %s", ip ? ip : "N/A");
  epaper_draw_text_ext(buf, 34, 42, 1, false);

  // 4. Power & Sensors
  const uint8_t *batt_icon = ICON_BATT_100;
  if (batt_pct <= 25)
    batt_icon = ICON_BATT_25;
  else if (batt_pct <= 50)
    batt_icon = ICON_BATT_50;
  else if (batt_pct <= 75)
    batt_icon = ICON_BATT_75;
  epaper_draw_icon(12, 60, batt_icon, false);
  snprintf(buf, sizeof(buf), "%d%% (%.2fV)", batt_pct, voltage);
  epaper_draw_text_ext(buf, 34, 64, 1, false);

  epaper_draw_icon(12, 80, ICON_TEMP, false);
  snprintf(buf, sizeof(buf), "%.1f C", temp);
  epaper_draw_text_ext(buf, 34, 84, 1, false);

  epaper_draw_icon(100, 80, ICON_DROP, false);
  snprintf(buf, sizeof(buf), "%.1f %%", hum);
  epaper_draw_text_ext(buf, 122, 84, 1, false);

  // Removed thinking/online status as requested

  // 5. Connectivity
  extern bool telegram_bot_is_connected(void);
  epaper_draw_icon(12, 100, ICON_MAIL, !telegram_bot_is_connected());
  epaper_draw_text_ext("TG", 34, 104, 1, false);

  extern bool discord_bot_is_connected(void);
  epaper_draw_icon(72, 100, ICON_MAIL, !discord_bot_is_connected());
  epaper_draw_text_ext("DISC", 94, 104, 1, false);

  epaper_draw_icon(132, 100, ICON_BT, !bt_on);
  snprintf(buf, sizeof(buf), "%s", bt_on ? "ON" : "OFF");
  epaper_draw_text_ext(buf, 154, 104, 1, false);



  // 7. Time & Uptime (Centered at the bottom)
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);
  char time_str[32] = "12:00  01.01.24";
  if (timeinfo.tm_year > (2020 - 1900)) {
    strftime(time_str, sizeof(time_str), "%H:%M  %d.%m.%y", &timeinfo);
  }

  int time_len = strlen(time_str) * 8;
  epaper_draw_text_ext(time_str, (EPD_WIDTH - time_len) / 2, 170, 1, false);

  snprintf(buf, sizeof(buf), "Uptime: %s", uptime_str ? uptime_str : "0m");
  int up_len = strlen(buf) * 8;
  epaper_draw_text_ext(buf, (EPD_WIDTH - up_len) / 2, 182, 1, false);

  // Send everything to hardware properly
  epaper_update_screen();
}

void epaper_full_refresh(void) {
  ESP_LOGI(TAG, "Executing full hardware refresh...");
  epd_reset();
  epd_wait_busy();
  epd_send_command(0x12); /* SWRESET */
  epd_wait_busy();
  epaper_clear();
}

void epaper_set_invert(bool invert) {
  s_inverted = invert;
  ESP_LOGI(TAG, "Display inversion set to: %s", invert ? "ON" : "OFF");
}
