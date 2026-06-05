# AI Agent Voice Control — M5Stack Core S3 SE

Architecture and implementation plan for a standalone WiFi-connected AI agent voice controller running on the M5Stack Core S3 SE.

---

## Hardware Overview

| Feature | M5Stack Core S3 SE spec |
|---------|------------------------|
| MCU | ESP32-S3FN8 (dual-core LX7, 240 MHz) |
| RAM | 512 KB SRAM + 8 MB PSRAM |
| Flash | 16 MB |
| Display | 2.0" IPS LCD, 320×240, capacitive touch |
| Microphone | SPM1423 PDM mic (I2S) |
| Speaker | 1 W via AW88298 class-D amp (I2C + I2S) |
| Connectivity | WiFi 802.11 b/g/n, BLE 5.0 |
| Battery | 900 mAh Li-Po |
| Buttons | A (front), B/C (side), Power |

The 8 MB PSRAM is essential — it's what makes buffering ~5 seconds of 16 kHz audio (≈160 KB) and an MP3 response comfortable without fighting the heap.

---

## Pipeline

```
[Button A / Wake Word]
        │
        ▼
[Record PCM from SPM1423 mic via I2S]
[store in PSRAM — 16 kHz, 16-bit mono]
        │
        ▼
[HTTP POST → local whisper.cpp server]
        │  /v1/audio/transcriptions
        ▼
[Transcript text]
        │
        ▼
[HTTP POST → local Qwen LLM]
        │  /v1/chat/completions (OpenAI-compat)
        ▼
[Response text]
        │
        ▼
[HTTPS POST → OpenAI TTS API]
        │  audio/speech → mp3
        ▼
[Decode MP3 → PCM via I2S → AW88298 → Speaker]
```

STT and LLM are local (plain HTTP, no key). Only TTS goes to OpenAI over HTTPS.

---

## State Machine

```
IDLE
  │  button press (or wake word detected)
  ▼
LISTENING  ── button released / silence timeout ──▶ TRANSCRIBING
  │  (recording audio into PSRAM ring buffer)          │
  │                                                     ▼
  │                                              STT_PENDING
  │                                                     │ transcript received
  │                                                     ▼
  │                                              LLM_PENDING
  │                                                     │ response received
  │                                                     ▼
  │                                              TTS_PENDING
  │                                                     │ audio received
  │                                                     ▼
  │                                               SPEAKING
  │                                                     │ playback complete
  └─────────────────────────────────────────────────── IDLE

Any state ── network error / API error ──▶ ERROR (show message, auto-return to IDLE)
```

---

## Module Structure

```
src/
  main.cpp          — setup(), loop(), state machine, WiFi init
  audio_capture.cpp — I2S PDM config, start/stop recording, ring buffer in PSRAM
  audio_capture.h
  audio_playback.cpp — I2S DAC config, AW88298 amp control (I2C), MP3 decode + stream
  audio_playback.h
  stt.cpp           — multipart HTTP POST to local whisper.cpp, parse JSON transcript
  stt.h
  llm.cpp           — HTTP POST to local Qwen (OpenAI-compat), parse response text
  llm.h
  tts.cpp           — HTTPS POST to OpenAI TTS, receive MP3 bytes
  tts.h
  display.cpp       — status screen, waveform animation, text scroll
  display.h
  config.h          — constants (SAMPLE_RATE, REC_MAX_BYTES); secrets loaded from LittleFS
```

Secrets are never compiled in. They live in `data/secrets.json` on LittleFS, loaded at boot. OTA firmware updates don't wipe credentials.

---

## Key Libraries (PlatformIO)

```ini
[env:m5stack-cores3]
platform = espressif32
board = m5stack-cores3
framework = arduino
board_build.filesystem = littlefs
board_build.partitions = default_16MB.csv
board_build.f_cpu = 240000000
monitor_speed = 115200

lib_deps =
    m5stack/M5Unified @ ^0.2.4        ; hardware abstraction (screen, IMU, power)
    m5stack/M5GFX @ ^0.2.4            ; display rendering
    bblanchon/ArduinoJson @ ^7.0.0    ; JSON parse/build
    earlephilhower/ESP8266Audio @ ^1.9.7  ; MP3 decode + I2S output
```

`M5Unified` handles the AW88298 speaker amp initialisation so you don't need to bit-bang its I2C registers manually.

---

## Audio Capture Detail

The SPM1423 is a PDM microphone connected via I2S in PDM mode.

```cpp
// Typical I2S PDM config for SPM1423 on Core S3 SE
i2s_config_t mic_cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_PCM_SHORT,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512,
};
// CLK pin: GPIO 0, DATA pin: GPIO 34 (verify against M5Unified pin map)
```

Record into a `uint8_t*` buffer allocated with `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` — PSRAM, not internal RAM. Cap recording at ~8 seconds (256 KB) to stay well inside PSRAM budget.

---

## API Calls

