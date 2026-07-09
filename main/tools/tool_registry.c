#include "tool_registry.h"
#include "tools/tool_agent.h"
#include "tools/tool_bluetooth.h"
#include "tools/tool_cli.h"
#include "tools/tool_display.h"
#include "tools/tool_files.h"
#include "tools/tool_get_time.h"
#include "tools/tool_network.h"
#include "tools/tool_power.h"
#include "tools/tool_sense.h"
#include "tools/tool_web_search.h"
#include "tools/tool_led.h"
#include "tools/tool_audio.h"

#include "agent/agent_metrics.h"
#include "cJSON.h"
#include "tools/tool_rules.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "tools";

#define MAX_TOOLS 64

static mimi_tool_t s_tools[MAX_TOOLS];
static int s_tool_count = 0;
static char *s_tools_json = NULL; /* cached JSON array string */

static void register_tool(const mimi_tool_t *tool) {
  if (s_tool_count >= MAX_TOOLS) {
    ESP_LOGE(TAG, "Tool registry full");
    return;
  }
  s_tools[s_tool_count++] = *tool;
  ESP_LOGI(TAG, "Registered tool: %s", tool->name);
}

static void build_tools_json(void) {
  cJSON *arr = cJSON_CreateArray();

  for (int i = 0; i < s_tool_count; i++) {
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", s_tools[i].name);
    cJSON_AddStringToObject(tool, "description", s_tools[i].description);

    cJSON *schema = cJSON_Parse(s_tools[i].input_schema_json);
    if (schema) {
      cJSON_AddItemToObject(tool, "input_schema", schema);
    }

    cJSON_AddItemToArray(arr, tool);
  }

  free(s_tools_json);
  s_tools_json = cJSON_PrintUnformatted(arr);
  cJSON_Delete(arr);

  ESP_LOGI(TAG, "Tools JSON built (%d tools)", s_tool_count);
}

