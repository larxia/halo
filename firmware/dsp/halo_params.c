/* SPDX-License-Identifier: MPL-2.0
 * HALO — the parameter table, enum texts, value formatting and the 20 factory presets. */
#include "halo_params.h"
#include "halo_comp.h"
#include "fastmath.h"

volatile uint8_t P[NPARAM];

#define X_INFO(id, label, def, fmt, tab) {label, def, fmt, tab},
const param_info_t PARAM_INFO[NPARAM] = {HALO_PARAMS(X_INFO)};
#undef X_INFO

static const char *const T_SHOCT[] = {"+1 OCT", "+2 OCT"};
static const char *const T_VOWEL[] = {"A  AH", "E  EH", "I  EE", "O  OH", "U  OO"};
static const char *const T_VOCT[] = {"LOW", "HIGH"};
static const char *const T_RATIOS[] = {"0.5", "1", "2", "3", "3.5", "4", "5.19", "7"};
static const char *const T_ARPS[] = {"OFF", "UP", "DOWN", "UP DN", "RANDOM", "CHORD"};
static const char *const T_DIVS[] = {"1/2", "1/4", "1/8", "1/8T", "1/16", "1/16T"};
static const char *const T_AOCTS[] = {"1", "2", "3"};
static const char *const T_RTYPE[] = {"HALL", "CATHEDRL", "INFINITE", "DARK", "SHIMMER"};
static const char *const T_DTIME[] = {"1/16", "1/8", "1/8D", "1/4", "1/4D", "1/2", "1/2D", "1 BAR"};
static const char *const T_DMODE[] = {"TAPE", "PINGPONG"};
static const char *const T_KEYS[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const T_MODES[] = {"SEQUENCE", "INFINITY"};
static const char *const T_CHORDS[] = {"AUTO", "MAJ7", "MIN7", "ADD9", "SUS2", "SUS4", "MIN9", "OPEN 5TH", "QUARTAL",
                                       "NOTES"};
static const char *const T_MOODS[] = {"DREAM", "CELESTIAL", "MELANCHOLY", "DARK", "SERENE", "MYSTERIOUS", "NOSTALGIC",
                                      "WISTFUL", "LANTERN", "ANALOG"};
static const char *const T_MODELS[] = {"FM", "PIANO", "LEAD"};

const char *const *const ENUM_TEXT[NENUMS] = {0, T_SHOCT, T_VOWEL, T_VOCT, T_RATIOS, T_ARPS, T_DIVS, T_AOCTS, T_RTYPE,
                                              T_DTIME, T_DMODE, T_KEYS, SCALE_NAME, T_MODES, T_CHORDS, T_MOODS, T_MODELS};
const uint8_t ENUM_COUNT[NENUMS] = {0, 2, 5, 2, 8, 6, 6, 3, 5, 8, 2, 12, NSCALES, 2, NCHORDS, NMOODS, 3};

uint32_t param_max(uint32_t id)
{
    const param_info_t *p = &PARAM_INFO[id];
    if (p->fmt == F_ENUM)
        return ENUM_COUNT[p->tab] - 1u;
    if (p->fmt == F_BOOL)
        return 1;
    return 127;
}

/* --------------------------------------------------------- formatting --- */
static int put_s(char *o, const char *s)
{
    int n = 0;
    while (s[n]) {
        o[n] = s[n];
        n++;
    }
    o[n] = 0;
    return n;
}
static int put_u(char *o, uint32_t v)
{
    char t[12];
    int n = 0, k = 0;
    do
        t[n++] = (char)('0' + v % 10u);
    while ((v /= 10u) && n < 11);
    while (n)
        o[k++] = t[--n];
    o[k] = 0;
    return k;
}
static int put_i(char *o, int32_t v)
{
    if (v < 0) {
        o[0] = '-';
        return 1 + put_u(o + 1, (uint32_t)-v);
    }
    if (v > 0) {
        o[0] = '+';
        return 1 + put_u(o + 1, (uint32_t)v);
    }
    return put_u(o, 0);
}
/* a value with one decimal under 10, none above */
static int put_f(char *o, float v, const char *unit)
{
    int n;
    if (v < 1.0f) {                    /* two decimals under 1: a slow LFO reads 0.04HZ, not 0.0HZ */
        uint32_t t = (uint32_t)(v * 100.0f + 0.5f);
        n = put_u(o, t / 100u);
        o[n++] = '.';
        o[n++] = (char)('0' + (t / 10u) % 10u);
        o[n++] = (char)('0' + t % 10u);
        o[n] = 0;
    } else if (v < 10.0f) {
        uint32_t t = (uint32_t)(v * 10.0f + 0.5f);
        n = put_u(o, t / 10u);
        o[n++] = '.';
        n += put_u(o + n, t % 10u);
    } else {
        n = put_u(o, (uint32_t)(v + 0.5f));
    }
    return n + put_s(o + n, unit);
}

int param_text(uint32_t id, uint32_t v, char *o)
{
    const param_info_t *p = &PARAM_INFO[id];
    float x = (float)v * (1.0f / 127.0f);
    switch (p->fmt) {
    case F_PCT:
        return put_u(o, (v * 100u + 63u) / 127u);
    case F_BIP:
        return put_i(o, (int32_t)v - 64);
    case F_NOTE: {
        int n = put_s(o, T_KEYS[v % 12u]);
        if (v < 12u)
            return n + put_s(o + n, "-1");
        return n + put_u(o + n, v / 12u - 1u);
    }
    case F_TIME: {
        float s = 0.003f * fm_exp2f(x * 12.7f) * 0.35f;   /* the engines' envelopes: about this */
        if (s < 1.0f) {
            int n = put_u(o, (uint32_t)(s * 1000.0f + 0.5f));
            return n + put_s(o + n, "MS");
        }
        return put_f(o, s, "S");
    }
    case F_HZ:
        return put_f(o, 0.01f * fm_exp2f(x * 10.0f), "HZ");
    case F_CUT: {
        float hz = 30.0f * fm_exp2f(x * 9.3f);
        return hz < 1000.0f ? put_f(o, hz, "") : put_f(o, hz * 0.001f, "K");
    }
    case F_ENUM: {
        uint32_t c = ENUM_COUNT[p->tab];
        return put_s(o, ENUM_TEXT[p->tab][v < c ? v : c - 1u]);
    }
    case F_TEMPO:
        return put_u(o, 30u + v);
    case F_BOOL:
        return put_s(o, v ? "ON" : "OFF");
    default:
        return put_u(o, v);
    }
}

void params_defaults(void)
{
    uint32_t i;
    for (i = 0; i < NPARAM; i++)
        P[i] = PARAM_INFO[i].def;
}

/* ------------------------------------------------------------- presets --- */
/* Each preset is the defaults plus these changes: synthesis, effects, mix, and the composer's mood, key, scale and
 * tempo, so that PLAY in INFINITY mode plays the atmosphere it is named for. Keys: C=0 .. B=11; scales: 0 MAJOR,
 * 1 MINOR, 2 DORIAN, 3 LYDIAN, 4 MIXOLYD, 5 PHRYGIAN, 6 PENT MAJ, 7 PENT MIN, 8 HARM MIN, 9 HIRAJOSHI. */
#define PV(...) (const pv_t[]){__VA_ARGS__}, sizeof((const pv_t[]){__VA_ARGS__}) / sizeof(pv_t)

const preset_t PRESETS[NPRESETS] = {
    {"PALE HORIZON", "AIRY LYDIAN PADS, GLASS BELLS, A WIDE HALL",
     PV({G_MOOD, MOOD_DREAM}, {G_KEY, 2}, {G_SCALE, 3}, {G_TEMPO, 40}, {A_SHAPE, 70}, {A_DETUNE, 40}, {A_AIR, 44},
        {A_CUT, 86}, {A_ATK, 88}, {E_RATIO, 2}, {E_INDEX, 30}, {E_DEC, 82}, {L_TIME, 4}, {L_MODE, DM_PINGPONG},
        {R_TYPE, RT_HALL}, {R_SIZE, 96}, {R_DECAY, 96}, {S_INT, 56})},
    {"GLASS CATHEDRAL", "BELLS AND SHIMMER IN A VAST STONE ROOM",
     PV({G_MOOD, MOOD_CELESTIAL}, {G_KEY, 4}, {G_SCALE, 0}, {G_TEMPO, 30}, {R_TYPE, RT_CATHEDRAL}, {R_DECAY, 104},
        {E_RATIO, 4}, {E_INDEX, 56}, {E_IDEC, 70}, {E_DEC, 92}, {E_TONE, 96}, {S_INT, 80}, {S_BRIGHT, 84},
        {A_SHAPE, 30}, {A_CUT, 62}, {T4_VOL, 56}, {T5_VOL, 92}, {T5_REV, 110}, {G_DENS, 40})},
    {"INFINITE BLOOM", "A FROZEN, SHIMMERING TAIL THAT NEVER ENDS",
     PV({G_MOOD, MOOD_CELESTIAL}, {G_KEY, 7}, {G_SCALE, 3}, {G_TEMPO, 24}, {R_TYPE, RT_INFINITE}, {R_DECAY, 110},
        {R_SHIM, 64}, {S_INT, 90}, {S_FDBK, 60}, {A_ATK, 100}, {A_REL, 108}, {A_AIR, 60}, {G_EVOL, 30}, {G_DENS, 24},
        {T1_REV, 110}, {M_SPACE, 90})},
    {"DISTANT VOICES", "A WORDLESS CHOIR, FAR AWAY, OH TO AH",
     PV({G_MOOD, MOOD_MELANCHOLY}, {G_KEY, 9}, {G_SCALE, 1}, {G_TEMPO, 30}, {V_VOWEL, 3}, {V_DRIFT, 70},
        {V_ENS, 80}, {V_BREATH, 44}, {T4_VOL, 104}, {T4_REV, 120}, {R_TYPE, RT_CATHEDRAL}, {A_CUT, 56},
        {T1_VOL, 70}, {T5_VOL, 50}, {V_BRIGHT, 40})},
    {"VELVET ATMOSPHERE", "WARM ANALOG PADS, CHORUS AND SOFT TAPE",
     PV({G_MOOD, MOOD_NOSTALGIC}, {G_KEY, 5}, {G_SCALE, 0}, {G_TEMPO, 46}, {A_SHAPE, 118}, {A_DETUNE, 52},
        {A_CUT, 56}, {A_RES, 34}, {A_ENV, 84}, {A_SUB, 30}, {C_DEPTH, 74}, {X_SAT, 40}, {L_WOW, 60}, {S_INT, 30},
        {R_SIZE, 70}, {R_DAMP, 70})},
    {"CELESTIAL DRIFT", "SUS2 HARMONY THAT RE-VOICES ITSELF SLOWLY",
     PV({G_MOOD, MOOD_CELESTIAL}, {G_KEY, 0}, {G_SCALE, 0}, {G_TEMPO, 28}, {G_CHORD, CH_SUS2}, {G_DRIFT, 110},
        {R_TYPE, RT_SHIMMER}, {S_INT, 76}, {S_SPARK, 70}, {A_MORPH, 70}, {A_DRIFT, 60}, {M_MOTION, 80})},
    {"HOLLOW MOON", "TRIANGLE PADS, A FIFTH, A SLOW PHASER",
     PV({G_MOOD, MOOD_MYSTERIOUS}, {G_KEY, 11}, {G_SCALE, 2}, {G_TEMPO, 26}, {A_SHAPE, 64}, {A_AIR, 10},
        {D_FIFTH, 80}, {D_DENS, 40}, {T2_VOL, 96}, {H_AMT, 70}, {H_RATE, 8}, {R_TYPE, RT_DARK}, {S_INT, 40})},
    {"DREAM SEQUENCE", "AN ARPEGGIO THROUGH PING-PONG ECHOES",
     PV({G_MOOD, MOOD_DREAM}, {G_KEY, 2}, {G_SCALE, 3}, {G_TEMPO, 62}, {E_ARP, ARP_CHORD}, {E_ARATE, 4},
        {E_AOCT, 1}, {E_DEC, 54}, {E_INDEX, 50}, {L_MODE, DM_PINGPONG}, {L_TIME, 3}, {T5_DLY, 96}, {G_DENS, 30},
        {T5_VOL, 74})},
    {"PINK NEBULA", "LYDIAN SHIMMER, CHORUSED AIR, A SOFT HISS",
     PV({G_MOOD, MOOD_DREAM}, {G_KEY, 8}, {G_SCALE, 3}, {G_TEMPO, 36}, {S_INT, 96}, {S_BRIGHT, 90},
        {C_DEPTH, 60}, {X_NOISE, 34}, {X_NTONE, 90}, {X_NMOVE, 80}, {A_AIR, 64}, {A_SHAPE, 56},
        {R_TYPE, RT_SHIMMER}, {M_SHIM, 96}, {X_EXCITE, 30})},
    {"FROZEN MEMORIES", "GLASSY MINOR PADS, WOW AND FLUTTER, DUST",
     PV({G_MOOD, MOOD_MELANCHOLY}, {G_KEY, 1}, {G_SCALE, 1}, {G_TEMPO, 32}, {A_SHAPE, 26}, {A_AIR, 54},
        {L_WOW, 100}, {L_FDBK, 84}, {X_NOISE, 28}, {X_NTONE, 30}, {X_SAT, 36}, {E_RATIO, 5}, {T5_DLY, 100})},
    {"SOFT COLLAPSE", "A DARK DRONE THAT BREATHES AND BUCKLES",
     PV({G_MOOD, MOOD_DARK}, {G_KEY, 4}, {G_SCALE, 5}, {G_TEMPO, 20}, {D_GRIT, 70}, {D_WOBBLE, 96},
        {D_DENS, 96}, {D_SUB, 90}, {T2_VOL, 110}, {R_TYPE, RT_DARK}, {A_CUT, 44}, {A_SHAPE, 100}, {G_DENS, 16},
        {S_INT, 20}, {M_DEPTH, 100})},
    {"LUNAR CHOIR", "A HUSHED ENSEMBLE: OO DRIFTING TOWARD AH",
     PV({G_MOOD, MOOD_SERENE}, {G_KEY, 7}, {G_SCALE, 6}, {G_TEMPO, 28}, {V_VOWEL, 4}, {V_DRIFT, 90},
        {V_DRATE, 14}, {V_ENS, 100}, {T4_VOL, 110}, {T1_VOL, 64}, {R_TYPE, RT_CATHEDRAL}, {V_FORM, 64})},
    {"ENDLESS RAIN", "MINOR PLUCKS THROUGH TAPE, A RAIN LAYER",
     PV({G_MOOD, MOOD_MELANCHOLY}, {G_KEY, 2}, {G_SCALE, 7}, {G_TEMPO, 52}, {X_NOISE, 48}, {X_NTONE, 108},
        {X_NMOVE, 50}, {E_RATIO, 1}, {E_DEC, 50}, {E_INDEX, 20}, {G_DENS, 86}, {L_MODE, DM_TAPE}, {L_WOW, 70},
        {T5_DLY, 90})},
    {"HARMONIC DUST", "GRAINS AND SPARKS ABOVE A SLOW CHORD",
     PV({G_MOOD, MOOD_MYSTERIOUS}, {G_KEY, 9}, {G_SCALE, 2}, {G_TEMPO, 34}, {D_GRAIN, 110}, {D_MOTION, 80},
        {S_SPARK, 110}, {S_INT, 84}, {G_DENS, 28}, {G_CPLX, 80}, {E_RATIO, 6}, {E_INDEX, 24})},
    {"GHOST SIGNAL", "METAL RESONANCE, TREMOLO, A FAR SIGNAL",
     PV({G_MOOD, MOOD_DARK}, {G_KEY, 6}, {G_SCALE, 8}, {G_TEMPO, 40}, {E_RATIO, 6}, {E_INDEX, 80}, {A_RES, 96},
        {A_CUT, 50}, {A_LFOD, 70}, {T_DEPTH, 60}, {T_RATE, 70}, {H_AMT, 50}, {X_NOISE, 20}, {R_TYPE, RT_DARK},
        {V_VOWEL, 4}, {T4_VOL, 70})},
    {"FLOATING GARDENS", "PENTATONIC BELLS, PING-PONG, LIGHT AS AIR",
     PV({G_MOOD, MOOD_SERENE}, {G_KEY, 5}, {G_SCALE, 6}, {G_TEMPO, 48}, {E_RATIO, 2}, {E_INDEX, 44},
        {L_MODE, DM_PINGPONG}, {T5_DLY, 84}, {G_DENS, 60}, {A_SHAPE, 36}, {S_INT, 60}, {D_DENS, 30})},
    {"MIDNIGHT ARIA", "A SOLO VOICE ON AH OVER A DARK MINOR HALL",
     PV({G_MOOD, MOOD_MELANCHOLY}, {G_KEY, 11}, {G_SCALE, 1}, {G_TEMPO, 26}, {V_VOWEL, 0}, {V_DRIFT, 20},
        {V_VIB, 54}, {V_ENS, 24}, {T4_VOL, 116}, {R_TYPE, RT_DARK}, {R_DECAY, 104}, {A_CUT, 52}, {G_DENS, 20})},
    {"SILENT GEOMETRY", "QUARTAL HARMONY, EUCLIDEAN POLYRHYTHMS",
     PV({G_MOOD, MOOD_MYSTERIOUS}, {G_KEY, 0}, {G_SCALE, 2}, {G_TEMPO, 58}, {G_CHORD, CH_QUARTAL},
        {G_CPLX, 112}, {G_DENS, 92}, {G_RAND, 20}, {E_DEC, 40}, {E_RATIO, 3}, {L_TIME, 2}, {A_SHAPE, 20},
        {S_INT, 40})},
    {"LOST FREQUENCIES", "HARMONIC MINOR TENSION, GRIT AND STATIC",
     PV({G_MOOD, MOOD_DARK}, {G_KEY, 3}, {G_SCALE, 8}, {G_TEMPO, 30}, {D_GRIT, 50}, {D_WOBBLE, 70},
        {X_NOISE, 40}, {X_NTONE, 20}, {E_RATIO, 7}, {E_INDEX, 70}, {X_SAT, 60}, {R_TYPE, RT_DARK},
        {T_DEPTH, 30}, {T_RATE, 20}, {M_DEPTH, 90})},
    {"AFTERGLOW", "WARM MAJ7 CHORDS, TAPE ECHOES, LAST LIGHT",
     PV({G_MOOD, MOOD_NOSTALGIC}, {G_KEY, 10}, {G_SCALE, 0}, {G_TEMPO, 44}, {G_CHORD, CH_MAJ7}, {A_SHAPE, 96},
        {A_CUT, 64}, {C_DEPTH, 50}, {L_WOW, 54}, {X_SAT, 28}, {E_RATIO, 1}, {S_INT, 50})},
    /* 21-24: the Minecraft-era palette (techniques only, no borrowed melodies): a warm analog lead, rhythmic
     * pentatonic ostinatos, a felt piano with long silences */
    {"WARM CIRCUIT", "AN ANALOG LEAD GLIDING OVER MINOR CHORDS",
     PV({G_MOOD, MOOD_ANALOG}, {G_KEY, 4}, {G_SCALE, 1}, {G_TEMPO, 36}, {G_PULSE, 0}, {G_REST, 40}, {G_BEAT, 0},
        {G_DENS, 54}, {G_CPLX, 30}, {E_MODEL, EM_LEAD}, {E_RATIO, 2}, {E_TONE, 58}, {E_INDEX, 52}, {E_IDEC, 74},
        {E_RES, 34}, {E_GLIDE, 52}, {E_VIB, 44}, {E_ATK, 34}, {E_DEC, 96}, {E_SUS, 92}, {E_REL, 74}, {T5_VOL, 90},
        {T5_DLY, 40}, {L_TIME, 4}, {L_FDBK, 50}, {A_SHAPE, 92}, {A_CUT, 64}, {T1_VOL, 72}, {T2_VOL, 50},
        {S_INT, 24}, {T4_VOL, 36}, {R_SIZE, 70}, {R_DECAY, 80}, {X_WARBLE, 50}, {X_SAT, 32}, {C_DEPTH, 30})},
    {"PAPER LANTERNS", "TURNING PENTATONIC BELLS, PADS, A CHOIR",
     PV({G_MOOD, MOOD_LANTERN}, {G_KEY, 1}, {G_SCALE, 6}, {G_TEMPO, 50}, {G_PULSE, 100}, {G_BEAT, 30},
        {G_REST, 10}, {G_DENS, 50}, {G_CPLX, 56}, {E_MODEL, EM_FM}, {E_RATIO, 2}, {E_INDEX, 54}, {E_DEC, 60},
        {E_SUS, 0}, {E_REL, 50}, {T5_VOL, 104}, {T5_DLY, 50}, {L_TIME, 2}, {L_MODE, DM_PINGPONG}, {A_DETUNE, 50},
        {A_AIR, 50}, {A_CUT, 70}, {T1_VOL, 78}, {V_VOWEL, 0}, {T4_VOL, 70}, {S_INT, 70}, {R_DECAY, 96}, {T2_VOL, 38},
        {D_SUB, 30})},
    {"RIVER LIGHTS", "SOFT PLUCKS, A QUIET BEAT, WARM PADS",
     PV({G_MOOD, MOOD_LANTERN}, {G_KEY, 3}, {G_SCALE, 7}, {G_TEMPO, 60}, {G_PULSE, 84}, {G_BEAT, 80},
        {G_BTONE, 40}, {G_REST, 16}, {G_DENS, 70}, {G_CPLX, 70}, {E_MODEL, EM_FM}, {E_RATIO, 1}, {E_INDEX, 30},
        {E_DEC, 44}, {E_SUS, 0}, {T5_DLY, 44}, {L_TIME, 1}, {L_MODE, DM_PINGPONG}, {A_SHAPE, 100}, {A_CUT, 70},
        {A_LFOD, 50}, {T2_VOL, 42}, {D_SUB, 24}, {S_INT, 50}, {X_WARBLE, 20})},
    {"SOFT KEYS", "FELT PIANO, PLAIN CHORDS, LONG SILENCES",
     PV({G_MOOD, MOOD_WISTFUL}, {G_KEY, 7}, {G_SCALE, 0}, {G_TEMPO, 44}, {G_REST, 80}, {G_PULSE, 0}, {G_BEAT, 0},
        {G_DENS, 36}, {G_CPLX, 24}, {G_CHORD, CH_AUTO}, {E_MODEL, EM_PIANO}, {E_INDEX, 40}, {E_IDEC, 62},
        {E_DEC, 96}, {E_REL, 60}, {E_TONE, 84}, {T5_VOL, 96}, {T5_DLY, 16}, {T5_REV, 70}, {T1_VOL, 56},
        {A_CUT, 60}, {A_AIR, 20}, {T2_VOL, 0}, {S_INT, 30}, {T4_VOL, 50}, {R_SIZE, 76}, {R_DECAY, 84},
        {X_WARBLE, 34}, {X_NOISE, 18}, {X_NTONE, 30}, {X_SAT, 20}, {X_MASTER, 116})},
};

void preset_apply(uint32_t i)
{
    uint32_t k;
    const preset_t *p = &PRESETS[i % NPRESETS];
    params_defaults();
    for (k = 0; k < p->n; k++)
        if (p->pv[k].id < NPARAM)
            P[p->pv[k].id] = p->pv[k].v;
}
