/* SPDX-License-Identifier: MPL-2.0
 * HALO's composer: the clock, the step sequencer, the chord generator (voice leading, harmonic drift), the
 * arpeggiator and INFINITY, the generative mode. It runs inside the audio render, between 32-frame chunks
 * (halo_render), so timing is chunk-accurate (0.7 ms). The main loop talks to it through comp_post (a lock-free
 * queue) and edits the song under plat_lock. */
#pragma once
#include <stdint.h>
#include "halo_params.h"

#define NPAT 4
#define NSTEP 32
#define NAUTO 16

/* a step: note (bit 7 = on), and prob (bits 5-7: (p+1)/8) with length (bits 0-4: 1..32 steps) */
typedef struct { uint8_t note, pl; } step_t;
#define STEP_ON 0x80u
#define STEP_NOTE(s) ((s).note & 0x7Fu)
#define STEP_PROB(s) (((s).pl >> 5) & 7u)       /* 0..7 = 12.5 % .. 100 % */
#define STEP_LEN(s) (((s).pl & 31u) + 1u)       /* steps */

enum { DIV_16, DIV_8, DIV_4, DIV_2, DIV_BAR, DIV_2BAR, NDIVS };
typedef struct {
    uint8_t len;                       /* 1..32 steps */
    uint8_t div;                       /* DIV_*: one step lasts this long */
    uint8_t chain;                     /* bit p: pattern p plays in the chain (0 = the edited one only) */
    uint8_t gen;                       /* 0..127: how often a step's note wanders to a neighbouring scale tone */
    uint8_t auto_id;                   /* a param automated across the pattern (0xFF none) */
    uint8_t rsv[3];
    step_t s[NPAT][NSTEP];
    uint8_t aut[NPAT][NAUTO];          /* 0xFF = no point */
} track_seq_t;

#define SONG_MAGIC 0x484C4F53u         /* "SOLH" */
typedef struct {
    uint32_t magic;
    track_seq_t t[NTRACK];
} song_t;
extern song_t song;

/* the main loop's events */
enum { EV_NOTE_ON, EV_NOTE_OFF, EV_PLAY, EV_STOP, EV_PANIC, EV_RELEASE_TRACK, EV_DRONE_HOLD };
void comp_post(uint32_t type, uint32_t track, int32_t note, uint32_t vel);

void comp_init(void);
void halo_render(int32_t *lr, uint32_t n, float out_gain);   /* the whole render: composer + DSP, 24-bit out */

/* scales and chords, for the UI and the composer */
extern const char *const SCALE_NAME[NSCALES];
int32_t scale_quantize(int32_t note);   /* to the nearest tone of P[G_KEY] / P[G_SCALE] */
uint32_t chord_build(int32_t root, uint32_t type, int32_t *out);   /* -> notes (up to 4), voice-led */

/* what the screen reads */
extern volatile uint8_t comp_playing, comp_edit_pat[NTRACK], comp_play_pat[NTRACK], comp_step[NTRACK];
extern volatile int32_t comp_chord[4];
extern volatile uint8_t comp_chord_n, comp_chord_deg;
extern volatile uint32_t comp_bar, comp_beat16;
extern volatile float comp_energy;
extern volatile uint8_t comp_section;   /* INFINITY: 0 intro, 1 bloom, 2 song, 3 thin, 4 swell */
extern volatile uint32_t comp_note_flash[NTRACK];   /* a counter per track: a note started (the screen pulses) */
extern volatile uint8_t comp_held_mask;             /* tracks with a key held (keyboard) */
extern volatile int8_t comp_chord_root_pc, comp_chord_type;   /* the chord sounding (-1 none); type: CH_*, or
                                                                 * 10 dom7, 11 m7b5, 12 m(maj7), 13 maj7#5, 14 other */
float comp_bpm(void);
extern const uint8_t MOOD_SCALE[NMOODS], MOOD_TEMPO[NMOODS];