esp_err_t tool_registry_init(void) {
  s_tool_count = 0;

  /* Register web_search */
  tool_web_search_init();

  mimi_tool_t ws = {
      .name = "web_search",
      .description = "Search the web for current information. Use this when "
                     "you need up-to-date facts, news, weather, or anything "
                     "beyond your training data.",
      .input_schema_json = "{\"type\":\"object\","
                           "\"properties\":{\"query\":{\"type\":\"string\","
                           "\"description\":\"The search query\"}},"
                           "\"required\":[\"query\"]}",
      .execute = tool_web_search_execute,
  };
  register_tool(&ws);

  /* Register get_current_time */
  mimi_tool_t gt = {
      .name = "get_current_time",
      .description =
          "Get the current date and time. Also sets the system clock. Call "
          "this when you need to know what time or date it is.",
      .input_schema_json = "{\"type\":\"object\","
                           "\"properties\":{},"
                           "\"required\":[]}",
      .execute = tool_get_time_execute,
  };
  register_tool(&gt);

  /* Register read_file */
  mimi_tool_t rf = {
      .name = "read_file",
      .description =
          "Read a file from storage. Path must start with /spiffs/ or /sdcard/.",
      .input_schema_json =
          "{\"type\":\"object\","
          "\"properties\":{\"path\":{\"type\":\"string\",\"description\":"
          "\"Absolute path starting with /spiffs/ or /sdcard/\"}},"
          "\"required\":[\"path\"]}",
      .execute = tool_read_file_execute,
  };
  register_tool(&rf);

  /* Register write_file */
  mimi_tool_t wf = {
      .name = "write_file",
      .description = "Write or overwrite a file on storage. Path must "
                     "start with /spiffs/ or /sdcard/.",
      .input_schema_json =
          "{\"type\":\"object\","
          "\"properties\":{\"path\":{\"type\":\"string\",\"description\":"
          "\"Absolute path starting with /spiffs/ or /sdcard/\"},"
          "\"content\":{\"type\":\"string\",\"description\":\"File content to "
          "write\"}},"
          "\"required\":[\"path\",\"content\"]}",
      .execute = tool_write_file_execute,
  };
  register_tool(&wf);

  /* Register edit_file */
  mimi_tool_t ef = {
      .name = "edit_file",
      .description = "Find and replace text in a file on storage. Replaces "
                     "first occurrence of old_string with new_string.",
      .input_schema_json =
          "{\"type\":\"object\","
          "\"properties\":{\"path\":{\"type\":\"string\",\"description\":"
          "\"Absolute path starting with /spiffs/ or /sdcard/\"},"
          "\"old_string\":{\"type\":\"string\",\"description\":\"Text to "
          "find\"},"
          "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement "
          "text\"}},"
          "\"required\":[\"path\",\"old_string\",\"new_string\"]}",
      .execute = tool_edit_file_execute,
  };
  register_tool(&ef);

  /* Register list_dir */
  mimi_tool_t ld = {
      .name = "list_dir",
      .description =
          "List files on storage. Pass path to directory (/spiffs or /sdcard).",
      .input_schema_json =
          "{\"type\":\"object\","
          "\"properties\":{\"path\":{\"type\":\"string\",\"description\":"
          "\"Path to directory to list, e.g. /spiffs or /sdcard\"}},"
          "\"required\":[]}",
      .execute = tool_list_dir_execute,
  };
  register_tool(&ld);
 
  /* Register delete_file */
  mimi_tool_t df = {
      .name = "delete_file",
      .description = "Permanently delete a file from the filesystem. Use with caution.",
      .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Full path starting with /spiffs/ or /sdcard/\"}},\"required\":[\"path\"]}",
      .execute = tool_delete_file_execute,
  };
  register_tool(&df);

  /* Register display_control */
  mimi_tool_t dc = {
      .name = "display_control",
      .description = "Display a text message on the physical AMOLED screen. Use action='show' or 'clear'.",
      .input_schema_json = "{\"type\":\"object\",\"properties\":{"
                "\"action\":{\"type\":\"string\",\"enum\":[\"show\",\"clear\"]},"
                "\"text\":{\"type\":\"string\",\"description\":\"Text to display on screen\"}"
                "},\"required\":[\"action\"]}",
      .execute = tool_display_execute,
  };
  register_tool(&dc);

  /* Register sense (DISABLED for AMOLED migration to prevent I2C conflicts)
  mimi_tool_t s = {
      .name = "sense",
      .description = "Activate the SHTC3 physical hardware to read ambient "
                     "Temperature and Humidity. Returns raw numeric data.",
      .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
      .execute = tool_sense_execute,
  };
  register_tool(&s);
  */

  /* Register run_cli */
  mimi_tool_t cli = {
      .name = "run_cli",
      .description = "Execute an internal firmware console command and get the "
                     "output. Use this for diagnostics, system info, and "
                     "advanced hardware control. Available commands: "
                     "help, i2c_scan, heap_info, wifi_status, etc.",
      .input_schema_json = "{\"type\":\"object\","
                           "\"properties\":{\"command\":{\"type\":\"string\","
                           "\"description\":\"The command to run\"}},"
                           "\"required\":[\"command\"]}",
      .execute = tool_cli_execute,
  };
  register_tool(&cli);

  /* Register manage_power */
  mimi_tool_t pwr = {
      .name = "manage_power",
      .description = "Get battery status and manage power modes (balanced, "
                     "performance) or hibernate the device.",
      .input_schema_json = "{\"type\":\"object\","
                           "\"properties\":{"
                           "\"action\":{\"type\":\"string\",\"enum\":["
                           "\"status\",\"set_mode\",\"hibernate\"]},"
                           "\"mode\":{\"type\":\"string\",\"enum\":["
                           "\"balanced\",\"performance\"]},"
                           "\"duration_ms\":{\"type\":\"number\"}"
                           "},\"required\":[\"action\"]}",
      .execute = tool_power_execute,
  };
  register_tool(&pwr);

  /* Register manage_network */
  mimi_tool_t net = {
      .name = "manage_network",
      .description = "Perform network diagnostics (ping, scan, info) and "
                     "synchronize time via NTP.",
      .input_schema_json = "{\"type\":\"object\","
                           "\"properties\":{"
                           "\"action\":{\"type\":\"string\",\"enum\":[\"ping\","
                           "\"scan\",\"info\",\"sync\"]},"
                           "\"host\":{\"type\":\"string\",\"description\":"
                           "\"Host to ping (default google.com)\"}"
                           "},\"required\":[\"action\"]}",
      .execute = tool_network_execute,
  };
  register_tool(&net);

  /* Register manage_agent */
  mimi_tool_t ag = {
      .name = "manage_agent",
      .description = "Monitor AI agent health, token usage, tool audit logs, "
                     "and system uptime.",
      .input_schema_json = "{\"type\":\"object\","
                           "\"properties\":{"
                           "\"action\":{\"type\":\"string\",\"enum\":["
                           "\"metrics\",\"audit\",\"stack\",\"uptime\"]}"
                           "},\"required\":[\"action\"]}",
      .execute = tool_agent_execute,
  };
  register_tool(&ag);

  /* Register manage_bluetooth */
  mimi_tool_t bt = {
      .name = "manage_bluetooth",
      .description = "Scan for nearby Bluetooth (BLE) devices, get local "
                     "status, toggle Bluetooth stack, or advertise/broadcast presence.",
      .input_schema_json =
          "{\"type\":\"object\","
          "\"properties\":{"
          "\"action\":{\"type\":\"string\",\"enum\":[\"scan\",\"info\","
          "\"toggle\",\"advertise\"]},"
          "\"duration_ms\":{\"type\":\"number\",\"description\":\"Scan "
          "duration in ms (default 3000)\"},"
          "\"state\":{\"type\":\"string\",\"enum\":[\"on\",\"off\"],"
          "\"description\":\"Target state for 'toggle' or 'advertise' actions\"}"
          "},\"required\":[\"action\"]}",
      .execute = tool_bluetooth_execute,
  };
  register_tool(&bt);

  /* Register led_control */
  mimi_tool_t lc = {
      .name = "led_control",
      .description = "Control the onboard red and green status LEDs. Use this to signal "
                     "success, warnings, or provide visual feedback.",
      .input_schema_json = "{\"type\":\"object\","
                           "\"properties\":{"
                           "\"action\":{\"type\":\"string\",\"enum\":[\"on\",\"off\",\"blink\"],\"description\":\"Action to perform\"},"
                           "\"color\":{\"type\":\"string\",\"enum\":[\"red\",\"green\"],\"description\":\"Target LED color (default red)\"},"
                           "\"ms\":{\"type\":\"integer\",\"description\":\"Blink duration in ms (default 500)\"}"
                           "},\"required\":[\"action\"]}",
      .execute = tool_led_control_execute,
  };
  register_tool(&lc);

  /* Register play_audio (DISABLED per user request)
  mimi_tool_t au = {
      .name = "play_audio",
      .description = "Play a sound effect or voice recording from the local filesystem.",
      .input_schema_json = "{\"type\":\"object\","
                           "\"properties\":{"
                           "\"file\":{\"type\":\"string\",\"description\":\"Path to raw 16kHz mono PCM file (e.g. /spiffs/audio/ping.raw)\"}"
                           "},\"required\":[\"file\"]}",
      .execute = tool_audio_play_execute,
  };
  register_tool(&au);
  */

  build_tools_json();

  ESP_LOGI(TAG, "Tool registry initialized");
  /* Register rule_manager */
  mimi_tool_t rm = {
      .name = "rule_manager",
      .description = "Manage local autonomous rules (subconscious). Rules fire instantly on hardware triggers.",
      .input_schema_json = "{\"type\":\"object\",\"properties\":{"
                      "\"action\":{\"type\":\"string\",\"enum\":[\"add\",\"list\",\"remove\"],\"description\":\"Action to perform\"},"
                      "\"name\":{\"type\":\"string\",\"description\":\"Rule name (for add)\"},"
                      "\"src\":{\"type\":\"string\",\"enum\":[\"temp\",\"hum\",\"batt\"],\"description\":\"Sensor source (for add)\"},"
                      "\"cond\":{\"type\":\"string\",\"enum\":[\"gt\",\"lt\",\"eq\",\"change\"],\"description\":\"Condition (for add)\"},"
                      "\"threshold\":{\"type\":\"number\",\"description\":\"Trigger threshold (for add)\"},"
                      "\"rule_action\":{\"type\":\"string\",\"description\":\"CLI command to run on trigger (e.g. 'color red')\"},"
                      "\"id\":{\"type\":\"string\",\"description\":\"Rule ID (for remove)\"}"
                      "},\"required\":[\"action\"]}",
      .execute = tool_rules_execute,
  };
  register_tool(&rm);

  return ESP_OK;
}

const char *tool_registry_get_tools_json(void) { return s_tools_json; }

esp_err_t tool_registry_execute(const char *name, const char *input_json,
                                char *output, size_t output_size) {
  for (int i = 0; i < s_tool_count; i++) {
    if (strcmp(s_tools[i].name, name) == 0) {
      ESP_LOGI(TAG, "Executing tool: %s", name);
      esp_err_t err = s_tools[i].execute(input_json, output, output_size);

      /* Log to audit buffer */
      agent_metrics_log_tool(name, NULL, err == ESP_OK);

      return err;
    }
  }

  ESP_LOGW(TAG, "Unknown tool: %s", name);
  snprintf(output, output_size, "Error: unknown tool '%s'", name);
  return ESP_ERR_NOT_FOUND;
}
