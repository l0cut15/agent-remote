#pragma once
#include <stdint.h>

void display_init();

// Top status bar — label is large text, sub is smaller below it
void display_status(const char* label, const char* sub, uint32_t bg_color);

// Append a line to the scrolling log area (auto-scrolls)
void display_log(const char* msg);
void display_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
