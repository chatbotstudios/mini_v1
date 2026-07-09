# Sense Tool (Hardware Migration)
The `sense` tool allows you to read the physical environment.

## Usage
- **Command**: `sense`
- **Parameters**: None

## Technical Notes (AMOLED 1.75)
- **STATUS: TEMPORARILY DISABLED**
- The original `QMI8658 IMU` temperature/humidity sensor is not present on the new Waveshare ESP32-S3 AMOLED 1.75 hardware.
- This tool is currently disabled in the registry to prevent I2C bus conflicts with the AMOLED's Touch Controller (CST9217).
- In the future, this tool will be rewritten to interface with the onboard **QMI8658 6-axis IMU** (Gyro/Accel) to give you spatial awareness!
