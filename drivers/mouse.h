

#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

typedef struct {
    int x, y;
    int dx, dy;
    uint8_t buttons;
    int scroll;
} mouse_state_t;

typedef void (*mouse_handler_t)(mouse_state_t *state);

void mouse_init(void);
void mouse_set_handler(mouse_handler_t h);
int mouse_poll(void);
mouse_state_t* mouse_get_state(void);
int mouse_button_left(void);
int mouse_button_right(void);
int mouse_button_middle(void);
void mouse_set_speed_preset(int preset);
int mouse_get_speed_preset(void);

#endif
