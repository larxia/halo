#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# HALO's tests in the host simulator (no FM-1, no toolchain): the whole firmware - UI, composer, engines - built for
# this machine by fm1-base, driven by scripts, its screens saved as PNGs and its sound as WAVs, then checked.
#   firmware/tests/run_tests.sh            from the repository root or anywhere
#   CPU=1 firmware/tests/run_tests.sh      also the CPU estimate (valgrind)
set -u
cd "$(dirname "$0")/.."
OUT=build/host
FAIL=0
python3 ../base/tools/build.py . --host > build-host.log 2>&1 || { tail -30 build-host.log; echo "FAIL build"; exit 1; }
rm -f build-host.log
for t in smoke engines compose; do
    if ./$OUT/halo_host tests/$t.txt $OUT > $OUT/$t.log 2>&1 && ! grep -q FAIL $OUT/$t.log; then
        echo "  ok   $t ($(grep -c '^ *ok' $OUT/$t.log) checks)"
    else
        echo "  FAIL $t"
        grep FAIL $OUT/$t.log | head -20 | sed 's/^/       /'
        FAIL=1
    fi
done
if python3 tests/analyse.py $OUT/*.wav > $OUT/audio.log; then
    echo "  ok   audio (level, peak, DC, NaN on every render)"
else
    echo "  FAIL audio"
    FAIL=1
fi
sed 's/^/       /' $OUT/audio.log
if [ "${CPU:-0}" = 1 ]; then
    sh tests/bench/cpu_estimate.sh || FAIL=1
fi
[ $FAIL = 0 ] && echo "all tests passed" || echo "TESTS FAILED"
exit $FAIL
