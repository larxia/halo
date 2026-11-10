/* SPDX-License-Identifier: MPL-2.0
 * HALO — the UI: pages, the knob cards, the step grid, the keys, MIDI in, saving. Part of fm1-base's unity build
 * (after the platform: gfx.c and announce.c are in scope). The sound and the composer are the o2 units in dsp/.
 *
 * Controls (docs/MANUAL.md):
 *   SELECT  turn: page                 ALGO  turn: row of the page        PRESET  turn: page-specific
 *   K1..K4  the four values of the row; on HOME the macros SPACE / MOTION / SHIMMER / DEPTH
 *   HOME  HOME page (held: + white key 1-5 solo, + black key 1-5 mute)   EDIT  next engine page
 *   SEQ   SEQUENCE page   GLO  INFINITY page   FX  FX page   SEL  PRESETS (on PRESETS: load)
 *   ENV   DRONE HOLD      LFO  held: FREEZE (the reverb holds its tail)   ARP  the echo's arpeggiator
 *   PLAY  start / stop    REC  record notes into the track's steps        SAVE  save sound, song, settings
 *   OCT- / OCT+  the keyboard's octave (on SEQUENCE: steps 1-16 / 17-32)
 */
#include "fastmath.h"
#include "dsp/halo_params.h"
#include "dsp/halo_dsp.h"
#include "dsp/halo_comp.h"

#define HALO_VERSION "0.2.0"

/* ---------------------------------------------------------------- colours --- */
#define PINK RGB(247, 170, 200)
#define PINK_HI RGB(255, 206, 226)
#define PINK_MID RGB(176, 112, 142)
#define PINK_DIM RGB(92, 54, 72)
#define PINK_DEEP RGB(44, 24, 34)
#define INK RGB(236, 230, 234)
#define GREY RGB(140, 128, 136)
#define DIMG RGB(70, 62, 68)
#define RULE RGB(36, 30, 34)

/* ---------------------------------------------------------------- settings --- */
#define SET_MAGIC 0x484C4F31u            /* "HLO1" */
typedef struct {
    uint32_t magic;
    int8_t octave;                       /* -2 .. +2 */
    uint8_t lock;                        /* the keys snap to the scale */
    uint8_t vel;                         /* velocity from the keys */
    uint8_t qual;                        /* 0 AUTO, 1 HIGH, 2 ECO */
    uint8_t midi;                        /* 0 channels 1-5 to tracks 1-5, 1 every channel to the selected track */
    uint8_t preset;                      /* the last one loaded */
    uint8_t track;                       /* the selected track */
    uint8_t rsv;
} settings_t;
static settings_t set;

#define SOUND_MAGIC 0x484C5031u          /* "HLP1" */
typedef struct {
    uint32_t magic;
    uint8_t n, mute, solo, rsv;
    uint8_t p[NPARAM];
} sound_rec_t;

static uint8_t dirty;
/* SETTINGS TEST (not saved): 1 a plain 440 Hz tone straight to the output, past every HALO sound (does the output
 * path itself sound clean?); 2..6 one engine alone, dry; 7 every engine, no effects */
static uint8_t test_mode;

/* ------------------------------------------------------------------- pages --- */
enum { PG_HOME, PG_SYNTH, PG_DRONE, PG_SHIMMER, PG_ARIA, PG_ECHO, PG_SEQ, PG_INFINITY, PG_FX, PG_PRESETS, PG_SETTINGS,
       NPAGES };
static const char *const PAGE_NAME[NPAGES] = {"HOME", "SYNTH", "DRONE", "SHIMMER", "ARIA", "ECHO", "SEQUENCE",
                                              "INFINITY", "FX", "PRESETS", "SETTINGS"};
static const char *const TRACK_NAME[NTRACK] = {"ATMOS", "DRONE", "SHIMMER", "ARIA", "ECHO"};
static const char *const TRACK_SHORT[NTRACK] = {"PAD", "DRN", "SHM", "ARI", "ECH"};

/* virtual values (not in P[]): the sequencer's step and track, and settings */
enum { V_NONE = 255, V_STEP_NOTE = 200, V_STEP_ON, V_STEP_PROB, V_STEP_LEN, V_TRK_LEN, V_TRK_DIV, V_TRK_CHAIN, V_TRK_GEN,
       V_PAT, V_AUTO_ID, V_AUTO_VAL, V_TRACK, SV_LOCK, SV_VEL, SV_QUAL, SV_MIDI, SV_OCT, SV_CLEAR, SV_INIT, V_PRESET, SV_TEST };

#define MAXROWS 7
typedef struct {
    uint8_t n;
    const char *name[MAXROWS];
    uint8_t id[MAXROWS][4];
} page_rows_t;
#define MIX(t) {TRK_P(t, 0), TRK_P(t, 1), TRK_P(t, 2), TRK_P(t, 3)}
static const page_rows_t ROWS[NPAGES] = {
    {3, {"MACROS", "LEVELS", "LEVELS"},
     {{M_SPACE, M_MOTION, M_SHIM, M_DEPTH}, {T1_VOL, T2_VOL, T3_VOL, T4_VOL}, {T5_VOL, G_TEMPO, G_KEY, X_MASTER}}},
    {5, {"OSCILLATORS", "FILTER", "ENVELOPE", "MOVEMENT", "MIX"},
     {{A_SHAPE, A_DETUNE, A_AIR, A_SUB}, {A_CUT, A_RES, A_ENV, A_DRIFT}, {A_ATK, A_DEC, A_SUS, A_REL},
      {A_LFOR, A_LFOD, A_MORPH, A_WIDTH}, MIX(0)}},
    {5, {"HARMONICS", "MOVEMENT", "TEXTURE", "HOLD AND FADES", "MIX"},
     {{D_ROOT, D_DENS, D_SUB, D_FIFTH}, {D_MOTION, D_TONE, D_BEAT, D_GLIDE}, {D_GRAIN, D_GRIT, D_CUT, D_WOBBLE},
      {D_HOLD, D_ATK, D_REL, D_FOLLOW}, MIX(1)}},
    {3, {"HARMONICS", "SHAPE", "MIX"}, {{S_INT, S_BRIGHT, S_SPARK, S_DIFF}, {S_ATK, S_REL, S_OCT, S_FDBK}, MIX(2)}},
    {4, {"VOWEL", "VOICE", "ENVELOPE", "MIX"},
     {{V_VOWEL, V_DRIFT, V_DRATE, V_BREATH}, {V_VIB, V_VRATE, V_ENS, V_FORM}, {V_ATK, V_REL, V_OCT, V_BRIGHT}, MIX(3)}},
    {5, {"MODEL", "TONE", "ENVELOPE", "ARPEGGIATOR", "MIX"},
     {{E_MODEL, E_GLIDE, E_VIB, E_RES}, {E_RATIO, E_INDEX, E_IDEC, E_TONE}, {E_ATK, E_DEC, E_SUS, E_REL},
      {E_ARP, E_ARATE, E_AOCT, E_VEL}, MIX(4)}},
    {4, {"STEP", "TRACK", "PATTERN", "MIX"},
     {{V_STEP_NOTE, V_STEP_PROB, V_STEP_LEN, V_STEP_ON}, {V_TRK_LEN, V_TRK_DIV, V_TRK_CHAIN, V_TRK_GEN},
      {V_PAT, V_AUTO_ID, V_AUTO_VAL, G_TEMPO}, {V_NONE, V_NONE, V_NONE, V_NONE}}},
    {4, {"MOOD", "BEHAVIOUR", "HARMONY", "RHYTHM"},
     {{G_MOOD, G_KEY, G_SCALE, G_TEMPO}, {G_CPLX, G_DENS, G_EVOL, G_RAND}, {G_MODE, G_CHORD, G_DRIFT, G_SWING},
      {G_PULSE, G_REST, G_BEAT, G_BTONE}}},
    {7, {"REVERB", "REVERB", "DELAY", "DELAY", "MODULATION", "MOTION AND HEAT", "TEXTURE"},
     {{R_TYPE, R_SIZE, R_DECAY, R_DAMP}, {R_MOD, R_SHIM, R_LEVEL, R_LOCUT}, {L_TIME, L_FDBK, L_TONE, L_WOW},
      {L_MODE, L_LEVEL, L_TOREV, X_WARBLE}, {C_DEPTH, C_RATE, H_AMT, H_RATE}, {T_DEPTH, T_RATE, X_SAT, X_EXCITE},
      {X_NOISE, X_NTONE, X_NMOVE, X_MASTER}}},
    {1, {"FACTORY"}, {{V_PRESET, V_NONE, V_NONE, V_NONE}}},
    {2, {"PLAYING", "SYSTEM"}, {{SV_LOCK, SV_VEL, SV_OCT, SV_MIDI}, {SV_QUAL, SV_CLEAR, SV_INIT, SV_TEST}}},
};

static struct {
    uint8_t page, row[NPAGES], track;
    uint32_t keys, btn;
    uint8_t vel[NKEYS];
    int32_t key_note[NKEYS];             /* the note each held key started, and its track */
    uint8_t key_track[NKEYS];
    char msg[2][26];
    uint32_t msg_until;
    int8_t knob_lit;                     /* the card turned last (-1 none) */
    uint32_t knob_at, knob_prev[4];
    uint8_t cursor, step_page, rec;
    int8_t hold_step;                    /* SEQUENCE: the white key held (-1 none) */
    uint8_t hold_turned, hold_was_on;
    uint8_t preset_hi;
    uint8_t freeze, freeze_type, freeze_decay;
    uint8_t clear_arm, init_arm;
    uint8_t redraw_head, redraw_main, redraw_cards;
    int32_t last_note;
    uint32_t seen_flash[NTRACK], seen_step, seen_chord, seen_bar;
    uint32_t vis_at;
} ui;

