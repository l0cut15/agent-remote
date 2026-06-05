# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Standalone WiFi AI agent voice controller running on the **M5Stack Core S3 SE** (ESP32-S3). Push-to-talk → local whisper.cpp STT → local Qwen LLM → OpenAI TTS → speaker. Built with PlatformIO + Arduino framework.

See `AGENT_VOICE_CONTROL_ARCHITECTURE.md` for the full design spec.

## Dev Environment

**VS Code + PlatformIO IDE extension** is the required setup. The extension installs the `pio` CLI and provides IntelliSense for Arduino/ESP32 headers. Without it, the LSP reports false errors for `M5Unified.h`, `Serial`, `M5`, etc. — those are not real.

```bash
pio run                      # compile
pio run -v                   # compile verbose (use to diagnose hangs/errors)
pio run --target upload      # compile + flash over USB
pio run --target uploadfs    # flash LittleFS (data/ directory)
pio device monitor           # serial monitor at 115200 baud
```

Flash order on first setup: `upload` then `uploadfs`. OTA updates only need `upload` — secrets survive in LittleFS.

## Current Status

**Phase 1 code is written but not yet tested on hardware.** The next step is to flash and confirm the mic→speaker loopback works before moving to Phase 2.

### What exists

```
platformio.ini              — board config, lib_deps
.gitignore                  — excludes data/secrets.json, .pio/
data/secrets.example.json   — template; copy to secrets.json and fill in
src/config.h                — SAMPLE_RATE, REC_MAX_BYTES constants (no LittleFS loader yet)
src/audio_capture.h/.cpp    — I2S PDM mic via M5Unified, PSRAM ring buffer, chunk-based recording
src/audio_playback.h/.cpp   — M5Unified Speaker, raw PCM playback
src/main.cpp                — Phase 1 state machine: IDLE → RECORDING → PLAYING
```

### What is planned (Phase 2+)

```
src/stt.cpp/.h      — multipart HTTP POST to whisper.cpp, WAV header, JSON parse
src/llm.cpp/.h      — HTTP POST to local Qwen, response parse
src/tts.cpp/.h      — HTTPS POST to OpenAI TTS, stream MP3 to decoder
src/display.cpp/.h  — M5GFX double-buffered canvas, state-driven UI
```

`config.h` also needs a LittleFS secrets loader added in Phase 2 before any of the network modules are wired up.

## Secrets

`data/secrets.json` — loaded from LittleFS at boot, never compiled in, gitignored.

```json
{
  "wifi_ssid": "",
  "wifi_pass": "",
  "openai_key": "sk-...",
  "stt_host": "10.10.11.111",
  "stt_port": "7124",
  "llm_host": "10.10.11.111",
  "llm_port": "7123"
}
```

`openai_key` is only used for TTS. STT and LLM are local — plain HTTP, no key.

Copy `data/secrets.example.json` → `data/secrets.json` and fill in before flashing.

## Architecture

### Pipeline

Button A press → record PCM into PSRAM → POST WAV to local whisper.cpp → transcript → POST to local Qwen model → response text → POST to OpenAI TTS → stream MP3 → speaker

### State Machine (Phase 4 — full)

```
IDLE → LISTENING → STT_PENDING → LLM_PENDING → TTS_PENDING → SPEAKING → IDLE
                                              (any state) → ERROR → IDLE
```

Phase 1 uses a simplified version: `IDLE → RECORDING → PLAYING → IDLE`

### Key Constraints

- **PSRAM for audio buffers** — always `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` for the record buffer (~256 KB) and MP3 buffer (~128 KB). Internal SRAM is only 512 KB total. Check for null after allocation.
- **M5Unified handles audio hardware** — use `M5.Mic` for recording and `M5.Speaker` for PCM playback. No manual I2S register writes needed. `M5.Speaker.setVolume(200)` is a good default.
- **Chunk-based recording** — `audio_capture_tick()` records 256 samples (~16 ms) per call and returns false when the buffer is full. Call once per loop iteration while Button A is held.
- **STT and LLM are plain HTTP** — use `HTTPClient`, not `WiFiClientSecure`. Only OpenAI TTS uses HTTPS.
  - STT: `http://10.10.11.111:7124/v1/audio/transcriptions` (whisper.cpp, OpenAI-compat)
  - LLM: `http://10.10.11.111:7123/v1/chat/completions` (Qwen3.6-35B-A3B-MLX-8bit, OpenAI-compat)
- **WAV header** — prepend a 44-byte WAV header to raw PCM before POSTing to whisper.cpp.
- **Stream TTS MP3** — pipe bytes directly into the ESP8266Audio decoder; don't buffer the full response.
- **Multi-turn cap** — keep `messages[]` to ≤6 turns to avoid memory pressure.
- **max_tokens 200–400** — long responses produce large TTS audio and feel bad on a speaker.

### PlatformIO Environment

```ini
[env:m5stack-cores3]
platform = espressif32
board = m5stack-cores3
framework = arduino
board_build.filesystem = littlefs
board_build.partitions = default_16MB.csv
board_build.f_cpu = 240000000

lib_deps =
    m5stack/M5Unified @ ^0.2.4
    m5stack/M5GFX @ ^0.2.4
    bblanchon/ArduinoJson @ ^7.0.0
    earlephilhower/ESP8266Audio @ ^1.9.7
```

Note: `board_build.f_cpu` must be a plain integer — the `L` suffix is invalid in INI files and causes a config parse error.

## Build Phases

| Phase | Status | Description |
|-------|--------|-------------|
| 1 | **Written, untested** | Audio loopback — Button A → mic → PSRAM → speaker |
| 2 | Not started | WiFi + local whisper.cpp STT (print transcript to Serial) |
| 3 | Not started | Full pipeline — STT → Qwen → TTS, Serial only |
| 4 | Not started | Display + full state machine |
| 5 | Deferred | Wake word (ESP-SR WakeNet), volume control, battery indicator |

## Server Setup (required before Phase 2)

whisper.cpp must be running on the Mac at `10.10.11.111` before testing STT:

```bash
brew install whisper-cpp
whisper-server --model models/ggml-small.en.bin --host 0.0.0.0 --port 7124
```

The Qwen model is already running on the same machine at port 7123.
