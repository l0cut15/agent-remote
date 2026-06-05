# AI Agent Voice Control — Architecture

M5Stack Core S3 SE standalone WiFi voice assistant. Touch to speak, fully local pipeline.

---

## Pipeline

```
[Touch screen — bottom zone held]
        │
        ▼
[Record PCM from SPM1423 mic — I2S PDM mode]
[16 kHz, 16-bit mono, stored in PSRAM ring buffer]
        │
        ▼
[HTTP POST → whisper.cpp]          port 7124
[endpoint: /inference]
[body: multipart/form-data, file=WAV]
        │
        ▼
[Transcript text]
        │
        ▼
[HTTP POST → Qwen3.6-35B LLM]     port 7123
[endpoint: /v1/chat/completions]
[chat_template_kwargs: {enable_thinking: false}]
        │
        ▼
[Response text]
        │
        ▼
[HTTP POST → Kokoro TTS]           port 7235
[endpoint: /v1/audio/speech]
[response_format: pcm, 24 kHz 16-bit mono]
[http.useHTTP10(true) — avoids chunked encoding corruption]
        │
        ▼
[PCM buffered in PSRAM → AW88298 amp → Speaker]
```

---

## State Machine

```
IDLE
  │  touch held in bottom zone
  ▼
RECORDING  ── touch released / 8 s timeout ──▶  STT_PENDING
  │  mic active, PCM into PSRAM                       │ whisper.cpp call
  │                                                   ▼
  │                                            LLM_PENDING
  │                                                   │ Qwen3 call
  │                                                   ▼
  │                                            SPEAKING
  │                                                   │ Kokoro TTS + playRaw
  └───────────────────────────────────────────────── IDLE

Any state ── STT/LLM/TTS failure ──▶ show error text ──▶ IDLE
```

---

## Module Structure

```
src/
  main.cpp          — setup(), loop(), 5-state machine, WiFi, touch input
  audio_capture.cpp — SPM1423 PDM mic via M5Unified, PSRAM ring buffer, chunk recording
  audio_capture.h
  audio_playback.cpp — M5Unified Speaker, raw PCM playback
  audio_playback.h
  stt.cpp           — WAV header + multipart POST to whisper.cpp, JSON parse
  stt.h
  llm.cpp           — HTTP POST to Qwen3, think-tag stripping, response parse
  llm.h
  tts.cpp           — HTTP POST to Kokoro, PCM stream into PSRAM, speaker playback
  tts.h
  display.cpp       — 4-zone layout: state bar / AI panel / transcript / touch bar
  display.h
  secrets.cpp       — LittleFS mount, JSON parse into Secrets struct
  secrets.h
  config.h          — SAMPLE_RATE=16000, REC_MAX_BYTES=256*1024
```

---

## Display Layout

Screen: 320 × 240 px

| Zone | Y range | Content |
|------|---------|---------|
| State bar | 0–50 | Pipeline state, coloured background |
| AI panel | 50–175 | LLM response, font size 2, word-wrapped |
| Transcript | 175–205 | STT output, font size 1 |
| Touch zone | 205–240 | "Hold to speak" / "Recording…" bar |

State bar colours (RGB565):

| State | Colour |
|-------|--------|
| READY | `0x0010` navy |
| RECORDING | `0xA000` dark red |
| THINKING (STT) | `0x8400` dark orange |
| THINKING (LLM) | `0x4010` dark purple |
| SPEAKING | `0x0400` dark green |

---

## Key Implementation Details

### I2S Peripheral Sharing
The SPM1423 mic and AW88298 speaker share the I2S peripheral on the Core S3 SE. Hard rule: always call `M5.Speaker.end()` before `M5.Mic.begin()` and `M5.Mic.end()` before `M5.Speaker.begin()`. Failure to do so causes "register I2S object to platform failed" and silence.

### Speaker Stack Size
`dma_buf_len` in `speaker_config_t` controls the speaker task stack: `stack = 1280 + dma_buf_len * 4`. Default `dma_buf_len=256` gives a 2304-byte stack which overflows. Set `dma_buf_len=1024` → 5376-byte stack.

### HTTP/1.0 for TTS
Kokoro returns `Transfer-Encoding: chunked`. `HTTPClient::getStreamPtr()` returns the raw TCP stream without decoding chunk headers, so they land in the PCM buffer as noise. `http.useHTTP10(true)` forces HTTP/1.0 and eliminates chunked encoding.

### Qwen3 Thinking Mode
Qwen3 defaults to chain-of-thought reasoning. Disable it with:
```json
"chat_template_kwargs": {"enable_thinking": false}
```
Additionally, user messages are prefixed with `/no_think` and `strip_think_tags()` removes any `<think>...</think>` blocks that slip through.

### PSRAM Allocation
All large buffers must use PSRAM — internal SRAM is only 512 KB:
```cpp
heap_caps_malloc(size, MALLOC_CAP_SPIRAM)
```
Always check for null. Record buffer: 256 KB. TTS PCM buffer: 600 KB.

### WAV Header for STT
whisper.cpp `/inference` expects a WAV file. Prepend a 44-byte WAV header to raw PCM before POSTing:
```
RIFF chunk → fmt subchunk (PCM, 1 ch, 16000 Hz, 16-bit) → data subchunk
```

---

## Server Configuration

### whisper.cpp (port 7124)

```bash
whisper-server --model models/ggml-small.en.bin --host 0.0.0.0 --port 7124
```

Endpoint used: `POST /inference` (multipart/form-data, `file` field only).

### Qwen3.6-35B-A3B-MLX-8bit (port 7123)

Any OpenAI-compatible server. MLX-LM example:
```bash
mlx_lm.server --model mlx-community/Qwen3.6-35B-A3B-MLX-8bit --host 0.0.0.0 --port 7123
```

Model name sent in requests: `"Qwen3.6-35B-A3B-MLX-8bit"` (exact, case-sensitive).

### Kokoro TTS (port 7235)

See `servers/kokoro-tts/docker-compose.yml`. CPU-only, runs on any x86-64 host.

Voice used: `af_sky`. Output: raw PCM, 24 kHz, 16-bit signed, mono, little-endian.

---

## Memory Budget

| Buffer | Location | Size |
|--------|----------|------|
| Audio record (8 s @ 16 kHz 16-bit) | PSRAM | 256 KB |
| TTS PCM (12 s @ 24 kHz 16-bit) | PSRAM | 600 KB |
| LLM response string | heap | ~1 KB |
| Transcript string | heap | ~512 B |
| **Total PSRAM** | | ~856 KB of 8 MB |

---

## Secrets File

`data/secrets.json` — loaded from LittleFS at boot, never compiled in, gitignored.

```json
{
  "wifi_ssid": "",
  "wifi_pass": "",
  "openai_key": "",
  "stt_host": "10.10.11.x",
  "stt_port": "7124",
  "llm_host": "10.10.11.x",
  "llm_port": "7123",
  "tts_host": "10.10.11.x",
  "tts_port": "7235"
}
```

Note: `openai_key` is present in the struct but not currently used — TTS is fully local.

---

## Future Work

| Feature | Notes |
|---------|-------|
| Wake word | ESP-SR WakeNet, ~200 KB flash, hands-free trigger |
| Multi-turn memory | Keep `messages[]` capped at 6 turns |
| Volume control | Side buttons B/C |
| Battery indicator | M5.Power.getBatteryLevel() |
| Silence detection | Energy threshold on mic input to skip silent recordings |