/* ----------------------------------------------------------------- text --- */
static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}
static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}
static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static void note_text(int32_t n, char *t)
{
    char o[8];
    t[0] = 0;
    text_cat(t, NOTE_NAME[((n % 12) + 12) % 12]);
    int_text(n / 12 - 1, o);
    text_cat(t, o);
}

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[56];
    for (i = 0; i < 25u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 25u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1400u;
    ui.redraw_head = 1;
    if (ann_on()) {
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

/* ------------------------------------------------------------ the track --- */
static uint32_t page_track(void)
{
    switch (ui.page) {
    case PG_SYNTH:
        return TR_ATMOS;
    case PG_DRONE:
        return TR_DRONE;
    case PG_SHIMMER:
        return TR_SHIM;
    case PG_ARIA:
        return TR_ARIA;
    case PG_ECHO:
        return TR_ECHO;
    default:
        return ui.track;
    }
}
static const int8_t TRACK_OCT[NTRACK] = {0, -2, 0, 0, 1};

/* --------------------------------------------------------- virtual values --- */
static step_t *cur_step(void)
{
    return &song.t[ui.track].s[comp_edit_pat[ui.track]][ui.cursor];
}

static const char *const DIV_NAME[NDIVS] = {"1/16", "1/8", "1/4", "1/2", "1 BAR", "2 BAR"};
static const char *const QUAL_NAME[3] = {"AUTO", "HIGH", "ECO"};

static const char *vlabel(uint32_t id)
{
    switch (id) {
    case V_STEP_NOTE: return "NOTE";
    case V_STEP_ON: return "STEP";
    case V_STEP_PROB: return "CHANCE";
    case V_STEP_LEN: return "LENGTH";
    case V_TRK_LEN: return "STEPS";
    case V_TRK_DIV: return "RATE";
    case V_TRK_CHAIN: return "CHAIN";
    case V_TRK_GEN: return "GENERT";
    case V_PAT: return "PATTERN";
    case V_AUTO_ID: return "AUTO";
    case V_AUTO_VAL: return "POINT";
    case V_TRACK: return "TRACK";
    case SV_LOCK: return "LOCK";
    case SV_VEL: return "VELO";
    case SV_QUAL: return "QUALITY";
    case SV_MIDI: return "MIDI IN";
    case SV_OCT: return "OCTAVE";
    case SV_CLEAR: return "CLR SONG";
    case SV_INIT: return "INIT SND";
    case V_PRESET: return "PRESET";
    case SV_TEST: return "TEST";
    default: return "";
    }
}

static void vtext(uint32_t id, char *t)
{
    track_seq_t *ts = &song.t[ui.track];
    step_t *s = cur_step();
    t[0] = 0;
    switch (id) {
    case V_STEP_NOTE:
        if (ui.track == TR_ATMOS && P[G_CHORD] != CH_NOTES) {
            note_text(STEP_NOTE(*s), t);
            text_cat(t, "+");
        } else {
            note_text(STEP_NOTE(*s), t);
        }
        break;
    case V_STEP_ON:
        text_cat(t, (s->note & STEP_ON) ? "ON" : "OFF");
        break;
    case V_STEP_PROB:
        int_text((int32_t)((STEP_PROB(*s) + 1u) * 125u / 10u), t);
        text_cat(t, "%");
        break;
    case V_STEP_LEN:
        int_text((int32_t)STEP_LEN(*s), t);
        break;
    case V_TRK_LEN:
        int_text(ts->len, t);
        break;
    case V_TRK_DIV:
        text_cat(t, DIV_NAME[ts->div % NDIVS]);
        break;
    case V_TRK_CHAIN: {
        uint32_t p;
        if (!ts->chain)
            text_cat(t, "OFF");
        for (p = 0; p < NPAT; p++)
            if ((ts->chain >> p) & 1u) {
                char c[2] = {(char)('A' + p), 0};
                text_cat(t, c);
            }
        break;
    }
    case V_TRK_GEN:
        int_text((int32_t)ts->gen * 100 / 127, t);
        break;
    case V_PAT: {
        char c[2] = {(char)('A' + comp_edit_pat[ui.track]), 0};
        text_cat(t, c);
        break;
    }
    case V_AUTO_ID:
        text_cat(t, ts->auto_id < NPARAM ? PARAM_INFO[ts->auto_id].label : "OFF");
        break;
    case V_AUTO_VAL: {
        uint8_t v = ts->aut[comp_edit_pat[ui.track]][ui.cursor * NAUTO / NSTEP];
        if (v == 0xFFu)
            text_cat(t, "-");
        else if (ts->auto_id < NPARAM)
            param_text(ts->auto_id, v, t);
        else
            int_text(v, t);
        break;
    }
    case V_TRACK:
        text_cat(t, TRACK_NAME[ui.track]);
        break;
    case SV_LOCK:
        text_cat(t, set.lock ? "ON" : "OFF");
        break;
    case SV_VEL:
        text_cat(t, set.vel ? "ON" : "OFF");
        break;
    case SV_QUAL:
        text_cat(t, QUAL_NAME[set.qual % 3u]);
        break;
    case SV_MIDI:
        text_cat(t, set.midi ? "OMNI" : "CH 1-5");
        break;
    case SV_OCT:
        int_text(set.octave, t);
        break;
    case SV_CLEAR:
        text_cat(t, ui.clear_arm ? "AGAIN" : "TURN");
        break;
    case SV_INIT:
        text_cat(t, ui.init_arm ? "AGAIN" : "TURN");
        break;
    case V_PRESET:
        int_text(ui.preset_hi + 1, t);
        break;
    case SV_TEST: {
        static const char *const TN[8] = {"OFF", "TONE", "ATMOS", "DRONE", "SHIMMR", "ARIA", "ECHO", "NO FX"};
        text_cat(t, TN[test_mode % 8u]);
        break;
    }
    default:
        break;
    }
}

/* a 0..1 position for the card's bar (-1: none) */
static float vpos(uint32_t id)
{
    track_seq_t *ts = &song.t[ui.track];
    step_t *s = cur_step();
    switch (id) {
    case V_STEP_NOTE: return (float)STEP_NOTE(*s) / 127.0f;
    case V_STEP_PROB: return (float)(STEP_PROB(*s) + 1u) / 8.0f;
    case V_STEP_LEN: return (float)STEP_LEN(*s) / 32.0f;
    case V_STEP_ON: return (s->note & STEP_ON) ? 1.0f : 0.0f;
    case V_TRK_LEN: return (float)ts->len / 32.0f;
    case V_TRK_GEN: return (float)ts->gen / 127.0f;
    case V_PRESET: return (float)ui.preset_hi / (float)(NPRESETS - 1);
    default: return -1.0f;
    }
}

static void song_dirty(void) { dirty = 1; }

static void clear_song(void)
{
    uint32_t f = plat_lock();
    song.magic = 0;
    comp_init();
    plat_unlock(f);
}

static void vturn(uint32_t id, int32_t e)
{
    track_seq_t *ts = &song.t[ui.track];
    step_t *s = cur_step();
    switch (id) {
    case V_STEP_NOTE: {
        int32_t n = clamp((int32_t)STEP_NOTE(*s) + e, 24, 108);
        if (set.lock)                    /* the scale's tones only */
            while (n != scale_quantize(n) && n > 24 && n < 108)
                n += e > 0 ? 1 : -1;
        s->note = (uint8_t)((s->note & STEP_ON) | (uint8_t)n | STEP_ON);
        ui.last_note = n;
        if (!comp_playing)               /* hear it */
            comp_post(EV_NOTE_ON, ui.track, n, 90), comp_post(EV_NOTE_OFF, ui.track, n, 0);
        song_dirty();
        break;
    }
    case V_STEP_ON:
        s->note = e > 0 ? (uint8_t)(s->note | STEP_ON) : (uint8_t)(s->note & ~STEP_ON);
        song_dirty();
        break;
    case V_STEP_PROB:
        s->pl = (uint8_t)((s->pl & 31u) | (uint32_t)clamp((int32_t)STEP_PROB(*s) + e, 0, 7) << 5);
        song_dirty();
        break;
    case V_STEP_LEN:
        s->pl = (uint8_t)((s->pl & 0xE0u) | (uint32_t)(clamp((int32_t)STEP_LEN(*s) + e, 1, 32) - 1));
        song_dirty();
        break;
    case V_TRK_LEN:
        ts->len = (uint8_t)clamp(ts->len + e, 1, NSTEP);
        song_dirty();
        break;
    case V_TRK_DIV:
        ts->div = (uint8_t)clamp(ts->div + e, 0, NDIVS - 1);
        song_dirty();
        break;
    case V_TRK_CHAIN:
        ts->chain = (uint8_t)clamp(ts->chain + e, 0, 15);
        song_dirty();
        break;
    case V_TRK_GEN:
        ts->gen = (uint8_t)clamp(ts->gen + e * 4, 0, 127);
        song_dirty();
        break;
    case V_PAT:
        comp_edit_pat[ui.track] = (uint8_t)clamp(comp_edit_pat[ui.track] + e, 0, NPAT - 1);
        break;
    case V_AUTO_ID: {
        /* the automatable values: every sound and effect parameter (not the composer's) */
        int32_t v = ts->auto_id < NPARAM ? (int32_t)ts->auto_id : -1;
        v = clamp(v + e, -1, (int32_t)M_DEPTH);
        ts->auto_id = v < 0 ? 0xFFu : (uint8_t)v;
        song_dirty();
        break;
    }
    case V_AUTO_VAL: {
        uint8_t *a = &ts->aut[comp_edit_pat[ui.track]][ui.cursor * NAUTO / NSTEP];
        int32_t v = *a == 0xFFu ? (ts->auto_id < NPARAM ? (int32_t)P[ts->auto_id] : 64) : (int32_t)*a;
        v += e * 2;
        *a = v < 0 ? 0xFFu : (uint8_t)clamp(v, 0, 127);
        song_dirty();
        break;
    }
    case SV_LOCK:
        set.lock = e > 0;
        dirty = 1;
        break;
    case SV_VEL:
        set.vel = e > 0;
        dirty = 1;
        break;
    case SV_QUAL:
        set.qual = (uint8_t)clamp(set.qual + e, 0, 2);
        dsp_quality = set.qual == 2u ? 0u : 2u;
        dirty = 1;
        break;
    case SV_MIDI:
        set.midi = e > 0;
        dirty = 1;
        break;
    case SV_OCT:
        set.octave = (int8_t)clamp(set.octave + e, -2, 2);
        dirty = 1;
        break;
    case SV_CLEAR:
        if (ui.clear_arm) {
            clear_song();
            ui.clear_arm = 0;
            app_say("SONG", "CLEARED");
            song_dirty();
        } else {
            ui.clear_arm = 1;
        }
        break;
    case SV_INIT:
        if (ui.init_arm) {
            params_defaults();
            ui.init_arm = 0;
            app_say("SOUND", "INITIALISED");
            dirty = 1;
        } else {
            ui.init_arm = 1;
        }
        break;
    case V_PRESET:
        ui.preset_hi = (uint8_t)clamp(ui.preset_hi + e, 0, NPRESETS - 1);
        break;
    case SV_TEST:
        test_mode = (uint8_t)clamp(test_mode + e, 0, 7);
        dsp_test = test_mode >= 2u ? test_mode : 0u;
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- presets --- */
static void load_preset(uint32_t i)
{
    uint8_t mode = P[G_MODE];
    preset_apply(i);
    P[G_MODE] = mode;                    /* the transport's mode is the player's, not the preset's */
    set.preset = (uint8_t)i;
    ui.preset_hi = (uint8_t)i;
    dirty = 1;
    {
        char t[8];
        int_text((int32_t)i + 1, t);
        app_say(t, PRESETS[i].name);
    }
    ui.redraw_main = ui.redraw_cards = 1;
}

/* ------------------------------------------------------------------ save --- */
static uint8_t backup_hold;
static void save_all(void)
{
    static sound_rec_t rec;
    uint32_t i;
    int r = 0;
    if (backup_hold)
        return;
    rec.magic = SOUND_MAGIC;
    rec.n = NPARAM;
    rec.mute = dsp_mute;
    rec.solo = dsp_solo;
    for (i = 0; i < NPARAM; i++)
        rec.p[i] = P[i];
    set.track = ui.track;
    r |= plat_store_save(OBJ_SETTINGS, &set, sizeof set);
    r |= plat_store_save(OBJ_SOUND, &rec, sizeof rec);
    r |= plat_store_save(OBJ_SONG, &song, sizeof song);
    if (r)
        app_say("SAVE", "FAILED");
    else {
        dirty = 0;
        app_say("SAVED", "SOUND, SONG, SETTINGS");
    }
}

static void settings_defaults(void)
{
    set.magic = SET_MAGIC;
    set.octave = 0;
    set.lock = 1;
    set.vel = 1;
    set.qual = 0;
    set.midi = 0;
    set.preset = NPRESETS - 1u;          /* LUSH CHORDS at a first boot */
    set.track = TR_ATMOS;
}

static void load_all(void)
{
    static sound_rec_t rec;
    uint32_t i;
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC ||
        set.octave < -2 || set.octave > 2 || set.qual > 2u || set.preset >= NPRESETS || set.track >= NTRACK)
        settings_defaults();
    if (plat_store_load(OBJ_SOUND, &rec, sizeof rec) == (int)sizeof rec && rec.magic == SOUND_MAGIC &&
        rec.n == NPARAM) {
        for (i = 0; i < NPARAM; i++)
            P[i] = rec.p[i] <= param_max(i) ? rec.p[i] : PARAM_INFO[i].def;
        dsp_mute = rec.mute & 31u;
        dsp_solo = rec.solo & 31u;
    } else {
        preset_apply(set.preset);
    }
    if (plat_store_load(OBJ_SONG, &song, sizeof song) != (int)sizeof song || song.magic != SONG_MAGIC)
        song.magic = 0;
    for (i = 0; i < NTRACK; i++)
        if (song.t[i].len == 0u || song.t[i].len > NSTEP)
            song.magic = 0;
    comp_init();
    dsp_quality = set.qual == 2u ? 0u : 2u;
    ui.track = set.track;
    dirty = 0;
}

void app_init(void)
{
    load_all();
    dsp_init();
    ui.preset_hi = set.preset;
    ui.knob_lit = -1;
    ui.hold_step = -1;
    ui.last_note = 60;
}

/* backup and restore over USB: the platform asks, all or nothing (fm1-base device/backup.c) */
int app_backup_playing(void) { return comp_playing; }
void app_backup_flush(void)
{
    if (dirty)
        save_all();
}
void app_backup_hold(int on) { backup_hold = (uint8_t)(on != 0); }
void app_backup_restored(void)
{
    load_all();
    ui.redraw_head = ui.redraw_main = ui.redraw_cards = 1;
}

/* ----------------------------------------------------------------- audio --- */
/* The FM-1's DAC and amplifier run clean only well under digital full scale: the render's nominal full scale is
 * 1 << 22 (-6 dBFS of the 24-bit word; fm1-base plat.h, as Felucca's OUT_SHIFT), and HALO's bus peaks below 0.92
 * after its limiter, so the output's ceiling is about -7 dBFS and its music sits near -19 dBFS. (0.1 and 0.2 used
 * 8e6, twice this: clean in the simulator, distorted on an FM-1.) */
#define HALO_FULL_SCALE 4194304.0f
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    if (test_mode == 1u) {               /* TEST TONE: a sine at -12 dB of the clean scale, nothing else */
        static float ph;
        uint32_t i;
        float g = (float)master_q12 * (1.0f / 4096.0f) * HALO_FULL_SCALE * 0.25f;
        for (i = 0; i < n; i++) {
            int32_t v;
            ph += 440.0f / 44100.0f;
            if (ph >= 1.0f)
                ph -= 1.0f;
            v = (int32_t)(fm_sin_turns(ph) * g);
            lr[2u * i] = v;
            lr[2u * i + 1u] = v;
        }
        return;
    }
    halo_render(lr, n, (float)master_q12 * (1.0f / 4096.0f) * HALO_FULL_SCALE);
}

void app_panic(void)
{
    dsp_kill();
    comp_post(EV_PANIC, 0, 0, 0);
}

static uint32_t overload_at;
void app_overload(void)
{
    if (set.qual == 0u && dsp_quality > 0u) {   /* AUTO: lighter engines for a while */
        dsp_quality--;
        overload_at = plat_ms();
    }
}

/* ------------------------------------------------------------------ input --- */
static void select_page(uint32_t p)
{
    if (p >= NPAGES)
        return;
    if (ui.page != p) {
        ui.page = (uint8_t)p;
        if (p == PG_PRESETS)
            ui.preset_hi = set.preset;
        ui.clear_arm = ui.init_arm = 0;
    }
    ui.redraw_head = ui.redraw_main = ui.redraw_cards = 1;
    ui.knob_lit = -1;
    if (ann_on())
        ann_say(PAGE_NAME[p], 0, ANN_NOW);
}

static void knob(uint32_t k, int32_t e)
{
    const page_rows_t *pr = &ROWS[ui.page];
    uint32_t id = pr->id[ui.row[ui.page] % pr->n][k], now = plat_ms();
    if (id == V_NONE)
        return;
    if (id < NPARAM) {
        uint32_t mx = param_max(id);
        int32_t step = mx > 12u && now - ui.knob_prev[k] < 45u ? 3 : 1;   /* fast turns go further */
        int32_t v = clamp((int32_t)P[id] + e * step, 0, (int32_t)mx);
        if (v != (int32_t)P[id]) {
            P[id] = (uint8_t)v;
            dirty = 1;
            if (id == G_MOOD) {          /* a mood brings its scale, tempo and rhythm; all stay editable */
                P[G_SCALE] = MOOD_SCALE[v];
                P[G_TEMPO] = MOOD_TEMPO[v];
                P[G_PULSE] = MOOD_PULSE[v];
                P[G_REST] = MOOD_REST[v];
                P[G_BEAT] = MOOD_BEAT[v];
            }
            if (id == E_MODEL)           /* a new model: the old one's notes let go */
                comp_post(EV_RELEASE_TRACK, TR_ECHO, 0, 0);
            if (id == D_HOLD)
                comp_post(EV_DRONE_HOLD, TR_DRONE, 0, 0);
        }
    } else {
        vturn(id, e);
    }
    ui.knob_prev[k] = now;
    ui.knob_lit = (int8_t)k;
    ui.knob_at = now;
    ui.redraw_cards = 1;
    if (ui.page == PG_SEQ || ui.page >= PG_INFINITY || ui.page == PG_HOME)
        ui.redraw_main = 1;
    if (ui.hold_step >= 0)
        ui.hold_turned = 1;
    if (ann_on()) {
        char t[40], v[20];
        t[0] = 0;
        text_cat(t, id < NPARAM ? PARAM_INFO[id].label : vlabel(id));
        text_cat(t, " ");
        if (id < NPARAM)
            param_text(id, P[id], v);
        else
            vtext(id, v);
        text_cat(t, v);
        ann_say(t, 0, ANN_NOW);
    }
}

static void rec_note(uint32_t t, int32_t note)
{
    track_seq_t *ts = &song.t[t];
    uint32_t pos = comp_playing ? comp_step[t] : ui.cursor;
    step_t *s = &ts->s[comp_edit_pat[t]][pos % NSTEP];
    s->note = (uint8_t)(STEP_ON | (uint32_t)clamp(note, 0, 127));
    if (!comp_playing) {
        ui.cursor = (uint8_t)((ui.cursor + 1u) % (ts->len ? ts->len : 16u));
        ui.step_page = ui.cursor >= 16u;
    }
    ui.track = (uint8_t)t;
    song_dirty();
    ui.redraw_main = 1;
}

static void seq_key(uint32_t k, int down)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    uint32_t i;
    for (i = 0; i < 16u; i++)
        if (WK[i] == k) {
            uint32_t pos = ui.step_page * 16u + i;
            step_t *s = &song.t[ui.track].s[comp_edit_pat[ui.track]][pos];
            if (down) {
                ui.cursor = (uint8_t)pos;
                ui.hold_step = (int8_t)i;
                ui.hold_turned = 0;
                ui.hold_was_on = (s->note & STEP_ON) != 0;
                if (!ui.hold_was_on) {
                    s->note = (uint8_t)(STEP_ON | (uint32_t)clamp(ui.last_note, 0, 127));
                    song_dirty();
                }
                ui.row[PG_SEQ] = 0;      /* the step's own row under the knobs */
            } else if (ui.hold_step == (int8_t)i) {
                if (ui.hold_was_on && !ui.hold_turned) {
                    s->note = (uint8_t)(s->note & ~STEP_ON);
                    song_dirty();
                }
                ui.hold_step = -1;
            }
            ui.redraw_main = ui.redraw_cards = 1;
            return;
        }
    if (!down)
        return;
    for (i = 0; i < 11u; i++)
        if (BK[i] == k) {
            if (i < NTRACK) {            /* black keys 1-5: the track */
                ui.track = (uint8_t)i;
                app_say("TRACK", TRACK_NAME[i]);
            } else if (i < NTRACK + NPAT) {   /* 6-9: the pattern edited */
                char c[2] = {(char)('A' + i - NTRACK), 0};
                comp_edit_pat[ui.track] = (uint8_t)(i - NTRACK);
                app_say("PATTERN", c);
            }
            ui.redraw_main = ui.redraw_cards = 1;
            return;
        }
}

static void home_key(uint32_t k)       /* HOME held: white 1-5 solo, black 1-5 mute */
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    uint32_t i;
    for (i = 0; i < NTRACK; i++) {
        if (WK[i] == k) {
            dsp_solo ^= (uint8_t)(1u << i);
            app_say((dsp_solo >> i) & 1u ? "SOLO" : "UNSOLO", TRACK_NAME[i]);
        }
        if (BK[i] == k) {
            dsp_mute ^= (uint8_t)(1u << i);
            app_say((dsp_mute >> i) & 1u ? "MUTE" : "UNMUTE", TRACK_NAME[i]);
        }
    }
    dirty = 1;
    ui.redraw_main = 1;
}

