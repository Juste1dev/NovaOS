#include <stddef.h>

#include "keyboard.h"
#include "../kernel/idt.h"
#include <stdint.h>

static const char scancode_ascii[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',
    0,'*',0,' ',0,0,0,0,0,0,0,0,0,0,0,0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
    0,0,0,0,0
};

static const char scancode_shift[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',
    0,'*',0,' ',0,0,0,0,0,0,0,0,0,0,0,0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
    0,0,0,0,0
};

static key_event_t key_buf[KEY_BUFFER_SIZE];
static volatile uint32_t key_buf_head = 0;
static volatile uint32_t key_buf_tail = 0;
static volatile uint8_t  shift_state  = 0;
static volatile uint8_t  ctrl_state   = 0;
static volatile uint8_t  alt_state    = 0;
static volatile uint8_t  caps_lock    = 0;
static volatile uint8_t  extended     = 0;
static key_handler_t global_handler   = NULL;

static inline uint8_t inb(uint16_t port) {
    uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}

static void keyboard_irq_handler(registers_t *regs) {
    (void)regs;
    uint8_t sc = inb(0x60);

    if (sc == 0xE0) { extended = 1; return; }

    uint8_t released = (sc & 0x80);
    sc &= 0x7F;

    if (sc == KEY_LSHIFT || sc == KEY_RSHIFT) {
        shift_state = released ? 0 : 1;
    } else if (sc == KEY_LCTRL) {
        ctrl_state = released ? 0 : 1;
    } else if (sc == KEY_LALT) {
        alt_state = released ? 0 : 1;
    } else if (sc == KEY_CAPS && !released) {
        caps_lock ^= 1;
    }

    char ascii = 0;
    if (!released) {
        int use_shift = shift_state ^ caps_lock;
        if (sc < 128) {
            ascii = use_shift ? scancode_shift[sc] : scancode_ascii[sc];
        }
    }

    key_event_t evt;
    evt.scancode = sc;
    evt.ascii    = ascii;
    evt.shift    = shift_state;
    evt.ctrl     = ctrl_state;
    evt.alt      = alt_state;
    evt.released = released ? 1 : 0;
    evt.special  = extended;
    extended     = 0;

    if (global_handler && !released) global_handler(&evt);

    uint32_t next = (key_buf_head + 1) % KEY_BUFFER_SIZE;
    if (next != key_buf_tail) {
        key_buf[key_buf_head] = evt;
        key_buf_head = next;
    }
}

void keyboard_init(void) {
    register_interrupt_handler(33, keyboard_irq_handler);
}

void keyboard_set_handler(key_handler_t h) {
    global_handler = h;
}

int keyboard_poll(key_event_t *evt) {
    if (key_buf_tail == key_buf_head) return 0;
    if (evt) *evt = key_buf[key_buf_tail];
    key_buf_tail = (key_buf_tail + 1) % KEY_BUFFER_SIZE;
    return 1;
}

char keyboard_getchar(void) {
    key_event_t evt;
    while (1) {
        if (keyboard_poll(&evt)) {
            if (!evt.released && evt.ascii) return evt.ascii;
        }
    }
}

int keyboard_shift(void) { return shift_state; }
int keyboard_ctrl(void)  { return ctrl_state; }
int keyboard_alt(void)   { return alt_state; }
