#include "serial_cli.h"
#include "agent/agent_loop.h"
#include "agent/agent_metrics.h"
#include "driver/i2c_master.h"
#include "hardware/battery.h"
#include "hardware/bluetooth_utils.h"
#include "hardware/display.h"
#include "hardware/network_utils.h"
#include "hardware/led.h"
#include "hardware/pm_system.h"
#include "hardware/shtc3.h"
#include "llm/llm_proxy.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"
#include "telegram/telegram_bot.h"
#include "tools/tool_agent.h"
#include "tools/tool_get_time.h"
#include "tools/tool_web_search.h"
#include "wifi/wifi_manager.h"

#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "cli";

/* --- Helper: Format byte size --- */
static void format_size(uint64_t size, char *buf, size_t buf_size) {
  if (size < 1024)
    snprintf(buf, buf_size, "%llu B", size);
  else if (size < 1024 * 1024)
    snprintf(buf, buf_size, "%.1f KB", (double)size / 1024.0);
  else if (size < 1024 * 1024 * 1024ULL)
    snprintf(buf, buf_size, "%.1f MB", (double)size / (1024.0 * 1024.0));
  else
    snprintf(buf, buf_size, "%.1f GB", (double)size / (1024.0 * 1024.0 * 1024.0));
}

/* --- Helper: Print filesystem tree --- */
static void print_tree(const char *base_path) {
  DIR *dir = opendir(base_path);
  if (!dir)
    return;
  struct dirent *ent;
  char last_folder[64] = "";
  printf("%s\n", base_path);
  while ((ent = readdir(dir)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0 ||
        strcmp(ent->d_name, ".DS_Store") == 0)
      continue;
    char *slash = strchr(ent->d_name, '/');
    if (slash) {
      char current_folder[64];
      size_t folder_len = slash - ent->d_name;
      strncpy(current_folder, ent->d_name, folder_len);
      current_folder[folder_len] = '\0';
      if (strcmp(current_folder, last_folder) != 0) {
        printf("├── %s/\n", current_folder);
        strncpy(last_folder, current_folder, sizeof(last_folder) - 1);
        last_folder[sizeof(last_folder) - 1] = '\0';
      }
      struct stat st;
      char full_path[512];
      snprintf(full_path, sizeof(full_path), "%s/%s", base_path, ent->d_name);
      stat(full_path, &st);
      char size_str[16];
      format_size(st.st_size, size_str, sizeof(size_str));
      printf("│   ├── %s (%s)\n", slash + 1, size_str);
    } else {
      struct stat st;
      char full_path[512];
      snprintf(full_path, sizeof(full_path), "%s/%s", base_path, ent->d_name);
      stat(full_path, &st);
      char size_str[16];
      format_size(st.st_size, size_str, sizeof(size_str));
      printf("├── %s (%s)\n", ent->d_name, size_str);
    }
  }
  closedir(dir);
}

/* --- Helper: Base64 to file --- */
static int file_b64_impl(const char *path, const char *b64, const char *mode) {
  size_t b64_len = strlen(b64);
  size_t out_len = 0;
  unsigned char *decoded = malloc(b64_len);
  if (!decoded)
    return 1;
  int ret = mbedtls_base64_decode(decoded, b64_len, &out_len,
                                  (const unsigned char *)b64, b64_len);
  if (ret != 0) {
    printf("Error: Base64 decoding failed (0x%X)\n", -ret);
    free(decoded);
    return 1;
  }
  FILE *f = fopen(path, mode);
  if (!f) {
    printf("Error: could not open %s with mode %s\n", path, mode);
    free(decoded);
    return 1;
  }
  fwrite(decoded, 1, out_len, f);
  fclose(f);
  free(decoded);
  printf("OK: %s %d bytes to %s\n",
         (strcmp(mode, "a") == 0 ? "appended" : "wrote"), (int)out_len, path);
  return 0;
}

/* --- sd_info command --- */
static int cmd_sd_info(int argc, char **argv) {
  uint64_t total, free_bytes;
  if (esp_vfs_fat_info("/sdcard", &total, &free_bytes) == ESP_OK) {
    char total_str[16], free_str[16];
    format_size(total, total_str, sizeof(total_str));
    format_size(free_bytes, free_str, sizeof(free_str));
    printf("SD Card (/sdcard):\n");
    printf("  Total Size: %s\n", total_str);
    printf("  Free Space: %s\n", free_str);
  } else {
    printf("Error: SD Card not mounted or inaccessible.\n");
  }
  return 0;
}

/* --- discord_set_token command --- */
static int cmd_discord_set_token(int argc, char **argv) {
  if (argc != 2) {
    printf("Usage: discord_set_token <token>\n");
    return 0;
  }
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_DISCORD, NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_str(nvs, MIMI_NVS_KEY_DISCORD_TOKEN, argv[1]);
    nvs_commit(nvs);
    nvs_close(nvs);
    printf("Discord Token saved to NVS. Reboot to apply.\n");
  } else {
    printf("Failed to open NVS.\n");
  }
  return 0;
}

/* --- wifi_set command --- */
static struct {
  struct arg_str *ssid;
  struct arg_str *password;
  struct arg_end *end;
} wifi_set_args;

static int cmd_wifi_set(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&wifi_set_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, wifi_set_args.end, argv[0]);
    return 1;
  }
  wifi_manager_set_credentials(wifi_set_args.ssid->sval[0],
                               wifi_set_args.password->sval[0]);
  printf("WiFi credentials saved. Restart to apply.\n");
  return 0;
}

/* --- wifi_status command --- */
static int cmd_wifi_status(int argc, char **argv) {
  printf("WiFi connected: %s\n", wifi_manager_is_connected() ? "yes" : "no");
  printf("IP: %s\n", wifi_manager_get_ip());
  return 0;
}

/* --- set_tg_token command --- */
static struct {
  struct arg_str *token;
  struct arg_end *end;
} tg_token_args;

static int cmd_set_tg_token(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&tg_token_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, tg_token_args.end, argv[0]);
    return 1;
  }
  telegram_set_token(tg_token_args.token->sval[0]);
  printf("Telegram bot token saved.\n");
  return 0;
}

/* --- set_api_key command --- */
static struct {
  struct arg_str *key;
  struct arg_end *end;
} api_key_args;

static int cmd_set_api_key(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&api_key_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, api_key_args.end, argv[0]);
    return 1;
  }
  llm_set_api_key(api_key_args.key->sval[0]);
  printf("API key saved.\n");
  return 0;
}

/* --- set_model command --- */
static struct {
  struct arg_str *model;
  struct arg_end *end;
} model_args;

static int cmd_set_model(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&model_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, model_args.end, argv[0]);
    return 1;
  }
  llm_set_model(model_args.model->sval[0]);
  printf("Model set.\n");
  return 0;
}

/* --- set_provider command --- */
static struct {
  struct arg_str *provider;
  struct arg_end *end;
} provider_args;