static void midi_poll(void)
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu, ch = st & 15u;
        uint32_t t = set.midi || ch >= NTRACK ? ui.track : ch;
        if ((st & 0xF0u) == 0x90u && d2)
            comp_post(EV_NOTE_ON, t, (int32_t)d1, d2);
        else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u)
            comp_post(EV_NOTE_OFF, t, (int32_t)d1, 0);
        else if ((st & 0xF0u) == 0xB0u) {   /* the macros on the usual controllers */
            if (d1 == 91u)
                P[M_SPACE] = (uint8_t)d2;
            else if (d1 == 1u)
                P[M_MOTION] = (uint8_t)d2;
            else if (d1 == 74u)
                P[M_SHIM] = (uint8_t)d2;
            else if (d1 == 71u)
                P[M_DEPTH] = (uint8_t)d2;
            else if (d1 == 123u)
                comp_post(EV_PANIC, 0, 0, 0);
            ui.redraw_cards = 1;
        } else if (st == 0xFAu)
            comp_post(EV_PLAY, 0, 0, 0);
        else if (st == 0xFCu)
            comp_post(EV_STOP, 0, 0, 0);
    }
}

static void freeze(int on)             /* LFO held: the reverb holds what it has */
{
    if (on && !ui.freeze) {
        ui.freeze = 1;
        ui.freeze_type = P[R_TYPE];
        ui.freeze_decay = P[R_DECAY];
        P[R_TYPE] = RT_INFINITE;
        P[R_DECAY] = 127;
        app_say("FREEZE", "");
    } else if (!on && ui.freeze) {
        ui.freeze = 0;
        P[R_TYPE] = ui.freeze_type;
        P[R_DECAY] = ui.freeze_decay;
    }
}

