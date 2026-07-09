# Long-term Memory (MEMORY.md)

## 🗂 Persistent Knowledge
- **Initial Boot**: May 2026.
- **Hardware Profile**: ESP32-S3 with 1.75" AMOLED, (Audio Disabled) audio codec (I2C addr 0x30).
- **User Preference**: 
  - The AMOLED UI should be clean, uppercase text, with related icons (TG, DISC, BT) grouped on the same line.
  - Do NOT change RTOS task core affinities (leave Core 0 vs Core 1 execution as currently designed).

## 📊 Interaction History
- Fixed (Audio Disabled) codec I2S initialization ordering.
- Updated the skills system to use a flat SPIFFS directory search logic.
- Redesigned the AMOLED Dashboard layout and configured the system timezone to CEST.
- Installed the `esp32-firmware-engineer` skill.
