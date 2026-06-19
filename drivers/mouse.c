
#include "mouse.h"
#include "../kernel/idt.h"
#include "vbe.h"
#include <stdint.h>
#include <stddef.h>

static mouse_state_t ms;
static mouse_handler_t g_handler = NULL;
static uint8_t  mcycle = 0;
static uint8_t  mbytes[4];
#define MOUSE_EVENT_QUEUE_CAP 128
static mouse_state_t mouse_queue[MOUSE_EVENT_QUEUE_CAP];
static volatile uint32_t mouse_queue_head = 0;
static volatile uint32_t mouse_queue_tail = 0;
static int32_t sub_x = 0, sub_y = 0;
static uint8_t packet_size = 3;
static uint8_t mouse_id = 0;
static int mouse_speed_preset = 1;

static inline uint8_t inb(uint16_t p) {
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v;
}
static inline void outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
}
static inline void io_wait(void) { outb(0x80, 0); }

static int ps2_wait_write(void) {
    uint32_t t = 100000;
    while (t--) if (!(inb(0x64) & 2)) return 1;
    return 0;
}
static int ps2_wait_read(void) {
    uint32_t t = 100000;
    while (t--) if (inb(0x64) & 1) return 1;
    return 0;
}

static int mouse_read_data(uint8_t *out) {
    uint32_t t = 200000;
    while (t--) {
        uint8_t status = inb(0x64);
        if (!(status & 1)) continue;
        uint8_t data = inb(0x60);

        if (!(status & 0x20)) continue;
        if (out) *out = data;
        return 1;
    }
    return 0;
}

static void mouse_flush(void) {
    for (int i = 0; i < 64; i++) {
        if (!(inb(0x64) & 1)) break;
        (void)inb(0x60);
        io_wait();
    }
}

static int mouse_cmd(uint8_t cmd) {
    if (!ps2_wait_write()) return 0;
    outb(0x64, 0xD4); io_wait();
    if (!ps2_wait_write()) return 0;
    outb(0x60, cmd); io_wait();
    return 1;
}

static uint8_t mouse_read_byte(void) {
    uint8_t data = 0;
    if (!mouse_read_data(&data)) return 0;
    return data;
}

static int mouse_cmd_ack(uint8_t cmd) {
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t resp = 0;
        mouse_flush();
        if (!mouse_cmd(cmd)) return 0;
        if (!mouse_read_data(&resp)) return 0;
        if (resp == 0xFA) return 1;
        if (resp != 0xFE) return 0;
    }
    return 0;
}

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int div_round_sym(int num, int den) {
    if (num >= 0) return (num + den / 2) / den;
    return -(((-num) + den / 2) / den);
}

static int mouse_set_rate(uint8_t rate) {
    if (!mouse_cmd_ack(0xF3)) return 0;
    mouse_flush();
    return mouse_cmd_ack(rate);
}

static int mouse_set_resolution(uint8_t value) {
    if (!mouse_cmd_ack(0xE8)) return 0;
    mouse_flush();
    return mouse_cmd_ack(value);
}

static int scale_axis(int raw, int32_t *carry);

static void mouse_queue_push(const mouse_state_t *evt) {
    uint32_t next = (mouse_queue_head + 1) % MOUSE_EVENT_QUEUE_CAP;
    if (next == mouse_queue_tail) {
        mouse_queue_tail = (mouse_queue_tail + 1) % MOUSE_EVENT_QUEUE_CAP;
    }
    mouse_queue[mouse_queue_head] = *evt;
    mouse_queue_head = next;
}

static int mouse_queue_pop(mouse_state_t *evt) {
    if (mouse_queue_tail == mouse_queue_head) return 0;
    *evt = mouse_queue[mouse_queue_tail];
    mouse_queue_tail = (mouse_queue_tail + 1) % MOUSE_EVENT_QUEUE_CAP;
    return 1;
}

static uint8_t mouse_get_id(void) {
    uint8_t ack = 0;
    uint8_t id = 0;
    mouse_flush();
    if (!mouse_cmd(0xF2)) return 0;
    if (!mouse_read_data(&ack)) return 0;
    if (ack == 0xFE) {
        if (!mouse_cmd(0xF2)) return 0;
        if (!mouse_read_data(&ack)) return 0;
    }
    if (ack != 0xFA) return 0;
    if (!mouse_read_data(&id)) return 0;
    return id;
}

static int mouse_enable_wheel(void) {
    if (!mouse_set_rate(200)) return 0;
    mouse_flush();
    if (!mouse_set_rate(100)) return 0;
    mouse_flush();
    if (!mouse_set_rate(80)) return 0;
    mouse_flush();
    mouse_id = mouse_get_id();
    if (mouse_id == 3 || mouse_id == 4) {
        packet_size = 4;
        return 1;
    }
    packet_size = 3;
    return 0;
}

