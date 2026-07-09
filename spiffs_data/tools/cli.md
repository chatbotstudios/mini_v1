# CLI Bridge Tool
The `run_cli` tool provides a gateway to the firmware's internal console commands.

## Usage
- **Command**: `run_cli`
- **Parameters**: 
    - `command`: The string to execute (exactly as you would type it in a serial terminal).

## Available Commands
- **Filesystem**: `ls_r` (recursive), `ls_ssd`, `ls_sd`, `cat`, `df`, `file_rm`, `mkdir`, `mv`, `cp`, `touch`, `file_put`, `file_b64`, `file_b64_append`
- **Network**: `wifi_set`, `wifi_status`, `wifi_scan`, `ping`, `ip_info`, `ntp_sync`, `set_proxy`, `clear_proxy`
- **System & Power**: `restart`, `heap_info`, `uptime`, `batt_status`, `pwr_mode`, `deep_sleep`, `log_level`
- **Hardware & Sensors**: `i2c_scan`, `sense_raw`, `amoled_dump`
- **LLM & Config**: `config_show`, `config_reset`, `set_api_key`, `set_model`, `set_provider`, `token_count`, `model_list`, `set_tg_token`, `set_search_key`
- **Memory & Session**: `memory_read`, `memory_write`, `session_list`, `session_clear`, `agent_stack`, `audit_log`
- `help`: Lists all registered commands with their exact syntax.

## Technical Notes
- Output is captured from `stdout` and returned as a JSON string.
- If a command is not found, an error is returned.
- Use this when you need low-level system data that isn't provided by other tools.