static void button(uint32_t b)
{
    switch (b) {
    case B_HOME:
        select_page(PG_HOME);
        break;
    case B_EDIT: {
        uint32_t p = ui.page;
        p = p >= PG_SYNTH && p < PG_ECHO ? p + 1u : (p == PG_ECHO ? (uint32_t)PG_SYNTH : PG_SYNTH + (uint32_t)ui.track);
        ui.track = (uint8_t)(p - PG_SYNTH);
        select_page(p);
        break;
    }
    case B_SEQ:
        select_page(PG_SEQ);
        break;
    case B_GLO:
        select_page(PG_INFINITY);
        break;
    case B_FX:
        select_page(PG_FX);
        break;
    case B_SEL:
        if (ui.page == PG_PRESETS)
            load_preset(ui.preset_hi);
        else
            select_page(PG_PRESETS);
        break;
    case B_ENV:
        P[D_HOLD] ^= 1u;
        comp_post(EV_DRONE_HOLD, TR_DRONE, 0, 0);
        app_say("DRONE HOLD", P[D_HOLD] ? "ON" : "OFF");
        dirty = 1;
        ui.redraw_cards = 1;
        break;
    case B_ARP: {
        static uint8_t last = ARP_UP;
        if (P[E_ARP] != ARP_OFF) {
            last = P[E_ARP];
            P[E_ARP] = ARP_OFF;
            comp_post(EV_RELEASE_TRACK, TR_ECHO, 0, 0);
        } else {
            P[E_ARP] = last;
        }
        app_say("ARPEGGIO", ENUM_TEXT[E_ARPS][P[E_ARP]]);
        dirty = 1;
        ui.redraw_cards = 1;
        break;
    }
    case B_PLAY:
        comp_post(comp_playing ? EV_STOP : EV_PLAY, 0, 0, 0);
        if (!comp_playing)
            app_say(P[G_MODE] == MODE_INFINITY ? "INFINITY" : "SEQUENCE", ENUM_TEXT[E_MOODS][P[G_MOOD]]);
        break;
    case B_REC:
        ui.rec ^= 1u;
        app_say("RECORD", ui.rec ? TRACK_NAME[page_track()] : "OFF");
        if (ui.rec)
            ui.track = (uint8_t)page_track();
        break;
    case B_SAVE:
        save_all();
        break;
    case B_OCTDN:
    case B_OCTUP:
        if (ui.page == PG_SEQ) {
            ui.step_page = b == B_OCTUP;
            ui.cursor = (uint8_t)((ui.cursor % 16u) + 16u * ui.step_page);
            ui.redraw_main = ui.redraw_cards = 1;
        } else {
            char t[8];
            set.octave = (int8_t)clamp(set.octave + (b == B_OCTUP ? 1 : -1), -2, 2);
            int_text(set.octave, t);
            app_say("OCTAVE", t);
            dirty = 1;
        }
        break;
    default:
        break;
    }
    ui.redraw_head = 1;
}

static void input(void)
{
    uint32_t k, keys, btn = plat_buttons();
    int32_t e;
    midi_poll();
    touch_poll();
    keys = touch_keys();
    for (k = 0; k < NKEYS; k++) {
        uint32_t bit = 1u << k;
        if ((keys & bit) && !(ui.keys & bit)) {          /* pressed */
            uint32_t v = set.vel ? touch_vel(k) : 100u, t = page_track();
            int32_t note = 53 + (int32_t)k + 12 * (set.octave + TRACK_OCT[t]);
            ui.vel[k] = (uint8_t)v;
            if ((btn >> B_HOME) & 1u) {
                home_key(k);
                ui.key_note[k] = -1;
                continue;
            }
            if (ui.page == PG_SEQ) {
                seq_key(k, 1);
                ui.key_note[k] = -1;
                continue;
            }
            if (set.lock)
                note = scale_quantize(note);
            ui.key_note[k] = note;
            ui.key_track[k] = (uint8_t)t;
            ui.last_note = note;
            comp_post(EV_NOTE_ON, t, note, v);
            plat_midi_send(0x09u | (0x90u | t) << 8 | (uint32_t)note << 16 | v << 24, TO_USB);
            if (ui.rec)
                rec_note(t, note);
        } else if (!(keys & bit) && (ui.keys & bit)) {   /* released */
            if (ui.page == PG_SEQ && ui.key_note[k] < 0)
                seq_key(k, 0);
            if (ui.key_note[k] >= 0) {
                comp_post(EV_NOTE_OFF, ui.key_track[k], ui.key_note[k], 0);
                plat_midi_send(0x08u | (0x80u | ui.key_track[k]) << 8 | (uint32_t)ui.key_note[k] << 16, TO_USB);
                ui.key_note[k] = -1;
            }
        }
    }
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if ((btn & bit) && !(ui.btn & bit))
            button(k);
    }
    freeze((btn >> B_LFO) & 1u);
    if (((btn ^ ui.btn) >> B_HOME) & 1u)
        ui.redraw_main = 1;
    for (k = 0; k < NE; k++) {
        if ((e = plat_enc((int)k)) == 0)
            continue;
        if (k == EN_SELECT)
            select_page((uint32_t)clamp((int32_t)ui.page + e, 0, NPAGES - 1));
        else if (k == EN_ALGO) {
            const page_rows_t *pr = &ROWS[ui.page];
            ui.row[ui.page] = (uint8_t)clamp((int32_t)ui.row[ui.page] + e, 0, pr->n - 1);
            ui.knob_lit = -1;
            ui.redraw_cards = ui.redraw_main = 1;
            if (ann_on())
                ann_say(pr->name[ui.row[ui.page]], 0, ANN_NOW);
        } else if (k == EN_PRESET) {
            switch (ui.page) {
            case PG_HOME:               /* presets, loaded as you turn */
                load_preset((uint32_t)clamp((int32_t)set.preset + e, 0, NPRESETS - 1));
                break;
            case PG_PRESETS:
                ui.preset_hi = (uint8_t)clamp((int32_t)ui.preset_hi + e, 0, NPRESETS - 1);
                ui.redraw_main = 1;
                break;
            case PG_SEQ:                 /* the step cursor */
                ui.cursor = (uint8_t)clamp((int32_t)ui.cursor + e, 0, NSTEP - 1);
                ui.step_page = ui.cursor >= 16u;
                ui.redraw_main = ui.redraw_cards = 1;
                break;
            case PG_INFINITY:
                knob(0, e);              /* the mood */
                break;
            default: {                   /* an engine or other page: the track's volume */
                uint32_t id = TRK_P(page_track(), 0);
                P[id] = (uint8_t)clamp((int32_t)P[id] + e * 2, 0, 127);
                {
                    char t[8];
                    param_text(id, P[id], t);
                    app_say(TRACK_NAME[page_track()], t);
                }
                dirty = 1;
                break;
            }
            }
        } else {
            knob(k - EN_K1, e);
        }
    }
    if (keys != ui.keys)
        ui.redraw_main |= ui.page == PG_SEQ;
    ui.keys = keys;
    ui.btn = btn;
}

