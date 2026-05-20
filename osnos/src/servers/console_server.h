#pragma once

#include <stdint.h>

void console_server_init(void);
void console_server_tick(void);

void console_write(const char *s);
void console_write_color(const char *s, uint32_t color);
void console_clear(void);
void console_backspace(void);
