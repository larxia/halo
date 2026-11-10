/* SPDX-License-Identifier: MPL-2.0
 * HALO — the sound engines and the effects bus. Float32 throughout: the AC79's single-precision FPU, and no
 * 64-bit integer multiplies in the hot loops (felucca-bsp docs/CPU.md: inlined 64-bit multiplies beside 16-bit
 * loads corrupt registers on this CPU). No doubles (fm1-base's build refuses them): every literal ends in f.
 *
 * Signal flow per 32-frame chunk:
 *   ATMOS ─┐                         ┌──> reverb (FDN, 4 lines, shimmer rectifier in the loop) ─┐
 *   DRONE ─┤ each: vol, pan, mute ──>├──> delay (tape / ping-pong, 2:1 decimated, int16) ──────┤
 *   SHIM  ─┤      sends to rev, dly  └──> dry ───────────────────────────────────────────────> master
 *   ARIA  ─┤                                                    master: chorus, phaser, tremolo,
 *   ECHO  ─┘                                                    exciter, noise layer, saturation, limiter
 */
#include "halo_dsp.h"
#include "fastmath.h"
#include "plat.h"

#define FS HALO_FS
#define INV_FS (1.0f / 44100.0f)
#define TWO_PI 6.28318531f

volatile uint8_t dsp_mute, dsp_solo, dsp_quality = 2, dsp_drone_gate;
volatile float dsp_tempo_bpm = 72.0f;
volatile uint8_t dsp_speaker;
volatile float dsp_meter[NTRACK], dsp_out_meter, dsp_drone_hz;
volatile uint32_t dsp_beats;

/* ------------------------------------------------------------------ tools --- */
#define SIN_N 1024u
static float sin_tab[SIN_N + 1u];

static inline __attribute__((always_inline)) float wrap1(float p)
{
    p -= (float)(int32_t)p;
    return p < 0.0f ? p + 1.0f : p;
}
/* sine of a phase in turns (any real phase) */
static inline __attribute__((always_inline)) float sin_t(float ph)
{
    float x = wrap1(ph) * (float)SIN_N;
    uint32_t i = (uint32_t)x;
    float f = x - (float)i;
    i &= SIN_N - 1u;
    return sin_tab[i] + (sin_tab[i + 1u] - sin_tab[i]) * f;
}

static uint32_t rng_state = 0x9E3779B9u;
static inline __attribute__((always_inline)) uint32_t rnd(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}
static inline __attribute__((always_inline)) float rndf(void) { return (float)(rnd() >> 8) * (1.0f / 16777216.0f); }   /* 0..1 */
static inline __attribute__((always_inline)) float rnds(void) { return rndf() * 2.0f - 1.0f; }                        /* -1..1 */

static float warble_semi;               /* the tape's wobble, in semitones, for every engine (X_WARBLE) */
static inline float note_hz(float note) { return 440.0f * fm_exp2f((note + warble_semi - 69.0f) * (1.0f / 12.0f)); }
static inline float u7(uint32_t id) { return (float)P[id] * (1.0f / 127.0f); }                 /* 0..1 */
static inline float b7(uint32_t id) { return ((float)P[id] - 64.0f) * (1.0f / 63.0f); }        /* -1..1 */
/* a 0..127 value as a time: 3 ms .. ~20 s, exponential */
static inline float v_time(float v01) { return 0.003f * fm_exp2f(v01 * 12.7f); }
/* one-pole coefficient for a time constant in seconds (per sample) */
static inline float tc(float sec) { return 1.0f - fm_expf(-INV_FS / fm_maxf(sec, 0.0005f)); }
/* a 0..1 cutoff position as Hz: 30 Hz .. ~19 kHz */
static inline float v_cut(float v01) { return 30.0f * fm_exp2f(fm_clampf(v01, 0.0f, 1.0f) * 9.3f); }
/* LFO rates: 0.01 .. ~10 Hz */
static inline float v_hz(float v01) { return 0.01f * fm_exp2f(v01 * 10.0f); }

/* a state-variable filter (Simper's trapezoidal form): stable under fast coefficient changes */
typedef struct { float ic1, ic2; } svf_t;
typedef struct { float a1, a2, a3, k; } svfc_t;
static void svf_set(svfc_t *c, float hz, float res01)
{
    float g = fm_tanf(3.14159265f * fm_clampf(hz, 20.0f, 0.45f * FS) * INV_FS);
    c->k = 2.0f - 1.94f * fm_clampf(res01, 0.0f, 1.0f);
    c->a1 = 1.0f / (1.0f + g * (g + c->k));
    c->a2 = g * c->a1;
    c->a3 = g * c->a2;
}
static inline __attribute__((always_inline)) float svf_lp(svf_t *s, const svfc_t *c, float v0)
{
    float v3 = v0 - s->ic2, v1 = c->a1 * s->ic1 + c->a2 * v3, v2 = s->ic2 + c->a2 * s->ic1 + c->a3 * v3;
    s->ic1 = 2.0f * v1 - s->ic1;
    s->ic2 = 2.0f * v2 - s->ic2;
    return v2;
}
static inline __attribute__((always_inline)) float svf_bp(svf_t *s, const svfc_t *c, float v0)   /* unity peak */
{
    float v3 = v0 - s->ic2, v1 = c->a1 * s->ic1 + c->a2 * v3, v2 = s->ic2 + c->a2 * s->ic1 + c->a3 * v3;
    s->ic1 = 2.0f * v1 - s->ic1;
    s->ic2 = 2.0f * v2 - s->ic2;
    return c->k * v1;
}

/* band-limited saw (polyBLEP) and the shape morph sine -> triangle -> saw */
static inline __attribute__((always_inline)) float blep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}
static inline __attribute__((always_inline)) float osc_morph(float ph, float dt, float shape)
{
    float s = sin_t(ph), tri = 1.0f - 4.0f * fm_fabsf(ph - 0.5f);
    if (shape < 0.5f)
        return s + (tri - s) * (shape * 2.0f);
    {
        float saw = 2.0f * ph - 1.0f - blep(ph, dt);
        return tri + (saw - tri) * (shape * 2.0f - 1.0f);
    }
}

/* ADSR with exponential segments; times in coefficients (tc) */
enum { EG_IDLE, EG_ATK, EG_DEC, EG_SUS, EG_REL };
typedef struct { float v; uint8_t st; } eg_t;
typedef struct { float a, d, s, r; } egc_t;
static inline __attribute__((always_inline)) float eg_run(eg_t *e, const egc_t *c)
{
    switch (e->st) {
    case EG_ATK:
        e->v += (1.25f - e->v) * c->a;     /* aims past 1 so it gets there */
        if (e->v >= 1.0f) {
            e->v = 1.0f;
            e->st = EG_DEC;
        }
        break;
    case EG_DEC:
        e->v += (c->s - e->v) * c->d;
        if (e->v - c->s < 0.0005f)
            e->st = EG_SUS;
        break;
    case EG_SUS:
        e->v += (c->s - e->v) * c->d;      /* follows SUSTAIN smoothly when it is turned */
        break;
    case EG_REL:
        e->v -= e->v * c->r;
        if (e->v < 0.0002f) {
            e->v = 0.0f;
            e->st = EG_IDLE;
        }
        break;
    default:
        break;
    }
    return e->v;
}

/* a "magic circle" sine oscillator: two multiplies and two adds a sample, no table, no wrap. Used for every
 * fixed-pitch sine partial (drone, shimmer, the pad's air and sub, grains). Its amplitude is renormalised once a
 * chunk (mc_norm) so hours of running, or a gliding frequency, never let it drift. */
typedef struct { float x, y; } mc_t;
static inline float mc_e(float hz) { return 2.0f * fm_sin_turns(fm_minf(hz, 0.45f * FS) * 0.5f * INV_FS); }
static inline __attribute__((always_inline)) float mc_run(mc_t *o, float e)
{
    o->x -= e * o->y;
    o->y += e * o->x;
    return o->y;
}
static inline void mc_norm(mc_t *o, float e)
{
    float inv = o->x * o->x + o->y * o->y - e * o->x * o->y, g;
    if (inv < 1e-6f) {                 /* stopped (never started, or flushed): a fresh start */
        o->x = 1.0f;
        o->y = 0.0f;
        return;
    }
    g = 1.5f - 0.5f * inv;             /* one Newton step toward amplitude 1 */
    o->x *= g;
    o->y *= g;
}
static inline void mc_start(mc_t *o, float turns)
{
    o->x = fm_sin_turns(turns + 0.25f);
    o->y = fm_sin_turns(turns);
}

/* an envelope stepped once per chunk with chunk-sized coefficients, its value interpolated across the chunk */
static void eg_chunkify(egc_t *out, const egc_t *in)
{
    out->a = 1.0f - fm_powf(1.0f - in->a, (float)HALO_CHUNK);
    out->d = 1.0f - fm_powf(1.0f - in->d, (float)HALO_CHUNK);
    out->r = 1.0f - fm_powf(1.0f - in->r, (float)HALO_CHUNK);
    out->s = in->s;
}

/* equal-power pan: p -1..1 */
static inline void pan_gains(float p, float *gl, float *gr)
{
    float a = (fm_clampf(p, -1.0f, 1.0f) + 1.0f) * 0.25f;     /* 0..0.5 turns of a quarter circle */
    *gl = sin_t(0.25f - a * 0.5f);                           /* cos */
    *gr = sin_t(a * 0.5f);
}

/* macros as modulation amounts, -1..1 (64 is neutral) */
static float mac_space, mac_motion, mac_shim, mac_depth;

/* ================================================================ ATMOS === */
typedef struct {
    int32_t note;
    float pitch, hz, ph[3], drift, drift_to, vel, pan;
    mc_t air, sub;
    eg_t eg;
    svf_t fl, fr;
    uint32_t age;
} pad_voice_t;
static struct {
    pad_voice_t v[ATMOS_VOICES];
    egc_t eg, eg32;
    svfc_t cut[ATMOS_VOICES];
    float lfo_ph, morph_ph, shape, det[3], air, sub, width, glide;
    uint32_t clock;
} atm;

