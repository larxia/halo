/* SPDX-License-Identifier: MPL-2.0
 * HALO — the composer. See halo_comp.h. Runs in the audio interrupt: no flash writes, no waiting, no doubles. */
#include "halo_comp.h"
#include "halo_dsp.h"
#include "fastmath.h"
#include "plat.h"

#define PPQ 24u                        /* clock ticks per quarter note (MIDI clock's resolution) */
#define BAR (PPQ * 4u)
#define T16 (PPQ / 4u)

song_t song;
volatile uint8_t comp_playing, comp_edit_pat[NTRACK], comp_play_pat[NTRACK], comp_step[NTRACK];
volatile int32_t comp_chord[4];
volatile uint8_t comp_chord_n, comp_chord_deg;
volatile uint32_t comp_bar, comp_beat16;
volatile float comp_energy;
volatile uint8_t comp_prog_i = 0xFFu;
volatile uint8_t comp_section;
volatile uint32_t comp_note_flash[NTRACK];
volatile uint8_t comp_held_mask;
volatile int8_t comp_chord_root_pc = -1, comp_chord_type;

/* ---------------------------------------------------------------- random --- */
static uint32_t crng = 0x2545F491u;
static uint32_t crnd(void)
{
    uint32_t x = crng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return crng = x;
}
static float crndf(void) { return (float)(crnd() >> 8) * (1.0f / 16777216.0f); }
static uint32_t crange(uint32_t n) { return n ? crnd() % n : 0u; }
static inline float u7(uint32_t id) { return (float)P[id] * (1.0f / 127.0f); }

/* ---------------------------------------------------------------- scales --- */
typedef struct { uint8_t n, iv[7]; } scale_t;
static const scale_t SCALES[NSCALES] = {
    {7, {0, 2, 4, 5, 7, 9, 11}},       /* MAJOR */
    {7, {0, 2, 3, 5, 7, 8, 10}},       /* MINOR (aeolian) */
    {7, {0, 2, 3, 5, 7, 9, 10}},       /* DORIAN */
    {7, {0, 2, 4, 6, 7, 9, 11}},       /* LYDIAN */
    {7, {0, 2, 4, 5, 7, 9, 10}},       /* MIXOLYDIAN */
    {7, {0, 1, 3, 5, 7, 8, 10}},       /* PHRYGIAN */
    {5, {0, 2, 4, 7, 9}},              /* PENTATONIC MAJOR */
    {5, {0, 3, 5, 7, 10}},             /* PENTATONIC MINOR */
    {7, {0, 2, 3, 5, 7, 8, 11}},       /* HARMONIC MINOR */
    {5, {0, 2, 3, 7, 8}},              /* HIRAJOSHI */
};
const char *const SCALE_NAME[NSCALES] = {"MAJOR", "MINOR", "DORIAN", "LYDIAN", "MIXOLYD", "PHRYGIAN", "PENT MAJ",
                                         "PENT MIN", "HARM MIN", "HIRAJOSHI"};

static const scale_t *scale(void) { return &SCALES[P[G_SCALE] < NSCALES ? P[G_SCALE] : 0]; }
static int32_t key(void) { return P[G_KEY] % 12u; }

static int in_scale(int32_t note)
{
    const scale_t *s = scale();
    int32_t pc = ((note - key()) % 12 + 12) % 12;
    uint32_t i;
    for (i = 0; i < s->n; i++)
        if (s->iv[i] == pc)
            return 1;
    return 0;
}

int32_t scale_quantize(int32_t note)
{
    int32_t d;
    for (d = 0; d < 7; d++) {          /* the nearest, preferring below */
        if (in_scale(note - d))
            return note - d;
        if (in_scale(note + d))
            return note + d;
    }
    return note;
}

/* a scale degree (any integer: negative and past an octave) as a MIDI note above base (a C) */
static int32_t degree_note(int32_t deg, int32_t base)
{
    const scale_t *s = scale();
    int32_t n = (int32_t)s->n, oct = deg >= 0 ? deg / n : -((-deg + n - 1) / n);
    return base + key() + s->iv[deg - oct * n] + 12 * oct;
}

/* the degree of a note in the scale (rounded down to a scale tone), relative to base */
static int32_t note_degree(int32_t note, int32_t base)
{
    const scale_t *s = scale();
    int32_t q = scale_quantize(note) - base - key(), oct = q >= 0 ? q / 12 : -((-q + 11) / 12), pc = q - oct * 12;
    uint32_t i;
    for (i = 0; i < s->n; i++)
        if (s->iv[i] == pc)
            return (int32_t)i + oct * (int32_t)s->n;
    return oct * (int32_t)s->n;
}

/* ---------------------------------------------------------------- chords --- */
static const int8_t CHORD_IV[NCHORDS][4] = {
    {0, 0, 0, 0},                      /* AUTO: stacked from the scale (below) */
    {0, 4, 7, 11},                     /* MAJ7 */
    {0, 3, 7, 10},                     /* MIN7 */
    {0, 4, 7, 14},                     /* ADD9 */
    {0, 2, 7, 12},                     /* SUS2 */
    {0, 5, 7, 12},                     /* SUS4 */
    {0, 3, 10, 14},                    /* MIN9 (no fifth: a pad's voicing) */
    {0, 7, 12, 19},                    /* OPEN FIFTHS */
    {0, 5, 10, 15},                    /* QUARTAL */
    {0, 0, 0, 0},                      /* NOTES: no chord */
};

static int32_t voicing[4], voicing_n;  /* the pad's notes now (voice order kept for gliding) */
static int32_t chord_root_note, chord_type_now;
static uint32_t chord_gen;             /* counts chords played: a scheduled release only ends its own chord */
static uint8_t shim_follow;            /* the shimmer follows the pad's chords */

/* pitch classes of a chord on a root note */
static uint32_t chord_pcs(int32_t root, uint32_t type, int32_t *pc)
{
    uint32_t i;
    if (type == CH_NOTES) {
        pc[0] = root;
        return 1;
    }
    if (type == CH_AUTO) {             /* the scale's own chord: every other scale tone */
        int32_t d = note_degree(root, 0);
        for (i = 0; i < 4u; i++)
            pc[i] = degree_note(d + 2 * (int32_t)i, 0);
        return 4;
    }
    for (i = 0; i < 4u; i++)
        pc[i] = root + CHORD_IV[type][i];
    return 4;
}

static int chord_fits(int32_t root, uint32_t type)
{
    int32_t pc[4];
    uint32_t i, n = chord_pcs(root, type, pc);
    for (i = 0; i < n; i++)
        if (!in_scale(pc[i]))
            return 0;
    return 1;
}

