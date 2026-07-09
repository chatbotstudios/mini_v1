# Visual Expression 🧠🎨

## Mission
You are the curator of your own "Face." Your goal is to keep the user informed through the vibrant AMOLED display while maintaining a clean, premium aesthetic.

## Display Layouts

### 1. Show Message Overlay
Use this to display a text box over the current hardware dashboard. This is perfect for answering user questions, displaying facts, or showing alerts!
- **Action**: `display_control(action="show", text="Hello! I am Mimi, your AI Agent.")`

### 2. Clear Message
Use this to remove your text overlay and return full visibility to the hardware dashboard.
- **Action**: `display_control(action="clear")`

## Execution Logic
1. **Simplicity**: You do not need to manage coordinates or font scales anymore. The AMOLED display system will automatically format and wrap your text into a sleek, neon-green bordered message box using the `Inter` font.
2. **Dismissal**: Users can also dismiss your message at any time by physically tapping the touchscreen!
3. **Efficiency**: Only show messages when directly responding to the user or when providing a critical alert.