static int cmd_set_provider(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&provider_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, provider_args.end, argv[0]);
    return 1;
  }
  llm_set_provider(provider_args.provider->sval[0]);
  printf("Model provider set.\n");
  return 0;
}

/* --- memory_read command --- */
static int cmd_memory_read(int argc, char **argv) {
  char *buf = malloc(4096);
  if (!buf) {
    printf("Out of memory.\n");
    return 1;
  }
  if (memory_read_long_term(buf, 4096) == ESP_OK && buf[0]) {
    printf("=== MEMORY.md ===\n%s\n=================\n", buf);
  } else {
    printf("MEMORY.md is empty or not found.\n");
  }
  free(buf);
  return 0;
}

/* --- memory_write command --- */
static struct {
  struct arg_str *content;
  struct arg_end *end;
} memory_write_args;

static int cmd_memory_write(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&memory_write_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, memory_write_args.end, argv[0]);
    return 1;
  }
  memory_write_long_term(memory_write_args.content->sval[0]);
  printf("MEMORY.md updated.\n");
  return 0;
}

/* --- session_list command --- */
static int cmd_session_list(int argc, char **argv) {
  printf("Sessions:\n");
  session_list();
  return 0;
}

/* --- session_clear command --- */
static struct {
  struct arg_str *channel;
  struct arg_str *chat_id;
  struct arg_end *end;
} session_clear_args;

static int cmd_session_clear(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&session_clear_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, session_clear_args.end, argv[0]);
    return 1;
  }
  if (session_clear(session_clear_args.channel->sval[0], session_clear_args.chat_id->sval[0]) == ESP_OK) {
    printf("Session cleared.\n");
  } else {
    printf("Session not found.\n");
  }
  return 0;
}

/* --- heap_info command --- */
static int cmd_heap_info(int argc, char **argv) {
  printf("Internal free: %d bytes\n",
         (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  printf("PSRAM free:    %d bytes\n",
         (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  printf("Total free:    %d bytes\n", (int)esp_get_free_heap_size());
  return 0;
}

/* --- cmd_get_time --- */
static int cmd_get_time(int argc, char **argv) {
  char buf[128];
  ESP_LOGI(TAG, "Fetching time...");
  if (tool_get_time_execute(NULL, buf, sizeof(buf)) == ESP_OK) {
    printf("\n[TIME]\n%s\n\n", buf);
  } else {
    printf("\n[ERROR] Failed to fetch time\n\n");
  }
  return 0;
}

/* --- set_proxy command --- */
static struct {
  struct arg_str *host;
  struct arg_int *port;
  struct arg_end *end;
} proxy_args;

static int cmd_set_proxy(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&proxy_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, proxy_args.end, argv[0]);
    return 1;
  }
  http_proxy_set(proxy_args.host->sval[0], (uint16_t)proxy_args.port->ival[0]);
  printf("Proxy set. Restart to apply.\n");
  return 0;
}

/* --- clear_proxy command --- */
static int cmd_clear_proxy(int argc, char **argv) {
  http_proxy_clear();
  printf("Proxy cleared. Restart to apply.\n");
  return 0;
}

/* --- file_rm command --- */
static int cmd_file_rm(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: file_rm <path>\n");
    return 1;
  }
  unlink(argv[1]);
  printf("OK: file_rm processed %s\n", argv[1]);
  return 0;
}

/* --- mkdir command --- */
static int cmd_mkdir(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: mkdir <path>\n");
    return 1;
  }
  if (mkdir(argv[1], 0755) == 0) {
    printf("OK: created directory %s\n", argv[1]);
  } else {
    printf("INFO: directory %s already exists or could not be created\n",
           argv[1]);
  }
  return 0;
}

/* --- df command --- */
static int cmd_df(int argc, char **argv) {
  size_t total = 0, used = 0;
  if (esp_spiffs_info(NULL, &total, &used) != ESP_OK) {
    printf("Error: Could not get SPIFFS info\n");
    return 1;
  }
  printf("Storage: SPIFFS (Internal Flash)\n");
  printf("  Total: %9d bytes\n", (int)total);
  printf("  Used:  %9d bytes\n", (int)used);
  printf("  Free:  %9d bytes\n", (int)(total - used));
  printf("  Usage: %d%%\n", (int)(used * 100 / total));
  return 0;
}

static int cmd_epaper_dump(int argc, char **argv) {
    printf("ePaper not supported on this hardware.\n");
    return 0;
}

static int cmd_i2c_scan(int argc, char **argv) {
  printf("Scanning I2C bus (SDA=47, SCL=48)...\n");
  extern i2c_master_bus_handle_t bus_handle;
  if (!bus_handle) {
    printf("I2C bus not initialized.\n");
    return 1;
  }
  
  /* Suppress expected driver timeouts during blind scan */
  esp_log_level_set("i2c.master", ESP_LOG_NONE);
  
  for (int i = 1; i < 127; i++) {
    esp_err_t err = i2c_master_probe(bus_handle, i, pdMS_TO_TICKS(100));
    if (err == ESP_OK) {
      printf("Found device at 0x%02X\n", i);
    }
    vTaskDelay(pdMS_TO_TICKS(5)); 
  }
  
  esp_log_level_set("i2c.master", ESP_LOG_INFO);
  printf("Scan complete.\n");
  return 0;
}

/* --- led command --- */
static int cmd_led(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: led <color_name|action>\n");
        return 1;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "red") == 0) led_set_color(0xFF0000);
    else if (strcmp(cmd, "green") == 0) led_set_color(0x00FF00);
    else if (strcmp(cmd, "blue") == 0) led_set_color(0x0000FF);
    else if (strcmp(cmd, "purple") == 0) led_set_color(0x800080);
    else if (strcmp(cmd, "yellow") == 0) led_set_color(0xFFFF00);
    else if (strcmp(cmd, "orange") == 0) led_set_color(0xFFA500);
    else if (strcmp(cmd, "off") == 0) led_set_color(0x000000);
    else {
        printf("Unknown color/action: %s\n", cmd);
        return 1;
    }
    printf("LED set to %s\n", cmd);
    return 0;
}

/* --- led_rgb command --- */
static int cmd_led_rgb(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: led_rgb <r> <g> <b>\n");
        return 1;
    }
    uint8_t r = atoi(argv[1]);
    uint8_t g = atoi(argv[2]);
    uint8_t b = atoi(argv[3]);
    led_set_rgb(r, g, b);
    printf("LED set to RGB(%d, %d, %d)\n", r, g, b);
    return 0;
}

/* --- color command --- */
static struct {
    struct arg_str *val;
    struct arg_int *duration;
    struct arg_end *end;
} color_args;

static void led_revert_task(void *pvParameters) {
    int delay_ms = (int)pvParameters;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    led_set_state_color(MIMI_COLOR_ONLINE); // Revert to Green
    vTaskDelete(NULL);
}