static int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

/* voice leading: the chord's tones placed so the voices move least from where they are, inside lo..hi */
static uint32_t lead(const int32_t *pc, uint32_t n, int32_t lo, int32_t hi, int32_t *out)
{
    static const uint8_t PERM[24][4] = {
        {0, 1, 2, 3}, {0, 1, 3, 2}, {0, 2, 1, 3}, {0, 2, 3, 1}, {0, 3, 1, 2}, {0, 3, 2, 1}, {1, 0, 2, 3}, {1, 0, 3, 2},
        {1, 2, 0, 3}, {1, 2, 3, 0}, {1, 3, 0, 2}, {1, 3, 2, 0}, {2, 0, 1, 3}, {2, 0, 3, 1}, {2, 1, 0, 3}, {2, 1, 3, 0},
        {2, 3, 0, 1}, {2, 3, 1, 0}, {3, 0, 1, 2}, {3, 0, 2, 1}, {3, 1, 0, 2}, {3, 1, 2, 0}, {3, 2, 0, 1}, {3, 2, 1, 0}};
    int32_t best[4] = {0, 0, 0, 0}, bestc = 0x7FFFFFFF, center = (lo + hi) / 2;
    uint32_t p, i, j;
    if (n > 4u)
        n = 4u;
    if (!voicing_n) {                  /* nothing sounding: a close voicing from the root upward, near the middle */
        int32_t b = pc[0];
        while (b < center - 9)
            b += 12;
        while (b > center - 3)
            b -= 12;
        out[0] = b;
        for (i = 1; i < n; i++) {
            int32_t v = pc[i];
            while (v <= out[i - 1])
                v += 12;
            while (v - 12 > out[i - 1])
                v -= 12;
            out[i] = v;
        }
        return n;
    }
    for (p = 0; p < 24u; p++) {
        int32_t cand[4], c = 0;
        int skip = 0;
        for (i = 0; i < n; i++) {
            if (PERM[p][i] >= n) {
                skip = 1;
                break;
            }
        }
        if (skip)
            continue;
        for (i = 0; i < n; i++) {
            int32_t ref = voicing[i < (uint32_t)voicing_n ? i : (uint32_t)voicing_n - 1u], t = pc[PERM[p][i]];
            int32_t d = ((t - ref) % 12 + 12) % 12;
            int32_t v = d > 6 ? ref + d - 12 : ref + d;
            while (v < lo)
                v += 12;
            while (v > hi)
                v -= 12;
            cand[i] = v;
            c += iabs(v - ref);
        }
        for (i = 0; i < n; i++)
            for (j = i + 1u; j < n; j++) {
                int32_t g = iabs(cand[i] - cand[j]);
                if (g == 0)
                    c += 8;            /* doubled: a lost voice */
                else if (g <= 2 && (cand[i] < 55 || cand[j] < 55))
                    c += 5;            /* a low cluster: mud */
            }
        if (c < bestc) {
            bestc = c;
            for (i = 0; i < n; i++)
                best[i] = cand[i];
        }
    }
    for (i = 0; i < n; i++)
        out[i] = best[i];
    return n;
}

uint32_t chord_build(int32_t root, uint32_t type, int32_t *out)
{
    int32_t pc[4];
    uint32_t n = chord_pcs(root, type, pc);
    return lead(pc, n, 48, 76, out);
}

static void send_chord(const int32_t *notes, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n && i < 4u; i++) {
        voicing[i] = notes[i];
        comp_chord[i] = notes[i];
    }
    voicing_n = (int32_t)n;
    comp_chord_n = (uint8_t)n;
    dsp_pad_chord(notes, n);
    dsp_shim_chord(notes, shim_follow ? n : 0u);
    comp_note_flash[TR_ATMOS]++;
    if (shim_follow)
        comp_note_flash[TR_SHIM]++;
}

static void play_chord(int32_t root, uint32_t type, int shimmer)
{
    int32_t pc[4], out[4];
    uint32_t n = chord_pcs(root, type, pc);
    chord_root_note = root;
    chord_type_now = (int32_t)type;
    shim_follow = (uint8_t)(shimmer != 0);
    chord_gen++;
    comp_chord_root_pc = (int8_t)(((root % 12) + 12) % 12);
    comp_chord_type = (int8_t)type;
    if (type == CH_AUTO && n == 4u) {  /* name the scale's own chord by what it is */
        int32_t a = ((pc[1] - pc[0]) % 12 + 12) % 12, b = ((pc[2] - pc[0]) % 12 + 12) % 12,
                c = ((pc[3] - pc[0]) % 12 + 12) % 12;
        comp_chord_type = a == 4 && b == 7 && c == 11 ? CH_MAJ7 : a == 3 && b == 7 && c == 10 ? CH_MIN7
                          : a == 4 && b == 7 && c == 10 ? 10 : a == 3 && b == 6 && c == 10 ? 11
                          : a == 3 && b == 7 && c == 11 ? 12 : a == 4 && b == 8 && c == 11 ? 13 : 14;
    }
    comp_chord_deg = (uint8_t)((note_degree(root, 0) % (int32_t)scale()->n + (int32_t)scale()->n) % (int32_t)scale()->n);
    n = lead(pc, n, 48, 76, out);
    send_chord(out, n);
}

static void chord_release(void)
{
    comp_chord_root_pc = -1;
    voicing_n = 0;
    comp_chord_n = 0;
    dsp_pad_chord(0, 0);
    dsp_shim_chord(0, 0);
}

/* HARMONIC DRIFT: one voice steps to a neighbouring colour tone of the chord (its 9th, 11th or 6th, if the scale
 * has them) or back to a chord tone, never into a semitone rub with another voice (DARK allows it) */
static void drift_step(int dark)
{
    int32_t colour[8], nc = 0, pc[4], i, v, best = -1, cand[8], ncand = 0;
    uint32_t n, k;
    if (voicing_n < 2)
        return;
    n = chord_pcs(chord_root_note, (uint32_t)chord_type_now, pc);
    for (k = 0; k < n; k++)
        colour[nc++] = ((pc[k] % 12) + 12) % 12;
    {
        static const int8_t EXT[3] = {2, 5, 9};
        for (k = 0; k < 3u; k++)
            if (in_scale(chord_root_note + EXT[k]))
                colour[nc++] = ((chord_root_note + EXT[k]) % 12 + 12) % 12;
    }
    v = (int32_t)crange((uint32_t)voicing_n);
    for (i = -2; i <= 2; i++) {
        int32_t t = voicing[v] + i, tpc = ((t % 12) + 12) % 12, ok = 0, j;
        if (!i)
            continue;
        for (j = 0; j < nc; j++)
            ok |= colour[j] == tpc;
        if (!ok || !in_scale(t))
            continue;
        for (j = 0; j < voicing_n; j++)
            if (j != v && (iabs(voicing[j] - t) == 0 || (!dark && iabs(voicing[j] - t) == 1)))
                ok = 0;
        if (ok)
            cand[ncand++] = t;
    }
    if (!ncand)
        return;
    best = cand[crange((uint32_t)ncand)];
    {
        int32_t nv[4];
        for (i = 0; i < voicing_n; i++)
            nv[i] = voicing[i];
        nv[v] = best;
        send_chord(nv, (uint32_t)voicing_n);
    }
}

