# MIMI AMOLED Dashboard Reference

The MIMI Dashboard is the primary visual interface displayed on the 1.75" AMOLED screen. It is rendered autonomously by the firmware (`hardware/amoled.c`) and provides a persistent, at-a-glance view of the system's vital statistics and connectivity states.

## 📐 Layout Structure

The dashboard is structured into organized sections to maximize readability on the high-density AMOLED display. *(Note: All text rendered on the dashboard is automatically converted to UPPERCASE by the firmware to ensure optimal font alignment and prevent glyph glitching.)*

### 1. Header (Top)
- **MIMI Logo**: A bold, centered header block featuring "MIMI" (scale 2 font) framed within a solid rectangle.

### 2. Network Status (Top Left)
- **Wi-Fi Icon**: Indicates connection presence.
- **SSID**: Displays the name of the connected network (or "OFFLINE" if disconnected).
- **IP Address**: Shows the local IPv4 address, which is crucial for accessing local proxies or WebSockets.

### 3. Power & Environment (Middle Left/Right)
- **Battery Status**: 
  - An adaptive battery icon that visually represents charge level (100%, 75%, 50%, 25%).
  - Text readout of the exact battery percentage and raw voltage reading (e.g., `85% (3.95V)`).
- **QMI8658 IMU Sensor Data**:
  - **Temperature**: A thermometer icon followed by the ambient temperature in Celsius (e.g., `24.5 C`).
  - **Humidity**: A water drop icon followed by the relative humidity percentage (e.g., `45.0 %`).

### 4. System Status
(The "STATUS: ONLINE" and "THINKING..." text elements have been intentionally removed to reduce display clutter).

### 5. Connectivity
- **TG**: Telegram Bot connection status icon (`M` icon).
- **DISC**: Discord Bot connection status icon (`M` icon).
- **BT**: Bluetooth radio status (Bluetooth icon and `ON`/`OFF`). 
All three (TG, DISC, BT) are grouped on a single line for optimal readability.

### 6. Footer (Bottom Center)
- **Clock & Date**: The current local time and date, synchronized via NTP (e.g., `14:30 27.06.26`). If NTP has not synced yet, a default placeholder time is shown.
- **Uptime**: How long the hardware has been running since its last boot or crash, formatted as `DD:HH:MM` (Days:Hours:Minutes).

---

## ⚙️ Refresh Mechanics
To preserve the longevity of the AMOLED screen and avoid annoying full-screen flickering, the display uses **partial updates** for most data changes. 
A **full hardware refresh** (which flashes the screen black and white to clear ghosting) is only triggered once every 10 dashboard updates. 

*You can manually force a full refresh or invert the screen colors using the `amoled_refresh` and `amoled_invert` CLI commands!*