static int cmd_color(int argc, char **argv) {
    int nerrors = arg_parse(argc, argv, (void **)&color_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, color_args.end, argv[0]);
        return 1;
    }

    const char *val = color_args.val->sval[0];
    uint32_t hex = 0;
    bool valid = true;

    if (val[0] == '#') {
        hex = (uint32_t)strtol(val + 1, NULL, 16);
    } else {
        if (strcmp(val, "red") == 0) hex = 0xFF0000;
        else if (strcmp(val, "green") == 0) hex = 0x00FF00;
        else if (strcmp(val, "blue") == 0) hex = 0x0000FF;
        else if (strcmp(val, "purple") == 0) hex = 0x800080;
        else if (strcmp(val, "yellow") == 0) hex = 0xFFFF00;
        else if (strcmp(val, "orange") == 0) hex = 0xFFA500;
        else if (strcmp(val, "off") == 0) hex = 0x000000;
        else valid = false;
    }

    if (!valid) {
        printf("Unknown color: %s (Use name or #hex)\n", val);
        return 1;
    }

    led_set_color(hex);
    printf("LED set to %s\n", val);

    if (color_args.duration->count > 0) {
        int seconds = color_args.duration->ival[0];
        printf("Reverting in %d seconds...\n", seconds);
        xTaskCreate(led_revert_task, "led_revert", 2048, (void *)(seconds * 1000), 5, NULL);
    }

    return 0;
}

/* --- ls_r command --- */
static int cmd_ls_r(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "/";
  print_tree(path);
  return 0;
}

/* --- ls_ssd command --- */
static int cmd_ls_ssd(int argc, char **argv) {
  printf("\033[1;33m[ Internal SSD (SPIFFS) ]\033[0m\n");
  print_tree(MIMI_SPIFFS_BASE);
  return 0;
}

/* --- ls_sd command --- */
static int cmd_ls_sd(int argc, char **argv) {
  printf("\033[1;32m[ External Media (SD Card) ]\033[0m\n");
  print_tree("/sdcard");
  return 0;
}

/* --- mv command --- */
static int cmd_mv(int argc, char **argv) {
  if (argc < 3) {
    printf("Usage: mv <src> <dst>\n");
    return 1;
  }
  if (rename(argv[1], argv[2]) == 0) {
    printf("OK: Moved %s to %s\n", argv[1], argv[2]);
  } else {
    printf("Error: Could not move file\n");
  }
  return 0;
}

/* --- cp command --- */
static int cmd_cp(int argc, char **argv) {
  if (argc < 3) {
    printf("Usage: cp <src> <dst>\n");
    return 1;
  }
  FILE *fsrc = fopen(argv[1], "rb");
  if (!fsrc) {
    printf("Error: Could not open source %s\n", argv[1]);
    return 1;
  }
  FILE *fdst = fopen(argv[2], "wb");
  if (!fdst) {
    printf("Error: Could not open destination %s\n", argv[2]);
    fclose(fsrc);
    return 1;
  }
  char buf[512];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
    fwrite(buf, 1, n, fdst);
  }
  fclose(fsrc);
  fclose(fdst);
  printf("OK: Copied %s to %s\n", argv[1], argv[2]);
  return 0;
}

/* --- cat command --- */
static int cmd_cat(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: cat <path>\n");
    return 1;
  }
  FILE *f = fopen(argv[1], "r");
  if (!f) {
    printf("Error: Could not open %s\n", argv[1]);
    return 1;
  }
  char buf[512];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    fwrite(buf, 1, n, stdout);
  }
  printf("\n");
  fclose(f);
  return 0;
}

/* --- touch command --- */
static int cmd_touch(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: touch <path>\n");
    return 1;
  }
  FILE *f = fopen(argv[1], "a");
  if (f) {
    fclose(f);
    printf("OK: touched %s\n", argv[1]);
  } else {
    printf("Error: could not touch %s\n", argv[1]);
  }
  return 0;
}

/* --- file_put command (Paste Mode) --- */
static int cmd_file_put(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: file_put <path>\n");
    return 1;
  }
  const char *path = argv[1];
  FILE *f = fopen(path, "w");
  if (!f) {
    printf("Error: could not open %s for writing\n", path);
    return 1;
  }
  printf("\033[1;33m>>> Mimi Paste Mode Active for: %s\033[0m\n", path);
  printf("Instructions: Paste your content now. When finished, type "
         "\033[1;36m---MIMI_EOF---\033[0m on a new line.\n");
  char line[512];
  size_t total_bytes = 0;
  while (fgets(line, sizeof(line), stdin)) {
    if (strncmp(line, "---MIMI_EOF---", 14) == 0)
      break;
    size_t len = strlen(line);
    fwrite(line, 1, len, f);
    total_bytes += len;
    if (total_bytes % 1024 < 512)
      printf(".");
  }
  fclose(f);
  printf("\n\033[1;32mOK: Received and saved %d bytes to %s\033[0m\n",
         (int)total_bytes, path);
  return 0;
}

/* --- log_level command --- */
static int cmd_log_level(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: log_level <level_0_to_5>\n");
    return 1;
  }
  int level = atoi(argv[1]);
  esp_log_level_set("*", (esp_log_level_t)level);
  printf("OK: global log level set to %d\n", level);
  return 0;
}

static int cmd_file_b64(int argc, char **argv) {
  if (argc < 3) {
    printf("Usage: file_b64 <path> <base64_content>\n");
    return 1;
  }
  return file_b64_impl(argv[1], argv[2], "w");
}

static int cmd_file_b64_append(int argc, char **argv) {
  if (argc < 3) {
    printf("Usage: file_b64_append <path> <base64_content>\n");
    return 1;
  }
  return file_b64_impl(argv[1], argv[2], "a");
}

/* --- set_search_key command --- */
static struct {
  struct arg_str *key;
  struct arg_end *end;
} search_key_args;

static int cmd_set_search_key(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&search_key_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, search_key_args.end, argv[0]);
    return 1;
  }
  tool_web_search_set_key(search_key_args.key->sval[0]);
  printf("Search API key saved.\n");
  return 0;
}

/* --- config_show command --- */
static void print_config(const char *label, const char *ns, const char *key,
                         const char *build_val, bool mask) {
  char nvs_val[128] = {0};
  const char *source = "not set";
  const char *display = "(empty)";

  /* NVS takes highest priority */
  nvs_handle_t nvs;
  if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
    size_t len = sizeof(nvs_val);
    if (nvs_get_str(nvs, key, nvs_val, &len) == ESP_OK && nvs_val[0]) {
      source = "NVS";
      display = nvs_val;
    }
    nvs_close(nvs);
  }

  /* Fall back to build-time value */
  if (strcmp(source, "not set") == 0 && build_val[0] != '\0') {
    source = "build";
    display = build_val;
  }

  if (mask && strlen(display) > 6 && strcmp(display, "(empty)") != 0) {
    printf("  %-14s: %.4s****  [%s]\n", label, display, source);
  } else {
    printf("  %-14s: %s  [%s]\n", label, display, source);
  }
}