void app_input_only(void) { input(); }

/* ------------------------------------------------------------- drawing --- */
static uint16_t shade(uint16_t c, uint32_t a)   /* c at a/255 over black */
{
    uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    return (uint16_t)(((r * a / 255u) << 11) | ((g * a / 255u) << 5) | (b * a / 255u));
}

/* the mark: a ring and a slash through it (Ø), anti-aliased, on black */
static void draw_logo(int32_t cx, int32_t cy, float r, float th, uint16_t ring, uint16_t slash)
{
    int32_t x, y, R = (int32_t)(r + th + 2.0f);
    float ax = (float)cx - r * 0.86f, ay = (float)cy + r * 0.86f, bx = (float)cx + r * 0.86f, by = (float)cy - r * 0.86f;
    float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    for (y = cy - R; y <= cy + R; y++)
        for (x = cx - R; x <= cx + R; x++) {
            float px = (float)x - (float)cx, py = (float)y - (float)cy, d = fm_sqrtf(px * px + py * py);
            float ar = 1.0f - fm_clampf(fm_fabsf(d - r) - th * 0.5f + 0.5f, 0.0f, 1.0f);
            float t = fm_clampf((((float)x - ax) * dx + ((float)y - ay) * dy) / len2, 0.0f, 1.0f);
            float qx = ax + dx * t - (float)x, qy = ay + dy * t - (float)y;
            float as = 1.0f - fm_clampf(fm_sqrtf(qx * qx + qy * qy) - th * 0.42f + 0.5f, 0.0f, 1.0f);
            if (as > 0.01f && as >= ar)
                cv_pset(x, y, shade(slash, (uint32_t)(as * 255.0f)));
            else if (ar > 0.01f)
                cv_pset(x, y, shade(ring, (uint32_t)(ar * 255.0f)));
        }
}

static void disc(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t dy;
    for (dy = -r; dy <= r; dy++) {
        int32_t dx = (int32_t)fm_sqrtf((float)(r * r - dy * dy));
        cv_rect(cx - dx, cy + dy, 2 * dx + 1, 1, c);
    }
}

static void ring(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t i;
    for (i = 0; i < 96; i++) {
        float a = (float)i * (1.0f / 96.0f);
        cv_pset(cx + (int32_t)(fm_sin_turns(a + 0.25f) * (float)r), cy + (int32_t)(fm_sin_turns(a) * (float)r), c);
    }
}

static int32_t text_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    return cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}

static void chord_name(char *t)
{
    static const char *const SFX[15] = {"", " MAJ7", " MIN7", " ADD9", " SUS2", " SUS4", " MIN9", " 5", " QRTL", "",
                                        " 7", " M7B5", " MMAJ7", " +MAJ7", " ADD"};
    t[0] = 0;
    if (comp_chord_root_pc < 0) {
        text_cat(t, "-");
        return;
    }
    text_cat(t, NOTE_NAME[comp_chord_root_pc % 12]);
    text_cat(t, SFX[(uint32_t)comp_chord_type % 15u]);
}

static void draw_head(void)
{
    uint32_t now = plat_ms(), i;
    char t[16];
    cv_begin(240, 34, C_BLACK);
    draw_logo(15, 15, 8.5f, 2.2f, PINK, INK);
    if (now < ui.msg_until) {
        int32_t x = cv_text(32, 7, &FONT_B, ui.msg[0], INK);
        cv_text(x + 8, 7, &FONT_B, ui.msg[1], PINK);
    } else {
        int32_t x = cv_text(32, 7, &FONT_B, "HALO", INK);
        cv_text(x + 10, 7, &FONT_B, PAGE_NAME[ui.page], PINK);
        if (dirty)
            disc(x + 14 + text_w(&FONT_B, PAGE_NAME[ui.page]) + 4, 15, 2, PINK_MID);
    }
    /* transport: a triangle playing, a square stopped; the tempo; REC */
    if (comp_playing) {
        int32_t y;
        for (y = 0; y < 11; y++)
            cv_rect(190, 10 + y, (y < 6 ? y : 10 - y) + 1, 1, PINK);
    } else {
        cv_rect(190, 11, 9, 9, GREY);
    }
    {   /* the audio load, always in sight: pink under 70 %, white to 90 %, red above (and while the engines are
         * lightened: the quality letter beside it) */
        uint32_t cpu = plat_cpu_pct();
        char q[2] = {dsp_quality >= 2u ? 0 : dsp_quality == 1u ? 'L' : 'E', 0};
        int_text((int32_t)cpu, t);
        text_cat(t, "%");
        cv_text(202, 4, &FONT_XS, t, cpu >= 90u ? RGB(255, 80, 80) : cpu >= 70u ? INK : PINK_MID);
        if (q[0])
            cv_text(230, 4, &FONT_XS, q, RGB(255, 80, 80));
        int_text((int32_t)comp_bpm(), t);
        cv_text(202, 17, &FONT_XS, t, comp_playing ? INK : GREY);
    }
    if (ui.rec)
        disc(232, 22, 4, RGB(255, 96, 140));
    /* where we are: one dot per page */
    for (i = 0; i < NPAGES; i++)
        cv_rect(120 - (int32_t)NPAGES * 5 + (int32_t)i * 10, 30, i == ui.page ? 8 : 4, 2, i == ui.page ? PINK : RULE);
    cv_blit(0, 0);
}

/* the ECHO tone row names what each knob does in the model chosen */
static const char *card_label(uint32_t id)
{
    if (P[E_MODEL] == EM_LEAD) {
        if (id == E_RATIO) return "WAVE";
        if (id == E_INDEX) return "ENV>F";
        if (id == E_IDEC) return "F.DECAY";
        if (id == E_TONE) return "CUTOFF";
    } else if (P[E_MODEL] == EM_PIANO) {
        if (id == E_RATIO) return "-";
        if (id == E_INDEX) return "HAMMER";
        if (id == E_IDEC) return "MELLOW";
        if (id == E_DEC) return "SUSTAIN";
        if (id == E_SUS) return "-";
    }
    return id < NPARAM ? PARAM_INFO[id].label : vlabel(id);
}

static void draw_cards(void)
{
    const page_rows_t *pr = &ROWS[ui.page];
    uint32_t row = ui.row[ui.page] % pr->n, k, now = plat_ms();
    cv_begin(240, 60, C_BLACK);
    cv_rect(0, 0, 240, 1, RULE);
    for (k = 0; k < 4u; k++) {
        uint32_t id = pr->id[row][k];
        int32_t x = 2 + (int32_t)k * 60;
        int lit = ui.knob_lit == (int8_t)k && now - ui.knob_at < 900u;
        char v[24], kn[4] = {'K', (char)('1' + k), 0, 0};
        float pos;
        if (id == V_NONE)
            continue;
        cv_text(x + 2, 5, &FONT_XS, kn, lit ? PINK : DIMG);
        cv_text(x + 16, 5, &FONT_XS, card_label(id), lit ? INK : GREY);
        if (id < NPARAM)
            param_text(id, P[id], v);
        else
            vtext(id, v);
        if (text_w(&FONT_B, v) > 56)
            cv_text(x + 2, 22, &FONT_XS, v, lit ? PINK_HI : PINK);
        else
            cv_text(x + 2, 19, &FONT_B, v, lit ? PINK_HI : PINK);
        pos = id < NPARAM ? (float)P[id] / (float)param_max(id) : vpos(id);
        if (id < NPARAM && PARAM_INFO[id].fmt == F_BIP) {   /* bipolar: from the middle */
            int32_t mid = x + 28, w = (int32_t)((pos - 0.5f) * 54.0f);
            cv_rect(x + 2, 42, 54, 2, RULE);
            if (w >= 0)
                cv_rect(mid, 41, w + 1, 4, lit ? PINK_HI : PINK_MID);
            else
                cv_rect(mid + w, 41, -w + 1, 4, lit ? PINK_HI : PINK_MID);
            cv_rect(mid, 39, 1, 8, GREY);
        } else if (pos >= 0.0f) {
            cv_rect(x + 2, 42, 54, 2, RULE);
            cv_rect(x + 2, 41, (int32_t)(pos * 54.0f) + 1, 4, lit ? PINK_HI : PINK_MID);
        }
    }
    /* the row: its name and where it sits among the page's rows */
    text_c(120, 49, &FONT_XS, pr->name[row], DIMG);
    for (k = 0; k < pr->n && pr->n > 1u; k++)
        cv_rect(214 + (int32_t)k * 4, 53, 2, 2, k == row ? PINK : DIMG);
    cv_blit(0, 180);
}

/* HOME: the orbit, five tracks circling a small halo, their size their level */
static void draw_orbit(void)
{
    uint32_t i, now = plat_ms();
    static const uint8_t STAR[14][2] = {{6, 10}, {100, 8}, {20, 96}, {96, 90}, {54, 4}, {4, 54}, {106, 48},
                                        {30, 30}, {84, 26}, {78, 78}, {16, 74}, {64, 100}, {104, 100}, {40, 60}};
    float base = (float)(now % 240000u) * (1.0f / 240000.0f);
    cv_begin(112, 104, C_BLACK);
    for (i = 0; i < 14u; i++) {           /* the stars twinkle */
        float tw = 0.5f + 0.5f * fm_sin_turns(base * (float)(7u + i * 3u) + (float)i * 0.37f);
        cv_pset(STAR[i][0], STAR[i][1], shade(PINK_HI, (uint32_t)(40.0f + 150.0f * tw)));
    }
    ring(56, 52, 44, PINK_DEEP);
    ring(56, 52, 30, PINK_DEEP);
    draw_logo(56, 52, 9.0f, 2.0f, PINK, INK);
    for (i = 0; i < NTRACK; i++) {
        float a = base * (float)(3u + i) + (float)i * 0.2f, m = fm_clampf(dsp_meter[i] * 6.0f, 0.0f, 1.0f);
        int32_t r = i & 1u ? 30 : 44;
        int32_t x = 56 + (int32_t)(fm_sin_turns(a + 0.25f) * (float)r), y = 52 + (int32_t)(fm_sin_turns(a) * (float)r);
        int muted = ((dsp_mute >> i) & 1u) || (dsp_solo && !((dsp_solo >> i) & 1u));
        uint16_t c = muted ? DIMG : m > 0.02f ? PINK : PINK_MID;
        disc(x, y, 2 + (int32_t)(m * 5.0f), c);
        if ((dsp_solo >> i) & 1u)
            ring(x, y, 9, INK);
    }
    cv_blit(4, 36 + 34);
}

