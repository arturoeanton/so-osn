#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char ascii;        /* printable char, '\n', '\b', '\t'; 0 if special */
    uint16_t keycode;  /* osnos_key_t (Linux KEY_*); 0 for plain ascii */
} keyboard_event_t;

void keyboard_init(void);
bool keyboard_poll(keyboard_event_t *ev);