static void atmos_cook(void)
{
    uint32_t i;
    float lfo, morph, base, envamt = b7(A_ENV), res = u7(A_RES) * 0.92f;
    float motion = fm_exp2f(mac_motion * 1.6f);
    atm.eg.a = tc(v_time(u7(A_ATK)) * 0.35f);
    atm.eg.d = tc(v_time(u7(A_DEC)) * 0.35f);
    atm.eg.s = u7(A_SUS);
    atm.eg.r = tc(v_time(u7(A_REL)) * 0.3f);
    eg_chunkify(&atm.eg32, &atm.eg);
    atm.lfo_ph = wrap1(atm.lfo_ph + v_hz(u7(A_LFOR)) * motion * (float)HALO_CHUNK * INV_FS);
    atm.morph_ph = wrap1(atm.morph_ph + 0.031f * motion * (float)HALO_CHUNK * INV_FS);
    lfo = sin_t(atm.lfo_ph);
    morph = sin_t(atm.morph_ph) * u7(A_MORPH) * 0.5f;           /* the shape wanders: wavetable-style movement */
    atm.shape = fm_clampf(u7(A_SHAPE) + morph, 0.0f, 1.0f);
    {
        float d = u7(A_DETUNE) * u7(A_DETUNE) * 0.35f;           /* up to ~35 cents */
        atm.det[0] = fm_exp2f(-d / 12.0f);
        atm.det[1] = 1.0f;
        atm.det[2] = fm_exp2f(d * 1.07f / 12.0f);
    }
    atm.air = u7(A_AIR) * (0.55f + 0.45f * fm_maxf(mac_shim, -0.9f));
    atm.sub = u7(A_SUB) * (1.0f + 0.8f * fm_maxf(mac_depth, 0.0f));
    atm.width = u7(A_WIDTH);
    atm.glide = fm_minf(tc(0.45f) * (float)HALO_CHUNK, 1.0f);        /* ~0.45 s glide between chords */
    base = u7(A_CUT) - 0.22f * fm_maxf(mac_depth, 0.0f) + 0.12f * fm_maxf(mac_shim, 0.0f)
           + lfo * u7(A_LFOD) * 0.25f;
    for (i = 0; i < ATMOS_VOICES; i++) {
        pad_voice_t *v = &atm.v[i];
        float c = base + envamt * v->eg.v * 0.45f + (v->vel - 0.7f) * 0.08f;
        if (v->eg.st == EG_IDLE)
            continue;
        svf_set(&atm.cut[i], v_cut(c), res);
        /* drift: each voice wanders a few cents, a new aim every couple of seconds */
        if ((atm.clock & 63u) == (i * 16u))
            v->drift_to = rnds() * u7(A_DRIFT) * 0.18f * motion;    /* semitones */
        v->drift += (v->drift_to - v->drift) * 0.02f;
        v->pitch += ((float)v->note - v->pitch) * atm.glide;   /* chord moves glide (voice leading) */
    }
    atm.clock++;
}

static void atmos_on(int32_t note, uint32_t vel)
{
    uint32_t i, best = 0, best_age = 0;
    pad_voice_t *v;
    for (i = 0; i < ATMOS_VOICES; i++)      /* the same note again: that voice */
        if (atm.v[i].note == note && atm.v[i].eg.st != EG_IDLE) {
            best = i;
            goto take;
        }
    for (i = 0; i < ATMOS_VOICES; i++) {    /* else a free one, else the released-longest, else the oldest */
        uint32_t score = atm.v[i].eg.st == EG_IDLE ? 0xFFFFFFFFu
                         : atm.v[i].eg.st == EG_REL ? 0x80000000u + atm.v[i].age : atm.v[i].age;
        if (score >= best_age) {
            best_age = score;
            best = i;
        }
    }
take:
    v = &atm.v[best];
    for (i = 0; i < ATMOS_VOICES; i++)
        atm.v[i].age++;
    if (v->eg.st == EG_IDLE) {
        v->ph[0] = rndf();
        v->ph[1] = rndf();
        v->ph[2] = rndf();
        v->drift = 0.0f;
        v->pitch = (float)note;
        mc_start(&v->air, rndf());
        mc_start(&v->sub, rndf());
        v->fl.ic1 = v->fl.ic2 = v->fr.ic1 = v->fr.ic2 = 0.0f;
    }
    v->note = note;
    v->vel = (float)vel * (1.0f / 127.0f);
    v->age = 0;
    {
        static const float PAN[ATMOS_VOICES] = {-0.8f, 0.8f, -0.35f, 0.35f};
        v->pan = PAN[best];
    }
    v->eg.st = EG_ATK;
}

static void atmos_off(int32_t note)
{
    uint32_t i;
    for (i = 0; i < ATMOS_VOICES; i++)
        if (atm.v[i].note == note && atm.v[i].eg.st != EG_IDLE && atm.v[i].eg.st != EG_REL)
            atm.v[i].eg.st = EG_REL;
}

static void atmos_run(float *l, float *r, uint32_t n)
{
    uint32_t i, k, o, nosc = dsp_quality >= 2u ? 3u : 2u;
    float shape = atm.shape, air = atm.air * 0.35f, sub = atm.sub * 0.45f;
    /* the morph is between two waveforms at a time: only those two are computed */
    int upper = shape >= 0.5f;
    float wb = upper ? shape * 2.0f - 1.0f : shape * 2.0f, wa = 1.0f - wb;
    for (i = 0; i < ATMOS_VOICES; i++) {
        pad_voice_t *v = &atm.v[i];
        float hz, inc[3], ea, eb, gl, gr, g, e0, e1, de;
        if (v->eg.st == EG_IDLE)
            continue;
        hz = note_hz(v->pitch + v->drift);
        for (k = 0; k < 3u; k++)
            inc[k] = hz * atm.det[k] * INV_FS;
        ea = mc_e(hz * 2.0f * 1.0015f);
        eb = mc_e(hz * 0.5f);
        mc_norm(&v->air, ea);
        mc_norm(&v->sub, eb);
        pan_gains(v->pan * atm.width, &gl, &gr);
        g = (0.20f + 0.10f * v->vel) * (nosc == 3u ? 0.80f : 1.0f);
        e0 = v->eg.v;
        e1 = eg_run(&v->eg, &atm.eg32);
        de = (e1 - e0) / (float)n;
        for (k = 0; k < n; k++) {
            float s = 0.0f, y;
            for (o = 0; o < nosc; o++) {
                float ph = v->ph[o] + inc[o], tri, b;
                if (ph >= 1.0f)
                    ph -= 1.0f;
                v->ph[o] = ph;
                tri = 1.0f - 4.0f * fm_fabsf(ph - 0.5f);
                if (upper) {
                    b = 2.0f * ph - 1.0f - blep(ph, inc[o]);
                } else {
                    float x = ph * (float)SIN_N;
                    uint32_t j = (uint32_t)x;
                    b = tri;
                    tri = sin_tab[j] + (sin_tab[j + 1u] - sin_tab[j]) * (x - (float)j);
                }
                s += tri * wa + b * wb;
            }
            s += air * mc_run(&v->air, ea) + sub * mc_run(&v->sub, eb);
            /* one filter per voice, then the pan */
            y = svf_lp(&v->fl, &atm.cut[i], s * (e0 + de * (float)k) * g);
            l[k] += y * gl;
            r[k] += y * gr;
        }
    }
}

/* ================================================================ DRONE === */
#define DR_PARTS 8
#define DR_GRAINS 4
#define DR_TWINS 5                     /* the first five partials have a slightly-off twin (the slow beating) */
static const float DR_HARM[DR_PARTS] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f, 10.0f};
typedef struct { mc_t o; float e, env_ph, env_inc, amp, pan; uint8_t on; } grain_t;
static struct {
    float root, root_to, glide;            /* the root note (MIDI, fractional while it glides) */
    mc_t osc[DR_PARTS], twin[DR_TWINS], sub, fifth, fifth_lo;
    float amp[DR_PARTS], amp_to[DR_PARTS];
    float ph_grit[2];
    float level, level_a, level_r;         /* the fade in / out */
    float wob, wob_to;
    svf_t fl, fr;
    svfc_t cut;
    grain_t gr[DR_GRAINS];
    float grain_acc, hp_l, hp_r;
    uint32_t clock;
} dr;

static void drone_cook(void)
{
    uint32_t i;
    float motion = u7(D_MOTION) * fm_exp2f(mac_motion * 1.5f), tilt = u7(D_TONE);
    float glide_s = v_time(u7(D_GLIDE)) * 0.5f;
    dr.glide = tc(glide_s) * (float)HALO_CHUNK;
    dr.root += (dr.root_to - dr.root) * fm_minf(dr.glide, 1.0f);
    dr.level_a = tc(v_time(u7(D_ATK)) * 0.35f);
    dr.level_r = tc(v_time(u7(D_REL)) * 0.35f);
    dsp_drone_hz = note_hz(dr.root);
    /* each partial wanders toward a random level: a new aim now and then (MOTION), brighter partials quieter (TONE) */
    for (i = 0; i < DR_PARTS; i++) {
        float t = fm_powf(DR_HARM[i], -1.6f + tilt * 1.3f);
        if (rndf() < motion * 0.010f + 0.0005f)
            dr.amp_to[i] = t * (0.25f + 0.75f * rndf());
        dr.amp[i] += (dr.amp_to[i] - dr.amp[i]) * (0.004f + motion * 0.03f);
    }
    /* the filter: CUTOFF, darker with DEPTH, wandering (WOBBLE) */
    if (rndf() < 0.01f + motion * 0.03f)
        dr.wob_to = rnds() * u7(D_WOBBLE) * 0.30f;
    dr.wob += (dr.wob_to - dr.wob) * (0.002f + motion * 0.01f);
    svf_set(&dr.cut, v_cut(u7(D_CUT) - 0.25f * fm_maxf(mac_depth, 0.0f) + dr.wob), 0.25f + u7(D_WOBBLE) * 0.3f);
    dr.clock++;
}

static void drone_set_root(int32_t note, int jump)
{
    dr.root_to = (float)note;
    if (jump)
        dr.root = (float)note;
}

static void drone_grain_spawn(void)
{
    uint32_t i;
    for (i = 0; i < DR_GRAINS; i++)
        if (!dr.gr[i].on) {
            grain_t *g = &dr.gr[i];
            static const float OCT[4] = {1.0f, 2.0f, 2.0f, 4.0f};
            float dur = 0.06f + 0.40f * rndf();
            g->on = 1;
            g->e = mc_e(note_hz(dr.root) * DR_HARM[rnd() % 6u] * OCT[rnd() & 3u]);
            mc_start(&g->o, rndf());
            g->env_ph = 0.0f;
            g->env_inc = INV_FS / dur;
            g->amp = 0.05f + 0.10f * rndf();
            g->pan = rnds();
            return;
        }
}