static void draw_home(void)
{
    char t[32];
    uint32_t i, held = (ui.btn >> B_HOME) & 1u;
    const preset_t *p = &PRESETS[set.preset % NPRESETS];
    cv_begin(240, 140, C_BLACK);
    int_text(set.preset + 1, t);
    if (set.preset < 9u) {
        char z[4] = "0";
        text_cat(z, t);
        t[0] = 0;
        text_cat(t, z);
    }
    cv_text(8, 4, &FONT_B, t, PINK_MID);
    cv_text(30, 4, &FONT_B, p->name, INK);
    cv_text(8, 22, &FONT_XS, p->about, GREY);
    /* the right column: the composer's state */
    cv_text(124, 40, &FONT_XS, "MOOD", DIMG);
    cv_text(124, 52, &FONT_B, ENUM_TEXT[E_MOODS][P[G_MOOD] % NMOODS], PINK);
    t[0] = 0;
    text_cat(t, NOTE_NAME[P[G_KEY] % 12u]);
    text_cat(t, " ");
    text_cat(t, SCALE_NAME[P[G_SCALE] % NSCALES]);
    cv_text(124, 72, &FONT_XS, t, INK);
    cv_text(124, 88, &FONT_XS, "CHORD", DIMG);
    chord_name(t);
    cv_text(164, 86, &FONT_B, t, comp_chord_root_pc >= 0 ? INK : DIMG);
    cv_text(124, 106, &FONT_XS, P[G_MODE] == MODE_INFINITY ? "INFINITY" : "SEQUENCE", PINK_MID);
    if (comp_playing) {
        char b[12];
        t[0] = 0;
        text_cat(t, "BAR ");
        int_text((int32_t)comp_bar + 1, b);
        text_cat(t, b);
        cv_text(190, 106, &FONT_XS, t, GREY);
    } else {
        cv_text(176, 106, &FONT_XS, "PRESS PLAY", GREY);
    }
    /* the tracks: muted grey, soloed lit; HOME held shows the keys for it */
    for (i = 0; i < NTRACK; i++) {
        int muted = (dsp_mute >> i) & 1u, solo = (dsp_solo >> i) & 1u;
        int32_t x = 124 + (int32_t)i * 23;
        cv_text(x, 124, &FONT_XS, TRACK_SHORT[i], muted ? DIMG : solo ? PINK_HI : GREY);
        if (solo)
            cv_rect(x, 136, 18, 2, PINK);
    }
    if (held)
        cv_text(8, 124, &FONT_XS, "WHITE: SOLO  BLACK: MUTE", PINK);
    cv_blit(0, 36);
    draw_orbit();
}

/* SYNTH: one cycle of the oscillator's shape, as the morph sets it */
static void draw_wave(int32_t x0, int32_t y0, int32_t w, int32_t h, float shape)
{
    int32_t i, py = 0;
    cv_rect(x0, y0 + h / 2, w, 1, RULE);
    for (i = 0; i < w; i++) {
        float ph = (float)i / (float)w, s = fm_sin_turns(ph), tri = 1.0f - 4.0f * fm_fabsf(ph - 0.5f);
        float saw = 2.0f * ph - 1.0f, v = shape < 0.5f ? s + (tri - s) * shape * 2.0f : tri + (saw - tri) * (shape * 2.0f - 1.0f);
        int32_t y = y0 + h / 2 - (int32_t)(v * (float)(h / 2 - 2));
        if (i)
            cv_line(x0 + i - 1, py, x0 + i, y, PINK);
        py = y;
    }
}

static void voices_line(uint32_t t, int32_t y)
{
    char s[32], n[8];
    uint32_t a = dsp_active(t);
    s[0] = 0;
    int_text((int32_t)a, n);
    text_cat(s, n);
    text_cat(s, a == 1u ? " VOICE SOUNDING" : " VOICES SOUNDING");
    cv_text(8, y, &FONT_XS, s, a ? PINK_MID : DIMG);
}

static void draw_engine(void)
{
    char t[32];
    uint32_t i, tr = page_track();
    cv_begin(240, 140, C_BLACK);
    cv_text(8, 2, &FONT_XS, TRACK_NAME[tr], DIMG);
    {
        static const char *const ROLE[NTRACK] = {"ATMOSPHERIC PADS", "EVOLVING DRONE", "CELESTIAL HARMONICS",
                                                 "WORDLESS CHOIR", "BELLS, PLUCKS, ARPEGGIOS"};
        static const char *const ECHO_ROLE[3] = {"BELLS, PLUCKS, ARPEGGIOS", "A SOFT FELT PIANO", "A SINGING ANALOG LEAD"};
        cv_text(8, 14, &FONT_B, tr == TR_ECHO ? ECHO_ROLE[P[E_MODEL] % 3u] : ROLE[tr], INK);
    }
    switch (ui.page) {
    case PG_SYNTH:
        draw_wave(8, 40, 140, 70, (float)P[A_SHAPE] / 127.0f);
        cv_text(160, 44, &FONT_XS, "CUTOFF", DIMG);
        param_text(A_CUT, P[A_CUT], t);
        cv_text(160, 56, &FONT_B, t, PINK);
        cv_text(160, 80, &FONT_XS, "CHORD", DIMG);
        param_text(G_CHORD, P[G_CHORD], t);
        cv_text(160, 92, &FONT_B, t, PINK);
        break;
    case PG_DRONE: {
        static const float H[8] = {1, 2, 3, 4, 5, 6, 8, 10};
        float dens = (float)P[D_DENS] / 127.0f * 7.0f + 1.0f, tilt = (float)P[D_TONE] / 127.0f;
        for (i = 0; i < 10u; i++) {      /* the spectrum: sub, partials 1..8, the fifth */
            float a = i == 0u ? (float)P[D_SUB] / 127.0f
                      : i == 9u ? (float)P[D_FIFTH] / 127.0f
                      : fm_clampf(dens - (float)(i - 1u), 0.0f, 1.0f) * fm_powf(H[i - 1u], -1.6f + tilt * 1.3f);
            int32_t hgt = (int32_t)(fm_clampf(a, 0.0f, 1.0f) * 60.0f) + 1;
            cv_rect(8 + (int32_t)i * 13, 108 - hgt, 9, hgt, i == 0u || i == 9u ? PINK_MID : PINK);
        }
        cv_text(8, 112, &FONT_XS, "SUB  1  2  3  4  5  6  8  10  5TH", DIMG);
        cv_text(150, 44, &FONT_XS, "ROOT NOW", DIMG);
        {
            float hz = dsp_drone_hz;
            int32_t note = 69 + (int32_t)(12.0f * fm_log2f(fm_maxf(hz, 8.0f) / 440.0f) + 100.5f) - 100;
            note_text(note, t);
            cv_text(150, 56, &FONT_M, t, dsp_active(TR_DRONE) ? PINK : DIMG);
        }
        cv_text(150, 88, &FONT_XS, "HOLD (ENV)", DIMG);
        cv_text(150, 100, &FONT_B, P[D_HOLD] ? "ON" : "OFF", P[D_HOLD] ? PINK : GREY);
        break;
    }
    case PG_SHIMMER: {
        uint32_t seed = 1234u;
        float sp = (float)P[S_SPARK] / 127.0f, in = (float)P[S_INT] / 127.0f;
        for (i = 0; i < 60u; i++) {      /* a field of stars: denser with intensity, brighter with sparkle */
            int32_t x, y;
            seed = seed * 1664525u + 1013904223u;
            x = 8 + (int32_t)((seed >> 8) % 224u);
            y = 40 + (int32_t)((seed >> 20) % 80u);
            if ((float)i > in * 60.0f)
                break;
            cv_pset(x, y, shade(PINK_HI, (uint32_t)(80.0f + 170.0f * sp * (float)((seed >> 4) & 7u) / 7.0f)));
            if (((seed >> 12) & 7u) == 0u)
                cv_rect(x - 1, y, 3, 1, PINK), cv_rect(x, y - 1, 1, 3, PINK);
        }
        cv_text(8, 112, &FONT_XS, "FOLLOWS THE PAD'S CHORDS, OCTAVES ABOVE", DIMG);
        break;
    }
    case PG_ARIA: {
        static const char *const V[5] = {"AH", "EH", "EE", "OH", "OO"};
        for (i = 0; i < 5u; i++) {
            int on = i == P[V_VOWEL];
            int32_t x = 8 + (int32_t)i * 46;
            if (on)
                cv_rect(x - 2, 46, 40, 30, PINK_DEEP);
            text_c(x + 18, 50, &FONT_M, V[i], on ? PINK : DIMG);
        }
        {
            int32_t span = (int32_t)((float)P[V_DRIFT] / 127.0f * 92.0f), cx = 26 + (int32_t)P[V_VOWEL] * 46;
            cv_rect(cx - span, 82, 2 * span + 1, 2, PINK_MID);
        }
        cv_text(8, 92, &FONT_XS, "THE LINE: HOW FAR THE VOWEL DRIFTS", DIMG);
        break;
    }
    case PG_ECHO: {
        /* the envelope's shape */
        float a = (float)P[E_ATK] / 127.0f, d = (float)P[E_DEC] / 127.0f, s = (float)P[E_SUS] / 127.0f,
              r = (float)P[E_REL] / 127.0f;
        int32_t x0 = 8, y0 = 110, xa = x0 + 4 + (int32_t)(a * 40.0f), xd = xa + 6 + (int32_t)(d * 50.0f),
                xs = xd + 24, xr = xs + 4 + (int32_t)(r * 40.0f), ys = y0 - (int32_t)(s * 60.0f);
        cv_line(x0, y0, xa, y0 - 60, PINK);
        cv_line(xa, y0 - 60, xd, ys, PINK);
        cv_line(xd, ys, xs, ys, PINK);
        cv_line(xs, ys, xr, y0, PINK);
        cv_text(160, 44, &FONT_XS, "ARPEGGIO (ARP)", DIMG);
        cv_text(160, 56, &FONT_B, ENUM_TEXT[E_ARPS][P[E_ARP] % 6u], P[E_ARP] ? PINK : GREY);
        cv_text(160, 80, &FONT_XS, "MODEL", DIMG);
        {
            static const char *const MN[3] = {"FM", "PIANO", "LEAD"};
            cv_text(160, 92, &FONT_B, MN[P[E_MODEL] % 3u], PINK);
        }
        break;
    }
    default:
        break;
    }
    voices_line(tr, 128);
    cv_blit(0, 36);
}