/* ----------------------------------------------------------- note-offs --- */
#define NPEND 32u
#define PEND_CHORD 0xFEu               /* a pending release of the pad's chord (note: its chord_gen, low 15 bits) */
static struct { uint8_t on, track; int16_t note; uint32_t ticks; } pend[NPEND];

static void chord_release(void);
static void pend_add(uint32_t track, int32_t note, uint32_t ticks);

static void note_for(uint32_t track, int32_t note, uint32_t vel, uint32_t ticks)
{
    dsp_note_on(track, note, vel);
    comp_note_flash[track]++;
    pend_add(track, note, ticks);
}

static void pend_add(uint32_t track, int32_t note, uint32_t ticks)
{
    uint32_t i;
    if (!ticks)
        return;
    for (i = 0; i < NPEND; i++)
        if (!pend[i].on) {
            pend[i].on = 1;
            pend[i].track = (uint8_t)track;
            pend[i].note = (int16_t)note;
            pend[i].ticks = ticks;
            return;
        }
    if (track == PEND_CHORD)
        return;
    dsp_note_off(track, note);         /* no room: a short note rather than a stuck one */
}

static void pend_tick(void)
{
    uint32_t i;
    for (i = 0; i < NPEND; i++)
        if (pend[i].on && !--pend[i].ticks) {
            pend[i].on = 0;
            if (pend[i].track == PEND_CHORD) {
                if ((uint32_t)pend[i].note == (chord_gen & 0x7FFFu))
                    chord_release();
            } else {
                dsp_note_off(pend[i].track, pend[i].note);
            }
        }
}

static void pend_clear(void)
{
    uint32_t i;
    for (i = 0; i < NPEND; i++)
        if (pend[i].on) {
            pend[i].on = 0;
            if (pend[i].track != PEND_CHORD)
                dsp_note_off(pend[i].track, pend[i].note);
        }
}

/* ---------------------------------------------------------------- moods --- */
typedef struct {
    uint8_t scale, tempo;              /* suggested (the UI sets them when the mood is chosen) */
    uint16_t chords;                   /* bit CH_*: the chord colours this mood reaches for */
    const char *w;                     /* 7 x 7 transition weights, degree to degree, '0'..'9' */
    int8_t mel_oct;                    /* the melody's register: octaves from C5 */
    uint8_t dark;                      /* semitone rubs allowed in the drift */
    uint8_t aria;                      /* 0..9: how often the choir enters */
    uint8_t lift;                      /* 0..9: how often the drone moves with the harmony */
} mood_t;
static const mood_t MOODS[NMOODS] = {
    /* DREAM: lydian, maj7 and add9, I-vi-IV gravitation */
    {3, 38, (1u << CH_MAJ7) | (1u << CH_ADD9) | (1u << CH_SUS2) | (1u << CH_AUTO),
     "1125250" "3002230" "1000531" "5122121" "4011203" "2232501" "3020000", 0, 0, 5, 2},
    /* CELESTIAL: major, sus2, quartal and open fifths, slow I-IV-V-I light */
    {0, 30, (1u << CH_SUS2) | (1u << CH_ADD9) | (1u << CH_QUARTAL) | (1u << CH_OPEN5),
     "1104420" "2003320" "1001520" "6110311" "5011022" "3012400" "2010000", 1, 0, 6, 1},
    /* MELANCHOLY: minor, i-VI-III-VII, min9 and maj7 */
    {1, 34, (1u << CH_MIN9) | (1u << CH_MIN7) | (1u << CH_MAJ7) | (1u << CH_AUTO) | (1u << CH_ADD9),
     "1112352" "2002120" "2010334" "5101213" "4100103" "3022104" "5021200", 0, 0, 5, 3},
    /* DARK: phrygian, i-bII, open fifths and quartal, low */
    {5, 22, (1u << CH_OPEN5) | (1u << CH_QUARTAL) | (1u << CH_SUS4) | (1u << CH_MIN7),
     "1600322" "6000101" "3100110" "4001220" "3100130" "3112002" "4200100", -1, 1, 4, 4},
    /* SERENE: pentatonic major, sus2 and add9, I-IV-I */
    {6, 32, (1u << CH_SUS2) | (1u << CH_ADD9) | (1u << CH_OPEN5) | (1u << CH_AUTO),
     "1215500" "3002200" "2100300" "6110200" "5121000" "0000000" "0000000", 0, 0, 3, 1},
    /* MYSTERIOUS: dorian, i-IV-bVII, sus4 and quartal, min9 */
    {2, 30, (1u << CH_SUS4) | (1u << CH_QUARTAL) | (1u << CH_MIN9) | (1u << CH_OPEN5),
     "1203520" "3001132" "2100314" "4201103" "3111024" "2112201" "5112110", 0, 0, 6, 3},
    /* NOSTALGIC: mixolydian, I-iii-IV-bVII, maj7 and min7 */
    {4, 40, (1u << CH_MAJ7) | (1u << CH_MIN7) | (1u << CH_ADD9) | (1u << CH_AUTO),
     "1134314" "3002320" "2100531" "5212102" "3211002" "2232200" "5101210", 0, 0, 4, 2},
    /* WISTFUL: gentle, sparse major-key piano: plain triads and add9, I-IV-vi-V, long rests, little choir */
    {0, 44, (1u << CH_AUTO) | (1u << CH_ADD9) | (1u << CH_SUS2) | (1u << CH_MAJ7),
     "1115430" "2003310" "1100430" "6211211" "4101002" "2122401" "3020000", 0, 0, 1, 1},
    /* LANTERN: pentatonic, rhythmic and bright: ostinatos over lush pads, sus and add9 colours */
    {6, 52, (1u << CH_SUS2) | (1u << CH_ADD9) | (1u << CH_AUTO) | (1u << CH_OPEN5),
     "1225400" "3002300" "2100400" "5220200" "4321000" "0000000" "0000000", 0, 0, 3, 2},
    /* ANALOG: warm minor, a singing lead over soft chords: i-VI-III-VII and i-iv */
    {1, 40, (1u << CH_AUTO) | (1u << CH_MIN7) | (1u << CH_ADD9) | (1u << CH_MAJ7),
     "1114343" "2002120" "2110323" "5101212" "3110103" "4122103" "4121200", 0, 0, 1, 2},
};
/* what the UI sets when a mood is chosen */
const uint8_t MOOD_SCALE[NMOODS] = {3, 0, 1, 5, 6, 2, 4, 0, 6, 1};
const uint8_t MOOD_TEMPO[NMOODS] = {38, 30, 34, 22, 32, 30, 40, 44, 52, 40};
/* and the rhythm row's starting points: PULSE (ostinato), REST (silences), BEAT (the soft beat) */
const uint8_t MOOD_PULSE[NMOODS] = {0, 0, 0, 0, 0, 10, 0, 0, 96, 0};
const uint8_t MOOD_REST[NMOODS] = {10, 16, 20, 30, 20, 16, 16, 80, 10, 40};
const uint8_t MOOD_BEAT[NMOODS] = {0, 0, 0, 0, 0, 0, 0, 0, 60, 24};

