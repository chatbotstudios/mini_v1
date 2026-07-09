# Mimi Local Rule Engine (Subconscious)

Mimi possesses a "Local Subconscious" that allows her to monitor hardware sensors and trigger actions instantly, even when offline. These rules are persistent and stored in her internal SSD.

## 🛠 Rule Structure
Rules consist of a **Source**, a **Condition**, a **Threshold**, and an **Action**.

### 📡 Triggers (Sources)
- `temp`: Temperature from QMI8658 IMU (°C).
- `hum`: Humidity from QMI8658 IMU (%).
- `batt`: Battery Voltage (V).
- `uptime`: Seconds since boot.

### ⚖️ Conditions
- `gt`: Greater than.
- `lt`: Less than.
- `eq`: Equal to.
- `change`: Fires whenever the value changes.

### 🎬 Actions
Actions are CLI commands. Examples:
- `color red`: Set LED to Red.
- `color orange -t 10`: Temporary alert.
- `amoled_refresh`: Clear the screen.
- `restart`: Force a reboot.

## 📜 Usage Patterns

### 🔋 Power Management
If Mimi's battery is low, she should protect herself:
- `rule_manager(action="add", name="Low Battery", src="batt", cond="lt", threshold=3.4, rule_action="color orange")`

### 🌡 Environmental Safety
Alert the user to extreme conditions:
- `rule_manager(action="add", name="Overheat", src="temp", cond="gt", threshold=40, rule_action="color red")`

## 📋 Best Practices
1. **Name Your Rules**: Use descriptive names like "Battery Alert" or "Morning Refresh".
2. **Persistence**: Rules are saved to `/spiffs/rules.json`. You only need to add them once.
3. **Throttling**: Rules use edge-triggering—they fire only when the threshold is *crossed*.
