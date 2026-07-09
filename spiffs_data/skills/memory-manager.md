# Memory Manager

Manage Mimi's long-term memory.

## When to use
When you learn something important about the user, their preferences, or a task you need to remember.

## How to use
1. Summarize the new information.
2. Read the current memory with `read_file path="/spiffs/memory/MEMORY.md"`.
3. Append or update the information using `write_file`.

## 🗂 SPIFFS File System Layout
For context, here is the complete folder and file tree of Mimi's persistent local storage (`/spiffs/`):

- **`/spiffs/audio/`**: Raw audio files.
  - `boot.raw`
  - `siren.raw`
- **`/spiffs/resources/`**: Static resource files and assets.
- **`/spiffs/skills/`**: The core behavioral skill modules (Markdown files).
  - `bluetooth-navigator.md`, `daily-briefing.md`, `amoled.md`, `hardware_maintenance.md`, `led-indicator.md`, `memory-manager.md`, `mimi-cli.md`, `mimi-rules.md`, `network_wizard.md`, `power_manager.md`, `sd-card.md`, `self_diagnostics.md`, `qmi8658.md`, `skill-creator.md`, `weather.md`
- **`/spiffs/tools/`**: Documentation and usage instructions for all registered AI tools.
  - `agent.md`, `bluetooth.md`, `cli.md`, `display.md`, `network.md`, `power.md`, `sense.md`, `storage.md`, `time.md`, `web_search.md`
- **`/spiffs/vault/`**: Secure storage for keys and tokens.
- **`/spiffs/workspace/`**: The dynamic configuration and persona workspace.
  - `AGENT.md` (Operating Rules)
  - `IDENTITY.md` (Identity constraints)
  - `MEMORY.md` (Long-term memory storage)
  - `SOUL.md` (Core persona and hardware architecture)
  - `TOOLS.md` (Tool conventions)
  - `USER.md` (User profile context)