/* ------------------------------------------------------------- INFINITY --- */
static struct {
    uint8_t on;
    int32_t deg;                       /* the chord's scale degree */
    uint32_t next_chord, chord_len, next_drift, next_section, bar;
    float energy_to, breath, breath_to;
    int8_t motif[8];
    uint8_t motif_len, motif_pos, k1, rot1, k2, n2, pos2;
    int32_t aria_notes[2];
    uint8_t aria_n;
    uint32_t aria_at[2];
    int32_t drone_note;
    uint8_t ost[16], ost_len;          /* the ostinato: indexes into the chord, one an 8th or a 16th */
    uint8_t phrase_on;                 /* the melody is in a phrase (1) or resting (0) */
    uint32_t phrase_until, lh_at;      /* lh: the piano's left hand, its second note */
    uint32_t prog_next;                /* MINE: the next slot of the progression */
    int32_t lh_note;
} inf;

static void inf_motif(void)
{
    uint32_t i;
    float cx = u7(G_CPLX);
    int32_t d = 0;
    inf.motif_len = (uint8_t)(4u + crange(2u) * 2u + (cx > 0.6f ? 2u : 0u));
    if (inf.motif_len > 8u)
        inf.motif_len = 8u;
    for (i = 0; i < inf.motif_len; i++) {
        int32_t step = (int32_t)crange(3u) - 1;                  /* mostly steps */
        if (crndf() < cx * 0.45f)
            step = (int32_t)crange(5u) - 2 + (step > 0 ? 2 : -1);   /* leaps, with complexity */
        d += step;
        if (d > 7)
            d -= 3;
        if (d < -3)
            d += 3;
        inf.motif[i] = (int8_t)d;
    }
    inf.motif_pos = 0;
}

/* an ostinato: a short figure over the chord's tones, rising and turning, repeated (it follows the chords) */
static void inf_ostinato(void)
{
    static const uint8_t SHAPES[6][8] = {{0, 1, 2, 3, 2, 1, 0, 1}, {0, 2, 1, 3, 0, 2, 1, 3}, {0, 1, 2, 1, 3, 2, 1, 2},
                                         {0, 2, 3, 2, 0, 2, 3, 4}, {3, 2, 1, 0, 1, 2, 3, 2}, {0, 1, 3, 1, 2, 1, 3, 4}};
    uint32_t sh = crange(6u), i, len = u7(G_CPLX) > 0.6f && crndf() < 0.5f ? 16u : 8u;
    for (i = 0; i < len; i++)
        inf.ost[i] = (uint8_t)(SHAPES[(sh + i / 8u) % 6u][i % 8u] + (len == 16u && i >= 8u && crndf() < 0.3f ? 1u : 0u));
    inf.ost_len = (uint8_t)len;
}

static void inf_rhythm(void)
{
    float dens = u7(G_DENS) * (0.4f + 0.6f * comp_energy), cx = u7(G_CPLX);
    inf.k1 = (uint8_t)(1u + (uint32_t)(dens * 6.0f));
    inf.rot1 = (uint8_t)crange(16u);
    if (cx > 0.5f) {                   /* a second layer on another length: polyrhythm */
        static const uint8_t NS[3] = {5, 7, 12};
        inf.n2 = NS[crange(3u)];
        inf.k2 = (uint8_t)(1u + crange(cx > 0.8f ? 3u : 2u));
    } else {
        inf.n2 = 0;
    }
}

static int euclid(uint32_t i, uint32_t k, uint32_t n, uint32_t rot)
{
    if (!n || !k)
        return 0;
    i = (i + rot) % n;
    return (i * k) % n < k;
}

static uint32_t pick_weighted(const char *row, uint32_t n, float temp)
{
    uint32_t i, tot = 0, w[7], r;
    for (i = 0; i < n; i++) {
        uint32_t v = (uint32_t)(row[i] - '0');
        w[i] = v * 4u + (uint32_t)(temp * 8.0f) * (v ? 1u : (temp > 0.6f ? 1u : 0u));   /* randomness flattens */
        tot += w[i];
    }
    if (!tot)
        return 0;
    r = crange(tot);
    for (i = 0; i < n; i++) {
        if (r < w[i])
            return i;
        r -= w[i];
    }
    return 0;
}

static uint32_t inf_chord_type(int32_t root)
{
    const mood_t *m = &MOODS[P[G_MOOD] % NMOODS];
    uint32_t cand[NCHORDS], nc = 0, t;
    float cx = u7(G_CPLX);
    if (P[G_CHORD] != CH_AUTO && P[G_CHORD] != CH_NOTES)   /* the player chose one: it stands */
        return P[G_CHORD];
    for (t = 0; t < CH_NOTES; t++)
        if ((m->chords >> t) & 1u) {
            if ((t == CH_QUARTAL || t == CH_MIN9) && cx < 0.25f)
                continue;              /* the richer colours come with complexity */
            if (chord_fits(root, t))
                cand[nc++] = t;
        }
    return nc ? cand[crange(nc)] : CH_AUTO;
}

static void inf_aria_release(void)
{
    uint32_t i;
    for (i = 0; i < inf.aria_n; i++)
        dsp_note_off(TR_ARIA, inf.aria_notes[i]);
    inf.aria_n = 0;
}

