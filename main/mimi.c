#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"

#include "agent/agent_loop.h"
#include "agent/agent_metrics.h"
#include "agent/context_builder.h"
#include "bus/message_bus.h"
#include "cli/serial_cli.h"
#include "discord/discord_bot.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "gateway/ws_server.h"
#include "hardware/audio_service.h"
#include "hardware/battery.h"
#include "hardware/bluetooth_utils.h"
#include "hardware/buttons.h"
#include "hardware/display.h"
#include "hardware/led.h"
#include "hardware/rules_engine.h"
#include "hardware/pm_system.h"
#include "hardware/sd_card.h"
#include "hardware/shtc3.h"
#include "llm/llm_proxy.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"
#include "skills/skill_loader.h"
#include "telegram/telegram_bot.h"
#include "tools/tool_registry.h"
#include "wifi/wifi_manager.h"
#include "mimi.h"

static const char *TAG = "mimi";

static esp_err_t init_nvs(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS partition truncated, erasing...");
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  return ret;
}

static esp_err_t init_spiffs(void) {
  esp_vfs_spiffs_conf_t conf = {
      .base_path = MIMI_SPIFFS_BASE,
      .partition_label = NULL,
      .max_files = 10,
      .format_if_mount_failed = true,
  };

  esp_err_t ret = esp_vfs_spiffs_register(&conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
    return ret;
  }

  size_t total = 0, used = 0;
  esp_spiffs_info(NULL, &total, &used);
  ESP_LOGI(TAG, "SPIFFS: total=%d, used=%d", (int)total, (int)used);

  return ESP_OK;
}

/* Outbound dispatch task: reads from outbound queue and routes to channels */
static void outbound_dispatch_task(void *arg) {
  ESP_LOGI(TAG, "Outbound dispatch started");

  while (1) {
    mimi_msg_t msg;
    if (message_bus_pop_outbound(&msg, UINT32_MAX) != ESP_OK)
      continue;

    ESP_LOGI(TAG, "Dispatching response to %s:%s", msg.channel, msg.chat_id);
    led_trigger_msg_tx();

    if (msg.content && strcmp(msg.content, "__MIMI_TYPING__") == 0) {
      if (strcmp(msg.channel, MIMI_CHAN_TELEGRAM) == 0) {
        telegram_send_typing(msg.chat_id);
      } else if (strcmp(msg.channel, MIMI_CHAN_DISCORD) == 0) {
        discord_bot_send_typing(msg.chat_id);
      }
    } else {
      if (strcmp(msg.channel, MIMI_CHAN_TELEGRAM) == 0) {
        telegram_send_message(msg.chat_id, msg.content);
      } else if (strcmp(msg.channel, MIMI_CHAN_WEBSOCKET) == 0) {
        ws_server_send(msg.chat_id, msg.content);
      } else if (strcmp(msg.channel, MIMI_CHAN_DISCORD) == 0) {
        discord_bot_send_message(msg.chat_id, msg.content);
      } else {
        ESP_LOGW(TAG, "Unknown channel: %s", msg.channel);
      }
    }

    free(msg.content);
  }
}

void mimi_update_dashboard(bool thinking, bool force_redraw) {
  // static shtc3_data_t s_cached_sd = {0};
  // static int64_t s_last_read_ms = 0;
  int64_t now_ms = esp_timer_get_time() / 1000;

  /* Only read sensor every 30 seconds to save power/clutter */
  /*
  if (now_ms - s_last_read_ms > 30000 || s_last_read_ms == 0) {
    if (shtc3_read(&s_cached_sd) == ESP_OK) {
      s_last_read_ms = now_ms;
    }
  }
  */

  shtc3_data_t sd = {0}; // s_cached_sd;

  char ssid_db[32] = {0};
  nvs_handle_t nvs_db;
  if (nvs_open(MIMI_NVS_WIFI, NVS_READONLY, &nvs_db) == ESP_OK) {
    size_t len = sizeof(ssid_db);
    nvs_get_str(nvs_db, MIMI_NVS_KEY_SSID, ssid_db, &len);
    nvs_close(nvs_db);
  }

  char up_db[32];
  agent_metrics_get_uptime_str(up_db, sizeof(up_db));

  static int64_t s_last_epaper_refresh = 0;
  // AMOLED doesn't need to skip frames like ePaper, but we still throttle
  if (force_redraw || now_ms - s_last_epaper_refresh > 60000 || s_last_epaper_refresh == 0) {
      display_update_dashboard(
          (ssid_db[0] && strcmp(ssid_db, "N/A") != 0) ? ssid_db
                                                       : MIMI_SECRET_WIFI_SSID,
          wifi_manager_is_connected() ? wifi_manager_get_ip() : "0.0.0.0",
          battery_get_voltage(), battery_get_percentage(), sd.temperature,
          sd.humidity, bluetooth_is_enabled(), pm_system_get_mode(), up_db,
          thinking);
      s_last_epaper_refresh = now_ms;
  }
}