static void drone_run(float *l, float *r, uint32_t n)
{
    uint32_t i, k, nparts, ntw;
    float dens = u7(D_DENS) * (1.0f + 0.5f * fm_maxf(mac_depth, 0.0f)), hz = note_hz(dr.root);
    float beat = u7(D_BEAT) * 1.2f + 0.02f, sub = u7(D_SUB) * (0.6f + 0.6f * fm_maxf(mac_depth, -0.6f));
    float fifth = u7(D_FIFTH) * 0.07f, grit = u7(D_GRIT), grain = u7(D_GRAIN), gate = dsp_drone_gate ? 1.0f : 0.0f;
    float e[DR_PARTS], et[DR_TWINS], wl[DR_PARTS], wr[DR_PARTS];
    /* the sub octave and the low fifth only where they are audible (a low root's would be below 30 Hz) */
    float es = mc_e(hz * 0.5f >= 30.0f ? hz * 0.5f : hz), e5 = mc_e(hz * 1.5f),
          e5lo = mc_e(hz * 0.75f >= 30.0f ? hz * 0.75f : hz * 1.5f);
    float inc_g0 = hz * INV_FS, inc_g1 = hz * 1.0041f * INV_FS;
    float fpart = fm_clampf(dens, 0.0f, 1.0f) * (float)(DR_PARTS - 1) + 1.0f;
    float sub_g = sub * 0.30f, grit_g = grit * 0.06f, lv, dlv;
    if (gate == 0.0f && dr.level < 0.00005f) {
        dr.level = 0.0f;
        return;
    }
    nparts = (uint32_t)fpart + 1u;
    if (nparts > DR_PARTS)
        nparts = DR_PARTS;
    if (dsp_quality < 2u && nparts > 5u)
        nparts = 5u;
    ntw = nparts < DR_TWINS ? nparts : DR_TWINS;
    if (dsp_quality < 1u)
        ntw = 2u;
    for (i = 0; i < nparts; i++) {
        float f = hz * DR_HARM[i], fade = fm_clampf(fpart - (float)i, 0.0f, 1.0f), pl, pr, a;
        if (f > 16000.0f)
            fade = 0.0f;
        e[i] = mc_e(f);
        mc_norm(&dr.osc[i], e[i]);
        if (i < ntw) {
            et[i] = mc_e(f + beat * (0.3f + 0.2f * (float)i));   /* the twin a little off: slow beating */
            mc_norm(&dr.twin[i], et[i]);
        }
        a = fade * 0.22f * dr.amp[i] * (i < ntw ? 1.0f : 1.6f);
        pan_gains(((i & 1u) ? 0.55f : -0.55f) * (i ? 1.0f : 0.2f), &pl, &pr);
        wl[i] = a * pl;
        wr[i] = a * pr;
    }
    mc_norm(&dr.sub, es);
    mc_norm(&dr.fifth, e5);
    mc_norm(&dr.fifth_lo, e5lo);
    if (dsp_quality >= 1u && grain > 0.01f) {
        dr.grain_acc += grain * grain * 14.0f * (float)n * INV_FS;   /* up to ~14 grains a second */
        while (dr.grain_acc >= 1.0f) {
            dr.grain_acc -= 1.0f;
            drone_grain_spawn();
        }
    }
    for (i = 0; i < DR_GRAINS; i++)
        if (dr.gr[i].on)
            mc_norm(&dr.gr[i].o, dr.gr[i].e);
    /* the fade, one step a chunk, interpolated */
    lv = dr.level;
    dr.level += (gate - dr.level) * (1.0f - fm_powf(1.0f - (gate > dr.level ? dr.level_a : dr.level_r), (float)n));
    dlv = (dr.level - lv) / (float)n;
    for (k = 0; k < n; k++) {
        float sl = 0.0f, sr = 0.0f, m;
        for (i = 0; i < nparts; i++) {
            float v = mc_run(&dr.osc[i], e[i]);
            if (i < ntw)
                v += mc_run(&dr.twin[i], et[i]);
            sl += v * wl[i];
            sr += v * wr[i];
        }
        m = sub_g * mc_run(&dr.sub, es) + fifth * (mc_run(&dr.fifth, e5) + mc_run(&dr.fifth_lo, e5lo));
        if (grit > 0.01f) {
            dr.ph_grit[0] += inc_g0;
            if (dr.ph_grit[0] >= 1.0f)
                dr.ph_grit[0] -= 1.0f;
            dr.ph_grit[1] += inc_g1;
            if (dr.ph_grit[1] >= 1.0f)
                dr.ph_grit[1] -= 1.0f;
            m += grit_g * (2.0f * dr.ph_grit[0] - 1.0f - blep(dr.ph_grit[0], inc_g0)
                           + 2.0f * dr.ph_grit[1] - 1.0f - blep(dr.ph_grit[1], inc_g1));
        }
        for (i = 0; i < DR_GRAINS; i++) {     /* grains: short sine bursts on the drone's harmonics */
            grain_t *g = &dr.gr[i];
            float w, v;
            if (!g->on)
                continue;
            g->env_ph += g->env_inc;
            if (g->env_ph >= 1.0f) {
                g->on = 0;
                continue;
            }
            w = 4.0f * g->env_ph * (1.0f - g->env_ph);   /* a parabola: smooth at both ends */
            v = mc_run(&g->o, g->e) * w * w * g->amp * grain;
            sl += v * (1.0f - g->pan) * 0.5f;
            sr += v * (1.0f + g->pan) * 0.5f;
        }
        lv += dlv;
        sl = svf_lp(&dr.fl, &dr.cut, (sl + m) * lv);
        sr = svf_lp(&dr.fr, &dr.cut, (sr + m) * lv);
        l[k] += sl * 2.6f;
        r[k] += sr * 2.6f;
    }
}

/* ============================================================== SHIMMER === */
/* An efficient stand-in for pitch-shifted feedback reverb: the notes' upper harmonics (octave, octave + fifth,
 * two octaves) synthesised directly, each twinkling on its own slow random envelope, smeared by two allpass
 * diffusers and sent hard into the reverb, where the rectifier in the loop adds further octaves. */
#define SH_PARTS 3
typedef struct {
    int32_t note;
    float pitch, tw[SH_PARTS], tw_to[SH_PARTS];
    mc_t o[SH_PARTS];
    eg_t eg;
} sh_slot_t;
static struct {
    sh_slot_t s[SHIM_SLOTS];
    egc_t eg, eg32;
    float mul[SH_PARTS], wgt[SH_PARTS];
    float ap1[347], ap2[521];
    uint32_t a1, a2;
} sh;

static void shim_cook(void)
{
    uint32_t i, j;
    float br = u7(S_BRIGHT) + 0.3f * fm_maxf(mac_shim, 0.0f), spark = u7(S_SPARK) * fm_exp2f(mac_motion);
    float oct = P[S_OCT] ? 2.0f : 1.0f;
    sh.mul[0] = 4.0f * oct;            /* two octaves up, two octaves and a fifth, three octaves */
    sh.mul[1] = 6.0f * oct;
    sh.mul[2] = 8.0f * oct;
    sh.wgt[0] = 1.0f;
    sh.wgt[1] = 0.25f + 0.6f * br;
    sh.wgt[2] = br * br * 0.8f;
    sh.eg.a = tc(v_time(u7(S_ATK)) * 0.35f);
    sh.eg.d = sh.eg.a;
    sh.eg.s = 1.0f;
    sh.eg.r = tc(v_time(u7(S_REL)) * 0.3f);
    eg_chunkify(&sh.eg32, &sh.eg);
    for (i = 0; i < SHIM_SLOTS; i++) {
        sh.s[i].pitch += ((float)sh.s[i].note - sh.s[i].pitch) * 0.03f;
        for (j = 0; j < SH_PARTS; j++) {
            if (rndf() < 0.004f + spark * 0.05f)
                sh.s[i].tw_to[j] = rndf() * rndf();
            sh.s[i].tw[j] += (sh.s[i].tw_to[j] - sh.s[i].tw[j]) * (0.01f + spark * 0.05f);
        }
    }
}

static void shim_on(int32_t note)
{
    uint32_t i, pick = 0;
    for (i = 0; i < SHIM_SLOTS; i++)
        if (sh.s[i].note == note && sh.s[i].eg.st != EG_IDLE) {
            sh.s[i].eg.st = EG_ATK;
            return;
        }
    for (i = 0; i < SHIM_SLOTS; i++) {
        if (sh.s[i].eg.st == EG_IDLE) {
            pick = i;
            break;
        }
        if (sh.s[i].eg.st == EG_REL && sh.s[i].eg.v < sh.s[pick].eg.v)
            pick = i;
    }
    sh.s[pick].note = note;
    if (sh.s[pick].eg.st == EG_IDLE)
        sh.s[pick].pitch = (float)note;
    if (sh.s[pick].eg.st == EG_IDLE)
        for (i = 0; i < SH_PARTS; i++)
            mc_start(&sh.s[pick].o[i], rndf());
    sh.s[pick].eg.st = EG_ATK;
}

static void shim_off(int32_t note)
{
    uint32_t i;
    for (i = 0; i < SHIM_SLOTS; i++)
        if (sh.s[i].note == note && sh.s[i].eg.st != EG_IDLE)
            sh.s[i].eg.st = EG_REL;
}

static void shim_run(float *l, float *r, uint32_t n)
{
    uint32_t i, j, k, na = 0;
    float g = u7(S_INT) * (0.5f + 0.5f * (mac_shim + 1.0f)) * 0.20f, diff = u7(S_DIFF) * 0.7f;
    /* the partials in two lists, left from the front, right from the back: no branch per sample */
    float e[SHIM_SLOTS * SH_PARTS], amp[SHIM_SLOTS * SH_PARTS], de[SHIM_SLOTS * SH_PARTS];
    mc_t *o[SHIM_SLOTS * SH_PARTS];
    uint32_t nl = 0, nr = SHIM_SLOTS * SH_PARTS, q;
    /* the sounding partials gathered into one flat list; each one's level ramps across the chunk */
    for (i = 0; i < SHIM_SLOTS; i++) {
        sh_slot_t *sl = &sh.s[i];
        float hz, e0, e1;
        if (sl->eg.st == EG_IDLE)
            continue;
        hz = note_hz(sl->pitch);
        e0 = sl->eg.v;
        e1 = eg_run(&sl->eg, &sh.eg32);
        for (j = 0; j < SH_PARTS; j++) {
            float f = hz * sh.mul[j], a = f < 14000.0f ? sh.wgt[j] * (0.15f + 0.85f * sl->tw[j]) * g * (f > 9000.0f ? 0.5f : 1.0f)
                                                    : 0.0f;
            if (a <= 0.0f)
                continue;
            q = ((i + j) & 1u) ? --nr : nl++;
            e[q] = mc_e(f);
            mc_norm(&sl->o[j], e[q]);
            o[q] = &sl->o[j];
            amp[q] = a * e0;
            de[q] = a * (e1 - e0) / (float)n;
            na++;
        }
    }
    for (k = 0; k < n; k++) {
        float sl = 0.0f, sr = 0.0f, x, y;
        for (j = 0; j < nl; j++) {
            sl += mc_run(o[j], e[j]) * amp[j];
            amp[j] += de[j];
        }
        for (j = nr; j < SHIM_SLOTS * SH_PARTS; j++) {
            sr += mc_run(o[j], e[j]) * amp[j];
            amp[j] += de[j];
        }
        /* diffusion: two allpasses across the channels, so each side gets the other's smear */
        x = sl + sr * 0.3f;
        y = sh.ap1[sh.a1];
        sh.ap1[sh.a1] = x + y * diff;
        sl = y - x * diff;
        if (++sh.a1 >= 347u)
            sh.a1 = 0;
        x = sr + sl * 0.3f;
        y = sh.ap2[sh.a2];
        sh.ap2[sh.a2] = x + y * diff;
        sr = y - x * diff;
        if (++sh.a2 >= 521u)
            sh.a2 = 0;
        l[k] += sl;
        r[k] += sr;
    }
}

/* ================================================================= ARIA === */
/* Formant choir: per voice two detuned band-limited saws (the ensemble) and breath noise into three parallel
 * band-pass formants, the vowel morphing A -> E -> I -> O -> U. Soprano-region formants (Hz), gains linear. */
static const float VOWEL_F[5][3] = {{800.0f, 1150.0f, 2900.0f}, {350.0f, 2000.0f, 2800.0f}, {270.0f, 2140.0f, 2950.0f},
                                    {450.0f, 800.0f, 2830.0f}, {325.0f, 700.0f, 2700.0f}};
