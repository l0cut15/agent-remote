#include <M5Unified.h>
#include <WiFi.h>
#include "audio_capture.h"
#include "audio_playback.h"
#include "secrets.h"
#include "stt.h"
#include "llm.h"
#include "tts.h"
#include "display.h"
#include "config.h"

static const uint32_t REC_MS       = 8000;
static const uint32_t IDLE_WAIT_MS = 30000;

// Status bar colors (RGB565)
static const uint32_t COL_IDLE      = 0x0010;  // navy
static const uint32_t COL_RECORDING = 0xA000;  // dark red
static const uint32_t COL_STT       = 0x8400;  // dark orange
static const uint32_t COL_LLM       = 0x4010;  // dark purple
static const uint32_t COL_SPEAKING  = 0x0400;  // dark green

enum State { IDLE, RECORDING, STT_PENDING, LLM_PENDING, SPEAKING };
static State    state    = IDLE;
static uint32_t state_ms = 0;

static char transcript[512];
static char llm_response[1024];

static void wifi_connect() {
    display_status("WIFI", "Connecting...", COL_IDLE);
    display_logf("Joining %s", g_secrets.wifi_ssid);
    Serial.printf("WiFi: connecting to %s", g_secrets.wifi_ssid);
    WiFi.begin(g_secrets.wifi_ssid, g_secrets.wifi_pass);
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 30) {
        delay(500);
        Serial.print(".");
        tries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        String ip = WiFi.localIP().toString();
        Serial.printf("\nWiFi: %s\n", ip.c_str());
        display_logf("IP: %s", ip.c_str());
    } else {
        Serial.println("\nWiFi: FAILED");
        display_log("WiFi FAILED - check secrets");
    }
}

static void enter_idle() {
    state    = IDLE;
    state_ms = millis();
    display_status("READY", "Speak when window opens", COL_IDLE);
}

static void enter_recording() {
    audio_capture_start();
    state    = RECORDING;
    state_ms = millis();
    Serial.println(">>> SPEAK NOW <<<");
    display_status("RECORDING", "Speak now!", COL_RECORDING);
    display_log("Recording...");
}

void setup() {
    M5.begin();
    Serial.begin(115200);
    display_init();

    audio_capture_init();
    audio_playback_init();

    if (!secrets_load()) {
        display_status("ERROR", "No secrets.json", 0x8000);
        while (true) delay(1000);
    }

    wifi_connect();
    Serial.println("Ready.");
    enter_idle();
}

void loop() {
    M5.update();

    switch (state) {
        case IDLE:
            if (millis() - state_ms >= IDLE_WAIT_MS)
                enter_recording();
            break;

        case RECORDING:
            audio_capture_tick();
            if (millis() - state_ms >= REC_MS) {
                audio_capture_stop();
                float secs = (float)audio_capture_length() / (SAMPLE_RATE * sizeof(int16_t));
                Serial.printf("Recorded %.1fs\n", secs);
                display_status("STT", "Transcribing...", COL_STT);
                display_logf("Recorded %.1fs", secs);
                state = STT_PENDING;
            }
            break;

        case STT_PENDING:
            if (stt_transcribe(audio_capture_buffer(), audio_capture_length(),
                               transcript, sizeof(transcript))) {
                Serial.printf("Transcript: \"%s\"\n", transcript);
                display_logf("> %s", transcript);
                display_status("LLM", "Thinking...", COL_LLM);
                state = LLM_PENDING;
            } else {
                display_log("STT failed");
                enter_idle();
            }
            break;

        case LLM_PENDING:
            if (llm_complete(transcript, llm_response, sizeof(llm_response))) {
                Serial.printf("LLM: \"%s\"\n", llm_response);
                display_logf("< %s", llm_response);
                display_status("SPEAKING", "Playing response", COL_SPEAKING);
                if (!tts_speak(llm_response)) {
                    display_log("TTS failed");
                }
                enter_idle();
            } else {
                display_log("LLM failed");
                enter_idle();
            }
            break;

        case SPEAKING:
            enter_idle();
            break;
    }
}