void execute_button_action(int action_id) {
  if (action_id == 1 || action_id == 2) {
    nvs_handle_t nvs_db;
    char ssid_db[32] = "N/A";
    if (nvs_open(MIMI_NVS_WIFI, NVS_READONLY, &nvs_db) == ESP_OK) {
      size_t len = sizeof(ssid_db);
      nvs_get_str(nvs_db, MIMI_NVS_KEY_SSID, ssid_db, &len);
      nvs_close(nvs_db);
    }

    shtc3_data_t sd = {0};
    /*
    esp_err_t err = shtc3_read(&sd);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to read SHTC3 sensor: %s", esp_err_to_name(err));
    }
    */

    char up_db[32];
    agent_metrics_get_uptime_str(up_db, sizeof(up_db));

    if (action_id == 1) {
      mimi_update_dashboard(false, false);
    } else if (action_id == 2) {
      char *buf = malloc(1024);
      if (buf) {
        const char *p_str = "BAL";
        if (pm_system_get_mode() == 2)
          p_str = "PERF";

        time_t now_db;
        struct tm t_info;
        time(&now_db);
        localtime_r(&now_db, &t_info);
        char t_str[32] = "No Time Sync";
        if (t_info.tm_year > (2020 - 1900)) {
          strftime(t_str, sizeof(t_str), "%H:%M  %d.%m.%y", &t_info);
        }

        char sd_str[32] = "N/A";
        uint64_t m_tot, m_free;
        if (esp_vfs_fat_info("/sdcard", &m_tot, &m_free) == ESP_OK) {
          uint64_t total_mb = m_tot / (1024 * 1024);
          snprintf(sd_str, sizeof(sd_str), "OK (%llu MB)", total_mb);
        }

        // Ensure Discord connection status is available (requires discord_bot.h
        // included in mimi.c!)
        extern bool discord_bot_is_connected(void);

        snprintf(buf, 1024,
                 "```text\n"
                 "=========================\n"
                 "     MIMI DASHBOARD      \n"
                 "=========================\n"
                 "WIFI: %.14s\n"
                 "IP:   %s\n\n"
                 "BATT: %d%% (%.2fV)\n"
                 "TEMP: %.1f C  HUM: %.1f %%\n\n"
                 "STATUS: ONLINE\n"
                 "TG: ACTIVE\n"
                 "DISCORD: %s\n\n"
                 "BLUETOOTH: %s\n"
                 "SD CARD:   %s\n"
                 "PWR MODE:  %s\n\n"
                 "      %s\n"
                 "    Uptime: %s\n"
                 "=========================\n"
                 "```",
                 strcmp(ssid_db, "N/A") != 0 ? ssid_db : MIMI_SECRET_WIFI_SSID,
                 wifi_manager_get_ip(), battery_get_percentage(),
                 battery_get_voltage(), sd.temperature, sd.humidity,
                 discord_bot_is_connected() ? "ACTIVE" : "INACTIVE",
                 bluetooth_is_enabled() ? "ON " : "OFF", sd_str, p_str, t_str,
                 up_db);

        mimi_msg_t msg = {.channel = "telegram",
                          .chat_id = "6357689474",
                          .content = strdup(buf)};
        message_bus_push_outbound(&msg);

        mimi_msg_t msg_dc = {.channel = "discord",
                             .chat_id = "1520521736817475797",
                             .content = strdup(buf)};
        message_bus_push_outbound(&msg_dc);

        free(buf);
      }
    }
  } else if (action_id == 3) {
    ESP_LOGI(TAG, "TRIPLE CLICK -> (Bluetooth toggle disabled)");
    /*
    if (bluetooth_is_enabled())
      bluetooth_deinit();
    else
      bluetooth_init();
    */
  } else if (action_id == 4) {
    ESP_LOGW(TAG, "LONG PRESS -> Force Reboot!");
    esp_restart();
  }
}