static void print_config_u16(const char *label, const char *ns, const char *key,
                             const char *build_val) {
  char nvs_val[16] = {0};
  const char *source = "not set";
  const char *display = "(empty)";
  nvs_handle_t nvs;
  if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
    uint16_t value = 0;
    if (nvs_get_u16(nvs, key, &value) == ESP_OK && value > 0) {
      snprintf(nvs_val, sizeof(nvs_val), "%u", (unsigned)value);
      source = "NVS";
      display = nvs_val;
    }
    nvs_close(nvs);
  }
  if (strcmp(source, "not set") == 0 && build_val[0] != '\0') {
    source = "build";
    display = build_val;
  }
  printf("  %-14s: %s  [%s]\n", label, display, source);
}

static int cmd_config_show(int argc, char **argv) {
  printf("=== Current Configuration ===\n");
  print_config("WiFi SSID", MIMI_NVS_WIFI, MIMI_NVS_KEY_SSID,
               MIMI_SECRET_WIFI_SSID, false);
  print_config("WiFi Pass", MIMI_NVS_WIFI, MIMI_NVS_KEY_PASS,
               MIMI_SECRET_WIFI_PASS, true);
  print_config("TG Token", MIMI_NVS_TG, MIMI_NVS_KEY_TG_TOKEN,
               MIMI_SECRET_TG_TOKEN, true);

  char provider[32] = MIMI_LLM_PROVIDER_DEFAULT;
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_LLM, NVS_READONLY, &nvs) == ESP_OK) {
    size_t len = sizeof(provider);
    nvs_get_str(nvs, MIMI_NVS_KEY_PROVIDER, provider, &len);
    nvs_close(nvs);
  }

  char key_key[32] = "api_key";
  char model_key[32] = "model";
  const char *build_api_key = MIMI_SECRET_API_KEY;
  const char *build_model = MIMI_SECRET_MODEL;

  if (strcmp(provider, "gemini") == 0) {
    strcpy(key_key, "api_key_gem");
    strcpy(model_key, "model_gem");
    build_api_key = MIMI_SECRET_API_KEY_GEM;
    build_model = MIMI_SECRET_MODEL_GEM;
  } else if (strcmp(provider, "anthropic") == 0) {
    strcpy(key_key, "api_key_ant");
    strcpy(model_key, "model_ant");
    build_api_key = MIMI_SECRET_API_KEY_ANT;
    build_model = MIMI_SECRET_MODEL_ANT;
  } else if (strcmp(provider, "openai") == 0) {
    strcpy(key_key, "api_key_oai");
    strcpy(model_key, "model_oai");
    build_api_key = MIMI_SECRET_API_KEY_OAI;
    build_model = MIMI_SECRET_MODEL_OAI;
  }

  print_config("Provider", MIMI_NVS_LLM, MIMI_NVS_KEY_PROVIDER,
               MIMI_LLM_PROVIDER_DEFAULT, false);
  print_config("API Key", MIMI_NVS_LLM, key_key, build_api_key, true);
  print_config("Model", MIMI_NVS_LLM, model_key, build_model, false);

  print_config("Proxy Host", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_HOST,
               MIMI_SECRET_PROXY_HOST, false);
  print_config_u16("Proxy Port", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_PORT,
                   MIMI_SECRET_PROXY_PORT);
  print_config("Search Key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_API_KEY,
               MIMI_SECRET_SEARCH_KEY, true);
  printf("=============================\n");
  return 0;
}

static int cmd_help(int argc, char **argv) {
  printf("\n\033[1;35m--- MimiClaw Command Center (S3 Edition) ---\033[0m\n\n");

  printf("\033[1;33m[ 🔋 POWER ]\033[0m\n");
  printf("  \033[1;36mbatt_status\033[0m      Voltage, %% and ASCII level bar\n");
  printf("  \033[1;36mpwr_mode\033[0m         Set profile (balanced|perf)\n");
  printf("  \033[1;36mdeep_sleep\033[0m       Enter low power sleep for N ms\n\n");

  printf("\033[1;33m[ 📡 NETWORK ]\033[0m\n");
  printf("  \033[1;36mwifi_status\033[0m      Connection state & IP\n");
  printf("  \033[1;36mwifi_scan\033[0m        Scan nearby WiFi networks\n");
  printf("  \033[1;36mwifi_rssi\033[0m        Live signal strength (dBm)\n");
  printf("  \033[1;36mping\033[0m             Test latency to a host\n");
  printf("  \033[1;36mip_info\033[0m          Show detailed interface info\n");
  printf("  \033[1;36mntp_sync\033[0m         Force clock synchronization\n");
  printf("  \033[1;36mget_time\033[0m         Fetch & print HTTP time\n");
  printf("  \033[1;36mset_proxy\033[0m        Configure HTTP proxy\n\n");

  printf("\033[1;33m[ 🤖 AI AGENT ]\033[0m\n");
  printf("  \033[1;36mset_model\033[0m        Select LLM (gemini-flash-latest, etc)\n");
  printf("  \033[1;36mtoken_count\033[0m      Show session token usage\n");
  printf("  \033[1;36muptime\033[0m           Total system run time\n");
  printf("  \033[1;36mmemory_read\033[0m      View long-term MEMORY.md\n");
  printf("  \033[1;36magent_stack\033[0m      Monitor AI task memory health\n");
  printf("  \033[1;36maudit_log\033[0m        View recent tool execution history\n\n");

  printf("\033[1;33m[ 💾 STORAGE ]\033[0m\n");
  printf("  \033[1;36mls_r\033[0m             Tree view of storage (ls_r /sdcard)\n");
  printf("  \033[1;36mls_ssd\033[0m           Tree view of Internal SSD (SPIFFS)\n");
  printf("  \033[1;36mdf\033[0m               Disk usage statistics\n");
  printf("  \033[1;36mfile_put\033[0m         \033[1;32mPaste Mode\033[0m file upload\n\n");

  printf("\033[1;33m[ 🛠️ HARDWARE ]\033[0m\n");
  printf("  \033[1;36msense_raw\033[0m        Read SHTC3 I2C registers\n");

  printf("  \033[1;36mi2c_scan\033[0m         Find devices on the bus\n");
  printf("  \033[1;36mcolor\033[0m            Set LED by name or #hex (ex: color purple -t 5)\n");
  printf("  \033[1;36mled\033[0m              Set LED by name (red, green, blue...)\n");
  printf("  \033[1;36mled_rgb\033[0m          Set LED by raw R G B values\n");
  printf("  \033[1;36mbt_info\033[0m          Bluetooth stack & address info\n");
  printf("  \033[1;36mbt_scan\033[0m          Scan for nearby BLE devices\n");
  printf("  \033[1;36mbt_toggle\033[0m        Enable/Disable Bluetooth radio\n\n");

  printf("\033[1;33m[ ⚙️ SYSTEM ]\033[0m\n");
  printf("  \033[1;36mconfig_show\033[0m      Display all NVS/Build settings\n");
  printf("  \033[1;36mheap_info\033[0m        RAM status (Internal/PSRAM)\n");
  printf("  \033[1;36mrestart\033[0m          Reboot Mimi\n\n");
  return 0;
}

