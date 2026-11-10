/* HALO typical: what INFINITY plays most of the time (a preset's effects, pad chord, drone, shimmer, 1 choir, 2 echo) */
#include <stdio.h>
#include <stdlib.h>
#include "halo_dsp.h"
#include "halo_comp.h"
int main(int argc, char **argv)
{
    uint32_t nb = (uint32_t)atoi(argv[1]), b;
    static float l[HALO_CHUNK], r[HALO_CHUNK];
    static volatile float sink;
    int32_t ch[4] = {55, 59, 62, 66};
    preset_apply(argc > 2 ? (uint32_t)atoi(argv[2]) : 0u);   /* the preset: its effects and mix */
    dsp_init();
    dsp_pad_chord(ch, 4);
    dsp_shim_chord(ch, 4);
    dsp_note_on(TR_DRONE, 38, 100);
    dsp_note_on(TR_ARIA, 69, 100);
    P[E_SUS] = 127;
    dsp_note_on(TR_ECHO, 74, 100);
    dsp_note_on(TR_ECHO, 78, 100);
    dsp_note_on(TR_ECHO, 81, 100);   /* three echo voices: a melody note and an ostinato overlapping */
    dsp_perc(PERC_KICK, 100);
    for (b = 0; b < nb; b++) {
        dsp_process(l, r, HALO_CHUNK);
        sink += l[5];
    }
    return 0;
}
