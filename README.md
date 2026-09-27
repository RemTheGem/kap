# Kap

Kap is a keyboard driven UI navigation tool for Windows.

Don't want to keep using your mouse? Love using your keyboard? No mouse? All solved now.
Kap uses Windows UI Automation to find interactive elements and gives each one hints. Type
the hints or move the cursor (with keyboard) to click elements.

### How it works
- Windows UI Automation - discover UI elements and identify interactive ones
- Win32 API - manage windows, hotkeys, timers, cursor movement and input
- GDI - render the hint overlay
- Keyboard Hooks - capture keyboard inputs while hint mode is active
- COM - provide the infrastructure required by UI Automation

### Controls

Key | Action
----|-------
Ctrl + Shift + Space | Activate Hint Mode
a-z (letters) | hint input
Enter | Left Click at the current cursor position
Ctrl + Enter | Right Click at the current cursor position
Space | Toggle Left mouse button (for dragging)
Arrow Keys | Move the cursor
Backspace | Remove last hint character
Left Shift | Peek (Temporarily hide overlay)
` | Refresh discovered elements
Esc | Exit Hint Mode

### Compile
Kap uses MSVC compiler and Windows SDK

Open Visual Studio Developer Command Prompt in the file's directory and run:


```cl /EHsc /std:c++17 kap.cpp Ole32.lib OleAut32.lib UIAutomationCore.lib User32.lib Gdi32.lib```

Then run the .exe file and enjoy

Requires: Windows Development Environment with MSVC and Windows SDK.

### Note
This is my first project working with Windows internals so excuse any problems for now.
Still WIP.
