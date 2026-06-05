#include "llm.h"
#include "secrets.h"
#include <M5Unified.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <stdio.h>

bool llm_complete(const char* transcript, char* response, size_t response_size) {
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%s/v1/chat/completions",
             g_secrets.llm_host, g_secrets.llm_port);

    // Build request JSON
    JsonDocument req;
    req["model"]      = "Qwen3.6-35B-A3B-MLX-8bit";
    req["max_tokens"] = 300;
    JsonArray msgs    = req["messages"].to<JsonArray>();
    JsonObject sys    = msgs.add<JsonObject>();
    sys["role"]       = "system";
    sys["content"]    = "You are a concise voice assistant. Reply in 1-2 short sentences only. No preamble, no thinking steps, no lists. Just the direct answer.";
    JsonObject usr    = msgs.add<JsonObject>();
    usr["role"]       = "user";
    usr["content"]    = transcript;

    String body;
    serializeJson(req, body);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(30000);
    http.addHeader("Content-Type", "application/json");

    int code = http.POST(body);
    if (code != 200) {
        Serial.printf("LLM: HTTP %d — %s\n", code, http.getString().c_str());
        http.end();
        return false;
    }

    String resp = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, resp) != DeserializationError::Ok) {
        Serial.printf("LLM: JSON parse failed: %s\n", resp.c_str());
        return false;
    }

    const char* text = doc["choices"][0]["message"]["content"] | "";
    if (!text[0]) {
        Serial.println("LLM: empty response");
        return false;
    }

    strlcpy(response, text, response_size);
    return true;
}
