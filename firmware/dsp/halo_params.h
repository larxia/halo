/* SPDX-License-Identifier: MPL-2.0
 * HALO — ambient workstation firmware for the M-VAVE FM-1.
 * The whole instrument state that a knob can touch is one array of 0..127 bytes, P[]. A preset is a list of
 * changes to the defaults below; the DSP reads P[] once per 64-frame chunk and turns it into coefficients
 * ("cooking"), so the UI only ever writes single bytes and nothing it does can tear a value the audio reads. */
#pragma once
#include <stdint.h>

/* display formats (the UI turns a 0..127 value into text with these) */
enum { F_PCT, F_BIP, F_NOTE, F_TIME, F_HZ, F_CUT, F_ENUM, F_TEMPO, F_RAW, F_BOOL };

/* X(id, label, default, format, enum-table-or-0)
 * Labels are at most 6 characters: they sit on the knob cards at the bottom of the screen. */
#define HALO_PARAMS(X) \
    /* ---- ATMOS: the pad (track 1) ---- */ \
    X(A_SHAPE,  "SHAPE",  44, F_PCT,  0) X(A_DETUNE, "DETUNE", 34, F_PCT,  0) \
    X(A_AIR,    "AIR",    30, F_PCT,  0) X(A_SUB,    "SUB",     8, F_PCT,  0) \
    X(A_CUT,    "CUTOFF", 82, F_CUT,  0) X(A_RES,    "RESO",   18, F_PCT,  0) \
    X(A_ENV,    "ENV>F",  74, F_BIP,  0) X(A_DRIFT,  "DRIFT",  40, F_PCT,  0) \
    X(A_ATK,    "ATTACK", 82, F_TIME, 0) X(A_DEC,    "DECAY",  80, F_TIME, 0) \
    X(A_SUS,    "SUSTN", 104, F_PCT,  0) X(A_REL,    "RELEAS", 92, F_TIME, 0) \
    X(A_LFOR,   "LFO HZ", 28, F_HZ,   0) X(A_LFOD,   "LFO>F",  30, F_PCT,  0) \
    X(A_MORPH,  "MORPH",  30, F_PCT,  0) X(A_WIDTH,  "WIDTH",  96, F_PCT,  0) \
    /* ---- DRONE (track 2) ---- */ \
    X(D_ROOT,   "ROOT",   36, F_NOTE, 0) X(D_DENS,   "DENSTY", 64, F_PCT,  0) \
    X(D_SUB,    "SUB",    56, F_PCT,  0) X(D_FIFTH,  "FIFTH",  30, F_PCT,  0) \
    X(D_MOTION, "MOTION", 44, F_PCT,  0) X(D_TONE,   "TONE",   56, F_PCT,  0) \
    X(D_BEAT,   "BEAT",   36, F_PCT,  0) X(D_GLIDE,  "GLIDE",  70, F_TIME, 0) \
    X(D_GRAIN,  "GRAIN",   0, F_PCT,  0) X(D_GRIT,   "GRIT",    6, F_PCT,  0) \
    X(D_CUT,    "CUTOFF", 84, F_CUT,  0) X(D_WOBBLE, "WOBBLE", 40, F_PCT,  0) \
    X(D_HOLD,   "HOLD",    1, F_BOOL, 0) X(D_ATK,    "F.IN",   96, F_TIME, 0) \
    X(D_REL,    "F.OUT", 100, F_TIME, 0) X(D_FOLLOW, "FOLLOW",  1, F_BOOL, 0) \
    /* ---- SHIMMER (track 3) ---- */ \
    X(S_INT,    "INTENS", 64, F_PCT,  0) X(S_BRIGHT, "BRIGHT", 58, F_PCT,  0) \
    X(S_SPARK,  "SPARKL", 24, F_PCT,  0) X(S_DIFF,   "DIFFUS", 70, F_PCT,  0) \
    X(S_ATK,    "ATTACK", 90, F_TIME, 0) X(S_REL,    "RELEAS",100, F_TIME, 0) \
    X(S_OCT,    "OCTAVE",  0, F_ENUM, E_SHOCT) X(S_FDBK, "FEEDBK", 34, F_PCT, 0) \
    /* ---- ARIA: the choir (track 4) ---- */ \
    X(V_VOWEL,  "VOWEL",   0, F_ENUM, E_VOWEL) X(V_DRIFT, "V.DRFT", 30, F_PCT, 0) \
    X(V_DRATE,  "V.RATE", 24, F_HZ,   0) X(V_BREATH, "BREATH", 12, F_PCT,  0) \
    X(V_VIB,    "VIBRAT", 30, F_PCT,  0) X(V_VRATE,  "V.SPEED",54, F_HZ,   0) \
    X(V_ENS,    "ENSMBL", 56, F_PCT,  0) X(V_FORM,   "FORMNT", 80, F_PCT,  0) \
    X(V_ATK,    "ATTACK", 84, F_TIME, 0) X(V_REL,    "RELEAS", 92, F_TIME, 0) \
    X(V_OCT,    "OCTAVE",  1, F_ENUM, E_VOCT) X(V_BRIGHT,"BRIGHT", 54, F_PCT, 0) \
    /* ---- ECHO: plucks, bells, arpeggios (track 5) ---- */ \
    X(E_RATIO,  "RATIO",   2, F_ENUM, E_RATIOS) X(E_INDEX, "INDEX",  40, F_PCT, 0) \
    X(E_IDEC,   "I.DECAY",56, F_TIME, 0) X(E_TONE,   "TONE",   78, F_CUT,  0) \
    X(E_ATK,    "ATTACK",  4, F_TIME, 0) X(E_DEC,    "DECAY",  76, F_TIME, 0) \
    X(E_SUS,    "SUSTN",   0, F_PCT,  0) X(E_REL,    "RELEAS", 72, F_TIME, 0) \
    X(E_ARP,    "ARP",     0, F_ENUM, E_ARPS) X(E_ARATE, "RATE",   2, F_ENUM, E_DIVS) \
    X(E_AOCT,   "OCTS",    1, F_ENUM, E_AOCTS) X(E_VEL,  "VEL",    80, F_PCT, 0) \
    X(E_MODEL,  "MODEL",   0, F_ENUM, E_MODELS) X(E_GLIDE, "GLIDE", 40, F_TIME, 0) \
    X(E_VIB,    "VIBRAT", 24, F_PCT,  0) X(E_RES,    "RESO",   30, F_PCT,  0) \
    /* ---- FX ---- */ \
    X(R_TYPE,   "REVERB",  0, F_ENUM, E_RTYPE) X(R_SIZE, "SIZE",  84, F_PCT, 0) \
    X(R_DECAY,  "DECAY",  88, F_PCT,  0) X(R_DAMP,   "DAMP",   46, F_PCT,  0) \
    X(R_MOD,    "MOD",    44, F_PCT,  0) X(R_SHIM,   "SHIMMR",  6, F_PCT,  0) \
    X(R_LEVEL,  "LEVEL", 100, F_PCT,  0) X(R_LOCUT,  "LOCUT",  24, F_PCT,  0) \
    X(L_TIME,   "TIME",    5, F_ENUM, E_DTIME) X(L_FDBK, "FEEDBK", 70, F_PCT, 0) \
    X(L_TONE,   "TONE",   60, F_PCT,  0) X(L_WOW,    "WOW",    36, F_PCT,  0) \
    X(L_MODE,   "MODE",    0, F_ENUM, E_DMODE) X(L_LEVEL, "LEVEL",  86, F_PCT, 0) \
    X(L_TOREV,  "TO REV", 50, F_PCT,  0) X(X_WARBLE, "WARBLE",  0, F_PCT,  0) \
    X(C_DEPTH,  "CHORUS", 34, F_PCT,  0) X(C_RATE,   "C.RATE", 18, F_HZ,   0) \
    X(H_AMT,    "PHASER",  0, F_PCT,  0) X(H_RATE,   "P.RATE", 10, F_HZ,   0) \
    X(T_DEPTH,  "TREMOL",  0, F_PCT,  0) X(T_RATE,   "T.RATE", 40, F_HZ,   0) \
    X(X_SAT,    "SATUR",   8, F_PCT,  0) X(X_EXCITE, "EXCITE",  0, F_PCT,  0) \
    X(X_NOISE,  "NOISE",   0, F_PCT,  0) X(X_NTONE,  "N.TONE", 40, F_PCT,  0) \
    X(X_NMOVE,  "N.MOVE", 40, F_PCT,  0) X(X_MASTER, "OUTPUT", 88, F_PCT,  0) \
    /* ---- track mix: volume, pan, reverb send, delay send ---- */ \
    X(T1_VOL, "VOLUME", 92, F_PCT, 0) X(T1_PAN, "PAN", 64, F_BIP, 0) X(T1_REV, "REV", 80, F_PCT, 0) X(T1_DLY, "DLY", 10, F_PCT, 0) \
    X(T2_VOL, "VOLUME", 84, F_PCT, 0) X(T2_PAN, "PAN", 64, F_BIP, 0) X(T2_REV, "REV", 60, F_PCT, 0) X(T2_DLY, "DLY",  0, F_PCT, 0) \
    X(T3_VOL, "VOLUME", 74, F_PCT, 0) X(T3_PAN, "PAN", 64, F_BIP, 0) X(T3_REV, "REV",110, F_PCT, 0) X(T3_DLY, "DLY", 30, F_PCT, 0) \
    X(T4_VOL, "VOLUME", 80, F_PCT, 0) X(T4_PAN, "PAN", 64, F_BIP, 0) X(T4_REV, "REV", 96, F_PCT, 0) X(T4_DLY, "DLY", 14, F_PCT, 0) \
    X(T5_VOL, "VOLUME", 82, F_PCT, 0) X(T5_PAN, "PAN", 64, F_BIP, 0) X(T5_REV, "REV", 84, F_PCT, 0) X(T5_DLY, "DLY", 70, F_PCT, 0) \
    /* ---- macros (64 = neutral) ---- */ \
    X(M_SPACE,  "SPACE",  64, F_BIP,  0) X(M_MOTION, "MOTION", 64, F_BIP,  0) \
    X(M_SHIM,   "SHIMMR", 64, F_BIP,  0) X(M_DEPTH,  "DEPTH",  64, F_BIP,  0) \
    /* ---- composition: key, scale, tempo, chords, INFINITY ---- */ \
    X(G_KEY,    "KEY",     0, F_ENUM, E_KEYS) X(G_SCALE, "SCALE", 1, F_ENUM, E_SCALES) \
    X(G_TEMPO,  "TEMPO",  42, F_TEMPO, 0) X(G_MODE,  "MODE",    1, F_ENUM, E_MODES) \
    X(G_CHORD,  "CHORD",   0, F_ENUM, E_CHORDS) X(G_DRIFT,"H.DRIFT",50, F_PCT, 0) \
    X(G_MOOD,   "MOOD",    0, F_ENUM, E_MOODS) X(G_CPLX, "COMPLX", 50, F_PCT, 0) \
    X(G_DENS,   "DENSTY", 50, F_PCT,  0) X(G_EVOL,   "EVOLVE", 50, F_PCT,  0) \
    X(G_RAND,   "RANDOM", 36, F_PCT,  0) X(G_SWING,  "HUMAN",  20, F_PCT,  0) \
    X(G_PULSE,  "PULSE",   0, F_PCT,  0) X(G_REST,   "REST",   10, F_PCT,  0) \
    X(G_BEAT,   "BEAT",    0, F_PCT,  0) X(G_BTONE,  "B.TONE", 50, F_PCT,  0) \
    X(G_PROG,   "CHORDS",  0, F_ENUM, E_PROGS)

