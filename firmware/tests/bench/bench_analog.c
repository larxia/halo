/* Calibration for tests/bench/cpu_estimate.sh: Felucca-Engines' ANALOG engine, 8 voices held, the case Felucca measured
 * on an FM-1 at 17 % of the CPU (Felucca BUILDING.md). Built against a Felucca-Engines checkout (MIT): this file is
 * fe_host.c's voice host with a main of its own; FE is that checkout's path. */
#define main fe_host_main
#include FE_HOST
#undef main
#include <stdlib.h>
int main(int argc, char **argv)
{
    uint32_t nb = (uint32_t)atoi(argv[1]), b, k;
    const engine_t *e = &ENG_ANALOG; const preset_t *pr = &e->presets[1];   /* SOFT_PAD */
    track_t *t = &trk[0];
    int32_t out[CTL];
    static volatile int32_t sink;
    memset(t, 0, sizeof *t);
    for (k = 0; k < 8u; k++) t->p[P_E0 + k] = pr->e[k];
    t->p[P_ATK] = pr->env[0]; t->p[P_REL] = pr->env[3];
    for (k = 0; k < NVOICE; k++) voice_on(t, e, &t->v[k], 48 + 3 * k, (int32_t)k * 5 - 20);
    for (b = 0; b < nb; b++) {
        if (e->block) e->block(t);
        memset(out, 0, sizeof out);
        for (k = 0; k < NVOICE; k++) {
            voice_t *v = &t->v[k]; vmod_t m; int32_t a; uint32_t base;
            a = env_tick(v); if (e->amp) a = e->amp(t, v, a);
            base = pitch_inc((uint32_t)clamp(v->pitch16, 0, 2047));
            memset(&m, 0, sizeof m);
            m.pitch16 = v->pitch16; m.inc = base; m.amp0 = a; m.amp1 = a; m.envq15 = a;
            m.cutoff = ((int32_t)(b % 96u) - 48) * 128; m.shape = 64 << 8;
            e->render(t, v, out, CTL, &m);
        }
        sink += out[3];
    }
    printf("CTL %d\n", CTL);
    return 0;
}