static void inf_drone(int32_t note)
{
    while (note > 47)
        note -= 12;
    while (note < 31)
        note += 12;
    inf.drone_note = note;
    comp_note_flash[TR_DRONE]++;
    if (P[D_FOLLOW])
        dsp_note_on(TR_DRONE, note, 100);
    else
        dsp_note_on(TR_DRONE, P[D_ROOT], 100);
}

static void inf_next_chord(uint32_t now, int first)
{
    const mood_t *m = &MOODS[P[G_MOOD] % NMOODS];
    const scale_t *s = scale();
    float ev = u7(G_EVOL), rnd = u7(G_RAND);
    int32_t root;
    uint32_t type, bars;
    if (P[G_PROG] == PROG_MINE && song.prog_n) {   /* the player's own progression, in order, each for its bars */
        const prog_t *pg;
        if (first)
            inf.prog_next = 0;
        pg = &song.prog[inf.prog_next % song.prog_n];
        comp_prog_i = (uint8_t)(inf.prog_next % song.prog_n);
        inf.prog_next = (inf.prog_next + 1u) % song.prog_n;
        root = 48 + (int32_t)(pg->root % 12u);
        type = pg->type < CH_NOTES ? pg->type : CH_AUTO;
        inf.deg = note_degree(scale_quantize(root), 48);   /* the melody leans on this chord */
        play_chord(root, type, comp_energy > 0.28f);
        bars = pg->bars ? pg->bars : 2u;
    } else {
        comp_prog_i = 0xFFu;
        if (!first) {
            uint32_t from = (uint32_t)(((inf.deg % (int32_t)s->n) + (int32_t)s->n) % (int32_t)s->n);
            inf.deg = (int32_t)pick_weighted(m->w + 7u * (from % 7u), s->n, rnd);
        } else {
            inf.deg = 0;
        }
        root = degree_note(inf.deg, 48);
        type = inf_chord_type(root);
        play_chord(root, type, comp_energy > 0.28f);
        /* how long it lasts: EVOLVE short, low EVOLVE long; randomness varies it */
        bars = 8u - (uint32_t)(ev * 6.0f);
        if (crndf() < rnd * 0.5f)
            bars = crndf() < 0.5f ? bars / 2u + 1u : bars + 2u;
    }
    inf.chord_len = bars * BAR;
    inf.next_chord = now + inf.chord_len;
    /* the drone: on the tonic, now and then lifting to the chord's root */
    if (first || crndf() < (float)m->lift * 0.06f * (0.5f + u7(G_CPLX)))
        inf_drone(first || crndf() < 0.5f ? degree_note(0, 36) : root);
    else if (inf.drone_note != degree_note(0, 36) && crndf() < 0.6f)
        inf_drone(degree_note(0, 36));
    /* the choir: enters on some chords, a beat or two late, on upper chord tones */
    inf_aria_release();
    if (comp_energy > 0.32f && crndf() < (float)m->aria * 0.1f * (0.4f + comp_energy)) {
        uint32_t i, n = comp_energy > 0.65f && crndf() < 0.6f ? 2u : 1u;
        for (i = 0; i < n; i++) {
            int32_t t = voicing[voicing_n - 1 - (int32_t)i * (voicing_n > 2 ? 2 : 1)];
            while (t < 64)
                t += 12;
            while (t > 79)
                t -= 12;
            inf.aria_notes[i] = t;
            inf.aria_at[i] = now + PPQ * (1u + crange(4u)) + i * PPQ * 2u;
        }
        inf.aria_n = (uint8_t)n;
    }
    inf.next_drift = now + BAR * (1u + crange(2u));
}

static void inf_start(uint32_t now)
{
    const mood_t *m = &MOODS[P[G_MOOD] % NMOODS];
    (void)m;
    inf.on = 1;
    comp_energy = 0.12f;
    inf.energy_to = 0.45f;
    comp_section = 0;
    inf.next_section = now + BAR * 8u;
    inf.breath = inf.breath_to = 0.7f;
    inf.aria_n = 0;
    voicing_n = 0;
    inf_motif();
    inf_rhythm();
    inf_ostinato();
    inf.phrase_on = 1;
    inf.phrase_until = now + BAR * 4u;
    inf.lh_at = 0;
    inf_next_chord(now, 1);
}

static void inf_stop(void)
{
    inf.on = 0;
    comp_prog_i = 0xFFu;
    inf_aria_release();
    chord_release();
    dsp_release_all(TR_ECHO);
    if (!P[D_HOLD])
        dsp_release_all(TR_DRONE);
}

