/* HALO's worst case: every engine sounding at its full voice count, every effect on. */
#include <stdio.h>
#include <stdlib.h>
#include "halo_dsp.h"
#include "halo_comp.h"
int main(int argc, char **argv)
{
    uint32_t nb = (uint32_t)atoi(argv[1]), b, i, q = argc > 2 ? (uint32_t)atoi(argv[2]) : 2u;
    static float l[HALO_CHUNK], r[HALO_CHUNK];
    static volatile float sink;
    int32_t ch[4] = {55, 59, 62, 66};
    params_defaults();
    P[D_DENS] = 127; P[D_GRAIN] = 127; P[D_GRIT] = 80; P[E_SUS] = 127; P[E_DEC] = 127;
    P[C_DEPTH] = 80; P[H_AMT] = 80; P[T_DEPTH] = 60; P[X_NOISE] = 60; P[X_EXCITE] = 60;
    dsp_quality = (uint8_t)q;
    dsp_init();
    dsp_pad_chord(ch, 4);
    dsp_shim_chord(ch, 4);
    dsp_note_on(TR_DRONE, 38, 100);
    for (i = 0; i < 3; i++) dsp_note_on(TR_ARIA, 64 + 4 * (int32_t)i, 100);
    for (i = 0; i < 4; i++) dsp_note_on(TR_ECHO, 72 + 3 * (int32_t)i, 100);
    for (b = 0; b < nb; b++) {
        dsp_process(l, r, HALO_CHUNK);
        sink += l[5];
    }
    printf("active %u %u %u %u %u\n", dsp_active(0), dsp_active(1), dsp_active(2), dsp_active(3), dsp_active(4));
    return 0;
}
