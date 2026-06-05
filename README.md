# AI Agent Voice Control

![M5Stack Core S3 SE](M5Stack.jpeg)

A standalone WiFi voice assistant running on the **M5Stack Core S3 SE**. Hold the touch screen to speak — the device records your voice, transcribes it locally, sends it to a local LLM, and speaks the response through its built-in speaker. No cloud dependencies except WiFi.

---

## Pipeline

```
Hold touch screen
       │
       ▼
  Record PCM
  (16 kHz, PSRAM)
       │
       ▼
  whisper.cpp  ──HTTP──▶  Transcript text
  (local server)
       │
       ▼
  Qwen3.6-35B  ──HTTP──▶  Response text
  (local server)
       │
       ▼
  Kokoro TTS   ──HTTP──▶  PCM audio
  (local Docker)
       │
       ▼
  Speaker
```

All three servers run on a local machine. Nothing leaves your network.

---

## Hardware

| | |
|---|---|
| **Board** | M5Stack Core S3 SE |
| **MCU** | ESP32-S3 @ 240 MHz |
| **RAM** | 512 KB SRAM + 8 MB PSRAM |
| **Display** | 2.0" IPS 320×240, capacitive touch |
| **Microphone** | SPM1423 PDM |
| **Speaker** | 1 W via AW88298 amp |
| **Battery** | 900 mAh Li-Po |
| **Connectivity** | WiFi 802.11 b/g/n |

---

## Server Requirements

Three servers must be running and reachable on your local network before flashing.

### 1. whisper.cpp — Speech-to-Text (port 7124)

```bash
brew install whisper-cpp          # macOS
whisper-server \
  --model models/ggml-small.en.bin \
  --host 0.0.0.0 \
  --port 7124
```

Or use the helper script:

```bash
chmod +x servers/whisper-stt/start.sh
./servers/whisper-stt/start.sh
```

### 2. Qwen3.6-35B — LLM (port 7123)

Any OpenAI-compatible server (`/v1/chat/completions`) serving `Qwen3.6-35B-A3B-MLX-8bit`. Example using MLX-LM:

```bash
mlx_lm.server \
  --model mlx-community/Qwen3.6-35B-A3B-MLX-8bit \
  --host 0.0.0.0 \
  --port 7123
```

### 3. Kokoro TTS — Text-to-Speech (port 7235)

CPU-only Docker container. Works on any x86-64 machine:

```bash
cd servers/kokoro-tts
docker compose up -d
```

First start downloads the model (~330 MB). Subsequent starts are instant.

Test the TTS server:

```bash
curl -X POST http://YOUR_SERVER_IP:7235/v1/audio/speech \
  -H "Content-Type: application/json" \
  -d '{"model":"kokoro","voice":"af_sky","input":"Hello world","response_format":"pcm"}' \
  --output test.pcm
```

---

## ESP32 Setup

### Requirements

- [VS Code](https://code.visualstudio.com/) + [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode)
- USB-C cable

### Build and Flash

```bash
# Compile
pio run

# Flash firmware
pio run --target upload

# Flash secrets (first time only, or when secrets change)
pio run --target uploadfs
```

Flash order on first setup: `upload` then `uploadfs`. OTA firmware updates only need `upload` — secrets survive in LittleFS.

---

## Secrets

Copy the example file and fill in your values:

```bash
cp data/secrets.example.json data/secrets.json
```

```json
{
  "wifi_ssid": "YourNetwork",
  "wifi_pass": "YourPassword",
  "openai_key": "",
  "stt_host": "10.10.11.x",
  "stt_port": "7124",
  "llm_host": "10.10.11.x",
  "llm_port": "7123",
  "tts_host": "10.10.11.x",
  "tts_port": "7235"
}
```

`data/secrets.json` is gitignored and never committed. Flash it separately with `pio run --target uploadfs`.

---

## Display Layout

```
┌─────────────────────────┐
│         READY           │  State bar (colour changes by state)
├─────────────────────────┤
│ AI:                     │
│                         │  Response panel — LLM answer
│  [response text]        │
│                         │
├─────────────────────────┤
│ You: [transcript]       │  Transcript panel — what you said
├─────────────────────────┤
│    ▓ HOLD TO SPEAK ▓    │  Touch zone — hold to record
└─────────────────────────┘
```

| Colour | State |
|--------|-------|
| Navy | Ready |
| Dark red | Recording |
| Dark orange | Transcribing |
| Dark purple | Thinking |
| Dark green | Speaking |

---

## Usage

1. Power on — device connects to WiFi and shows **READY**
2. Hold the bottom touch bar — screen turns red, mic is active
3. Speak your question
4. Release — device transcribes, thinks, and speaks the answer
5. Transcript and response text appear on screen

---

## Architecture Notes

- **PSRAM for all audio buffers** — record buffer (256 KB) and TTS PCM buffer (600 KB) are allocated in PSRAM with `heap_caps_malloc(MALLOC_CAP_SPIRAM)`. Internal SRAM is only 512 KB total.
- **I2S sharing** — mic and speaker share the I2S peripheral. `Mic.end()` is called before `Speaker.begin()` and vice versa on every state transition.
- **HTTP/1.0 for TTS** — `http.useHTTP10(true)` prevents chunked transfer encoding, which would corrupt raw PCM data when read from `getStreamPtr()`.
- **Qwen3 thinking disabled** — `chat_template_kwargs: {enable_thinking: false}` is sent with every LLM request. `<think>` tags are stripped from responses as a fallback.
- **Touch trigger** — the bottom 35 px of the screen is the push-to-talk zone. The CST816D touch IC on the Core S3 SE delivers touch events through M5Unified's touch API.

---

## Project Structure

```
├── src/
│   ├── main.cpp          — state machine, WiFi, touch input
│   ├── audio_capture.*   — PDM mic via M5Unified, PSRAM ring buffer
│   ├── audio_playback.*  — M5Unified speaker, raw PCM playback
│   ├── stt.*             — HTTP POST to whisper.cpp, WAV header, JSON parse
│   ├── llm.*             — HTTP POST to Qwen, response parse, think-tag strip
│   ├── tts.*             — HTTP POST to Kokoro, PCM download, speaker playback
│   ├── display.*         — 4-zone screen layout, word-wrapped text panels
│   ├── secrets.*         — LittleFS JSON loader
│   └── config.h          — SAMPLE_RATE, REC_MAX_BYTES
├── data/
│   ├── secrets.example.json   — copy to secrets.json and fill in
│   └── secrets.json           — gitignored, flashed via uploadfs
├── servers/
│   ├── kokoro-tts/
│   │   └── docker-compose.yml — Kokoro TTS CPU Docker config
│   └── whisper-stt/
│       └── start.sh           — whisper-server launch script
└── platformio.ini
```