static void mouse_commit_packet(void) {
    int raw_dx;
    int raw_dy;
    int dx;
    int dy;
    int new_x;
    int new_y;
    int applied_dx;
    int applied_dy;
    int wheel = 0;
    uint8_t old_buttons;

    if (mbytes[0] & 0xC0) return;

    raw_dx = (int)(int8_t)mbytes[1];
    raw_dy = (int)(int8_t)mbytes[2];
    dx = scale_axis(raw_dx, &sub_x);
    dy = scale_axis(-raw_dy, &sub_y);

    if (packet_size >= 4) {
        wheel = (int)(mbytes[3] & 0x0F);
        if (wheel & 0x08) wheel -= 16;
    }

    old_buttons = ms.buttons;
    ms.buttons = mbytes[0] & 0x07;

    new_x = ms.x + dx;
    new_y = ms.y + dy;
    if (vbe.width) new_x = clampi(new_x, 0, (int)vbe.width - 1);
    else new_x = clampi(new_x, 0, SCREEN_WIDTH - 1);
    if (vbe.height) new_y = clampi(new_y, 0, (int)vbe.height - 1);
    else new_y = clampi(new_y, 0, SCREEN_HEIGHT - 1);

    applied_dx = new_x - ms.x;
    applied_dy = new_y - ms.y;
    ms.x = new_x;
    ms.y = new_y;
    ms.dx = applied_dx;
    ms.dy = applied_dy;
    ms.scroll = wheel;

    if (applied_dx || applied_dy || wheel || ms.buttons != old_buttons) {
        mouse_state_t snapshot = ms;
        snapshot.dx = applied_dx;
        snapshot.dy = applied_dy;
        snapshot.scroll = wheel;
        snapshot.buttons = ms.buttons;
        mouse_queue_push(&snapshot);
    }
}

static int scale_axis(int raw, int32_t *carry) {
    int a = raw < 0 ? -raw : raw;
    int gain_num = 4;
    int gain_den = 4;
    int clamp_out = 48;

    if (mouse_speed_preset < 0 || mouse_speed_preset > 2) mouse_speed_preset = 1;

    if (raw > 64) raw = 64;
    if (raw < -64) raw = -64;
    a = raw < 0 ? -raw : raw;

    if (mouse_speed_preset == 0) {
        gain_num = 3;
        if (a >= 12) gain_num = 4;
        if (a >= 24) gain_num = 5;
        clamp_out = 36;
    } else if (mouse_speed_preset == 2) {
        gain_num = 5;
        if (a >= 10) gain_num = 6;
        if (a >= 20) gain_num = 7;
        clamp_out = 56;
    } else {
        if (a >= 10) gain_num = 5;
        if (a >= 20) gain_num = 6;
    }

    *carry += raw * gain_num;
    {
        int out = div_round_sym(*carry, gain_den);
        *carry -= out * gain_den;
        if (out == 0 && raw != 0) out = raw > 0 ? 1 : -1;
        return clampi(out, -clamp_out, clamp_out);
    }
}

static void mouse_irq(registers_t *r) {
    (void)r;
    uint8_t status = inb(0x64);
    if (!(status & 0x20)) return;

    int8_t data = (int8_t)inb(0x60);

    switch (mcycle) {
        case 0:
            if (!(data & 0x08)) { mcycle = 0; break; }
            mbytes[0] = (uint8_t)data;
            mcycle = 1;
            break;
        case 1:
            mbytes[1] = (uint8_t)data;
            mcycle = 2;
            break;
        case 2:
            mbytes[2] = (uint8_t)data;
            if (packet_size >= 4) mcycle = 3;
            else {
                mcycle = 0;
                mouse_commit_packet();
            }
            break;
        case 3:
            mbytes[3] = (uint8_t)data;
            mcycle = 0;
            mouse_commit_packet();
            break;
        default:
            mcycle = 0;
            break;
    }
}

void mouse_init(void) {
    ms.x = vbe.width ? (int)(vbe.width / 2) : (SCREEN_WIDTH / 2);
    ms.y = vbe.height ? (int)(vbe.height / 2) : (SCREEN_HEIGHT / 2);
    ms.dx = ms.dy = ms.buttons = ms.scroll = 0;
    sub_x = sub_y = 0;
    mouse_queue_head = 0;
    mouse_queue_tail = 0;
    mcycle = 0;
    packet_size = 3;
    mouse_id = 0;
    mouse_speed_preset = 1;

    register_interrupt_handler(44, mouse_irq);
    mouse_flush();

    if (!ps2_wait_write()) return;
    outb(0x64, 0xA8); io_wait();

    if (!ps2_wait_write()) return;
    outb(0x64, 0x20); io_wait();
    uint8_t cfg = 0;
    if (ps2_wait_read()) cfg = inb(0x60);
    cfg |= 0x02;
    cfg &= ~0x20u;
    if (!ps2_wait_write()) return;
    outb(0x64, 0x60); io_wait();
    if (!ps2_wait_write()) return;
    outb(0x60, cfg); io_wait();
    mouse_flush();

    if (!mouse_cmd_ack(0xF6)) return;

    if (!mouse_set_rate(200)) return;
    mouse_flush();

    if (!mouse_set_resolution(0x03)) return;
    mouse_flush();

    mouse_enable_wheel();
    mouse_flush();

    (void)mouse_cmd_ack(0xF4);
    mouse_flush();
}

void mouse_set_handler(mouse_handler_t h) { g_handler = h; }

int mouse_poll(void) {
    mouse_state_t snapshot;
    if (!g_handler) return 0;
    if (!mouse_queue_pop(&snapshot)) return 0;
    g_handler(&snapshot);
    return 1;
}

mouse_state_t* mouse_get_state(void)                 { return &ms; }
int            mouse_button_left(void)               { return ms.buttons & 1; }
int            mouse_button_right(void)              { return (ms.buttons >> 1) & 1; }
int            mouse_button_middle(void)             { return (ms.buttons >> 2) & 1; }
void           mouse_set_speed_preset(int preset)       { if (preset < 0) preset = 0; if (preset > 2) preset = 2; mouse_speed_preset = preset; }
int            mouse_get_speed_preset(void)             { return mouse_speed_preset; }
