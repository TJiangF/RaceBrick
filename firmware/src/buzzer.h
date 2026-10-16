/* Passive buzzer on GPIO47 (PIN_BEE). PWM frequency = pitch, duty = loudness.
 * A tiny non-blocking sequencer plays short melodies; call buzzer_update()
 * every iteration of the main loop. */
#pragma once
#include <stdint.h>

typedef struct { uint16_t freq; uint16_t ms; } note_t;   /* freq 0 = rest */

void buzzer_init(void);
void buzzer_tone(int freq_hz, int volume_percent);       /* volume 0..100 */
void buzzer_off(void);
void buzzer_set_volume(int volume_percent);              /* global volume, 0 = mute */
void buzzer_play(const note_t *seq, int n);              /* start a melody */
void buzzer_update(void);                                /* advance the melody */

enum { SND_CONFIRM, SND_CALIB, SND_FIX };
void buzzer_play_id(int id);                             /* play a built-in effect */
