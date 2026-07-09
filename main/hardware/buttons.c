#include "hardware/buttons.h"
#include "bus/message_bus.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hardware/display.h"
#include "hardware/shtc3.h"
#include "telegram/telegram_bot.h"

#include "esp_timer.h"

static const char *TAG = "buttons";

#define BTN1_PIN 0
#define LONG_PRESS_THRESHOLD_MS 1000

static QueueHandle_t gpio_evt_queue = NULL;

static void IRAM_ATTR gpio_isr_handler(void *arg) {
  uint32_t gpio_num = (uint32_t)arg;
  xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

extern void execute_button_action(int action_id);

static void button_task(void *arg) {
  uint32_t io_num;
  while (1) {
    if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
      int click_count = 0;
      bool is_long_press = false;

      while (1) {
        int64_t press_time = esp_timer_get_time();
        
        /* Wait for release or long press timeout */
        while (gpio_get_level(io_num) == 0) {
          vTaskDelay(pdMS_TO_TICKS(20));
          if ((esp_timer_get_time() - press_time) / 1000 >= LONG_PRESS_THRESHOLD_MS) {
             is_long_press = true;
             break;
          }
        }

        if (is_long_press) {
            /* Wait until physically released to prevent loop re-triggering */
            while(gpio_get_level(io_num) == 0) vTaskDelay(pdMS_TO_TICKS(20));
            break;
        }

        int64_t duration_ms = (esp_timer_get_time() - press_time) / 1000;
        if (duration_ms >= 50) { /* Debounce */
            click_count++;
        }

        /* Wait for next press within 400ms timeout for multi-click */
        if (!xQueueReceive(gpio_evt_queue, &io_num, pdMS_TO_TICKS(400))) {
            break; /* No more clicks within timeout */
        }
      }

      if (is_long_press) {
          ESP_LOGW(TAG, "LONG PRESS detected");
          execute_button_action(4);
      } else if (click_count > 0) {
          ESP_LOGI(TAG, "%dx PRESS detected", click_count);
          execute_button_action(click_count > 3 ? 3 : click_count); // Cap at 3
      }
    }
  }
}

esp_err_t buttons_init(void) {
  gpio_config_t io_conf = {
      .intr_type = GPIO_INTR_NEGEDGE,
      .pin_bit_mask = (1ULL << BTN1_PIN),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = 1,
  };
  gpio_config(&io_conf);

  gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
  xTaskCreate(button_task, "button_task", 4096, NULL, 10, NULL);

  esp_err_t isr_err = gpio_install_isr_service(0);
  if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "Failed to install ISR service: %s", esp_err_to_name(isr_err));
    return isr_err;
  }
  gpio_isr_handler_add(BTN1_PIN, gpio_isr_handler, (void *)BTN1_PIN);

  ESP_LOGI(
      TAG,
      "Super Button initialized on GPIO 0 (Single: Dashboard, Long: Ping)");
  return ESP_OK;
}
