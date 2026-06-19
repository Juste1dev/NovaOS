#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

void sound_init(void);
void sound_play_boot_jingle(void);
void sound_run_self_test(void);
void sound_speak_text(const char *text);
void sound_fill_summary(char *buf, int max);
void sound_fill_audioinfo(char *buf, int max);
const char *sound_output_backend(void);
const char *sound_driver_stack(void);
int sound_pc_speaker_ready(void);
int sound_sb16_detected(void);
int sound_self_test_ok(void);

#endif
