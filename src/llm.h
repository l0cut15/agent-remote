#pragma once
#include <stddef.h>

// POST transcript to local Qwen, fill response on success.
bool llm_complete(const char* transcript, char* response, size_t response_size);