static const float VOWEL_G[5][3] = {{1.0f, 0.50f, 0.18f}, {1.0f, 0.20f, 0.14f}, {1.0f, 0.10f, 0.12f},
                                    {1.0f, 0.35f, 0.08f}, {1.0f, 0.16f, 0.05f}};
typedef struct {
    int32_t note;
    float ph[2], vib_ph, vib_amt, vel;
    eg_t eg;
    uint32_t age;
    float pan;
} aria_voice_t;
static struct {
    aria_voice_t v[ARIA_VOICES];
    egc_t eg, eg32;
    svfc_t fc[3];
    float fg[3], vowel_ph, src_lp_a;
    svf_t fl[3], fr[3];                /* one formant bank a side, shared by the voices (they share the vowel) */
    svf_t tone_l;
    float src_l[HALO_CHUNK], src_r[HALO_CHUNK];
} ar;

static void aria_cook(void)
{
    uint32_t j;
    float vw, f, shift, q;
    uint32_t a;
    float motion = fm_exp2f(mac_motion * 1.4f);
    ar.eg.a = tc(v_time(u7(V_ATK)) * 0.35f);
    ar.eg.d = ar.eg.a;
    ar.eg.s = 1.0f;
    ar.eg.r = tc(v_time(u7(V_REL)) * 0.3f);
    eg_chunkify(&ar.eg32, &ar.eg);
    ar.vowel_ph = wrap1(ar.vowel_ph + v_hz(u7(V_DRATE)) * 0.25f * motion * (float)HALO_CHUNK * INV_FS);
    /* the vowel: the chosen one, swinging toward its neighbours by V.DRFT */
    vw = (float)P[V_VOWEL] + sin_t(ar.vowel_ph) * u7(V_DRIFT) * 2.0f;
    while (vw < 0.0f)
        vw += 5.0f;
    while (vw >= 5.0f)
        vw -= 5.0f;
    a = (uint32_t)vw;
    f = vw - (float)a;
    shift = fm_exp2f((u7(V_FORM) - 0.6f) * 0.9f);           /* FORMNT: lower = darker, a male choir */
    q = 0.55f + u7(V_BRIGHT) * 0.25f;
    for (j = 0; j < 3u; j++) {
        float hz = (VOWEL_F[a][j] + (VOWEL_F[(a + 1u) % 5u][j] - VOWEL_F[a][j]) * f) * shift;
        ar.fg[j] = VOWEL_G[a][j] + (VOWEL_G[(a + 1u) % 5u][j] - VOWEL_G[a][j]) * f;
        svf_set(&ar.fc[j], hz, q + (float)j * 0.08f);
    }
    ar.src_lp_a = 0.15f + u7(V_BRIGHT) * 0.8f;
}

static void aria_on(int32_t note, uint32_t vel)
{
    uint32_t i, best = 0, score_best = 0;
    for (i = 0; i < ARIA_VOICES; i++) {
        uint32_t score = ar.v[i].eg.st == EG_IDLE ? 0xFFFFFFFFu
                         : ar.v[i].note == note ? 0xFFFFFFFEu
                         : ar.v[i].eg.st == EG_REL ? 0x80000000u + ar.v[i].age : ar.v[i].age;
        if (score >= score_best) {
            score_best = score;
            best = i;
        }
    }
    for (i = 0; i < ARIA_VOICES; i++)
        ar.v[i].age++;
    {
        aria_voice_t *v = &ar.v[best];
        if (v->eg.st == EG_IDLE) {
            v->ph[0] = rndf();
            v->ph[1] = rndf();
            v->vib_amt = 0.0f;
        }
        v->note = note;
        v->vel = (float)vel * (1.0f / 127.0f);
        v->age = 0;
        v->pan = (float)((int32_t)best - 1) * 0.6f;
        v->eg.st = EG_ATK;
    }
}

static void aria_off(int32_t note)
{
    uint32_t i;
    for (i = 0; i < ARIA_VOICES; i++)
        if (ar.v[i].note == note && ar.v[i].eg.st != EG_IDLE)
            ar.v[i].eg.st = EG_REL;
}

static void aria_run(float *l, float *r, uint32_t n)
{
    uint32_t i, k, j, any = 0;
    float vib_depth = u7(V_VIB) * 0.012f, vib_inc = (2.0f + u7(V_VRATE) * 5.0f) * INV_FS;
    float ens = u7(V_ENS) * 0.012f, breath = u7(V_BREATH) * 0.5f, oct = P[V_OCT] ? 12.0f : 0.0f;
    float lpa = ar.src_lp_a, keep = 1.0f - breath * 0.6f;
    for (k = 0; k < n; k++)
        ar.src_l[k] = ar.src_r[k] = 0.0f;
    /* the voices' sources (two saws and breath each), panned into two sums */
    for (i = 0; i < ARIA_VOICES; i++) {
        aria_voice_t *v = &ar.v[i];
        float hz, gl, gr, e0, e1, de, f0b, f1b, vib, f0, f1, g;
        if (v->eg.st == EG_IDLE)
            continue;
        any = 1;
        hz = note_hz((float)v->note + oct);
        pan_gains(v->pan, &gl, &gr);
        e0 = v->eg.v;
        e1 = eg_run(&v->eg, &ar.eg32);
        de = (e1 - e0) / (float)n;
        /* vibrato fades in over a second or so, as a singer's does; its rate is slow enough to step per chunk */
        v->vib_amt += (1.0f - v->vib_amt) * 0.001f;
        v->vib_ph = wrap1(v->vib_ph + vib_inc * (float)n);
        vib = 1.0f + sin_t(v->vib_ph) * vib_depth * v->vib_amt;
        f0b = hz * vib * (1.0f - ens) * INV_FS;
        f1b = hz * vib * (1.0f + ens) * INV_FS;
        g = 0.30f * (0.7f + 0.3f * v->vel);
        for (k = 0; k < n; k++) {
            float e = (e0 + de * (float)k) * g, src;
            f0 = f0b;
            f1 = f1b;
            v->ph[0] += f0;
            if (v->ph[0] >= 1.0f)
                v->ph[0] -= 1.0f;
            v->ph[1] += f1;
            if (v->ph[1] >= 1.0f)
                v->ph[1] -= 1.0f;
            src = (2.0f * v->ph[0] - 1.0f - blep(v->ph[0], f0)) + (2.0f * v->ph[1] - 1.0f - blep(v->ph[1], f1));
            src = (src * keep + rnds() * breath * (0.4f + 0.6f * e)) * e;
            ar.src_l[k] += src * gl;
            ar.src_r[k] += src * gr;
        }
    }
    if (!any && fm_fabsf(ar.tone_l.ic1) + fm_fabsf(ar.tone_l.ic2) < 1e-6f)
        return;
    /* the formants, once for all voices; then BRIGHT: a gentle one-pole, so a low setting sounds distant */
    for (k = 0; k < n; k++) {
        float ol = 0.0f, orr = 0.0f;
        for (j = 0; j < 3u; j++) {
            ol += svf_bp(&ar.fl[j], &ar.fc[j], ar.src_l[k]) * ar.fg[j];
            orr += svf_bp(&ar.fr[j], &ar.fc[j], ar.src_r[k]) * ar.fg[j];
        }
        ar.tone_l.ic1 += (ol - ar.tone_l.ic1) * lpa;
        ar.tone_l.ic2 += (orr - ar.tone_l.ic2) * lpa;
        l[k] += ar.tone_l.ic1;
        r[k] += ar.tone_l.ic2;
    }
}

/* ================================================================= ECHO === */
/* Three models (E_MODEL):
 *   FM     two-operator FM: plucks, bells, glass, metal (RATIO 0.5..7, INDEX, I.DECAY)
 *   PIANO  a soft felt piano: two slightly detuned "strings" (FM at ratio 1 for the hammer's brightness), a felt
 *          hammer thump, decay that shortens up the keyboard, brightness that fades faster than the level
 *   LEAD   a monophonic analog lead: saw-to-square oscillators (RATIO picks the blend), a soft drive, a resonant
 *          4-pole low-pass (two state-variable stages), a filter envelope (INDEX, I.DECAY), glide and delayed
 *          vibrato; legato playing glides without a new attack, last note wins */
static const float ECHO_RATIO[8] = {0.5f, 1.0f, 2.0f, 3.0f, 3.5f, 4.0f, 5.19f, 7.0f};
typedef struct {
    int32_t note;
    float pitch, pc, pc2, idx, vel, lp, hammer, bright, fenv, vib_ph, vib_amt;
    mc_t m;
    eg_t eg;
    egc_t pe;                          /* PIANO: this voice's envelope (its decay set by its key) */
    svf_t f1, f2;
    uint32_t age;
    float pan;
} echo_voice_t;
static struct {
    echo_voice_t v[ECHO_VOICES];
    egc_t eg;
    float idec, tone_a, glide;
    int32_t stack[6];                  /* LEAD: the keys held, newest last */
    uint32_t nstack;
    svfc_t lf1, lf2;
} ec;

static void echo_cook(void)
{
    ec.eg.a = tc(v_time(u7(E_ATK)) * 0.3f);
    ec.eg.d = tc(v_time(u7(E_DEC)) * 0.3f);
    ec.eg.s = u7(E_SUS);
    ec.eg.r = tc(v_time(u7(E_REL)) * 0.3f);
    ec.idec = tc(v_time(u7(E_IDEC)) * 0.25f);
    ec.tone_a = 1.0f - fm_expf(-TWO_PI * fm_minf(v_cut(u7(E_TONE)), 18000.0f) * INV_FS);
    ec.glide = fm_minf(tc(v_time(u7(E_GLIDE)) * 0.12f) * (float)HALO_CHUNK, 1.0f);
    if (P[E_MODEL] == EM_LEAD) {       /* the lead's filter: TONE, its envelope, the key; RESO on the second stage */
        echo_voice_t *v = &ec.v[0];
        float c = u7(E_TONE) + u7(E_INDEX) * v->fenv * 0.45f + ((float)v->note - 60.0f) * 0.0025f;
        svf_set(&ec.lf1, v_cut(c), 0.1f);
        svf_set(&ec.lf2, v_cut(c), u7(E_RES) * 0.88f);
        v->fenv -= v->fenv * fm_minf(ec.idec * (float)HALO_CHUNK, 1.0f);
    }
}

static void echo_lead_target(int32_t note)
{
    ec.v[0].note = note;
}

