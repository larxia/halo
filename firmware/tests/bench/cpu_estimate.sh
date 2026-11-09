#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# HALO's estimated FM-1 CPU load, from instruction counts on this machine (valgrind), calibrated against a figure
# measured on a real FM-1: Felucca's ANALOG engine with 8 voices uses 17 % of the CPU (Felucca BUILDING.md).
#   estimate = HALO's instructions per sample / ANALOG-8's instructions per sample x 17 %
# An estimate, not a measurement: x86 instruction counts stand in for pi32v2 cycles (HALO is float, ANALOG fixed-point;
# the FM-1 also runs from an XIP flash cache). Confirm on a device: SETTINGS shows the audio load.
#   tests/bench/cpu_estimate.sh          (needs valgrind; clones Felucca-Engines once, MIT)
set -eu
cd "$(dirname "$0")/../.."
OUT=build/bench
mkdir -p "$OUT"
command -v valgrind >/dev/null || { echo "cpu_estimate: valgrind is needed"; exit 1; }
FE="$OUT/Felucca-Engines"
[ -d "$FE" ] || git clone -q --depth 1 https://github.com/hugelton/Felucca-Engines "$FE"
python3 "$FE/tools/gen_engine_tables.py" "$OUT/fe_tables.h" >/dev/null
CC="${CC:-cc}"
$CC -O2 -std=c99 -w -I"$OUT" -DFE_HOST="\"$(pwd)/$FE/tests/fe_host.c\"" -o "$OUT/bench_analog" tests/bench/bench_analog.c
for b in bench_halo bench_typ; do
    $CC -O2 -ffp-contract=off -std=c99 -w -DPLAT_HOST -I../base/include -Idsp -o "$OUT/$b" tests/bench/$b.c \
        dsp/halo_dsp.c dsp/halo_params.c dsp/halo_comp.c -lm
done
ir() { valgrind --tool=callgrind --callgrind-out-file=/dev/null "$@" 2>&1 | sed -n 's/.*Collected : //p'; }
per() { a=$(ir "$1" 200 ${2:-}); b=$(ir "$1" 2200 ${2:-}); echo $(( (b - a) / 64000 )); }
ref=$(per "$OUT/bench_analog")
echo "calibration: Felucca ANALOG, 8 voices: $ref instructions a sample = 17 % on an FM-1 (measured there)"
for q in 2 1 0; do
    n=$(per "$OUT/bench_halo" $q)
    echo "HALO worst case, quality $q: $n instructions a sample, estimated $(( n * 17 / ref )) %"
done
n=$(per "$OUT/bench_typ")
echo "HALO typical (INFINITY, preset 01): $n instructions a sample, estimated $(( n * 17 / ref )) %"