static void draw_seq(void)
{
    track_seq_t *ts = &song.t[ui.track];
    uint32_t i, pat = comp_edit_pat[ui.track];
    char t[40], n[12];
    cv_begin(240, 140, C_BLACK);
    for (i = 0; i < NTRACK; i++) {       /* the tracks (black keys 1-5) */
        int sel = i == ui.track;
        int32_t x = 8 + (int32_t)i * 46;
        if (sel)
            cv_rect(x - 2, 1, 42, 14, PINK_DEEP);
        static const char *const SEQ_NAME[NTRACK] = {"ATMOS", "DRONE", "SHIM", "ARIA", "ECHO"};
        cv_text(x, 3, &FONT_XS, SEQ_NAME[i], sel ? PINK : (dsp_mute >> i) & 1u ? DIMG : GREY);
    }
    for (i = 0; i < NSTEP; i++) {        /* the steps: two rows of 16 */
        step_t s = ts->s[pat][i];
        int32_t x = 8 + (int32_t)(i % 16u) * 14, y = 24 + (int32_t)(i / 16u) * 24;
        int in = i < ts->len, on = (s.note & STEP_ON) != 0, play = comp_playing && comp_play_pat[ui.track] == pat
                                                                    && comp_step[ui.track] == i;
        uint16_t edge = !in ? RULE : (i / 16u) == ui.step_page ? DIMG : RULE;
        cv_rect(x, y, 12, 18, edge);
        cv_rect(x + 1, y + 1, 10, 16, C_BLACK);
        if (on && in) {
            int32_t h = (int32_t)(STEP_PROB(s) + 1u) * 2;
            cv_rect(x + 1, y + 17 - h, 10, h, play ? PINK_HI : PINK);
        } else if (on) {
            cv_rect(x + 3, y + 7, 6, 4, PINK_DEEP);
        }
        if (play)
            cv_rect(x, y + 19, 12, 2, INK);
        if (i == ui.cursor) {
            cv_rect(x - 1, y - 1, 14, 1, INK);
            cv_rect(x - 1, y + 18, 14, 1, INK);
        }
    }
    /* the cursor's step in words */
    {
        step_t s = ts->s[pat][ui.cursor];
        t[0] = 0;
        text_cat(t, "STEP ");
        int_text(ui.cursor + 1, n);
        text_cat(t, n);
        cv_text(8, 76, &FONT_XS, t, GREY);
        if (s.note & STEP_ON) {
            note_text(STEP_NOTE(s), t);
            if (ui.track == TR_ATMOS && P[G_CHORD] != CH_NOTES)
                text_cat(t, " CHORD");
            cv_text(8, 88, &FONT_B, t, PINK);
            t[0] = 0;
            int_text((int32_t)(STEP_PROB(s) + 1u) * 125 / 10, n);
            text_cat(t, n);
            text_cat(t, "%  LEN ");
            int_text((int32_t)STEP_LEN(s), n);
            text_cat(t, n);
            cv_text(110, 92, &FONT_XS, t, GREY);
        } else {
            cv_text(8, 88, &FONT_B, "REST", DIMG);
        }
    }
    t[0] = 0;
    text_cat(t, "PATTERN ");
    n[0] = (char)('A' + pat);
    n[1] = 0;
    text_cat(t, n);
    text_cat(t, "  ");
    text_cat(t, DIV_NAME[ts->div % NDIVS]);
    if (ts->chain) {
        text_cat(t, "  CHAIN ");
        for (i = 0; i < NPAT; i++)
            if ((ts->chain >> i) & 1u) {
                n[0] = (char)('A' + i);
                text_cat(t, n);
            }
    }
    cv_text(8, 110, &FONT_XS, t, GREY);
    cv_text(8, 124, &FONT_XS, "WHITE: STEPS  BLACK 1-5: TRACK  6-9: PATTERN", DIMG);
    if (P[G_MODE] != MODE_SEQ)
        cv_text(150, 76, &FONT_XS, "MODE IS INFINITY", PINK_MID);
    cv_blit(0, 36);
}

static void draw_infinity(void)
{
    static const char *const SECTION[5] = {"INTRO", "BLOOM", "SONG", "THINNING", "SWELL"};
    char t[32], n[8];
    cv_begin(240, 140, C_BLACK);
    text_c(120, 4, &FONT_L, ENUM_TEXT[E_MOODS][P[G_MOOD] % NMOODS], PINK);
    t[0] = 0;
    text_cat(t, NOTE_NAME[P[G_KEY] % 12u]);
    text_cat(t, " ");
    text_cat(t, SCALE_NAME[P[G_SCALE] % NSCALES]);
    text_cat(t, "   ");
    int_text((int32_t)comp_bpm(), n);
    text_cat(t, n);
    text_cat(t, " BPM");
    text_c(120, 40, &FONT_XS, t, INK);
    if (P[G_MODE] != MODE_INFINITY) {
        text_c(120, 70, &FONT_B, "MODE IS SEQUENCE", GREY);
        text_c(120, 92, &FONT_XS, "ALGO TO ROW 3, K1 TO INFINITY", DIMG);
    } else if (!comp_playing) {
        text_c(120, 70, &FONT_B, "PRESS PLAY", PINK_MID);
        text_c(120, 92, &FONT_XS, "IT COMPOSES, FOREVER, IN THIS MOOD", DIMG);
    } else {
        cv_text(16, 62, &FONT_XS, "CHORD", DIMG);
        chord_name(t);
        cv_text(16, 74, &FONT_M, t, INK);
        cv_text(140, 62, &FONT_XS, "SECTION", DIMG);
        cv_text(140, 76, &FONT_B, SECTION[comp_section % 5u], PINK);
        cv_text(16, 106, &FONT_XS, "ENERGY", DIMG);
        cv_rect(60, 109, 120, 3, RULE);
        cv_rect(60, 108, (int32_t)(fm_clampf(comp_energy, 0.0f, 1.0f) * 120.0f) + 1, 5, PINK);
        t[0] = 0;
        text_cat(t, "BAR ");
        int_text((int32_t)comp_bar + 1, n);
        text_cat(t, n);
        cv_text(190, 106, &FONT_XS, t, GREY);
    }
    cv_blit(0, 36);
}

static void draw_fx(void)
{
    char t[24];
    cv_begin(240, 140, C_BLACK);
    cv_text(8, 2, &FONT_XS, "REVERB", DIMG);
    param_text(R_TYPE, P[R_TYPE], t);
    cv_text(8, 14, &FONT_M, t, ui.freeze ? PINK_HI : PINK);
    {                                    /* the space: a tail drawn as a decaying curve, longer with DECAY and SPACE */
        int32_t i, py = 0;
        float dec = (float)P[R_DECAY] / 127.0f + (float)((int32_t)P[M_SPACE] - 64) / 180.0f;
        for (i = 0; i < 220; i++) {
            float env = fm_expf(-(float)i / (8.0f + 160.0f * fm_clampf(dec, 0.0f, 1.0f)));
            int32_t y = 92 - (int32_t)(env * 44.0f);
            if (i)
                cv_line(8 + i - 1, py, 8 + i, y, P[R_TYPE] == RT_DARK ? PINK_MID : PINK);
            py = y;
        }
    }
    cv_text(8, 100, &FONT_XS, "DELAY", DIMG);
    param_text(L_TIME, P[L_TIME], t);
    cv_text(50, 98, &FONT_B, t, INK);
    param_text(L_MODE, P[L_MODE], t);
    cv_text(110, 100, &FONT_XS, t, GREY);
    cv_text(8, 124, &FONT_XS, "HOLD LFO: FREEZE THE ROOM", DIMG);
    cv_blit(0, 36);
}

static void draw_presets(void)
{
    int32_t i, first = clamp((int32_t)ui.preset_hi - 3, 0, NPRESETS - 7);
    char t[8];
    cv_begin(240, 140, C_BLACK);
    for (i = 0; i < 7; i++) {
        int32_t p = first + i, y = 2 + i * 16;
        int hi = p == (int32_t)ui.preset_hi, cur = p == (int32_t)set.preset;
        if (hi)
            cv_rect(4, y - 1, 232, 15, PINK_DEEP);
        int_text(p + 1, t);
        cv_text(10, y + 1, &FONT_XS, t, hi ? PINK : DIMG);
        cv_text(30, y + 1, &FONT_XS, PRESETS[p].name, hi ? INK : cur ? PINK_MID : GREY);
        if (cur)
            disc(228, y + 6, 2, PINK);
    }
    cv_text(8, 116, &FONT_XS, PRESETS[ui.preset_hi].about, PINK_MID);
    cv_text(8, 128, &FONT_XS, "PRESET OR K1: CHOOSE   SEL: LOAD", DIMG);
    cv_blit(0, 36);
}

static void draw_settings(void)
{
    char t[64];
    uint32_t cpu = plat_cpu_pct();
    cv_begin(240, 140, C_BLACK);
    draw_logo(40, 50, 26.0f, 4.5f, PINK, INK);
    cv_text(80, 30, &FONT_M, "HALO", INK);
    cv_text(80, 58, &FONT_XS, "VERSION " HALO_VERSION, GREY);
    cv_text(80, 70, &FONT_XS, "ONE DEVICE.", PINK_MID);
    cv_text(80, 82, &FONT_XS, "INFINITE ATMOSPHERES.", PINK_MID);
    t[0] = 0;
    text_cat(t, "AUDIO LOAD ");
    {
        char n[8];
        int_text((int32_t)cpu, n);
        text_cat(t, n);
    }
    text_cat(t, "%   ENGINES ");
    text_cat(t, dsp_quality >= 2u ? "FULL" : dsp_quality == 1u ? "LIGHT" : "LIGHTEST");

    cv_text(8, 100, &FONT_XS, t, GREY);
    {
        char n[12];
        t[0] = 0;
        text_cat(t, "XRUNS ");
        int_text((int32_t)plat_xruns(), n);
        text_cat(t, n);
        cv_text(8, 112, &FONT_XS, t, DIMG);
    }
    cv_text(8, 124, &FONT_XS, "SAVE KEEPS SOUND, SONG AND SETTINGS", DIMG);
    cv_blit(0, 36);
}

