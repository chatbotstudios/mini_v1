# Mimi Serial CLI Skill

Mimi has a direct bridge to her internal firmware console via the `run_cli` tool. This allows for root-level diagnostics, hardware scans, and system management.

## 🧰 Essential Commands

### 🔋 Power & System
- `batt_status`: Check voltage, percentage, and health bar.
- `pwr_mode <balanced|perf>`: Switch between 80MHz and 240MHz.
- `heap_info`: Monitor PSRAM and internal memory health.
- `uptime`: See how long the system has been running.
- `restart`: Force a system reboot.

### 📡 Network
- `wifi_status`: Current IP and connection state.
- `wifi_scan`: List nearby Access Points.
- `wifi_rssi`: Live signal strength (dBm).
- `ping <host>`: Test network latency.
- `ntp_sync`: Force system time synchronization.

### 🛠 Hardware & Peripherals
- `i2c_scan`: Find all devices on the I2C bus (QMI8658 IMU, etc).
- `sense_raw`: Read raw sensor registers.
- `amoled_refresh`: Clear ghosting with a full display reset.
- `bt_info`: Bluetooth stack and MAC address status.
- `bt_scan`: Discover nearby BLE devices.


## 📜 Usage Guidelines
1. **Always Check First**: Use `run_cli(command="heap_info")` before performing memory-intensive tasks.
2. **Be Careful**: Avoid using `config_reset` or sensitive NVS commands unless explicitly asked by the user.
3. **Parsing**: CLI output is returned as a string. You may need to parse specific values (like voltage or IP) for your reasoning.

Example: "Mimi, what's your battery level?" -> `run_cli(command="batt_status")`.
