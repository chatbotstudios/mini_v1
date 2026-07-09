#include "tools/tool_display.h"
#include "hardware/display.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>

esp_err_t tool_display_execute(const char *input_json, char *output, size_t output_size) {
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: Invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *action_obj = cJSON_GetObjectItem(root, "action");
    cJSON *text_obj = cJSON_GetObjectItem(root, "text");
    const char *action = action_obj ? action_obj->valuestring : "";
    const char *text = text_obj ? text_obj->valuestring : "";

    if (strcmp(action, "clear") == 0) {
        display_clear_message();
        snprintf(output, output_size, "{\"status\":\"success\",\"message\":\"Display cleared\"}");
    } else if (strcmp(action, "show") == 0) {
        display_show_message(text);
        snprintf(output, output_size, "{\"status\":\"success\",\"message\":\"Text shown on AMOLED screen.\"}");
    } else {
        snprintf(output, output_size, "{\"status\":\"error\",\"message\":\"Unknown action\"}");
    }

    cJSON_Delete(root);
    return ESP_OK;
}