static void echo_on(int32_t note, uint32_t vel)
{
    uint32_t i, best = 0, score_best = 0;
    float vs = u7(E_VEL), v01 = (float)vel * (1.0f / 127.0f);
    if (P[E_MODEL] == EM_LEAD) {
        echo_voice_t *v = &ec.v[0];
        for (i = 0; i < ec.nstack; i++)          /* the stack: this note to the top */
            if (ec.stack[i] == note) {
                for (; i + 1u < ec.nstack; i++)
                    ec.stack[i] = ec.stack[i + 1u];
                ec.nstack--;
                break;
            }
        if (ec.nstack >= 6u) {
            for (i = 0; i + 1u < 6u; i++)
                ec.stack[i] = ec.stack[i + 1u];
            ec.nstack = 5u;
        }
        ec.stack[ec.nstack++] = note;
        v->vel = 1.0f - vs + vs * v01;
        if (v->eg.st != EG_IDLE && v->eg.st != EG_REL) {   /* legato: glide, no new attack */
            echo_lead_target(note);
            return;
        }
        if (v->eg.st == EG_IDLE) {
            v->pitch = (float)note;
            v->f1.ic1 = v->f1.ic2 = v->f2.ic1 = v->f2.ic2 = 0.0f;
        }
        echo_lead_target(note);
        v->fenv = 1.0f;
        v->vib_amt = 0.0f;
        v->eg.st = EG_ATK;               /* from wherever the level is: no click */
        for (i = 1; i < ECHO_VOICES; i++)
            ec.v[i].eg.st = EG_IDLE;
        return;
    }
    for (i = 0; i < ECHO_VOICES; i++) {
        uint32_t score = ec.v[i].eg.st == EG_IDLE ? 0xFFFFFFFFu : 0x80000000u * (ec.v[i].eg.st == EG_REL) + ec.v[i].age;
        if (score >= score_best) {
            score_best = score;
            best = i;
        }
    }
    if (dsp_quality < 2u && best >= 4u)  /* lighter: four voices */
        best = ec.v[0].age > ec.v[1].age ? 0u : 1u;
    for (i = 0; i < ECHO_VOICES; i++)
        ec.v[i].age++;
    {
        echo_voice_t *v = &ec.v[best];
        v->note = note;
        v->pitch = (float)note;
        v->vel = 1.0f - vs + vs * v01;
        v->age = 0;
        v->pc = 0.0f;
        v->pc2 = 0.37f;
        mc_start(&v->m, 0.0f);
        v->pan = rnds() * 0.5f;
        v->eg.v = 0.0f;
        v->eg.st = EG_ATK;
        if (P[E_MODEL] == EM_PIANO) {
            /* the hammer: harder with velocity (INDEX); the decay shorter up the keyboard */
            float key = fm_exp2f(((float)note - 60.0f) * (1.0f / 18.0f));
            v->idx = (0.25f + 0.75f * v01) * (0.3f + u7(E_INDEX) * 1.6f);
            v->hammer = 0.25f + 0.5f * v01;
            v->bright = 1.0f;
            v->pe.a = tc(0.0025f);
            v->pe.d = fm_minf(tc(v_time(u7(E_DEC)) * 0.9f + 0.4f) * key, 0.5f);
            v->pe.s = 0.0f;
            v->pe.r = ec.eg.r;
            v->pan = fm_clampf(((float)note - 64.0f) * 0.025f, -0.6f, 0.6f);   /* low left, high right */
        } else {
            v->idx = u7(E_INDEX) * (1.0f - vs * 0.5f + vs * 0.5f * v01) * 2.2f;
        }
    }
}

static void echo_off(int32_t note)
{
    uint32_t i;
    if (P[E_MODEL] == EM_LEAD) {
        for (i = 0; i < ec.nstack; i++)
            if (ec.stack[i] == note) {
                for (; i + 1u < ec.nstack; i++)
                    ec.stack[i] = ec.stack[i + 1u];
                ec.nstack--;
                break;
            }
        if (ec.v[0].note == note) {
            if (ec.nstack)
                echo_lead_target(ec.stack[ec.nstack - 1u]);   /* back to the key still held */
            else if (ec.v[0].eg.st != EG_IDLE)
                ec.v[0].eg.st = EG_REL;
        }
        return;
    }
    for (i = 0; i < ECHO_VOICES; i++)
        if (ec.v[i].note == note && ec.v[i].eg.st != EG_IDLE && ec.v[i].eg.st != EG_REL)
            ec.v[i].eg.st = EG_REL;
}

static inline __attribute__((always_inline)) float saw_b(float ph, float dt) { return 2.0f * ph - 1.0f - blep(ph, dt); }

static void echo_lead_run(float *l, float *r, uint32_t n)
{
    echo_voice_t *v = &ec.v[0];
    uint32_t k;
    float hz, i1, i2, sq = (float)(P[E_RATIO] & 7u) / 7.0f, g, vib;
    if (v->eg.st == EG_IDLE)
        return;
    v->pitch += ((float)v->note - v->pitch) * ec.glide;
    /* vibrato: it arrives after the note has sung a moment */
    v->vib_amt += (1.0f - v->vib_amt) * 0.012f;
    v->vib_ph = wrap1(v->vib_ph + 5.2f * (float)n * INV_FS);
    vib = sin_t(v->vib_ph) * u7(E_VIB) * 0.35f * v->vib_amt * v->vib_amt;
    hz = note_hz(v->pitch + vib);
    i1 = hz * INV_FS;
    i2 = hz * 1.0035f * INV_FS;
    g = 0.30f * v->vel;
    for (k = 0; k < n; k++) {
        float e = eg_run(&v->eg, &ec.eg), p2, s, y;
        v->pc += i1;
        if (v->pc >= 1.0f)
            v->pc -= 1.0f;
        v->pc2 += i2;
        if (v->pc2 >= 1.0f)
            v->pc2 -= 1.0f;
        p2 = v->pc + 0.5f;
        if (p2 >= 1.0f)
            p2 -= 1.0f;
        /* oscillator 1 blends saw to square (a saw minus a half-turned saw); oscillator 2 is a saw, a touch sharp */
        s = saw_b(v->pc, i1) - sq * saw_b(p2, i1) + 0.6f * saw_b(v->pc2, i2);
        s = s * 0.6f;
        s = s / (1.0f + fm_fabsf(s) * 0.5f);   /* drive into the filter: the warmth */
        y = svf_lp(&v->f2, &ec.lf2, svf_lp(&v->f1, &ec.lf1, s));
        y *= e * g;
        l[k] += y;
        r[k] += y;
    }
}

static void echo_run(float *l, float *r, uint32_t n)
{
    uint32_t i, k, model = P[E_MODEL];
    float ratio = ECHO_RATIO[P[E_RATIO] & 7u], a = ec.tone_a;
    if (model == EM_LEAD) {
        echo_lead_run(l, r, n);
        return;
    }
    for (i = 0; i < ECHO_VOICES; i++) {
        echo_voice_t *v = &ec.v[i];
        float hz, ic, em, gl, gr, g;
        if (v->eg.st == EG_IDLE)
            continue;
        hz = note_hz((float)v->note);
        ic = hz * INV_FS;
        pan_gains(v->pan, &gl, &gr);
        if (model == EM_PIANO) {
            float ic2 = ic * 1.0011f, ab, br0 = v->bright, br1, dbr;
            em = mc_e(hz);
            mc_norm(&v->m, em);
            g = 0.62f * v->vel;
            v->bright *= 1.0f - fm_minf(ec.idec * 0.5f * (float)n, 0.9f);   /* the tone dulls faster than it fades */
            br1 = v->bright;
            dbr = (br1 - br0) / (float)n;
            for (k = 0; k < n; k++) {
                float e = eg_run(&v->eg, &v->pe), br = br0 + dbr * (float)k, mod, s, hmr;
                v->pc += ic;
                if (v->pc >= 1.0f)
                    v->pc -= 1.0f;
                v->pc2 += ic2;
                if (v->pc2 >= 1.0f)
                    v->pc2 -= 1.0f;
                mod = mc_run(&v->m, em) * v->idx * br;
                s = (sin_t(v->pc + mod * 0.22f) + sin_t(v->pc2 + mod * 0.18f)) * 0.5f;
                hmr = 0.0f;
                if (v->hammer > 0.001f) {   /* the felt's thump: a few milliseconds of soft noise */
                    v->hammer *= 0.9955f;
                    hmr = rnds() * v->hammer * 0.35f;
                }
                s = s * e * (0.55f + 0.45f * br) + hmr * (0.5f + 0.5f * e);
                ab = a * (0.18f + 0.82f * br);
                v->lp += (s - v->lp) * ab;
                l[k] += v->lp * g * gl;
                r[k] += v->lp * g * gr;
            }
            continue;
        }
        em = mc_e(hz * ratio);
        mc_norm(&v->m, em);
        g = 0.40f * v->vel;
        for (k = 0; k < n; k++) {
            float e = eg_run(&v->eg, &ec.eg), s;
            v->pc += ic;
            if (v->pc >= 1.0f)
                v->pc -= 1.0f;
            v->idx -= v->idx * ec.idec;
            s = sin_t(v->pc + v->idx * mc_run(&v->m, em) * 0.25f) * e * g;
            v->lp += (s - v->lp) * a;
            l[k] += v->lp * gl;
            r[k] += v->lp * gr;
        }
    }
}

/* ================================================================= BEAT === */
/* The soft beat INFINITY can add (G_BEAT): a round kick, a tick, a shaker. Tiny on purpose: it marks time under the
 * music rather than driving it. B.TONE darker or brighter, shorter or longer. */
static struct {
    float k_ph, k_hz, k_env, t_env, s_env, s_att, t_lp, s_hp, s_lp, t_hp;
} bt;

void dsp_perc(uint32_t type, uint32_t vel)
{
    float v = (float)vel * (1.0f / 127.0f);
    dsp_beats++;
    switch (type) {
    case PERC_KICK:
        bt.k_hz = 110.0f + 40.0f * u7(G_BTONE);
        bt.k_env = 0.9f * v;
        break;
    case PERC_TICK:
        bt.t_env = 0.30f * v;
        break;
    default:
        bt.s_att = 0.22f * v;
        break;
    }
}

static void beat_run(float *l, float *r, float *rl, float *rr, uint32_t n)
{
    uint32_t k;
    float lvl = u7(G_BEAT), tone = u7(G_BTONE);
    float kdec = 1.0f - INV_FS / (0.10f + 0.25f * (1.0f - tone)), tdec = 1.0f - INV_FS / (0.012f + 0.02f * tone);
    float sdec = 1.0f - INV_FS / (0.03f + 0.06f * tone), tcoef = 0.25f + 0.6f * tone;
    if (lvl < 0.01f || (bt.k_env < 1e-4f && bt.t_env < 1e-4f && bt.s_env < 1e-4f && bt.s_att < 1e-4f))
        return;
    lvl *= lvl * 1.2f;
    for (k = 0; k < n; k++) {
        float kick = 0.0f, tick = 0.0f, shk = 0.0f, nz;
        if (bt.k_env > 1e-4f) {        /* the kick: a sine falling to ~45 Hz */
            bt.k_hz += (45.0f - bt.k_hz) * 0.0012f;
            bt.k_ph = wrap1(bt.k_ph + bt.k_hz * INV_FS);
            kick = sin_t(bt.k_ph) * bt.k_env;
            bt.k_env *= kdec;
        }
        nz = rnds();
        if (bt.t_env > 1e-4f) {        /* the tick: bright noise, a few milliseconds */
            bt.t_hp += (nz - bt.t_hp) * 0.5f;
            bt.t_lp += ((nz - bt.t_hp) - bt.t_lp) * tcoef;
            tick = bt.t_lp * bt.t_env;
            bt.t_env *= tdec;
        }
        if (bt.s_att > 1e-4f || bt.s_env > 1e-4f) {   /* the shaker: a softer swell of noise */
            bt.s_env += bt.s_att * 0.02f;
            bt.s_att *= 0.98f;
            bt.s_hp += (nz - bt.s_hp) * 0.3f;
            shk = (nz - bt.s_hp) * bt.s_env;
            bt.s_env *= sdec;
        }
        kick *= lvl;
        tick *= lvl;
        shk *= lvl;
        l[k] += kick + tick * 0.6f + shk * 0.9f;
        r[k] += kick + tick * 0.9f + shk * 0.6f;
        rl[k] += (tick + shk) * 0.3f;
        rr[k] += (tick + shk) * 0.3f;
    }
}

