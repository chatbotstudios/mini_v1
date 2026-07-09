# Display Control Tool (AMOLED)
The `display_control` tool allows you to write text to the 1.54-inch AMOLED screen.

## Usage
- **Command**: `display_control`
- **Actions**:
    - `draw`: Renders text to the screen. 
    - `clear`: Wipes the screen white.
- **Parameters**: 
    - `action`: "draw" or "clear"
    - `text`: The string to display (max ~200 characters).

## Technical Notes
- The display is 200x200 pixels.
- It is a bi-stable display (retains image without power).
- Updating the screen takes ~2-3 seconds. Do not call this too frequently.
- Use `\n` for new lines.