/* --- config_reset command --- */
static int cmd_config_reset(int argc, char **argv) {
  const char *namespaces[] = {MIMI_NVS_WIFI, MIMI_NVS_TG, MIMI_NVS_LLM,
                              MIMI_NVS_PROXY, MIMI_NVS_SEARCH};
  for (int i = 0; i < 5; i++) {
    nvs_handle_t nvs;
    if (nvs_open(namespaces[i], NVS_READWRITE, &nvs) == ESP_OK) {
      nvs_erase_all(nvs);
      nvs_commit(nvs);
      nvs_close(nvs);
    }
  }
  printf(
      "All NVS config cleared. Build-time defaults will be used on restart.\n");
  return 0;
}

/* --- restart command --- */
static int cmd_restart(int argc, char **argv) {
  printf("Restarting...\n");
  esp_restart();
  return 0; /* unreachable */
}

/* --- [🔋 BATTERY] Commands --- */

static int cmd_batt_status(int argc, char **argv) {
  float v = battery_get_voltage();
  int p = battery_get_percentage();

  printf("\n[🔋 BATTERY STATUS]\n");
  printf("Voltage:    %.2f V\n", v);
  printf("Percentage: %d %%\n", p);

  // ASCII Bar
  printf("Level:      [");
  for (int i = 0; i < 20; i++) {
    if (i < p / 5)
      printf("|");
    else
      printf(" ");
  }
  printf("]\n\n");

  return 0;
}

static int cmd_pwr_mode(int argc, char **argv) {
  if (argc < 2) {
    mimi_pwr_mode_t current = pm_system_get_mode();
    const char *modes[] = {"BALANCED", "PERFORMANCE"};
    printf("Current Power Mode: %s\n", modes[current]);
    printf("Usage: pwr_mode <balanced|perf>\n");
    return 0;
  }

  if (strcmp(argv[1], "balanced") == 0)
    pm_system_set_mode(MIMI_PWR_BALANCED);
  else if (strcmp(argv[1], "perf") == 0)
    pm_system_set_mode(MIMI_PWR_PERFORMANCE);
  else {
    printf("Error: Unknown mode '%s'\n", argv[1]);
    return 1;
  }

  return 0;
}

static struct {
  struct arg_int *ms;
  struct arg_end *end;
} deep_sleep_args;

static int cmd_deep_sleep(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&deep_sleep_args);
  if (nerrors != 0) {
    arg_print_errors(stdout, deep_sleep_args.end, "deep_sleep");
    return 1;
  }

  uint64_t ms = deep_sleep_args.ms->ival[0];
  printf("Mimi is entering deep sleep for %llu ms...\n", ms);

  pm_system_deep_sleep(ms);
  return 0;
}

/* --- [ 📡 NETWORK ] Commands --- */

static int cmd_ping(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: ping <host>\n");
    return 1;
  }
  printf("Pinging %s...\n", argv[1]);
  int ms = network_ping(argv[1]);
  if (ms >= 0)
    printf("Response from %s: time=%dms\n", argv[1], ms);
  else
    printf("Ping failed (timeout or unknown host)\n");
  return 0;
}

static int cmd_wifi_scan(int argc, char **argv) {
  char buf[1024];
  network_wifi_scan(buf, sizeof(buf));
  printf("\n%s\n", buf);
  return 0;
}

static int cmd_ip_info(int argc, char **argv) {
  char buf[256];
  if (network_get_ip_info(buf, sizeof(buf)) == ESP_OK) {
    printf("\n[NETWORK INFO]\n%s\n\n", buf);
  } else {
    printf("Error: Network not initialized\n");
  }
  return 0;
}

static int cmd_ntp_sync(int argc, char **argv) {
  printf("Syncing with NTP servers...\n");
  if (network_ntp_sync() == ESP_OK)
    printf("Time synced successfully\n");
  else
    printf("NTP sync failed\n");
  return 0;
}

/* --- [ 🤖 AI AGENT ] Commands --- */

static int cmd_token_count(int argc, char **argv) {
  agent_stats_t stats = agent_metrics_get_stats();
  printf("\n[TOKEN USAGE]\n");
  printf("Input Tokens:  %lu\n", (unsigned long)stats.tokens_in);
  printf("Output Tokens: %lu\n", (unsigned long)stats.tokens_out);
  printf("Total:         %lu\n\n",
         (unsigned long)(stats.tokens_in + stats.tokens_out));
  return 0;
}

static int cmd_model_list(int argc, char **argv) {
  printf("\n[SUPPORTED MODELS]\n");
  printf("- gemini-2.0-flash (Recommended)\n");
  printf("- gemini-2.0-pro-exp\n");
  printf("- claude-3-5-sonnet-20241022\n");
  printf("- gpt-4o\n\n");
  return 0;
}

static int cmd_agent_stack(int argc, char **argv) {
  TaskHandle_t h = agent_loop_get_task_handle();
  if (h) {
    UBaseType_t watermark = uxTaskGetStackHighWaterMark(h);
    printf("Agent Task Stack High Water Mark: %lu bytes free\n",
           (unsigned long)watermark);
  } else {
    printf("Agent task not running\n");
  }
  return 0;
}

static int cmd_audit_log(int argc, char **argv) {
  char buf[1024];
  agent_metrics_get_audit_log(buf, sizeof(buf));
  printf("\n%s\n", buf);
  return 0;
}

static int cmd_uptime(int argc, char **argv) {
  char buf[64];
  agent_metrics_get_uptime_str(buf, sizeof(buf));
  printf("Mimi Uptime: %s\n", buf);
  return 0;
}

/* --- [ 🛠️ HARDWARE ] Commands --- */

static int cmd_sense_raw(int argc, char **argv) {
  shtc3_raw_data_t raw;
  if (shtc3_read_raw(&raw) == ESP_OK) {
    printf("\n[SHTC3 RAW DATA]\n");
    printf("Temp Raw: 0x%04X\n", raw.temp_raw);
    printf("Hum Raw:  0x%04X\n\n", raw.hum_raw);
  } else {
    printf("Error: Failed to read SHTC3 raw data\n");
  }
  return 0;
}

static int cmd_epaper_refresh(int argc, char **argv) {
  printf("Refresh not supported on AMOLED\n");
  return 0;
}

static int cmd_epaper_invert(int argc, char **argv) {
  printf("Invert not supported on AMOLED\n");
  return 0;
}

static int cmd_wifi_rssi(int argc, char **argv) {
  int rssi = wifi_manager_get_rssi();
  if (rssi > -127) {
    printf("Current WiFi RSSI: %d dBm\n", rssi);
  } else {
    printf("WiFi not connected\n");
  }
  return 0;
}

/* --- [ 🛠️ HARDWARE ] Bluetooth Commands --- */