static void draw_main(void)
{
    switch (ui.page) {
    case PG_HOME:
        draw_home();
        break;
    case PG_SEQ:
        draw_seq();
        break;
    case PG_INFINITY:
        draw_infinity();
        break;
    case PG_FX:
        draw_fx();
        break;
    case PG_PRESETS:
        draw_presets();
        break;
    case PG_SETTINGS:
        draw_settings();
        break;
    default:
        draw_engine();
        break;
    }
}

void app_splash(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    cv_begin(240, 120, C_BLACK);
    draw_logo(120, 62, 40.0f, 6.0f, PINK, INK);
    cv_blit(0, 30);
    cv_begin(240, 80, C_BLACK);
    text_c(120, 6, &FONT_L, "HALO", INK);
    text_c(120, 46, &FONT_XS, "ONE DEVICE. INFINITE ATMOSPHERES.", PINK_MID);
    cv_blit(0, 150);
}

void app_ui_init(void)
{
    touch_on(1);
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.redraw_head = ui.redraw_main = ui.redraw_cards = 1;
}

/* the built-in speaker (nothing in the headphone jack): a low cut, so the drone and the kick do not overdrive it */
static void speaker_check(void)
{
    int hp = plat_headphones();
    dsp_speaker = hp == 0;             /* -1 (not read yet): as headphones, the full range */
}

static void lights(void)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    uint32_t b = 0, k = 0, dim = 0, i;
    b |= (uint32_t)(ui.page == PG_HOME) << B_HOME;
    b |= (uint32_t)(ui.page >= PG_SYNTH && ui.page <= PG_ECHO) << B_EDIT;
    b |= (uint32_t)(ui.page == PG_SEQ) << B_SEQ;
    b |= (uint32_t)(ui.page == PG_INFINITY) << B_GLO;
    b |= (uint32_t)(ui.page == PG_FX) << B_FX;
    b |= (uint32_t)(ui.page == PG_PRESETS) << B_SEL;
    b |= (uint32_t)(P[D_HOLD] != 0) << B_ENV;
    b |= (uint32_t)(ui.freeze != 0) << B_LFO;
    b |= (uint32_t)(P[E_ARP] != 0) << B_ARP;
    b |= (uint32_t)(ui.rec != 0) << B_REC;
    b |= (uint32_t)(dirty != 0) << B_SAVE;
    if (comp_playing)
        b |= (1u << B_PLAY) | LED_PLAY_GREEN;
    if (ui.page == PG_SEQ) {             /* the steps on the white keys, the playing one bright */
        track_seq_t *ts = &song.t[ui.track];
        uint32_t pat = comp_edit_pat[ui.track];
        for (i = 0; i < 16u; i++) {
            uint32_t pos = ui.step_page * 16u + i;
            if (pos < ts->len && (ts->s[pat][pos].note & STEP_ON))
                dim |= 1u << WK[i];
            if (comp_playing && comp_step[ui.track] == pos && comp_play_pat[ui.track] == pat)
                k |= 1u << WK[i];
        }
        k |= 1u << BK[ui.track];
        dim |= 1u << BK[NTRACK + comp_edit_pat[ui.track]];
    } else if ((ui.btn >> B_HOME) & 1u) {
        for (i = 0; i < NTRACK; i++) {
            if ((dsp_solo >> i) & 1u)
                k |= 1u << WK[i];
            if ((dsp_mute >> i) & 1u)
                k |= 1u << BK[i];
            dim |= (1u << WK[i]) | (1u << BK[i]);
        }
    } else {
        k = ui.keys;
    }
    plat_lights(b, k, 0, dim, 1);
}

void app_frame(void)
{
    uint32_t now = plat_ms(), i;
    static uint32_t last_draw_main, cards_lit_until;
    input();
    speaker_check();
    /* AUTO quality: a level back up only after 10 s with the load under 50 % (a step up is about +10 %: no
     * bouncing between levels, each bounce a dropout) */
    {
        static uint32_t calm_since;
        if (plat_cpu_pct() >= 50u || dsp_quality >= 2u)
            calm_since = now;
        if (set.qual == 0u && dsp_quality < 2u && now - overload_at > 10000u && now - calm_since > 10000u) {
            dsp_quality++;
            overload_at = now;
            calm_since = now;
        }
    }
    /* the header's load figure, twice a second */
    {
        static uint32_t load_at, load_seen = 0xFFFFu;
        uint32_t c = plat_cpu_pct() | (uint32_t)dsp_quality << 8 | plat_xruns() << 10;
        if (now - load_at > 500u && c != load_seen) {
            load_at = now;
            load_seen = c;
            ui.redraw_head = 1;
        }
    }
    if (ui.msg_until && now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.redraw_head = 1;
    }
    if (ui.knob_lit >= 0 && now - ui.knob_at >= 900u && cards_lit_until != ui.knob_at) {
        cards_lit_until = ui.knob_at;
        ui.redraw_cards = 1;
    }
    /* what moves on its own: the step, the chord, the bar, the transport */
    {
        uint32_t st = comp_step[ui.track] | (uint32_t)comp_playing << 8 | (uint32_t)comp_play_pat[ui.track] << 9;
        uint32_t ch = (uint32_t)(comp_chord_root_pc + 1) | (uint32_t)comp_chord_type << 8 | (uint32_t)comp_section << 16;
        if (st != ui.seen_step) {
            ui.seen_step = st;
            ui.redraw_head = 1;
            if (ui.page == PG_SEQ)
                ui.redraw_main = 1;
        }
        if (ch != ui.seen_chord || comp_bar != ui.seen_bar) {
            ui.seen_chord = ch;
            ui.seen_bar = comp_bar;
            if (ui.page == PG_HOME || ui.page == PG_INFINITY)
                ui.redraw_main = 1;
        }
        for (i = 0; i < NTRACK; i++)
            if (comp_note_flash[i] != ui.seen_flash[i]) {
                ui.seen_flash[i] = comp_note_flash[i];
                if (ui.page >= PG_SYNTH && ui.page <= PG_ECHO && now - last_draw_main > 250u)
                    ui.redraw_main = 1;
            }
    }
    if (ui.redraw_head) {
        ui.redraw_head = 0;
        draw_head();
    }
    if (ui.redraw_main) {
        ui.redraw_main = 0;
        last_draw_main = now;
        draw_main();
        ui.vis_at = now;
    } else if (ui.page == PG_HOME && now - ui.vis_at >= 66u) {   /* the orbit, ~15 frames a second */
        ui.vis_at = now;
        draw_orbit();
    } else if (ui.page >= PG_SYNTH && ui.page <= PG_ECHO && now - ui.vis_at >= 400u) {   /* live: voices, root */
        ui.vis_at = now;
        draw_main();
    } else if (ui.page == PG_SETTINGS && now - ui.vis_at >= 1000u) {
        ui.vis_at = now;
        draw_main();
    }
    if (ui.redraw_cards) {
        ui.redraw_cards = 0;
        draw_cards();
    }
    lights();
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "page"))
        *got = ui.page;
    else if (!strcmp(what, "playing"))
        *got = comp_playing;
    else if (!strcmp(what, "preset"))
        *got = set.preset;
    else if (!strcmp(what, "track"))
        *got = ui.track;
    else if (!strcmp(what, "dirty"))
        *got = dirty;
    else if (!strcmp(what, "chord_n"))
        *got = comp_chord_n;
    else if (!strcmp(what, "cursor"))
        *got = ui.cursor;
    else if (!strcmp(what, "hold"))
        *got = P[D_HOLD];
    else if (!strcmp(what, "quality"))
        *got = dsp_quality;
    else if (!strcmp(what, "mute"))
        *got = dsp_mute;
    else if (!strcmp(what, "solo"))
        *got = dsp_solo;
    else if (!strcmp(what, "rec"))
        *got = ui.rec;
    else if (!strcmp(what, "bar"))
        *got = (int)comp_bar;
    else if (!strcmp(what, "drone_note"))   /* the drone's root now, as a MIDI note */
        *got = (int)(12.0f * fm_log2f(fm_maxf(dsp_drone_hz, 8.0f) / 440.0f) + 69.5f);
    else if (!strcmp(what, "model"))
        *got = P[E_MODEL];
    else if (!strcmp(what, "mode"))
        *got = P[G_MODE];
    else if (!strcmp(what, "beats"))        /* the soft beat has played (0 / 1) */
        *got = dsp_beats != 0;
    else if (!strcmp(what, "energy"))
        *got = (int)(comp_energy * 100.0f);
    else if (!strncmp(what, "active", 6))   /* activeN: voices sounding on track N */
        *got = (int)dsp_active((uint32_t)atoi(what + 6) % NTRACK);
    else if (!strncmp(what, "played", 6))   /* playedN: track N has started a note since boot (0 / 1) */
        *got = comp_note_flash[(uint32_t)atoi(what + 6) % NTRACK] != 0;
    else if (!strncmp(what, "flash", 5))    /* flashN: notes started on track N */
        *got = (int)comp_note_flash[(uint32_t)atoi(what + 5) % NTRACK];
    else if (!strncmp(what, "step_on", 7))  /* step_onN: step N of the edited pattern of the selected track is on */
        *got = (song.t[ui.track].s[comp_edit_pat[ui.track]][(uint32_t)atoi(what + 7) % NSTEP].note & STEP_ON) != 0;
    else if (what[0] == 'p' && what[1] >= '0' && what[1] <= '9')   /* pN: parameter N */
        *got = P[(uint32_t)atoi(what + 1) % NPARAM];
    else
        return 0;
    return 1;
}
#endif
