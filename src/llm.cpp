#include "llm.h"
#include "secrets.h"
#include <M5Unified.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <string.h>

static const char* SYSTEM_PROMPT =
    "You are a voice assistant built into a handheld device. "
    "Your replies are spoken aloud through a speaker — not displayed as text. "
    "Rules: reply in 1 to 2 short spoken sentences only. "
    "No bullet points, no lists, no markdown. "
    "No filler phrases like Sure or Great question. "
    "No preamble, no thinking steps, no reasoning. "
    "Use natural conversational language with contractions. "
    "If you do not know something, say so in one sentence.";

// Strip <think>...</think> blocks Qwen3 sometimes emits despite instructions.
static void strip_think_tags(char* buf, size_t buf_size) {
    const char* src = buf;
    char tmp[1024];
    size_t out = 0;

    while (*src && out < buf_size - 1) {
        if (strncmp(src, "<think>", 7) == 0) {
            const char* end = strstr(src, "</think>");
            if (end) { src = end + 8; continue; }
        }
        tmp[out++] = *src++;
    }
    tmp[out] = '\0';

    // Trim leading whitespace left after stripping
    const char* trimmed = tmp;
    while (*trimmed == ' ' || *trimmed == '\n' || *trimmed == '\r') trimmed++;
    strlcpy(buf, trimmed, buf_size);
}

bool llm_complete(const char* transcript, char* response, size_t response_size) {
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%s/v1/chat/completions",
             g_secrets.llm_host, g_secrets.llm_port);

    JsonDocument req;
    req["model"]      = "Qwen3.6-35B-A3B-MLX-8bit";
    req["max_tokens"] = 150;
    // Disable Qwen3 thinking mode at the chat template level
    req["chat_template_kwargs"]["enable_thinking"] = false;
    JsonArray msgs = req["messages"].to<JsonArray>();
    JsonObject sys = msgs.add<JsonObject>();
    sys["role"]    = "system";
    sys["content"] = SYSTEM_PROMPT;
    JsonObject usr = msgs.add<JsonObject>();
    usr["role"]    = "user";
    // /no_think prefix disables Qwen3 chain-of-thought at the model level
    String user_content = String("/no_think\n") + transcript;
    usr["content"] = user_content;

    String body;
    serializeJson(req, body);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(30000);
    http.addHeader("Content-Type", "application/json");

    int code = http.POST(body);
    if (code != 200) {
        Serial.printf("LLM: HTTP %d\n", code);
        http.end();
        return false;
    }

    String resp = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, resp) != DeserializationError::Ok) {
        Serial.println("LLM: JSON parse failed");
        return false;
    }

    const char* text = doc["choices"][0]["message"]["content"] | "";
    if (!text[0]) {
        Serial.println("LLM: empty response");
        return false;
    }

    strlcpy(response, text, response_size);
    strip_think_tags(response, response_size);

    if (!response[0]) {
        Serial.println("LLM: response was only think tags");
        return false;
    }

    return true;
}
