# MimiClaw Gemini: Tools & Skills Guide

MimiClaw operates on a **two-tier intelligence architecture**. To understand how Mimi interacts with the real world, you must understand the relationship between **Tools** (The Body) and **Skills** (The Mind).

By combining low-level C-tools with highly flexible Markdown Skills, you get an autonomous agent that can **evolve its own software** without ever needing to be re-flashed!

---

## 🧰 1. TOOLS (The Body / Low-Level Firmware)

Tools are hardcoded C-functions built directly into the ESP32-S3 firmware. They are registered inside the internal tool registry and exposed to the AI model as callable API endpoints. These are the physical "muscles" Mimi uses to interact with the hardware and the internet.

When the AI decides to use a tool, it outputs a JSON function call. The ESP32 parses this, executes the C-code, and feeds the raw text result back to the AI.

### Hardware Control Tools
| Tool Name | Description | Target Hardware |
| :--- | :--- | :--- |
| `sense` | Queries the I2C sensor and returns precise ambient temperature and humidity. | QMI8658 IMU Sensor |
| `display_control` | Allows Mimi to take over the AMOLED display, scale fonts, draw 16x16 icons, or force an anti-ghosting refresh. | 1.75" AMOLED |
| `led_control` | Gives Mimi direct control over the Green (Health) and Red (Processing) LEDs to visually signal her status. | GPIO 1 / GPIO 3 |
| `play_audio` | Accesses the (Audio Disabled) to stream raw PCM audio files from the SSD directly to the speaker. | (Audio Disabled) DAC |
| `manage_power` | Toggles the CPU between Performance (240MHz) and Balanced (160/80MHz) modes to save battery, and reads the internal ADC voltage. | ESP32-S3 PMU |
| `manage_bluetooth` | Allows Mimi to scan the area for nearby Bluetooth beacons (phones, trackers) or advertise herself. | NimBLE Stack |

### System & Cloud Tools
| Tool Name | Description | Target |
| :--- | :--- | :--- |
| `run_cli` | A massive privilege. Allows Mimi to run raw internal firmware console commands (like `i2c_scan`, `heap_info`) exactly as if typed into the USB serial port. | Serial Console |
| `filesystem` | Atomic operations (`read_file`, `write_file`, `list_dir`, `delete_file`, `edit_file`) for traversing the internal SPIFFS and external SD card. | SPIFFS / FAT32 |
| `web_search` | Hits an external API to scrape live data from the internet. | Tavily API |
| `get_current_time` | Checks the synchronized network time. | SNTP |
| `manage_agent` | Audits token usage, tool execution logs, and real-time PSRAM heap health. | System Metrics |
| `manage_network` | Performs WiFi pings and network scans. | ESP-WiFi / LWIP |

---

## 🧠 2. SKILLS (The Mind / High-Level Logic)

While Tools are written in `C` and require compiling, **Skills are written in Markdown (`.md`)**. 

Skills live in the `/spiffs/skills/` directory on the internal SSD. They act as "System Prompts on Demand." When the user asks a question, the ESP32 checks if any skill files match the context of the conversation and appends that Markdown file to the AI's instructions.

This means **you don't need to recompile the code to teach Mimi new tricks!** You (or Mimi herself) can just write a text file to the SSD.

### Installed Core Skills
| Skill File | Purpose |
| :--- | :--- |
| `weather.md` | Teaches Mimi to combine the `sense` tool (local temp) with the `web_search` tool (global forecast) to provide hyper-localized weather advice. |
| `skill-creator.md` | The ultimate self-evolution module. Teaches Mimi how to use the `write_file` tool to generate brand new `.md` skills and save them to her own SSD. |
| `memory-manager.md` | Instructs Mimi on how to read long conversation logs and compress them into concise bullet points inside a `MEMORY.md` file, preserving tokens. |
| `daily-briefing.md` | Defines a workflow where Mimi checks the time, reads the battery voltage, checks the weather, and outputs a formatted "Good Morning" message. |
| `amoled.md` | The "UI Guidelines". Tells Mimi exactly what coordinates, fonts, and limits she must obey if she decides to draw custom UI using `display_control`. |
| `bluetooth-navigator.md` | Instructs Mimi on how to interpret nearby BLE beacons to determine if specific people or devices are nearby. |

---

## 🔄 How They Work Together (User Examples)

The true power of MimiClaw is unleashed when Skills orchestrate Tools. Here are examples of how the execution loop handles user requests:

### Example 1: The Environmental Check
**User Request:** *"Mimi, is it going to rain today? Should I turn on the AC inside?"*

**The ReAct Loop:**
1. **Context Load:** The ESP32 loads `weather.md` from the SSD and feeds it to the AI.
2. **Tool Call 1:** AI calls `sense{}`. The ESP32 physically pings the QMI8658 IMU and returns `Temp: 30.5C, Hum: 33%`.
3. **Tool Call 2:** AI calls `web_search{"query": "weather forecast today"}`. The ESP32 returns *"Clear skies, no rain."*
4. **Tool Call 3:** AI realizes it's hot inside and wants to show she's thinking. It calls `led_control{"color": "red", "state": "blink"}`.
5. **Response:** Finally, the AI formulates the answer: *"It's quite hot inside at 30.5°C, but there's no rain expected outside today. Turning on the AC is recommended!"*

### Example 2: Self-Modification (Skill Creator)
**User Request:** *"Mimi, I want you to learn a new skill called 'Crypto Tracker'. When I ask for crypto prices, search the web and then display the top coin on the AMOLED screen."*

**The ReAct Loop:**
1. **Context Load:** The ESP32 loads `skill-creator.md` and `amoled.md`.
2. **Tool Call 1:** AI calls `write_file{"path": "/spiffs/skills/crypto-tracker.md", "content": "..."}`.
3. **Result:** The C-driver writes the new Markdown file to the physical SSD. 
4. **Response:** *"I've successfully created the Crypto Tracker skill! You can now ask me for prices anytime."*
5. *(Next time the user asks for crypto, the ESP32 will automatically load `crypto-tracker.md` into context!)*

### Example 3: Diagnostic Root Access
**User Request:** *"Mimi, why is my SD card not working? Check the system logs."*

**The ReAct Loop:**
1. **Tool Call 1:** AI calls `run_cli{"command": "sd_info"}`. 
2. **Result:** The ESP32 executes the internal firmware console command and returns the raw terminal output indicating a mounting failure or absence of the card.
3. **Response:** *"It looks like the SD card is currently not detected by the hardware. Please verify that it is properly inserted into the slot and try rebooting."*
