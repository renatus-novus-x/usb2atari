[![windows](https://github.com/renatus-novus-x/usb2atari/workflows/windows/badge.svg)](https://github.com/renatus-novus-x/usb2atari/actions?query=workflow%3Awindows)
[![macos](https://github.com/renatus-novus-x/usb2atari/workflows/macos/badge.svg)](https://github.com/renatus-novus-x/usb2atari/actions?query=workflow%3Amacos)
[![ubuntu](https://github.com/renatus-novus-x/usb2atari/workflows/ubuntu/badge.svg)](https://github.com/renatus-novus-x/usb2atari/actions?query=workflow%3Aubuntu)

# usb2atari

USB gamepad -> (PC) -> UART/FT245RL -> ATARI 9-pin controller bridge for MSX/X68000 (compatible joystick port)

This project reads input from USB controllers (or keyboard) on a PC, encodes it as a compact 6-bit digital state, and sends it over UART (via FT245RL) to a small adapter that outputs ATARI 9-pin joystick signals. It is intended to control retro machines that accept ATARI-style controllers.

## Features

- 2 virtual pads supported (Pad1 / Pad2)
- Digital 6-bit state per pad:
  - Up / Down / Left / Right / Button1 / Button2
- Flexible input mapping:
  - Any USB controller recognized by GLFW (gamepad or joystick)
  - Keyboard fallback (always available)
  - Interactive rebinding ("learning mode")
- Real-time visualization (OpenGL fixed pipeline):
  - Two on-screen pads
  - Live highlight of pressed directions/buttons
  - Shows current bindings on screen
- Save/Load mapping to `padmap.txt`
- Designed for FT245RL-based UART bridge to ATARI 9-pin output

## Architecture Overview

PC side:
1. Read controller input using GLFW (`glfwGetGamepadState` if available, otherwise joystick raw)
2. Convert input to 6-bit digital state (per pad)
3. Send state to FT245RL/UART device (planned / WIP)

Adapter side (MCU/logic side, planned / WIP):
1. Receive 6-bit state via UART
2. Drive ATARI 9-pin lines accordingly
3. Output appears as a standard ATARI joystick to MSX/X68000

## Status

- [x] Input capture (GLFW)
- [x] Two-pad digital mapping + rebinding UI
- [x] OpenGL visualization
- [x] Save/Load mappings
- [ ] UART/FT245RL output (WIP)