static void inf_tick(uint32_t now)
{
    const mood_t *m = &MOODS[P[G_MOOD] % NMOODS];
    float dens = u7(G_DENS), cx = u7(G_CPLX), rnd = u7(G_RAND), hum = u7(G_SWING);
    uint32_t i;
    /* arrangement: an arc of sections, the energy gliding toward each one's level */
    if (now % BAR == 0u) {
        inf.bar++;
        comp_energy += (inf.energy_to - comp_energy) * 0.18f;
        if (crndf() < 0.25f)
            inf.breath_to = 0.45f + 0.55f * crndf();
        inf.breath += (inf.breath_to - inf.breath) * 0.3f;
        if (now >= inf.next_section) {
            static const float LEVEL[5] = {0.15f, 0.5f, 0.8f, 0.3f, 1.0f};
            static const uint8_t NEXT[5] = {1, 2, 3, 4, 2};
            comp_section = NEXT[comp_section % 5u];
            if (crndf() < rnd * 0.4f)
                comp_section = (uint8_t)(1u + crange(4u));
            inf.energy_to = LEVEL[comp_section] * (0.85f + 0.3f * crndf());
            inf.next_section = now + BAR * (8u + 8u * crange(2u + (uint32_t)((1.0f - u7(G_EVOL)) * 2.0f)));
            if (crndf() < 0.5f + rnd * 0.5f)
                inf_motif();           /* a new theme for the new section, sometimes */
            inf_rhythm();
            if (crndf() < 0.6f)
                inf_ostinato();
        }
    }
    /* REST: the melody speaks in phrases, with silences between, longer as REST rises */
    if (now >= inf.phrase_until) {
        float rest = u7(G_REST);
        if (inf.phrase_on && crndf() < rest) {
            inf.phrase_on = 0;
            inf.phrase_until = now + BAR * (1u + crange(1u + (uint32_t)(rest * 4.0f)));
        } else {
            inf.phrase_on = 1;
            inf.phrase_until = now + BAR * (2u + crange(3u));
            inf.motif_pos = 0;
        }
    }
    if (now >= inf.next_chord) {
        inf_next_chord(now, 0);
        /* sparse moods: the pad sits some chords out, the piano's left hand carries them */
        if (u7(G_REST) > 0.5f && crndf() < (u7(G_REST) - 0.5f) * 1.2f)
            dsp_pad_chord(0, 0);
    }
    /* the piano's left hand: a low root and a chord tone, at each bar (WISTFUL and other resting moods) */
    if (P[E_MODEL] == EM_PIANO && now % BAR == 0u && u7(G_REST) > 0.3f && voicing_n && comp_energy > 0.15f) {
        int32_t root = voicing[0];
        while (root > 52)
            root -= 12;
        while (root < 40)
            root += 12;
        note_for(TR_ECHO, root, 52u + crange(14u), BAR * 3u / 4u);
        if (crndf() < 0.6f && voicing_n > 1) {   /* and a chord tone a beat or two later */
            int32_t t = voicing[1 + (int32_t)crange((uint32_t)voicing_n - 1u)];
            while (t > 64)
                t -= 12;
            while (t <= root)
                t += 12;
            inf.lh_note = t;
            inf.lh_at = now + PPQ * (1u + crange(2u));
        }
    }
    if (inf.lh_at && now >= inf.lh_at) {
        inf.lh_at = 0;
        note_for(TR_ECHO, inf.lh_note, 44u + crange(14u), PPQ * 2u);
    }
    /* PULSE: the ostinato on 8ths (16ths when the figure is long), over the chord's tones an octave up */
    if (u7(G_PULSE) > 0.01f && voicing_n && comp_energy > 0.25f) {
        uint32_t step = inf.ost_len == 16u ? T16 : T16 * 2u;
        if (now % step == 0u) {
            uint32_t i = (now / step) % inf.ost_len, idx = inf.ost[i];
            int32_t note = voicing[idx % (uint32_t)voicing_n] + 12 * (int32_t)(idx / (uint32_t)voicing_n) + 12;
            float p = u7(G_PULSE) * (0.55f + 0.45f * comp_energy);
            while (note > 88)
                note -= 12;
            if (crndf() < p)
                note_for(TR_ECHO, note, (i % 4u == 0u ? 84u : 62u) + crange(12u), step * 3u / 4u);
        }
    }
    /* BEAT: a soft kick on the bar (and the half with density), ticks on a euclidean figure, a shaker off the beat */
    if (u7(G_BEAT) > 0.01f && comp_energy > 0.35f && now % T16 == 0u) {
        uint32_t s16 = (now / T16) % 16u;
        if (s16 == 0u || (s16 == 8u && dens > 0.45f))
            dsp_perc(PERC_KICK, 90u + crange(20u));
        if (euclid(s16, 3u + (uint32_t)(dens * 4.0f), 16u, 2u) && crndf() < 0.85f)
            dsp_perc(PERC_TICK, 50u + crange(40u));
        if (s16 % 4u == 2u && crndf() < 0.5f + 0.4f * comp_energy)
            dsp_perc(PERC_SHAKER, 60u + crange(30u));
    }
    if (now >= inf.next_drift && P[G_DRIFT]) {
        if (crndf() < u7(G_DRIFT))
            drift_step(m->dark);
        inf.next_drift = now + (uint32_t)(BAR * 2u) - (uint32_t)((float)BAR * 1.5f * u7(G_DRIFT)) + crange(PPQ * 2u);
    }
    for (i = 0; i < inf.aria_n; i++)   /* the choir's late entries */
        if (inf.aria_at[i] && now >= inf.aria_at[i]) {
            inf.aria_at[i] = 0;
            note_for(TR_ARIA, inf.aria_notes[i], 70u + crange(30u), 0);
        }
    /* the melody: euclidean hits on the 16th grid, the motif's degrees over the chord's root */
    if (now % T16 == 0u && comp_energy > 0.22f && inf.phrase_on) {
        uint32_t s16 = (now / T16) % 16u, len16;
        int strong = s16 % 4u == 0u;
        float p = (0.35f + 0.65f * dens) * inf.breath * (0.4f + 0.8f * comp_energy);
        if (euclid(s16, inf.k1, 16u, inf.rot1) && crndf() < p) {
            int32_t deg = inf.deg + inf.motif[inf.motif_pos % inf.motif_len], note;
            note = degree_note(deg, 72 + 12 * m->mel_oct);
            if (strong && crndf() < 0.7f) {   /* the strong beats lean on the chord */
                int32_t bestd = 99, bn = note, k;
                for (k = 0; k < voicing_n; k++) {
                    int32_t c = voicing[k], d;
                    while (c < note - 6)
                        c += 12;
                    while (c > note + 6)
                        c -= 12;
                    d = iabs(c - note);
                    if (d < bestd) {
                        bestd = d;
                        bn = c;
                    }
                }
                note = bn;
            }
            while (note > 96)
                note -= 12;
            len16 = 1u + crange(3u);
            note_for(TR_ECHO, note, (strong ? 92u : 70u) + crange(1u + (uint32_t)(hum * 30.0f)), len16 * T16);
            inf.motif_pos++;
            if (inf.motif_pos >= inf.motif_len) {
                inf.motif_pos = 0;
                if (crndf() < rnd * 0.6f) {   /* the theme varies as it repeats */
                    uint32_t j = crange(inf.motif_len);
                    inf.motif[j] = (int8_t)(inf.motif[j] + (crndf() < 0.5f ? 1 : -1));
                }
            }
        }
        if (inf.n2 && cx > 0.5f) {     /* the second layer: sparse, high, on its own cycle */
            if (euclid(inf.pos2, inf.k2, inf.n2, 0) && crndf() < 0.6f * comp_energy) {
                int32_t note = degree_note(inf.deg + (int32_t)crange(5u) * 2, 84 + 12 * m->mel_oct);
                while (note > 100)
                    note -= 12;
                note_for(TR_ECHO, note, 52u + crange(20u), T16 * 2u);
            }
            inf.pos2 = (uint8_t)((inf.pos2 + 1u) % inf.n2);
        }
    }
}

/* ------------------------------------------------------------- sequencer --- */
static const uint16_t DIV_TICKS[NDIVS] = {T16, T16 * 2u, PPQ, PPQ * 2u, BAR, BAR * 2u};
static uint8_t seq_pos[NTRACK], seq_pat[NTRACK];