void app_main(void) {
  /* Silence noisy components */
  esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

  /* Phase 0: Power Stabilization (Crucial for wall chargers) */
  vTaskDelay(pdMS_TO_TICKS(2000));

  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "  MimiClaw - ESP32-S3 AI Agent");
  ESP_LOGI(TAG, "========================================");

  /* Print memory info */
  ESP_LOGI(TAG, "Internal free: %d bytes",
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  ESP_LOGI(TAG, "PSRAM free:    %d bytes",
           (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  /* Phase 1: Core infrastructure */
  ESP_ERROR_CHECK(init_nvs());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ESP_ERROR_CHECK(init_spiffs());

  /* Try to mount SD Card BEFORE display_init so JPEGs can be found */
  esp_err_t sd_err = sd_card_init();
  if (sd_err != ESP_OK) {
    ESP_LOGW(TAG, "SD Card not available.");
  } else {
    // Now that SD is mounted, load files!
    ui_load_welcome_messages();
  }

  ESP_ERROR_CHECK(display_init());

  xTaskCreate(ui_task, "ui_task", 4096, NULL, 5, NULL);

  // ESP_ERROR_CHECK(shtc3_init()); // Disabled for AMOLED migration
  ESP_ERROR_CHECK(buttons_init());
  ESP_ERROR_CHECK(battery_init());
  ESP_ERROR_CHECK(led_init());
  led_set_state_color(MIMI_COLOR_OFFLINE); // Start Red (Offline)
  
  ESP_ERROR_CHECK(pm_system_init());

  /* Initialize subsystems */
  ESP_ERROR_CHECK(message_bus_init());
  ESP_ERROR_CHECK(memory_store_init());
  ESP_ERROR_CHECK(session_mgr_init());
  ESP_ERROR_CHECK(wifi_manager_init());
  ESP_ERROR_CHECK(http_proxy_init());
  ESP_ERROR_CHECK(telegram_bot_init());
  discord_bot_init(); // Not checked via ESP_ERROR_CHECK as it gracefully
                      // ignores missing tokens
  ESP_ERROR_CHECK(llm_proxy_init());
  ESP_ERROR_CHECK(tool_registry_init());
  ESP_ERROR_CHECK(context_cache_init());
  ESP_ERROR_CHECK(agent_loop_init());
  ESP_ERROR_CHECK(skill_loader_init());

  /* 5. Start Hardware Systems */
  rules_engine_init();
  // audio_service_init();
  // audio_service_set_volume(70);
  // audio_service_play_file("/spiffs/audio/boot.raw");

  /* Start Serial CLI */
  ESP_ERROR_CHECK(serial_cli_init());

  /* Start WiFi */
  led_set_state_color(MIMI_COLOR_CONNECTING); // Set Yellow (Connecting)

  esp_err_t wifi_err = wifi_manager_start();
  if (wifi_err == ESP_OK) {
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    if (wifi_manager_wait_connected(30000) == ESP_OK) {
      ESP_LOGI(TAG, "WiFi connected: %s", wifi_manager_get_ip());

      /* Sync Time */
      ESP_LOGI(TAG, "Syncing time via SNTP...");
      setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
      tzset();
      esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
      esp_sntp_setservername(0, "pool.ntp.org");
      esp_sntp_init();

      int retry = 0;
      time_t now = 0;
      struct tm timeinfo = {0};
      while (++retry < 15) {
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year > (2023 - 1900)) {
          break; // Time is synced!
        }
        ESP_LOGI(TAG, "Waiting for system time... (%d/15)", retry);
        vTaskDelay(pdMS_TO_TICKS(2000));
      }
      ESP_LOGI(TAG, "Time is set: %s", asctime(&timeinfo));
      led_set_state_color(MIMI_COLOR_ONLINE); // Set Green (Online)

      /* Start network-dependent services */
      telegram_bot_start();
      /* Start AI Agent, Discord, etc. */
      discord_bot_start();
      agent_loop_start();
      ws_server_start();

      /* Outbound dispatch task */
      xTaskCreatePinnedToCore(outbound_dispatch_task, "outbound",
                              MIMI_OUTBOUND_STACK, NULL, MIMI_OUTBOUND_PRIO,
                              NULL, MIMI_OUTBOUND_CORE);

      /* Wait for Discord to establish connection so Dashboard reflects true
       * status */
      int discord_retry = 0;
      extern bool discord_bot_is_connected(void);
      while (!discord_bot_is_connected() && ++discord_retry < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
      }
    }
  }

  /* Return to Balanced mode */
  // pm_system_set_mode(MIMI_PWR_BALANCED);

  mimi_update_dashboard(false, true);

  ESP_LOGI(TAG, "MimiClaw system phases complete!");
}
