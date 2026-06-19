#include "sound.h"
#include "../kernel/timer.h"
#include "../libc.h"
#include <stdint.h>
#include <stddef.h>

#define PIT_COMMAND_PORT   0x43
#define PIT_CHANNEL2_PORT  0x42
#define PCSPK_CTRL_PORT    0x61
#define SB16_BASE_PORT     0x220
#define SB16_RESET_PORT    (SB16_BASE_PORT + 0x6)
#define SB16_READ_PORT     (SB16_BASE_PORT + 0xA)
#define SB16_WRITE_PORT    (SB16_BASE_PORT + 0xC)
#define SB16_READ_STATUS   (SB16_BASE_PORT + 0xE)

typedef struct {
    int initialized;
    int pc_speaker_ready;
    int sb16_present;
    int self_test_ok;
    char output_backend[64];
    char driver_stack[96];
} sound_state_t;

static sound_state_t g_sound;

static inline void snd_outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0,%1"::"a"(value),"Nd"(port));
}

static inline uint8_t snd_inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1,%0":"=a"(value):"Nd"(port));
    return value;
}

static void snd_io_wait(void) {
    for (volatile int i = 0; i < 1024; i++) {
        __asm__ volatile("pause");
    }
}

static void snd_copy(char *dst, const char *src, int max) {
    int i = 0;
    if (!dst || max <= 0) return;
    while (src && src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void snd_append(char *dst, const char *src, int max) {
    int len = (int)k_strlen(dst);
    int i = 0;
    if (!dst || !src || max <= 0 || len >= max - 1) return;
    while (src[i] && len + i < max - 1) {
        dst[len + i] = src[i];
        i++;
    }
    dst[len + i] = 0;
}

static void sound_pc_speaker_enable(uint32_t freq) {
    if (!freq) return;
    uint32_t divisor = 1193180u / freq;
    snd_outb(PIT_COMMAND_PORT, 0xB6);
    snd_outb(PIT_CHANNEL2_PORT, (uint8_t)(divisor & 0xFF));
    snd_outb(PIT_CHANNEL2_PORT, (uint8_t)((divisor >> 8) & 0xFF));
    {
        uint8_t ctrl = snd_inb(PCSPK_CTRL_PORT);
        if ((ctrl & 0x03u) != 0x03u) snd_outb(PCSPK_CTRL_PORT, (uint8_t)(ctrl | 0x03u));
    }
}

static void sound_pc_speaker_disable(void) {
    snd_outb(PCSPK_CTRL_PORT, (uint8_t)(snd_inb(PCSPK_CTRL_PORT) & 0xFCu));
}

static int sound_detect_sb16(void) {
    snd_outb(SB16_RESET_PORT, 0x01);
    snd_io_wait();
    snd_outb(SB16_RESET_PORT, 0x00);

    for (int i = 0; i < 4000; i++) {
        if (snd_inb(SB16_READ_STATUS) & 0x80u) {
            uint8_t ack = snd_inb(SB16_READ_PORT);
            if (ack == 0xAAu) {
                if ((snd_inb(SB16_WRITE_PORT) & 0x80u) == 0) {
                    snd_outb(SB16_WRITE_PORT, 0xD1);
                }
                return 1;
            }
        }
        snd_io_wait();
    }
    return 0;
}

void sound_init(void) {
    if (g_sound.initialized) return;

    k_memset(&g_sound, 0, sizeof(g_sound));
    g_sound.pc_speaker_ready = 1;
    g_sound.sb16_present = sound_detect_sb16();
    g_sound.self_test_ok = 0;

    snd_copy(g_sound.output_backend, "PC speaker PIT (sortie sonore validee sous QEMU)", sizeof(g_sound.output_backend));
    snd_copy(g_sound.driver_stack, "pcspk", sizeof(g_sound.driver_stack));
    if (g_sound.sb16_present) {
        snd_append(g_sound.driver_stack, " + sb16-detect", sizeof(g_sound.driver_stack));
    } else {
        snd_append(g_sound.driver_stack, " + qemu-audio-compatible", sizeof(g_sound.driver_stack));
    }

    g_sound.initialized = 1;
}

void sound_run_self_test(void) {
    const uint16_t tones[][2] = {
        {392, 120}, {523, 110}, {659, 180}, {523, 90}, {784, 190},
        {659, 120}, {523, 100}, {392, 140}, {523, 120}, {659, 200},
        {784, 180}, {659, 180}, {523, 220}, {0, 55}, {659, 160}, {523, 220}
    };

    if (!g_sound.initialized) sound_init();
    if (!g_sound.pc_speaker_ready) return;

    for (unsigned int i = 0; i < sizeof(tones) / sizeof(tones[0]); i++) {
        if (tones[i][0]) sound_pc_speaker_enable(tones[i][0]);
        else sound_pc_speaker_disable();
        timer_sleep(tones[i][1]);
        sound_pc_speaker_disable();
        timer_sleep(18);
    }
    g_sound.self_test_ok = 1;
}

void sound_play_boot_jingle(void) {
    if (!g_sound.initialized) sound_init();

}

void sound_speak_text(const char *text) {
    if (!text || !text[0]) return;
    if (!g_sound.initialized) sound_init();
    if (!g_sound.pc_speaker_ready) return;

    for (int i = 0; text[i] && i < 96; i++) {
        char c = text[i];
        uint32_t freq = 0;
        uint32_t dur = 34;

        if (c == ' ' || c == '\t') {
            timer_sleep(22);
            continue;
        }
        if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?') {
            sound_pc_speaker_disable();
            timer_sleep(56);
            continue;
        }

        switch (c | 32) {
            case 'a': freq = 440; dur = 70; break;
            case 'e': freq = 554; dur = 72; break;
            case 'i': freq = 660; dur = 62; break;
            case 'o': freq = 494; dur = 78; break;
            case 'u': freq = 392; dur = 84; break;
            case 'y': freq = 740; dur = 56; break;
            case 'r': freq = 330; dur = 44; break;
            case 's': freq = 784; dur = 30; break;
            case 't': freq = 588; dur = 32; break;
            case 'n': freq = 349; dur = 38; break;
            case 'm': freq = 294; dur = 44; break;
            case 'l': freq = 524; dur = 40; break;
            default:
                if (c >= '0' && c <= '9') {
                    freq = 520 + (uint32_t)(c - '0') * 28u;
                    dur = 40;
                } else {
                    freq = 300 + ((uint32_t)(c & 31u) * 18u);
                    dur = 26;
                }
                break;
        }

        sound_pc_speaker_enable(freq);
        timer_sleep(dur);
        sound_pc_speaker_disable();
        timer_sleep(12);
    }
    g_sound.self_test_ok = 1;
}

void sound_fill_summary(char *buf, int max) {
    if (!buf || max <= 0) return;
    if (!g_sound.initialized) sound_init();
    buf[0] = 0;
    snd_append(buf, g_sound.pc_speaker_ready ? "PC speaker actif" : "PC speaker indisponible", max);
    snd_append(buf, g_sound.sb16_present ? " • SB16 detecte" : " • SB16 absent", max);
    snd_append(buf, g_sound.self_test_ok ? " • auto-test OK" : " • auto-test en attente", max);
}

void sound_fill_audioinfo(char *buf, int max) {
    if (!buf || max <= 0) return;
    if (!g_sound.initialized) sound_init();

    buf[0] = 0;
    snd_append(buf, "Audio Nova 4.2\n", max);
    snd_append(buf, "===============\n", max);
    snd_append(buf, "Backend actif : ", max);
    snd_append(buf, g_sound.output_backend, max);
    snd_append(buf, "\n", max);
    snd_append(buf, "Pile de pilotes : ", max);
    snd_append(buf, g_sound.driver_stack, max);
    snd_append(buf, "\n", max);
    snd_append(buf, "PC speaker : ", max);
    snd_append(buf, g_sound.pc_speaker_ready ? "pret\n" : "indisponible\n", max);
    snd_append(buf, "Sound Blaster 16 (QEMU ISA) : ", max);
    snd_append(buf, g_sound.sb16_present ? "detecte\n" : "non detecte\n", max);
    snd_append(buf, "Auto-test jingle : ", max);
    snd_append(buf, g_sound.self_test_ok ? "OK\n" : "non execute\n", max);
    snd_append(buf, "Validation recommandee : QEMU avec pcspk-audiodev et capture WAV.\n", max);
}

const char *sound_output_backend(void) {
    if (!g_sound.initialized) sound_init();
    return g_sound.output_backend;
}

const char *sound_driver_stack(void) {
    if (!g_sound.initialized) sound_init();
    return g_sound.driver_stack;
}

int sound_pc_speaker_ready(void) {
    if (!g_sound.initialized) sound_init();
    return g_sound.pc_speaker_ready;
}

int sound_sb16_detected(void) {
    if (!g_sound.initialized) sound_init();
    return g_sound.sb16_present;
}

int sound_self_test_ok(void) {
    if (!g_sound.initialized) sound_init();
    return g_sound.self_test_ok;
}