static uint32_t chain_next(uint32_t t, uint32_t p)
{
    uint32_t c = song.t[t].chain, i;
    if (!c)
        return comp_edit_pat[t];
    for (i = 1; i <= NPAT; i++) {
        uint32_t q = (p + i) % NPAT;
        if ((c >> q) & 1u)
            return q;
    }
    return p;
}

static int track_empty(uint32_t t)
{
    uint32_t i;
    for (i = 0; i < NSTEP; i++)
        if (song.t[t].s[seq_pat[t]][i].note & STEP_ON)
            return 0;
    return 1;
}

static void seq_start(void)
{
    uint32_t t;
    for (t = 0; t < NTRACK; t++) {
        seq_pos[t] = 0;
        seq_pat[t] = song.t[t].chain ? (uint8_t)chain_next(t, NPAT - 1u) : comp_edit_pat[t];
        comp_play_pat[t] = seq_pat[t];
    }
    voicing_n = 0;
    if (P[D_FOLLOW] && track_empty(TR_DRONE))   /* nothing written for the drone: it holds the key's tonic */
        inf_drone(degree_note(0, 36));
}

static void seq_step(uint32_t t)
{
    track_seq_t *ts = &song.t[t];
    step_t st = ts->s[seq_pat[t]][seq_pos[t]];
    uint32_t div = DIV_TICKS[ts->div < NDIVS ? ts->div : 0];
    comp_step[t] = seq_pos[t];
    /* automation: the lane's points spread over the pattern, the value between them interpolated */
    if (ts->auto_id < NPARAM) {
        uint32_t len = ts->len ? ts->len : 16u, x = seq_pos[t] * (NAUTO - 1u) * 16u / (len > 1u ? len - 1u : 1u);
        uint32_t i0 = x / 16u, i1 = i0 + 1u < NAUTO ? i0 + 1u : i0, f = x % 16u;
        uint8_t a = ts->aut[seq_pat[t]][i0], b = ts->aut[seq_pat[t]][i1];
        if (a != 0xFFu)
            P[ts->auto_id] = (uint8_t)(b != 0xFFu ? (a * (16u - f) + b * f) / 16u : a);
    }
    if ((st.note & STEP_ON) && crndf() * 8.0f < (float)(STEP_PROB(st) + 1u)) {
        int32_t note = STEP_NOTE(st);
        uint32_t len = STEP_LEN(st) * div, vel = 96u - (uint32_t)(u7(G_SWING) * 20.0f) + crange(1u + (uint32_t)(u7(G_SWING) * 40.0f));
        if (ts->gen && crndf() * 127.0f < (float)ts->gen * 0.6f)   /* generative: a neighbouring scale tone */
            note = degree_note(note_degree(note, 0) + (crndf() < 0.5f ? 1 : -1) * (int32_t)(1u + crange(2u)), 0);
        note = scale_quantize(note);
        if (len > 2u)
            len -= 1u;                 /* a hair short, so a legato line re-attacks */
        if (t == TR_ATMOS && P[G_CHORD] != CH_NOTES) {
            play_chord(note, P[G_CHORD], 1);
            pend_add(PEND_CHORD, (int32_t)(chord_gen & 0x7FFFu), len);   /* released at its length, unless */
                                                                         /* another chord came first */
        } else if (t == TR_DRONE) {
            dsp_note_on(TR_DRONE, note, vel);
            comp_note_flash[t]++;
        } else {
            note_for(t, note, vel, len);
        }
    }
    if (++seq_pos[t] >= (ts->len ? ts->len : 16u)) {
        seq_pos[t] = 0;
        seq_pat[t] = (uint8_t)chain_next(t, seq_pat[t]);
        comp_play_pat[t] = seq_pat[t];
    }
    (void)div;
}

/* ------------------------------------------------------------ arpeggiator --- */
static int32_t held[NTRACK][8];
static uint8_t held_n[NTRACK];
static uint32_t arp_i;
static const uint8_t ARP_TICKS[6] = {PPQ * 2u, PPQ, PPQ / 2u, PPQ / 3u, PPQ / 4u, PPQ / 6u};

static void held_add(uint32_t t, int32_t note)
{
    uint32_t i, j;
    for (i = 0; i < held_n[t]; i++)
        if (held[t][i] == note)
            return;
    if (held_n[t] >= 8u)
        return;
    for (i = 0; i < held_n[t] && held[t][i] < note; i++)
        ;
    for (j = held_n[t]; j > i; j--)
        held[t][j] = held[t][j - 1];
    held[t][i] = note;
    held_n[t]++;
    comp_held_mask |= (uint8_t)(1u << t);
}

static void held_del(uint32_t t, int32_t note)
{
    uint32_t i, j;
    for (i = 0; i < held_n[t]; i++)
        if (held[t][i] == note) {
            for (j = i; j + 1u < held_n[t]; j++)
                held[t][j] = held[t][j + 1];
            held_n[t]--;
            break;
        }
    if (!held_n[t])
        comp_held_mask &= (uint8_t)~(1u << t);
}

static void arp_tick(uint32_t now)
{
    uint32_t mode = P[E_ARP], rate = ARP_TICKS[P[E_ARATE] % 6u], octs = (uint32_t)P[E_AOCT] + 1u, n, total, k;
    int32_t src[8], note;
    if (mode == ARP_OFF || now % rate)
        return;
    if (mode == ARP_CHORD || !held_n[TR_ECHO]) {
        if (mode != ARP_CHORD || !voicing_n || (!comp_playing && !held_n[TR_ATMOS]))
            return;
        n = (uint32_t)voicing_n;
        for (k = 0; k < n; k++)
            src[k] = voicing[k] + 12;
    } else {
        n = held_n[TR_ECHO];
        for (k = 0; k < n; k++)
            src[k] = held[TR_ECHO][k];
    }
    if (octs > 3u)
        octs = 3u;
    total = n * octs;
    switch (mode) {
    case ARP_DOWN:
        k = total - 1u - arp_i % total;
        break;
    case ARP_UPDOWN: {
        uint32_t cyc = total > 1u ? 2u * total - 2u : 1u, x = arp_i % cyc;
        k = x < total ? x : cyc - x;
        break;
    }
    case ARP_RANDOM:
        k = crange(total);
        break;
    default:
        k = arp_i % total;
        break;
    }
    arp_i++;
    note = src[k % n] + 12 * (int32_t)(k / n);
    note_for(TR_ECHO, note, 72u + crange(30u), rate * 3u / 4u + 1u);
}

/* --------------------------------------------------------------- events --- */
#define NEV 64u
static struct { uint8_t type, track; int16_t note; uint8_t vel; } evq[NEV];
static volatile uint32_t ev_w, ev_r;