/* =============================================================== REVERB === */
/* A four-line feedback delay network with a Hadamard mix, four input diffusers, modulated read taps, damping in
 * each line and an optional rectifier (octave-up) in the loop for shimmer. Lines up to 4608 samples (104 ms). */
#define RV_LINES 4
#define RV_MAX 4608u
static float rv_buf[RV_LINES][RV_MAX] PLAT_POOL;
static float rv_ap[4][384] PLAT_POOL;
static const float RV_BASE[RV_LINES] = {1931.0f, 2213.0f, 2557.0f, 2969.0f};
static const uint16_t RV_AP_LEN[4] = {142, 107, 379, 277};
static struct {
    uint32_t w, apw[4];
    float len[RV_LINES], len_to[RV_LINES], g[RV_LINES], damp, lp[RV_LINES], mod_ph[RV_LINES], mod_depth;
    float shim, shim_hp, shim_lp, hp_l, hp_r, hpa, level;
} rv;

static void reverb_cook(void)
{
    uint32_t i;
    float size, decay, damp, t60;
    uint32_t type = P[R_TYPE];
    float sp = mac_space;
    size = fm_clampf(u7(R_SIZE) + sp * 0.35f, 0.0f, 1.0f);
    decay = fm_clampf(u7(R_DECAY) + sp * 0.35f, 0.0f, 1.0f);
    damp = u7(R_DAMP);
    switch (type) {
    case RT_CATHEDRAL:
        size = 0.55f + size * 0.45f;
        decay = 0.4f + decay * 0.6f;
        break;
    case RT_DARK:
        damp = 0.55f + damp * 0.45f;
        break;
    case RT_INFINITE:
        decay = 1.0f;
        break;
    case RT_SHIMMER:
        damp = damp * 0.6f;
        break;
    default:
        break;
    }
    damp = fm_clampf(damp + 0.25f * fm_maxf(mac_depth, 0.0f), 0.0f, 0.97f);
    t60 = 0.6f * fm_exp2f(decay * 5.6f);                      /* 0.6 .. ~29 s */
    for (i = 0; i < RV_LINES; i++) {
        float L;
        rv.len_to[i] = RV_BASE[i] * (0.45f + size * 1.05f);
        rv.len[i] += (rv.len_to[i] - rv.len[i]) * 0.02f;      /* a slow glide: a size change sweeps, never clicks */
        L = rv.len[i];
        rv.g[i] = type == RT_INFINITE && P[R_DECAY] > 100u ? 0.9985f
                  : fm_exp2f(-3.0f * 3.3219281f * L / (t60 * FS));   /* 10^(-3L/(t60 fs)) */
    }
    rv.damp = 0.08f + (1.0f - damp) * 0.88f;                 /* the loop's one-pole: 1 = bright */
    rv.mod_depth = 2.0f + u7(R_MOD) * 14.0f * fm_exp2f(mac_motion * 0.8f);
    rv.shim = fm_clampf((u7(R_SHIM) + (type == RT_SHIMMER ? 0.35f : 0.0f)) * (0.6f + 0.6f * (mac_shim + 1.0f) * 0.5f
                        + u7(S_FDBK) * 0.4f), 0.0f, 0.95f) * 0.28f;
    rv.hpa = 1.0f - fm_expf(-TWO_PI * (20.0f + u7(R_LOCUT) * 400.0f) * INV_FS);
    rv.level = u7(R_LEVEL) * 1.1f;
}

static inline __attribute__((always_inline)) float rv_tap(uint32_t line, float delay)
{
    float pos = (float)rv.w - delay;
    uint32_t i0;
    float f;
    if (pos < 0.0f)
        pos += (float)RV_MAX;
    i0 = (uint32_t)pos;
    f = pos - (float)i0;
    {
        uint32_t i1 = i0 + 1u >= RV_MAX ? 0u : i0 + 1u;
        return rv_buf[line][i0] + (rv_buf[line][i1] - rv_buf[line][i0]) * f;
    }
}

static void reverb_run(const float *il, const float *ir, float *l, float *r, uint32_t n)
{
    uint32_t k, i;
    float mi[RV_LINES];
    int32_t rvl[RV_LINES];
    for (i = 0; i < RV_LINES; i++)
        rvl[i] = (int32_t)(rv.len[i] + 0.5f);
    for (i = 0; i < RV_LINES; i++) {
        rv.mod_ph[i] = wrap1(rv.mod_ph[i] + (0.07f + 0.031f * (float)i) * (float)n * INV_FS);
        mi[i] = sin_t(rv.mod_ph[i]) * rv.mod_depth;
    }
    for (k = 0; k < n; k++) {
        float x, d[RV_LINES], h0, h1, h2, h3, sm;
        /* low cut on the way in: a muddy bottom never builds up in a long tail */
        rv.hp_l += (il[k] - rv.hp_l) * rv.hpa;
        rv.hp_r += (ir[k] - rv.hp_r) * rv.hpa;
        x = (il[k] - rv.hp_l + ir[k] - rv.hp_r) * 0.5f;
        for (i = 0; i < 4u; i++) {          /* input diffusion */
            uint32_t L = RV_AP_LEN[i], p = rv.apw[i];
            float y = rv_ap[i][p];
            rv_ap[i][p] = x + y * 0.62f;
            x = y - x * 0.62f;
            rv.apw[i] = p + 1u >= L ? 0u : p + 1u;
        }
        for (i = 0; i < RV_LINES; i++) {
            float v;
            if (i < 2u)                  /* two lines modulated (the chorus in the tail), two read whole */
                v = rv_tap(i, rv.len[i] + mi[i] * (i ? -1.0f : 1.0f));
            else {
                int32_t p = (int32_t)rv.w - rvl[i];
                v = rv_buf[i][p < 0 ? p + (int32_t)RV_MAX : p];
            }
            rv.lp[i] += (v - rv.lp[i]) * rv.damp;
            d[i] = rv.lp[i] * rv.g[i];
        }
        /* the shimmer: line 0, band-limited, rectified (an octave up), fed back in */
        rv.shim_hp += (d[0] - rv.shim_hp) * 0.02f;
        sm = fm_fabsf(d[0] - rv.shim_hp);
        rv.shim_lp += (sm - rv.shim_lp) * 0.25f;
        sm = (sm - rv.shim_lp * 0.98f) * rv.shim;
        h0 = d[0] + d[1];
        h1 = d[0] - d[1];
        h2 = d[2] + d[3];
        h3 = d[2] - d[3];
        /* the loop's gain is below 1 by construction (Hadamard / 2, line gains < 1); only the shimmer can add,
         * so the two lines it feeds are held within bounds */
        rv_buf[0][rv.w] = fm_clampf((h0 + h2) * 0.5f + x + sm, -4.0f, 4.0f);
        rv_buf[1][rv.w] = (h1 + h3) * 0.5f + x * 0.85f;
        rv_buf[2][rv.w] = fm_clampf((h0 - h2) * 0.5f + x * 0.75f + sm, -4.0f, 4.0f);
        rv_buf[3][rv.w] = (h1 - h3) * 0.5f + x * 0.9f;
        if (++rv.w >= RV_MAX)
            rv.w = 0;
        l[k] += (d[0] + d[2] * 0.7f) * rv.level * 0.55f;
        r[k] += (d[1] + d[3] * 0.7f) * rv.level * 0.55f;
    }
}

/* ================================================================ DELAY === */
/* Stored at half rate (22.05 kHz) in 16 bits: 32768 samples are 1.49 s a side in 128 KB. The darker, slightly
 * soft repeats are the tape character, not a compromise one hears. */
#define DL_N 32768u
static int16_t dl_buf[2][DL_N] PLAT_POOL;
static const float DL_DIV[8] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};   /* beats */
static struct {
    uint32_t w;
    uint8_t half;
    float in_l, in_r, time, time_to, fb, tone, lp_l, lp_r, hp_l, hp_r, wow_ph, flut_ph, wow, level, out_l, out_r,
        prev_l, prev_r;
} dl;

static void delay_cook(void)
{
    float beats = DL_DIV[P[L_TIME] & 7u], sec = beats * 60.0f / fm_maxf(dsp_tempo_bpm, 20.0f);
    while (sec > 1.45f)
        sec *= 0.5f;
    dl.time_to = sec * FS * 0.5f;                            /* in half-rate samples */
    dl.fb = fm_clampf(u7(L_FDBK) * 0.97f + mac_space * 0.08f, 0.0f, 0.97f);
    dl.tone = 0.05f + u7(L_TONE) * 0.7f;
    dl.wow = u7(L_WOW) * 18.0f;
    dl.level = u7(L_LEVEL);
}

static inline float dl_read(uint32_t ch, float pos)
{
    uint32_t i0, i1;
    float f;
    while (pos < 0.0f)
        pos += (float)DL_N;
    i0 = (uint32_t)pos & (DL_N - 1u);
    i1 = (i0 + 1u) & (DL_N - 1u);
    f = pos - (float)(uint32_t)pos;
    return ((float)dl_buf[ch][i0] + (float)(dl_buf[ch][i1] - dl_buf[ch][i0]) * f) * (1.0f / 16384.0f);
}

