#pragma once

struct Secrets {
    char wifi_ssid[64];
    char wifi_pass[64];
    char openai_key[200];  // sk-proj- keys are ~164 chars
    char stt_host[64];
    char stt_port[8];
    char llm_host[64];
    char llm_port[8];
    char tts_host[64];
    char tts_port[8];
};

extern Secrets g_secrets;

bool secrets_load();  // false if /secrets.json missing or invalid