#define X_ENUM(id, label, def, fmt, tab) id,
enum { HALO_PARAMS(X_ENUM) NPARAM };
#undef X_ENUM

/* the five logical tracks */
enum { TR_ATMOS, TR_DRONE, TR_SHIM, TR_ARIA, TR_ECHO, NTRACK };
#define TRK_P(t, k) (T1_VOL + 4 * (t) + (k))   /* k: 0 vol, 1 pan, 2 reverb send, 3 delay send */

/* enum tables (texts in halo_params.c) */
enum { E_SHOCT = 1, E_VOWEL, E_VOCT, E_RATIOS, E_ARPS, E_DIVS, E_AOCTS, E_RTYPE, E_DTIME, E_DMODE, E_KEYS, E_SCALES,
       E_MODES, E_CHORDS, E_MOODS, E_MODELS, E_PROGS, NENUMS };
enum { RT_HALL, RT_CATHEDRAL, RT_INFINITE, RT_DARK, RT_SHIMMER };
enum { ARP_OFF, ARP_UP, ARP_DOWN, ARP_UPDOWN, ARP_RANDOM, ARP_CHORD };
enum { DM_TAPE, DM_PINGPONG };
enum { MODE_SEQ, MODE_INFINITY };
enum { PROG_AUTO, PROG_MINE };          /* INFINITY's chords: its own, or the CHORDS page's */
enum { CH_AUTO, CH_MAJ7, CH_MIN7, CH_ADD9, CH_SUS2, CH_SUS4, CH_MIN9, CH_OPEN5, CH_QUARTAL, CH_NOTES, NCHORDS };
enum { MOOD_DREAM, MOOD_CELESTIAL, MOOD_MELANCHOLY, MOOD_DARK, MOOD_SERENE, MOOD_MYSTERIOUS, MOOD_NOSTALGIC,
       MOOD_WISTFUL, MOOD_LANTERN, MOOD_ANALOG, NMOODS };
enum { EM_FM, EM_PIANO, EM_LEAD };      /* ECHO's models: FM bells and plucks, a felt piano, a mono analog lead */
#define NSCALES 10

typedef struct {
    const char *label;
    uint8_t def, fmt, tab;
} param_info_t;

extern const param_info_t PARAM_INFO[NPARAM];
extern const char *const *const ENUM_TEXT[NENUMS];
extern const uint8_t ENUM_COUNT[NENUMS];

/* the state the audio reads; the UI and the composer write single bytes */
extern volatile uint8_t P[NPARAM];

/* presets: a name, a line about it, and the changes to the defaults */
typedef struct { uint8_t id, v; } pv_t;
typedef struct {
    const char *name, *about;
    const pv_t *pv;
    uint8_t n;
} preset_t;
#define NPRESETS 25
extern const preset_t PRESETS[NPRESETS];
void params_defaults(void);
void preset_apply(uint32_t i);
int param_text(uint32_t id, uint32_t v, char *out);   /* the value as shown; returns the length */
uint32_t param_max(uint32_t id);                       /* 127, or the enum's last index */