static void delay_run(const float *il, const float *ir, float *l, float *r, float *to_rev_l, float *to_rev_r,
                      uint32_t n)
{
    uint32_t k;
    float trv = u7(L_TOREV), ping = P[L_MODE] == DM_PINGPONG ? 1.0f : 0.0f;
    for (k = 0; k < n; k++) {
        dl.in_l += il[k];
        dl.in_r += ir[k];
        if (dl.half) {                       /* every other sample: read, filter, write */
            float wow, rl, rr, wl, wr, pos;
            dl.time += (dl.time_to - dl.time) * 0.0008f;     /* time changes glide like a tape's speed */
            dl.wow_ph = wrap1(dl.wow_ph + 0.45f * 2.0f * INV_FS);
            dl.flut_ph = wrap1(dl.flut_ph + 5.7f * 2.0f * INV_FS);
            wow = sin_t(dl.wow_ph) * dl.wow + sin_t(dl.flut_ph) * dl.wow * 0.08f;
            pos = (float)dl.w - dl.time - wow;
            rl = dl_read(0, pos);
            rr = dl_read(1, pos - (ping > 0.0f ? 0.0f : 7.0f));
            dl.lp_l += (rl - dl.lp_l) * dl.tone;
            dl.lp_r += (rr - dl.lp_r) * dl.tone;
            dl.hp_l += (dl.lp_l - dl.hp_l) * 0.01f;
            dl.hp_r += (dl.lp_r - dl.hp_r) * 0.01f;
            rl = dl.lp_l - dl.hp_l;
            rr = dl.lp_r - dl.hp_r;
            if (ping > 0.0f) {               /* ping-pong: the input into the left, each side feeding the other */
                wl = (dl.in_l + dl.in_r) * 0.5f + rr * dl.fb;
                wr = rl * dl.fb;
            } else {
                wl = dl.in_l * 0.5f + rl * dl.fb;
                wr = dl.in_r * 0.5f + rr * dl.fb;
            }
            wl = wl / (1.0f + fm_fabsf(wl) * 0.3f);          /* tape saturation in the loop */
            wr = wr / (1.0f + fm_fabsf(wr) * 0.3f);
            dl_buf[0][dl.w] = (int16_t)fm_clampf(wl * 16384.0f, -32767.0f, 32767.0f);
            dl_buf[1][dl.w] = (int16_t)fm_clampf(wr * 16384.0f, -32767.0f, 32767.0f);
            dl.w = (dl.w + 1u) & (DL_N - 1u);
            dl.prev_l = dl.out_l;
            dl.prev_r = dl.out_r;
            dl.out_l = rl;
            dl.out_r = rr;
            dl.in_l = dl.in_r = 0.0f;
        }
        dl.half ^= 1u;
        {
            /* half-rate output, linearly interpolated back to 44.1 kHz */
            float ol = dl.half ? (dl.prev_l + dl.out_l) * 0.5f : dl.out_l;
            float orr = dl.half ? (dl.prev_r + dl.out_r) * 0.5f : dl.out_r;
            l[k] += ol * dl.level;
            r[k] += orr * dl.level;
            to_rev_l[k] += ol * trv * 0.5f;
            to_rev_r[k] += orr * trv * 0.5f;
        }
    }
}

/* =============================================================== MASTER === */
#define CH_N 1024u
static float ch_buf[2][CH_N] PLAT_POOL;
static struct {
    uint32_t w;
    float ph, ph2, depth, rate;
    float ap[2][4], ph_ph, ph_fb[2], ph_amt, ph_rate;
    float tr_ph, tr_depth, tr_rate;
    float sat, excite, ex_hp[2], noise, ntone, nmove, n_lp[2], n_ph, master, out_g, dc_x[2], dc_y[2];
    float spk[2][2], spk_mix;          /* the speaker's low cut: two one-pole stages a side, faded in and out */
} ms;

static void master_cook(void)
{
    float motion = fm_exp2f(mac_motion * 1.2f);
    ms.depth = u7(C_DEPTH) * 220.0f;
    ms.rate = v_hz(u7(C_RATE)) * motion;
    ms.ph_amt = u7(H_AMT);
    ms.ph_rate = v_hz(u7(H_RATE)) * motion;
    ms.tr_depth = u7(T_DEPTH);
    ms.tr_rate = v_hz(u7(T_RATE)) * motion;
    ms.sat = 1.0f + u7(X_SAT) * 5.0f;
    ms.excite = u7(X_EXCITE) * (0.5f + 0.5f * (mac_shim + 1.0f) * 0.5f);
    ms.noise = u7(X_NOISE) * u7(X_NOISE) * 0.08f;
    ms.ntone = 0.01f + u7(X_NTONE) * 0.6f;
    ms.nmove = u7(X_NMOVE);
    ms.master = u7(X_MASTER) * 1.15f;
}

static void master_run(float *l, float *r, uint32_t n)
{
    uint32_t k, j, c;
    float sat_norm = 1.0f / fm_tanhf(ms.sat * 0.5f), drive = ms.master * ms.sat * 0.5f, fn = (float)n;
    /* the slow modulators move once a chunk; their values ramp across it */
    float cd0[2], cd1[2], pa0 = 0.0f, pa1 = 0.0f, tm0 = 1.0f, tm1 = 1.0f, ng0 = 0.0f, ng1 = 0.0f;
    int chorus = ms.depth > 1.0f, phaser = ms.ph_amt > 0.01f, trem = ms.tr_depth > 0.01f, noise = ms.noise > 0.0f;
    int excite = ms.excite > 0.01f;
    for (c = 0; c < 2u; c++)
        cd0[c] = 300.0f + ms.depth * (1.0f + sin_t(ms.ph + (float)c * 0.25f));
    ms.ph = wrap1(ms.ph + ms.rate * fn * INV_FS);
    for (c = 0; c < 2u; c++)
        cd1[c] = 300.0f + ms.depth * (1.0f + sin_t(ms.ph + (float)c * 0.25f));
    if (phaser) {
        pa0 = 0.35f + 0.6f * (0.5f + 0.5f * sin_t(ms.ph_ph));
        ms.ph_ph = wrap1(ms.ph_ph + ms.ph_rate * fn * INV_FS);
        pa1 = 0.35f + 0.6f * (0.5f + 0.5f * sin_t(ms.ph_ph));
    }
    if (trem) {
        tm0 = 1.0f - ms.tr_depth * (0.5f + 0.5f * sin_t(ms.tr_ph));
        ms.tr_ph = wrap1(ms.tr_ph + ms.tr_rate * fn * INV_FS);
        tm1 = 1.0f - ms.tr_depth * (0.5f + 0.5f * sin_t(ms.tr_ph));
    }
    if (noise) {
        ng0 = ms.noise * (1.0f - ms.nmove * 0.5f * (1.0f + sin_t(ms.n_ph)));
        ms.n_ph = wrap1(ms.n_ph + 0.07f * fn * INV_FS);
        ng1 = ms.noise * (1.0f - ms.nmove * 0.5f * (1.0f + sin_t(ms.n_ph)));
    }
    for (k = 0; k < n; k++) {
        float x[2] = {l[k], r[k]}, t = (float)k / fn;
        if (chorus) {                    /* chorus: two taps, the sides in quadrature */
            ch_buf[0][ms.w] = x[0];
            ch_buf[1][ms.w] = x[1];
            for (c = 0; c < 2u; c++) {
                float pos = (float)ms.w - (cd0[c] + (cd1[c] - cd0[c]) * t), f;
                uint32_t i0, i1;
                if (pos < 0.0f)
                    pos += (float)CH_N;
                i0 = (uint32_t)pos;
                f = pos - (float)i0;
                i0 &= CH_N - 1u;
                i1 = (i0 + 1u) & (CH_N - 1u);
                x[c] = x[c] * 0.75f + (ch_buf[c][i0] + (ch_buf[c][i1] - ch_buf[c][i0]) * f) * 0.55f;
            }
            ms.w = (ms.w + 1u) & (CH_N - 1u);
        }
        if (phaser) {                    /* phaser: four first-order allpasses swept slowly */
            float a = pa0 + (pa1 - pa0) * t;
            for (c = 0; c < 2u; c++) {
                float y = x[c] + ms.ph_fb[c] * 0.4f;
                for (j = 0; j < 4u; j++) {
                    float o = ms.ap[c][j] - a * y;
                    ms.ap[c][j] = y + a * o;
                    y = o;
                }
                ms.ph_fb[c] = y;
                x[c] = x[c] + (y - x[c]) * ms.ph_amt * 0.5f;
            }
        }
        if (trem) {
            float m = tm0 + (tm1 - tm0) * t;
            x[0] *= m;
            x[1] *= m;
        }
        if (noise) {                     /* noise layer: air or rain, moving in level */
            float g = ng0 + (ng1 - ng0) * t;
            for (c = 0; c < 2u; c++) {
                ms.n_lp[c] += (rnds() - ms.n_lp[c]) * ms.ntone;
                x[c] += ms.n_lp[c] * g;
            }
        }
        if (excite)                      /* exciter: the top octave, rectified a little and added back */
            for (c = 0; c < 2u; c++) {
                float h;
                ms.ex_hp[c] += (x[c] - ms.ex_hp[c]) * 0.35f;
                h = x[c] - ms.ex_hp[c];
                x[c] += (fm_fabsf(h) - 0.01f) * ms.excite * 0.6f + h * ms.excite * 0.4f;
            }
        ms.spk_mix += ((dsp_speaker ? 1.0f : 0.0f) - ms.spk_mix) * 0.0005f;
        if (ms.spk_mix > 0.001f)         /* the built-in speaker: 12 dB an octave under ~110 Hz */
            for (c = 0; c < 2u; c++) {
                float h;
                ms.spk[c][0] += (x[c] - ms.spk[c][0]) * 0.0155f;
                h = x[c] - ms.spk[c][0];
                ms.spk[c][1] += (h - ms.spk[c][1]) * 0.0155f;
                h -= ms.spk[c][1];
                x[c] += (h - x[c]) * ms.spk_mix;
            }
        for (c = 0; c < 2u; c++) {
            float y = x[c] - ms.dc_x[c] + 0.9993f * ms.dc_y[c];   /* DC blocker (~5 Hz): the rectifiers' offset */
            ms.dc_x[c] = x[c];
            ms.dc_y[c] = y;
            x[c] = fm_tanhf(y * drive) * sat_norm * 0.92f;   /* saturation and the limiter in one */
        }
        l[k] = x[0];
        r[k] = x[1];
    }
}

/* ================================================================== MIX === */
static float tr_l[NTRACK][HALO_CHUNK], tr_r[NTRACK][HALO_CHUNK];
static float tr_gain[NTRACK][2], tr_gain_to[NTRACK][2], tr_send[NTRACK][2];
static uint32_t tr_hold[NTRACK];
static uint8_t tr_live[NTRACK];

void dsp_init(void)
{
    uint32_t i;
    for (i = 0; i <= SIN_N; i++)
        sin_tab[i] = fm_sin_turns((float)i / (float)SIN_N);
    for (i = 0; i < RV_LINES; i++)
        rv.len[i] = RV_BASE[i];
    for (i = 0; i < ATMOS_VOICES; i++)
        atm.v[i].note = -1;
    for (i = 0; i < SHIM_SLOTS; i++)
        sh.s[i].note = -1;
    for (i = 0; i < ARIA_VOICES; i++)
        ar.v[i].note = -1;
    for (i = 0; i < ECHO_VOICES; i++)
        ec.v[i].note = -1;
    for (i = 0; i < DR_PARTS; i++) {
        dr.amp[i] = dr.amp_to[i] = 0.5f / (float)(i + 1u);
        mc_start(&dr.osc[i], rndf());
    }
    for (i = 0; i < DR_TWINS; i++)
        mc_start(&dr.twin[i], rndf());
    mc_start(&dr.sub, 0.0f);
    mc_start(&dr.fifth, 0.3f);
    mc_start(&dr.fifth_lo, 0.6f);
    dr.root = dr.root_to = (float)P[D_ROOT];
    dl.time = dl.time_to = 11025.0f;
}