static int cmd_bt_info(int argc, char **argv) {
  char buf[512];
  if (bluetooth_get_info(buf, sizeof(buf)) == ESP_OK) {
    printf("\n[BT INFO]\n%s\n\n", buf);
  } else {
    printf("Error fetching Bluetooth info\n");
  }
  return 0;
}

static int cmd_bt_scan(int argc, char **argv) {
  printf("Scanning for BLE devices (3s)...\n");
  char buf[2048];
  if (bluetooth_ble_scan(3000, buf, sizeof(buf)) == ESP_OK) {
    printf("\n[BT SCAN RESULTS]\n%s\n\n", buf);
  } else {
    printf("Scan failed\n");
  }
  return 0;
}

static int cmd_bt_toggle(int argc, char **argv) {
  if (argc < 2) {
    printf("Current BT State: %s\n", bluetooth_is_enabled() ? "ON" : "OFF");
    printf("Usage: bt_toggle <on|off>\n");
    return 0;
  }

  if (strcmp(argv[1], "on") == 0) {
    if (bluetooth_init() == ESP_OK) printf("Bluetooth ON\n");
    else printf("Failed to enable Bluetooth\n");
  } else if (strcmp(argv[1], "off") == 0) {
    if (bluetooth_deinit() == ESP_OK) printf("Bluetooth OFF\n");
    else printf("Failed to disable Bluetooth\n");
  } else {
    printf("Usage: bt_toggle <on|off>\n");
  }
  return 0;
}

static int cmd_bt_show(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: bt_show <on|off>\n");
    return 0;
  }

  if (strcmp(argv[1], "on") == 0) {
    if (bluetooth_advertise_start() == ESP_OK) printf("BLE Broadcasting ON\n");
    else printf("Failed to start broadcasting (is Bluetooth enabled?)\n");
  } else if (strcmp(argv[1], "off") == 0) {
    if (bluetooth_advertise_stop() == ESP_OK) printf("BLE Broadcasting OFF\n");
    else printf("Failed to stop broadcasting\n");
  } else {
    printf("Usage: bt_show <on|off>\n");
  }
  return 0;
}

#include "driver/usb_serial_jtag.h"

