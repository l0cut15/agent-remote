#include "display.h"
#include <M5Unified.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

static const int SCR_W    = 320;
static const int STATUS_H = 80;   // top status bar height
static const int DIV_Y    = STATUS_H;
static const int LOG_Y    = DIV_Y + 1;
static const int LOG_LINE = 15;   // pixels per log line (font2 = 12px + 3 gap)
static const int LOG_COLS = 53;   // max chars per line at font size 1
static const int LOG_ROWS = (240 - LOG_Y) / LOG_LINE;  // 10 rows

static char _lines[12][LOG_COLS + 1];
static int  _nlines = 0;

static void redraw_log() {
    M5.Display.fillRect(0, LOG_Y, SCR_W, 240 - LOG_Y, TFT_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0xAD75, TFT_BLACK);  // light grey
    for (int i = 0; i < _nlines; i++) {
        M5.Display.setCursor(3, LOG_Y + i * LOG_LINE + 2);
        M5.Display.print(_lines[i]);
    }
}

void display_init() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.drawFastHLine(0, DIV_Y, SCR_W, 0x4208);
    display_status("INIT", "Starting up...", 0x2104);
}

void display_status(const char* label, const char* sub, uint32_t bg_color) {
    M5.Display.fillRect(0, 0, SCR_W, STATUS_H, bg_color);
    M5.Display.drawFastHLine(0, DIV_Y, SCR_W, 0x4208);

    // Large label — font size 4 = 24x32px per char
    M5.Display.setTextSize(4);
    M5.Display.setTextColor(TFT_WHITE, bg_color);
    int lw = M5.Display.textWidth(label);
    M5.Display.setCursor((SCR_W - lw) / 2, 8);
    M5.Display.print(label);

    // Subtitle — font size 2 = 12x16px
    if (sub && sub[0]) {
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(0xC618, bg_color);  // light grey on bg
        int sw = M5.Display.textWidth(sub);
        M5.Display.setCursor((SCR_W - sw) / 2, 52);
        M5.Display.print(sub);
    }
}

void display_log(const char* msg) {
    if (_nlines < LOG_ROWS) {
        strncpy(_lines[_nlines], msg, LOG_COLS);
        _lines[_nlines][LOG_COLS] = '\0';
        _nlines++;
    } else {
        for (int i = 0; i < LOG_ROWS - 1; i++)
            memcpy(_lines[i], _lines[i + 1], LOG_COLS + 1);
        strncpy(_lines[LOG_ROWS - 1], msg, LOG_COLS);
        _lines[LOG_ROWS - 1][LOG_COLS] = '\0';
    }
    redraw_log();
}

void display_logf(const char* fmt, ...) {
    char buf[LOG_COLS + 1];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    display_log(buf);
}