void dsp_note_on(uint32_t track, int32_t note, uint32_t vel)
{
    if (note < 0 || note > 127)
        return;
    switch (track) {
    case TR_ATMOS:
        atmos_on(note, vel);
        break;
    case TR_DRONE:
        drone_set_root(note, !dsp_drone_gate && dr.level < 0.001f);
        dsp_drone_gate = 1;
        break;
    case TR_SHIM:
        shim_on(note);
        break;
    case TR_ARIA:
        aria_on(note, vel);
        break;
    case TR_ECHO:
        echo_on(note, vel);
        break;
    default:
        break;
    }
}

void dsp_note_off(uint32_t track, int32_t note)
{
    switch (track) {
    case TR_ATMOS:
        atmos_off(note);
        break;
    case TR_DRONE:
        if (!P[D_HOLD] && (int32_t)(dr.root_to + 0.5f) == note)
            dsp_drone_gate = 0;
        break;
    case TR_SHIM:
        shim_off(note);
        break;
    case TR_ARIA:
        aria_off(note);
        break;
    case TR_ECHO:
        echo_off(note);
        break;
    default:
        break;
    }
}

void dsp_release_all(uint32_t track)
{
    uint32_t i;
    switch (track) {
    case TR_ATMOS:
        for (i = 0; i < ATMOS_VOICES; i++)
            if (atm.v[i].eg.st != EG_IDLE)
                atm.v[i].eg.st = EG_REL;
        break;
    case TR_DRONE:
        dsp_drone_gate = 0;
        break;
    case TR_SHIM:
        for (i = 0; i < SHIM_SLOTS; i++)
            if (sh.s[i].eg.st != EG_IDLE)
                sh.s[i].eg.st = EG_REL;
        break;
    case TR_ARIA:
        for (i = 0; i < ARIA_VOICES; i++)
            if (ar.v[i].eg.st != EG_IDLE)
                ar.v[i].eg.st = EG_REL;
        break;
    case TR_ECHO:
        ec.nstack = 0;
        for (i = 0; i < ECHO_VOICES; i++)
            if (ec.v[i].eg.st != EG_IDLE)
                ec.v[i].eg.st = EG_REL;
        break;
    default:
        break;
    }
}

void dsp_pad_chord(const int32_t *notes, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < ATMOS_VOICES; i++) {
        pad_voice_t *v = &atm.v[i];
        if (i >= n) {
            if (v->eg.st != EG_IDLE && v->eg.st != EG_REL)
                v->eg.st = EG_REL;
            continue;
        }
        if (v->eg.st == EG_IDLE) {
            v->ph[0] = rndf();
            v->ph[1] = rndf();
            v->ph[2] = rndf();
            v->drift = 0.0f;
            v->pitch = (float)notes[i];
            mc_start(&v->air, rndf());
            mc_start(&v->sub, rndf());
            v->fl.ic1 = v->fl.ic2 = v->fr.ic1 = v->fr.ic2 = 0.0f;
        }
        if (v->eg.st == EG_IDLE || v->eg.st == EG_REL)
            v->eg.st = EG_ATK;
        v->note = notes[i];
        v->vel = 0.8f;
        v->age = 0;
        {
            static const float PAN[ATMOS_VOICES] = {-0.8f, 0.8f, -0.35f, 0.35f};
            v->pan = PAN[i];
        }
    }
}

void dsp_shim_chord(const int32_t *notes, uint32_t n)
{
    uint32_t i, j;
    for (i = 0; i < SHIM_SLOTS; i++) {
        sh_slot_t *s = &sh.s[i];
        if (i >= n) {
            if (s->eg.st != EG_IDLE)
                s->eg.st = EG_REL;
            continue;
        }
        if (s->eg.st == EG_IDLE) {
            s->pitch = (float)notes[i];
            for (j = 0; j < SH_PARTS; j++)
                mc_start(&s->o[j], rndf());
        }
        if (s->eg.st == EG_IDLE || s->eg.st == EG_REL)
            s->eg.st = EG_ATK;
        s->note = notes[i];
    }
}

uint32_t dsp_active(uint32_t track)
{
    uint32_t i, c = 0;
    switch (track) {
    case TR_ATMOS:
        for (i = 0; i < ATMOS_VOICES; i++)
            c += atm.v[i].eg.st != EG_IDLE;
        break;
    case TR_DRONE:
        c = dsp_drone_gate || dr.level > 0.001f;
        break;
    case TR_SHIM:
        for (i = 0; i < SHIM_SLOTS; i++)
            c += sh.s[i].eg.st != EG_IDLE;
        break;
    case TR_ARIA:
        for (i = 0; i < ARIA_VOICES; i++)
            c += ar.v[i].eg.st != EG_IDLE;
        break;
    case TR_ECHO:
        for (i = 0; i < ECHO_VOICES; i++)
            c += ec.v[i].eg.st != EG_IDLE;
        break;
    default:
        break;
    }
    return c;
}

void dsp_kill(void)
{
    uint32_t i;
    for (i = 0; i < ATMOS_VOICES; i++)
        atm.v[i].eg.st = EG_IDLE, atm.v[i].eg.v = 0.0f;
    for (i = 0; i < SHIM_SLOTS; i++)
        sh.s[i].eg.st = EG_IDLE, sh.s[i].eg.v = 0.0f;
    for (i = 0; i < ARIA_VOICES; i++)
        ar.v[i].eg.st = EG_IDLE, ar.v[i].eg.v = 0.0f;
    for (i = 0; i < ECHO_VOICES; i++)
        ec.v[i].eg.st = EG_IDLE, ec.v[i].eg.v = 0.0f;
    dsp_drone_gate = 0;
    dr.level = 0.0f;
}

static float warble_ph, flutter_ph;

static void cook_all(void)
{
    uint32_t t;
    /* the tape's wobble: a slow wow and a little flutter, a few cents at most */
    warble_ph = wrap1(warble_ph + 0.55f * (float)HALO_CHUNK * INV_FS);
    flutter_ph = wrap1(flutter_ph + 4.7f * (float)HALO_CHUNK * INV_FS);
    warble_semi = u7(X_WARBLE) * (0.16f * sin_t(warble_ph) + 0.035f * sin_t(flutter_ph));
    mac_space = b7(M_SPACE);
    mac_motion = b7(M_MOTION);
    mac_shim = b7(M_SHIM);
    mac_depth = b7(M_DEPTH);
    atmos_cook();
    drone_cook();
    shim_cook();
    aria_cook();
    echo_cook();
    reverb_cook();
    delay_cook();
    master_cook();
    for (t = 0; t < NTRACK; t++) {
        float vol = u7(TRK_P(t, 0)), gl, gr;
        int on = !((dsp_mute >> t) & 1u) && (!dsp_solo || ((dsp_solo >> t) & 1u));
        if (t == TR_DRONE)                   /* DEPTH: the drone forward or back */
            vol *= 1.0f + 0.5f * mac_depth;
        if (t == TR_SHIM)
            vol *= 1.0f + 0.6f * mac_shim;
        vol = on ? vol * vol * 1.4f : 0.0f;  /* an audio taper */
        pan_gains(b7(TRK_P(t, 1)), &gl, &gr);
        tr_gain_to[t][0] = vol * gl * 1.4142f;
        tr_gain_to[t][1] = vol * gr * 1.4142f;
        tr_send[t][0] = u7(TRK_P(t, 2)) * (1.0f + 0.5f * mac_space);
        tr_send[t][1] = u7(TRK_P(t, 3));
    }
}

void dsp_process(float *l, float *r, uint32_t n)
{
    uint32_t t, k;
    static float rev_in_l[HALO_CHUNK], rev_in_r[HALO_CHUNK], dl_in_l[HALO_CHUNK], dl_in_r[HALO_CHUNK];
    float out_peak = 0.0f;
    if (n > HALO_CHUNK)
        n = HALO_CHUNK;
    cook_all();
    for (t = 0; t < NTRACK; t++) {
        /* an engine is live while it has voices, and for a while after (its own tails: filters, diffusers) */
        if (dsp_active(t))
            tr_hold[t] = 4000u;          /* chunks: ~3 s */
        else if (tr_hold[t])
            tr_hold[t]--;
        /* a track turned all the way down (and faded out) costs nothing */
        tr_live[t] = tr_hold[t] != 0u && (tr_gain_to[t][0] + tr_gain_to[t][1] > 1e-5f
                                          || tr_gain[t][0] + tr_gain[t][1] > 1e-4f);
        if (tr_live[t])
            for (k = 0; k < n; k++)
                tr_l[t][k] = tr_r[t][k] = 0.0f;
    }
    if (tr_live[TR_ATMOS])
        atmos_run(tr_l[TR_ATMOS], tr_r[TR_ATMOS], n);
    if (tr_live[TR_DRONE])
        drone_run(tr_l[TR_DRONE], tr_r[TR_DRONE], n);
    if (tr_live[TR_SHIM])
        shim_run(tr_l[TR_SHIM], tr_r[TR_SHIM], n);
    if (tr_live[TR_ARIA])
        aria_run(tr_l[TR_ARIA], tr_r[TR_ARIA], n);
    if (tr_live[TR_ECHO])
        echo_run(tr_l[TR_ECHO], tr_r[TR_ECHO], n);
    for (k = 0; k < n; k++)
        l[k] = r[k] = rev_in_l[k] = rev_in_r[k] = dl_in_l[k] = dl_in_r[k] = 0.0f;
    for (t = 0; t < NTRACK; t++) {
        float pk = 0.0f, g0 = tr_gain[t][0], g1 = tr_gain[t][1], d0, d1;
        /* volume moves glide (~10 ms), ramped across the chunk: never a click */
        tr_gain[t][0] += (tr_gain_to[t][0] - tr_gain[t][0]) * 0.12f;
        tr_gain[t][1] += (tr_gain_to[t][1] - tr_gain[t][1]) * 0.12f;
        d0 = (tr_gain[t][0] - g0) / (float)n;
        d1 = (tr_gain[t][1] - g1) / (float)n;
        if (!tr_live[t]) {               /* nothing came from this engine: nothing to mix */
            dsp_meter[t] *= 0.98f;
            continue;
        }
        for (k = 0; k < n; k++) {
            float a, b;
            a = tr_l[t][k] * g0;
            b = tr_r[t][k] * g1;
            g0 += d0;
            g1 += d1;
            l[k] += a;
            r[k] += b;
            rev_in_l[k] += a * tr_send[t][0];
            rev_in_r[k] += b * tr_send[t][0];
            dl_in_l[k] += a * tr_send[t][1];
            dl_in_r[k] += b * tr_send[t][1];
            pk = fm_maxf(pk, fm_fabsf(a) + fm_fabsf(b));
        }
        dsp_meter[t] += (pk - dsp_meter[t]) * (pk > dsp_meter[t] ? 0.3f : 0.02f);
    }
    beat_run(l, r, rev_in_l, rev_in_r, n);
    delay_run(dl_in_l, dl_in_r, l, r, rev_in_l, rev_in_r, n);
    reverb_run(rev_in_l, rev_in_r, l, r, n);
    master_run(l, r, n);
    for (k = 0; k < n; k++)
        out_peak = fm_maxf(out_peak, fm_fabsf(l[k]));
    dsp_out_meter += (out_peak - dsp_out_meter) * (out_peak > dsp_out_meter ? 0.3f : 0.02f);
}
