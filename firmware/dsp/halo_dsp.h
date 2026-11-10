/* SPDX-License-Identifier: MPL-2.0
 * HALO's sound: five engines, a shared effects bus and the mixer. Everything here runs in the audio interrupt
 * (dsp_note_on / dsp_note_off / dsp_process are called by the composer inside the render), so the main loop
 * never calls these directly: it posts events (halo_comp.h). */
#pragma once
#include <stdint.h>
#include "halo_params.h"

#define HALO_FS 44100.0f
#define HALO_CHUNK 32u                 /* frames per processing chunk: params cooked and the clock advanced per chunk */

/* voice budget (fixed, predictable): see docs/ARCHITECTURE.md */
#define ATMOS_VOICES 4
#define ARIA_VOICES 3
#define ECHO_VOICES 6
#define SHIM_SLOTS 4

void dsp_init(void);
void dsp_note_on(uint32_t track, int32_t note, uint32_t vel);   /* vel 1..127 */
void dsp_note_off(uint32_t track, int32_t note);
void dsp_release_all(uint32_t track);
/* a chord moved by voice leading: sounding voices glide to their new notes without a new attack (notes[i] goes to
 * voice i); voices beyond n are released. For the pad (ATMOS) and the shimmer, which follows the harmony */
void dsp_pad_chord(const int32_t *notes, uint32_t n);
void dsp_shim_chord(const int32_t *notes, uint32_t n);                         /* every held note of a track released */
void dsp_kill(void);                                          /* silence now (panic) */
enum { PERC_KICK, PERC_TICK, PERC_SHAKER };
void dsp_perc(uint32_t type, uint32_t vel);                   /* the soft beat (INFINITY's RHYTHM) */
extern volatile uint32_t dsp_beats;                           /* beats played (the tests count them) */
void dsp_process(float *l, float *r, uint32_t n);             /* n <= HALO_CHUNK, adds nothing: writes */

/* mix state (written by the UI with single stores; read once per chunk) */
extern volatile uint8_t dsp_mute, dsp_solo;                   /* bit t = track t */
extern volatile uint8_t dsp_quality;                          /* 2 full, 1 lighter, 0 lightest (overload) */
extern volatile uint8_t dsp_drone_gate;                       /* the drone sounds (HOLD or a key / the composer) */
extern volatile float dsp_tempo_bpm;
extern volatile uint8_t dsp_speaker;
extern volatile uint8_t dsp_test;                             /* SETTINGS TEST: 0 off, 2..6 one engine dry, 7 no effects */                          /* the built-in speaker: a low cut at ~110 Hz */                          /* for the synced delay */
/* what the screen shows: each track's level (0..1, slow), the drone's root now, the shimmer's activity */
extern volatile float dsp_meter[NTRACK], dsp_out_meter;
extern volatile float dsp_drone_hz;
uint32_t dsp_active(uint32_t track);                          /* voices sounding */