### 1. Speech-to-Text — local whisper.cpp

```
POST http://<stt_host>:<stt_port>/v1/audio/transcriptions
Content-Type: multipart/form-data

file=<WAV bytes>
model=whisper-1
language=en
response_format=json
```

Send the PCM buffer as a WAV file (prepend a 44-byte WAV header to the raw PCM before POSTing). Use plain `HTTPClient` — no TLS needed for local server.

Response:
```json
{ "text": "What's the weather like today?" }
```

### 2. LLM — local Qwen (OpenAI-compatible)

```
POST http://<llm_host>:<llm_port>/v1/chat/completions
Content-Type: application/json

{
  "model": "qwen",
  "max_tokens": 300,
  "messages": [
    { "role": "system", "content": "You are a concise voice assistant. Keep responses under 3 sentences." },
    { "role": "user", "content": "<transcript>" }
  ]
}
```

Response → `choices[0].message.content`. Use plain `HTTPClient` — no TLS needed for local server.

Keep `max_tokens` low (200–400) — long responses produce large TTS audio and feel bad on a speaker.

For multi-turn conversation, maintain a small `messages[]` array in RAM (cap at ~6 turns to avoid memory pressure).

### 3. Text-to-Speech — OpenAI TTS

```
POST https://api.openai.com/v1/audio/speech
Authorization: Bearer <openai_key>
Content-Type: application/json

{
  "model": "tts-1",          (tts-1-hd is higher quality, ~2× slower)
  "voice": "nova",           (alloy/echo/fable/onyx/nova/shimmer)
  "input": "<response text>",
  "response_format": "mp3"
}
```

Stream the MP3 bytes directly into the audio decoder rather than buffering the whole file — saves ~50–100 KB RAM.

---

## Display

Use `M5GFX` / `M5Canvas` for smooth double-buffered drawing.

| State | Display |
|-------|---------|
| IDLE | Waveform flatline, "Hold A to speak" |
| LISTENING | Live amplitude waveform bar |
| TRANSCRIBING | Spinner + "Listening…" |
| LLM_PENDING | Spinner + "Thinking…" |
| TTS_PENDING | Spinner + "Preparing…" |
| SPEAKING | Scrolling response text + animated waveform |
| ERROR | Red banner + error message |

---

## Secrets File (data/secrets.json)

```json
{
  "wifi_ssid": "YourNetwork",
  "wifi_pass": "YourPassword",
  "openai_key": "sk-...",
  "stt_host": "10.10.11.111",
  "stt_port": "7124",
  "llm_host": "10.10.11.111",
  "llm_port": "7123"
}
```

Add `data/secrets.json` to `.gitignore`. Provide a `data/secrets.example.json` with empty values (already done).

---

## Memory Budget

| Item | Size |
|------|------|
| Audio record buffer (8 s @ 16 kHz 16-bit) | ~256 KB PSRAM |
| MP3 response buffer | ~64–128 KB PSRAM |
| Conversation history (6 turns) | ~10 KB heap |
| TLS session (WiFiClientSecure, TTS only) | ~40 KB heap |
| M5GFX canvas (320×240×2) | ~150 KB heap |
| **Total** | well within 8 MB PSRAM + 512 KB SRAM |

---

## Build Phases

### Phase 1 — Audio I/O only
Get mic recording and speaker playback working in isolation. Record 3 seconds on button press, play it back immediately (loopback test). Confirms I2S pin assignments are correct before involving network.

### Phase 2 — WiFi + STT
Connect to WiFi, POST a hardcoded WAV clip to local whisper.cpp, print transcript to Serial. Confirms HTTP + local server connection works.

### Phase 3 — Full pipeline (no display)
Wire all three calls together: Button → record → whisper.cpp → Qwen → OpenAI TTS → speaker. Status goes to Serial only.

### Phase 4 — Display + state machine
Add the UI. Transition through states with appropriate screen content.

### Phase 5 — Polish
- Wake word (ESP-SR `AFE` + `WakeNet`) so hands-free works
- Conversation memory (multi-turn `messages[]`)
- Volume control (side buttons)
- Battery indicator

---

## Optional: Wake Word (Phase 5)

Espressif's `esp-sr` library runs entirely on-device:

```
ESP-SR AFE (Acoustic Front End)
  → noise suppression + echo cancellation
  → WakeNet (custom phrase or built-in English keyword)
      → triggers LISTENING state
```

Wake word models are ~200 KB flash. Custom wake words require Espressif's model training service or you can use one of the built-in English phrases.

---

## Server Setup (required before Phase 2)

whisper.cpp must be running on the Mac at the configured `stt_host` before testing STT:

```bash
brew install whisper-cpp
whisper-server --model models/ggml-small.en.bin --host 0.0.0.0 --port 7124
```

The Qwen model must be running at `llm_host:llm_port` with an OpenAI-compatible `/v1/chat/completions` endpoint.
