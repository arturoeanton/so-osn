#pragma once

#include <stdint.h>

#define SERVER_KEYBOARD 1
#define SERVER_SHELL    2
#define SERVER_CONSOLE  3
#define SERVER_FS       4

void service_register(uint64_t service_id, uint64_t pid);
uint64_t service_get_pid(uint64_t service_id);