esp_err_t serial_cli_init(void) {
  if (!usb_serial_jtag_is_connected()) {
      ESP_LOGW("cli", "No USB Host connected. Skipping Serial REPL initialization to avoid headless crash.");
      return ESP_OK;
  }

  esp_console_repl_t *repl = NULL;
  esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  repl_config.task_stack_size = 8192;
  repl_config.prompt = "mimi> ";
  repl_config.max_cmdline_length = 256;

  /* USB Serial JTAG */
  esp_console_dev_usb_serial_jtag_config_t hw_config =
      ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

  ESP_ERROR_CHECK(
      esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));

  /* Register commands */
  static esp_console_cmd_t help_cmd = {
      .command = "help",
      .help = "Show available commands",
      .func = &cmd_help,
  };
  esp_console_cmd_register(&help_cmd);

  /* config_show */
  static esp_console_cmd_t config_show_cmd = {
      .command = "config_show",
      .help = "Show current configuration",
      .func = &cmd_config_show,
  };
  esp_console_cmd_register(&config_show_cmd);

  /* config_reset */
  static esp_console_cmd_t config_reset_cmd = {
      .command = "config_reset",
      .help = "Reset all NVS overrides",
      .func = &cmd_config_reset,
  };
  esp_console_cmd_register(&config_reset_cmd);

  /* wifi_set */
  wifi_set_args.ssid = arg_str1(NULL, NULL, "<ssid>", "WiFi SSID");
  wifi_set_args.password = arg_str1(NULL, NULL, "<password>", "WiFi password");
  wifi_set_args.end = arg_end(2);
  static esp_console_cmd_t wifi_set_cmd = {
      .command = "wifi_set",
      .help = "Set WiFi credentials",
      .func = &cmd_wifi_set,
      .argtable = &wifi_set_args,
  };
  esp_console_cmd_register(&wifi_set_cmd);

  /* wifi_status */
  static esp_console_cmd_t wifi_status_cmd = {
      .command = "wifi_status",
      .help = "Show WiFi status",
      .func = &cmd_wifi_status,
  };
  esp_console_cmd_register(&wifi_status_cmd);

  /* set_tg_token */
  tg_token_args.token = arg_str1(NULL, NULL, "<token>", "TG Token");
  tg_token_args.end = arg_end(1);
  static esp_console_cmd_t tg_token_cmd = {
      .command = "set_tg_token",
      .help = "Set Telegram token",
      .func = &cmd_set_tg_token,
      .argtable = &tg_token_args,
  };
  esp_console_cmd_register(&tg_token_cmd);

  /* set_api_key */
  api_key_args.key = arg_str1(NULL, NULL, "<key>", "API Key");
  api_key_args.end = arg_end(1);
  static esp_console_cmd_t api_key_cmd = {
      .command = "set_api_key",
      .help = "Set LLM API key",
      .func = &cmd_set_api_key,
      .argtable = &api_key_args,
  };
  esp_console_cmd_register(&api_key_cmd);

  /* set_model */
  model_args.model = arg_str1(NULL, NULL, "<model>", "Model");
  model_args.end = arg_end(1);
  static esp_console_cmd_t model_cmd = {
      .command = "set_model",
      .help = "Set LLM model",
      .func = &cmd_set_model,
      .argtable = &model_args,
  };
  esp_console_cmd_register(&model_cmd);

  /* set_provider */
  provider_args.provider = arg_str1(NULL, NULL, "<provider>", "Provider");
  provider_args.end = arg_end(1);
  static esp_console_cmd_t provider_cmd = {
      .command = "set_provider",
      .help = "Set LLM provider",
      .func = &cmd_set_provider,
      .argtable = &provider_args,
  };
  esp_console_cmd_register(&provider_cmd);

  /* memory_read */
  static esp_console_cmd_t memory_read_cmd = {
      .command = "memory_read",
      .help = "Read MEMORY.md",
      .func = &cmd_memory_read,
  };
  esp_console_cmd_register(&memory_read_cmd);

  /* memory_write */
  memory_write_args.content = arg_str1(NULL, NULL, "<content>", "Content");
  memory_write_args.end = arg_end(1);
  static esp_console_cmd_t memory_write_cmd = {
      .command = "memory_write",
      .help = "Write to MEMORY.md",
      .func = &cmd_memory_write,
      .argtable = &memory_write_args,
  };
  esp_console_cmd_register(&memory_write_cmd);

  /* session_list */
  static esp_console_cmd_t session_list_cmd = {
      .command = "session_list",
      .help = "List active sessions",
      .func = &cmd_session_list,
  };
  esp_console_cmd_register(&session_list_cmd);

  /* session_clear */
  session_clear_args.channel = arg_str1(NULL, NULL, "<channel>", "Channel (e.g. telegram)");
  session_clear_args.chat_id = arg_str1(NULL, NULL, "<chat_id>", "Chat ID");
  session_clear_args.end = arg_end(2);
  static esp_console_cmd_t session_clear_cmd = {
      .command = "session_clear",
      .help = "Clear a session",
      .func = &cmd_session_clear,
      .argtable = &session_clear_args,
  };
  esp_console_cmd_register(&session_clear_cmd);

  /* heap_info */
  static esp_console_cmd_t heap_info_cmd = {
      .command = "heap_info",
      .help = "Show heap usage",
      .func = &cmd_heap_info,
  };
  esp_console_cmd_register(&heap_info_cmd);

  /* set_proxy */
  proxy_args.host = arg_str1(NULL, NULL, "<host>", "Host");
  proxy_args.port = arg_int1(NULL, NULL, "<port>", "Port");
  proxy_args.end = arg_end(2);
  static esp_console_cmd_t set_proxy_cmd = {
      .command = "set_proxy",
      .help = "Set HTTP proxy",
      .func = &cmd_set_proxy,
      .argtable = &proxy_args,
  };
  esp_console_cmd_register(&set_proxy_cmd);

  /* clear_proxy */
  static esp_console_cmd_t clear_proxy_cmd = {
      .command = "clear_proxy",
      .help = "Clear proxy",
      .func = &cmd_clear_proxy,
  };
  esp_console_cmd_register(&clear_proxy_cmd);

  /* set_search_key */
  search_key_args.key = arg_str1(NULL, NULL, "<key>", "Search Key");
  search_key_args.end = arg_end(1);
  static esp_console_cmd_t set_search_key_cmd = {
      .command = "set_search_key",
      .help = "Set search API key",
      .func = &cmd_set_search_key,
      .argtable = &search_key_args,
  };
  esp_console_cmd_register(&set_search_key_cmd);

  /* file_rm */
  static esp_console_cmd_t file_rm_cmd = {
      .command = "file_rm",
      .help = "Delete a file (e.g. file_rm /sdcard/a.txt)",
      .func = &cmd_file_rm,
  };
  esp_console_cmd_register(&file_rm_cmd);

  /* mkdir */
  static esp_console_cmd_t mkdir_cmd = {
      .command = "mkdir",
      .help = "Create a directory (e.g. mkdir /sdcard/logs)",
      .func = &cmd_mkdir,
  };
  esp_console_cmd_register(&mkdir_cmd);

  /* cat */
  static esp_console_cmd_t cat_cmd = {
      .command = "cat",
      .help = "Read a file (e.g. cat /sdcard/log.txt)",
      .func = &cmd_cat,
  };
  esp_console_cmd_register(&cat_cmd);

  /* df */
  static esp_console_cmd_t df_cmd = {
      .command = "df",
      .help = "Show disk usage",
      .func = &cmd_df,
      .argtable = NULL,
  };
  esp_console_cmd_register(&df_cmd);

  /* ls_r */
  static esp_console_cmd_t ls_r_cmd = {
      .command = "ls_r",
      .help = "List files recursively (e.g. ls_r /sdcard)",
      .func = &cmd_ls_r,
  };
  esp_console_cmd_register(&ls_r_cmd);
 
  /* ls_ssd */
  static esp_console_cmd_t ls_ssd_cmd = {
      .command = "ls_ssd",
      .help = "Tree view of the Internal SSD (SPIFFS)",
      .func = &cmd_ls_ssd,
  };
  esp_console_cmd_register(&ls_ssd_cmd);

  /* ls_sd */
  static esp_console_cmd_t ls_sd_cmd = {
      .command = "ls_sd",
      .help = "Tree view of the SD Card",
      .func = &cmd_ls_sd,
  };
  esp_console_cmd_register(&ls_sd_cmd);

  /* mv */
  static esp_console_cmd_t mv_cmd = {
      .command = "mv",
      .help = "Move or rename a file (e.g. mv /spiffs/a /sdcard/b)",
      .func = &cmd_mv,
  };
  esp_console_cmd_register(&mv_cmd);

  /* cp */
  static esp_console_cmd_t cp_cmd = {
      .command = "cp",
      .help = "Copy a file (e.g. cp /spiffs/a /sdcard/b)",
      .func = &cmd_cp,
  };
  esp_console_cmd_register(&cp_cmd);

  /* touch */
  static esp_console_cmd_t touch_cmd = {
      .command = "touch",
      .help = "Create an empty file",
      .func = &cmd_touch,
  };
  esp_console_cmd_register(&touch_cmd);

  /* file_put */
  static esp_console_cmd_t file_put_cmd = {
      .command = "file_put",
      .help = "Upload a file in Paste Mode (e.g. file_put /sdcard/a.txt)",
      .func = &cmd_file_put,
  };
  esp_console_cmd_register(&file_put_cmd);

  /* file_b64 */
  static esp_console_cmd_t file_b64_cmd = {
      .command = "file_b64",
      .help = "Write Base64 content to file",
      .func = &cmd_file_b64,
  };
  esp_console_cmd_register(&file_b64_cmd);

  /* file_b64_append */
  static esp_console_cmd_t file_b64_append_cmd = {
      .command = "file_b64_append",
      .help = "Append Base64 content to file",
      .func = &cmd_file_b64_append,
  };
  esp_console_cmd_register(&file_b64_append_cmd);

  /* log_level */
  static esp_console_cmd_t log_level_cmd = {
      .command = "log_level",
      .help = "Set log level (0-5)",
      .func = &cmd_log_level,
  };
  esp_console_cmd_register(&log_level_cmd);

  /* i2c_scan */
  static esp_console_cmd_t i2c_scan_cmd = {
      .command = "i2c_scan",
      .help = "Scan the I2C bus",
      .func = &cmd_i2c_scan,
  };
  esp_console_cmd_register(&i2c_scan_cmd);

  /* epaper_dump */
  static esp_console_cmd_t epaper_dump_cmd = {
      .command = "epaper_dump",
      .help = "Dump ePaper framebuffer",
      .func = &cmd_epaper_dump,
  };
  esp_console_cmd_register(&epaper_dump_cmd);

  /* restart */
  static esp_console_cmd_t restart_cmd = {
      .command = "restart",
      .help = "Restart the device",
      .func = &cmd_restart,
  };
  esp_console_cmd_register(&restart_cmd);

  printf("\n[🔋 BATTERY]\n");

  /* batt_status */
  static esp_console_cmd_t batt_status_cmd = {
      .command = "batt_status",
      .help = "Show battery voltage and percentage",
      .func = &cmd_batt_status,
  };
  esp_console_cmd_register(&batt_status_cmd);

  /* pwr_mode */
  static esp_console_cmd_t pwr_mode_cmd = {
      .command = "pwr_mode",
      .help = "Set power profile (balanced, perf)",
      .func = &cmd_pwr_mode,
  };
  esp_console_cmd_register(&pwr_mode_cmd);

  /* deep_sleep */
  deep_sleep_args.ms = arg_int1(NULL, NULL, "<ms>", "Duration in milliseconds");
  deep_sleep_args.end = arg_end(2);
  static esp_console_cmd_t deep_sleep_cmd = {
      .command = "deep_sleep",
      .help = "Enter deep sleep for N ms",
      .func = &cmd_deep_sleep,
      .argtable = &deep_sleep_args,
  };
  esp_console_cmd_register(&deep_sleep_cmd);

  printf("\n[ 📡 NETWORK ]\n");

  /* ping */
  static esp_console_cmd_t ping_cmd = {
      .command = "ping",
      .help = "Test latency to a host",
      .func = &cmd_ping,
  };
  esp_console_cmd_register(&ping_cmd);

  /* wifi_scan */
  static esp_console_cmd_t wifi_scan_cmd = {
      .command = "wifi_scan",
      .help = "List nearby access points",
      .func = &cmd_wifi_scan,
  };
  esp_console_cmd_register(&wifi_scan_cmd);

  /* ip_info */
  static esp_console_cmd_t ip_info_cmd = {
      .command = "ip_info",
      .help = "Show network addresses",
      .func = &cmd_ip_info,
  };
  esp_console_cmd_register(&ip_info_cmd);

  /* ntp_sync */
  static esp_console_cmd_t ntp_sync_cmd = {
      .command = "ntp_sync",
      .help = "Force time synchronization",
      .func = &cmd_ntp_sync,
  };
  esp_console_cmd_register(&ntp_sync_cmd);

  printf("\n[ 🤖 AI AGENT ]\n");

  /* token_count */
  static esp_console_cmd_t token_count_cmd = {
      .command = "token_count",
      .help = "Show session token usage",
      .func = &cmd_token_count,
  };
  esp_console_cmd_register(&token_count_cmd);

  /* model_list */
  static esp_console_cmd_t model_list_cmd = {
      .command = "model_list",
      .help = "List available LLM models",
      .func = &cmd_model_list,
  };
  esp_console_cmd_register(&model_list_cmd);

  /* agent_stack */
  static esp_console_cmd_t agent_stack_cmd = {
      .command = "agent_stack",
      .help = "Check AI task stack usage",
      .func = &cmd_agent_stack,
  };
  esp_console_cmd_register(&agent_stack_cmd);

  /* audit_log */
  static esp_console_cmd_t audit_log_cmd = {
      .command = "audit_log",
      .help = "Show recent tool execution log",
      .func = &cmd_audit_log,
  };
  esp_console_cmd_register(&audit_log_cmd);

  /* uptime */
  static esp_console_cmd_t uptime_cmd = {
      .command = "uptime",
      .help = "Show system uptime",
      .func = &cmd_uptime,
  };
  esp_console_cmd_register(&uptime_cmd);

  printf("\n[ 🛠️ HARDWARE ]\n");

  /* sense_raw */
  static esp_console_cmd_t sense_raw_cmd = {
      .command = "sense_raw",
      .help = "Read raw SHTC3 values",
      .func = &cmd_sense_raw,
  };
  esp_console_cmd_register(&sense_raw_cmd);

  /* led */
  static esp_console_cmd_t led_cmd = {
      .command = "led",
      .help = "Set LED color (red, green, blue, purple, yellow, orange, off)",
      .func = &cmd_led,
  };
  esp_console_cmd_register(&led_cmd);

  /* led_rgb */
  static esp_console_cmd_t led_rgb_cmd = {
      .command = "led_rgb",
      .help = "Set LED to specific RGB values (0-255)",
      .func = &cmd_led_rgb,
  };
  esp_console_cmd_register(&led_rgb_cmd);

  /* color */
  color_args.val = arg_str1(NULL, NULL, "<name|#hex>", "Color name or hex code");
  color_args.duration = arg_int0("t", "time", "<seconds>", "Time in seconds before reverting to green");
  color_args.end = arg_end(2);

  static esp_console_cmd_t color_cmd = {
      .command = "color",
      .help = "Set LED color by name or #hex, optionally for -t seconds",
      .func = &cmd_color,
      .argtable = &color_args,
  };
  esp_console_cmd_register(&color_cmd);

  /* epaper_refresh */
  static esp_console_cmd_t epaper_refresh_cmd = {
      .command = "epaper_refresh",
      .help = "Trigger full display refresh",
      .func = &cmd_epaper_refresh,
  };
  esp_console_cmd_register(&epaper_refresh_cmd);

  /* epaper_invert */
  static esp_console_cmd_t epaper_invert_cmd = {
      .command = "epaper_invert",
      .help = "Set display inversion (0/1)",
      .func = &cmd_epaper_invert,
  };
  esp_console_cmd_register(&epaper_invert_cmd);

  /* get_time */
  static esp_console_cmd_t get_time_cmd = {
      .command = "get_time",
      .help = "Fetch current time via HTTP",
      .func = &cmd_get_time,
  };
  esp_console_cmd_register(&get_time_cmd);

  /* wifi_rssi */
  static esp_console_cmd_t wifi_rssi_cmd = {
      .command = "wifi_rssi",
      .help = "Show current signal strength",
      .func = &cmd_wifi_rssi,
  };
  esp_console_cmd_register(&wifi_rssi_cmd);

  const esp_console_cmd_t bt_info_cmd = {
      .command = "bt_info",
      .help = "Show Bluetooth stack info and MAC address",
      .func = &cmd_bt_info,
  };
  esp_console_cmd_register(&bt_info_cmd);

  const esp_console_cmd_t bt_scan_cmd = {
      .command = "bt_scan",
      .help = "Scan for nearby BLE devices",
      .func = &cmd_bt_scan,
  };
  esp_console_cmd_register(&bt_scan_cmd);

  const esp_console_cmd_t bt_toggle_cmd = {
      .command = "bt_toggle",
      .help = "Enable (on) or Disable (off) Bluetooth radio",
      .func = &cmd_bt_toggle,
  };
  esp_console_cmd_register(&bt_toggle_cmd);

  const esp_console_cmd_t bt_advertise_cmd = {
      .command = "bt_advertise",
      .help = "Start (on) or Stop (off) BLE broadcasting/advertising",
      .func = &cmd_bt_show,
  };
  esp_console_cmd_register(&bt_advertise_cmd);

  /* sd_info */
  static esp_console_cmd_t sd_info_cmd = {
      .command = "sd_info",
      .help = "Show SD Card capacity and free space",
      .func = &cmd_sd_info,
  };
  esp_console_cmd_register(&sd_info_cmd);

  const esp_console_cmd_t discord_set_token_cmd = {
      .command = "discord_set_token",
      .help = "Set the Discord Bot Token",
      .hint = "<token>",
      .func = &cmd_discord_set_token,
  };
  esp_console_cmd_register(&discord_set_token_cmd);

  /* Start REPL */
  ESP_ERROR_CHECK(esp_console_start_repl(repl));
  ESP_LOGI(TAG, "Serial CLI started");

  return ESP_OK;
}