void comp_post(uint32_t type, uint32_t track, int32_t note, uint32_t vel)
{
    uint32_t w = ev_w, nx = (w + 1u) % NEV;
    if (nx == ev_r)
        return;                        /* full: dropped (64 events within 0.7 ms does not happen by hand) */
    evq[w].type = (uint8_t)type;
    evq[w].track = (uint8_t)track;
    evq[w].note = (int16_t)note;
    evq[w].vel = (uint8_t)vel;
    __asm__ volatile("" ::: "memory");
    ev_w = nx;
}

static uint32_t tick_all, tick_play;

static void transport(int play)
{
    uint32_t t;
    if (play && !comp_playing) {
        comp_playing = 1;
        tick_play = 0;
        comp_bar = 0;
        if (P[G_MODE] == MODE_INFINITY)
            inf_start(0);
        else
            seq_start();
    } else if (!play && comp_playing) {
        comp_playing = 0;
        pend_clear();
        if (inf.on)
            inf_stop();
        else {
            chord_release();
            for (t = 0; t < NTRACK; t++)
                if (t != TR_DRONE || !P[D_HOLD])
                    dsp_release_all(t);
        }
    }
}

static void events(void)
{
    while (ev_r != ev_w) {
        uint32_t r = ev_r, t = evq[r].track;
        int32_t note = evq[r].note;
        switch (evq[r].type) {
        case EV_NOTE_ON:
            if (t == TR_ATMOS && P[G_CHORD] != CH_NOTES) {
                held_add(t, note);
                play_chord(note, P[G_CHORD], 1);
            } else if (t == TR_ECHO && P[E_ARP] != ARP_OFF && P[E_ARP] != ARP_CHORD) {
                if (!held_n[t])
                    arp_i = 0;
                held_add(t, note);
            } else {
                held_add(t, note);
                dsp_note_on(t, note, evq[r].vel);
                comp_note_flash[t]++;
            }
            break;
        case EV_NOTE_OFF:
            held_del(t, note);
            if (t == TR_ATMOS && P[G_CHORD] != CH_NOTES) {
                if (!held_n[t] && !inf.on && !comp_playing)
                    chord_release();
                else if (held_n[t] && !inf.on)
                    play_chord(held[t][held_n[t] - 1u], P[G_CHORD], 1);
            } else if (!(t == TR_ECHO && P[E_ARP] != ARP_OFF && P[E_ARP] != ARP_CHORD)) {
                dsp_note_off(t, note);
            }
            break;
        case EV_PLAY:
            transport(1);
            break;
        case EV_STOP:
            transport(0);
            break;
        case EV_RELEASE_TRACK:
            dsp_release_all(t);
            break;
        case EV_DRONE_HOLD:            /* HOLD switched off: the drone stops unless a key holds it */
            if (!P[D_HOLD] && !held_n[TR_DRONE] && !(comp_playing))
                dsp_release_all(TR_DRONE);
            break;
        case EV_CHORD:                 /* the CHORDS page: a slot heard */
            play_chord(note, t < CH_NOTES ? t : CH_AUTO, 1);
            break;
        case EV_CHORD_OFF:
            if (!comp_playing)
                chord_release();
            break;
        case EV_PANIC:
            pend_clear();
            transport(0);
            dsp_kill();
            voicing_n = 0;
            comp_chord_n = 0;
            break;
        default:
            break;
        }
        ev_r = (r + 1u) % NEV;
    }
}

/* ----------------------------------------------------------------- clock --- */
float comp_bpm(void) { return 30.0f + (float)P[G_TEMPO]; }

static void tick(void)
{
    uint32_t t;
    pend_tick();
    arp_tick(tick_all);
    if (comp_playing) {
        if (tick_play % BAR == 0u)
            comp_bar = tick_play / BAR;
        comp_beat16 = tick_play / T16;
        if (inf.on)
            inf_tick(tick_play);
        else
            for (t = 0; t < NTRACK; t++) {
                uint32_t div = DIV_TICKS[song.t[t].div < NDIVS ? song.t[t].div : 0];
                if (tick_play % div == 0u)
                    seq_step(t);
            }
        tick_play++;
    }
    tick_all++;
}

static float tick_acc;

void comp_init(void)
{
    uint32_t t, p, i;
    if (song.magic != SONG_MAGIC) {
        song.magic = SONG_MAGIC;
        for (t = 0; t < NTRACK; t++) {
            song.t[t].len = 16;
            song.t[t].div = t == TR_ATMOS ? DIV_BAR : t == TR_DRONE ? DIV_2BAR : t == TR_ECHO ? DIV_8 : DIV_2;
            song.t[t].chain = 0;
            song.t[t].gen = 0;
            song.t[t].auto_id = 0xFF;
            for (p = 0; p < NPAT; p++) {
                for (i = 0; i < NSTEP; i++) {
                    song.t[t].s[p][i].note = 60;
                    song.t[t].s[p][i].pl = (7u << 5) | 3u;   /* 100 %, 4 steps */
                }
                for (i = 0; i < NAUTO; i++)
                    song.t[t].aut[p][i] = 0xFF;
            }
        }
        {   /* a progression to start from, in the key: I, vi, IV, V (maj7, min7, maj7, sus2), two bars each */
            static const uint8_t DEG[4] = {0, 9, 5, 7}, TYP[4] = {CH_MAJ7, CH_MIN7, CH_MAJ7, CH_SUS2};
            song.prog_n = 4;
            for (i = 0; i < NPROG; i++) {
                song.prog[i].root = (uint8_t)((P[G_KEY] + DEG[i % 4u]) % 12u);
                song.prog[i].type = TYP[i % 4u];
                song.prog[i].bars = 2;
                song.prog[i].rsv = 0;
            }
        }
    }
}

void halo_render(int32_t *lr, uint32_t n, float out_gain)
{
    static float l[HALO_CHUNK], r[HALO_CHUNK];
    uint32_t done = 0, k;
    float fpt = HALO_FS * 60.0f / (comp_bpm() * (float)PPQ);
    dsp_tempo_bpm = comp_bpm();
    events();
    while (done < n) {
        uint32_t m = n - done < HALO_CHUNK ? n - done : HALO_CHUNK;
        tick_acc += (float)m;
        while (tick_acc >= fpt) {
            tick_acc -= fpt;
            tick();
        }
        dsp_process(l, r, m);
        for (k = 0; k < m; k++) {
            lr[2u * (done + k)] = (int32_t)(l[k] * out_gain);
            lr[2u * (done + k) + 1u] = (int32_t)(r[k] * out_gain);
        }
        done += m;
    }
}
