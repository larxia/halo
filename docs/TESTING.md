# HALO — testing

## What has been tested, and where

| Level | Status | Where |
|---|---|---|
| Host simulator: the whole firmware (UI, composer, engines, storage, backup) built from the same sources | **passing** | fm1-base's host simulator on Linux x86-64 |
| Audio: level, peak, DC, NaN, stereo, spectrum on every render | **passing** | `firmware/tests/analyse.py` on the simulator's WAVs |
| CPU load on the FM-1 | **estimated, not measured** | `firmware/tests/bench/cpu_estimate.sh` (valgrind, calibrated) |
| Compiling for the FM-1's CPU (JieLi clang, pi32v2) | **not yet done** | `.github/workflows/build.yml`, first run pending |
| Running on an FM-1 | **not yet done** | your FM-1, following the hardware plan below |

Nothing in this repository has run on FM-1 hardware yet. Every result below is from simulation.

## Running the tests

```
firmware/tests/run_tests.sh          # builds the simulator, runs the scripts, checks the audio
CPU=1 firmware/tests/run_tests.sh    # also the CPU estimate (needs valgrind)
```

The scripts (`firmware/tests/*.txt`) press buttons, turn encoders, play keys and send MIDI on a simulated clock.
Each `expect` checks the app's state. `shot` saves the screen and `wav` records the output.

| Script | What it proves |
|---|---|
| `smoke.txt` (21 checks) | boot; the pages; a preset loads; INFINITY starts with a 4-voice chord and stops; a key plays a chord; DRONE HOLD toggles and holds; SEQUENCE steps toggle from the white keys; SAVE survives a reboot; USB backup and restore bring the saved state back |
| `engines.txt` (5 checks) | each engine alone, a note or chord held then released: five WAVs for the audio checks |
| `styles.txt` (14 checks) | presets 21–24 in INFINITY: the lead, the ostinato, the beat (it plays), the piano; the LEAD model by hand stays one voice with two keys held and after one is let go |
| `compose.txt` (21 checks) | 150 s of INFINITY: pad, drone, shimmer and melody all play, chords stay 4-voice; SEQUENCE mode: chords from the pad's steps; REC writes played notes into the steps |

## Results (simulated)

Audio, from `analyse.py` (dBFS; LR is the left–right correlation; bands are energy share, in dB, of 20–200 / 200–1k /
1–4k / 4–10k / 10–20k Hz):

| Render | RMS | Peak | DC | LR | Note |
|---|---|---|---|---|---|
| ATMOS alone, a chord | −11 | −3.2 | 0.001 | +0.13 | mid-range warmth, filtered top |
| DRONE alone, held | −27 | −11.6 | 0.000 | +0.57 | the bed: energy under 200 Hz, partials above |
| SHIMMER alone, 3 notes | −33 | −16.5 | 0.000 | +0.09 | centroid 1.65 kHz: celestial, never harsh (−42 dB above 4 kHz) |
| ARIA alone, 2 voices | −19 | −6.3 | 0.000 | +0.47 | formant peaks in the 1–4 kHz band |
| ECHO alone, 5 plucks | −24 | −10.0 | 0.000 | +0.13 | |
| INFINITY, 6 presets × 98 s | −8 to −14 | −2.8 to −3.3 | ≤ 0.0015 | 0.0 to +0.3 | no clipping, no clicks, wide but mono-safe |
| INFINITY, presets 21–24 × 102 s | −13 to −20 | −3.1 to −5.6 | ≤ 0.0002 | +0.16 to +0.55 | the lead, the ostinato, the beat and the piano's rests visible in the spectrograms |

CPU, estimated (`cpu_estimate.sh`; calibration: Felucca's ANALOG engine with 8 voices, 17% measured on an FM-1):

| Case | x86 instructions / sample | Estimated FM-1 load |
|---|---|---|
| Calibration (ANALOG, 8 voices) | 1048 | 17% (measured by Felucca) |
| HALO typical, presets 01 / 21 / 22 / 23 / 24 | 3056 / 2957 / 3203 / 3145 / 2918 | ≈ 49 / 47 / 51 / 51 / 47% |
| HALO worst case (6 PIANO voices, every effect, the beat), FULL | 4282 | ≈ 69% |
| the same, ECHO as FM / LEAD | 3919 / 3624 | ≈ 63 / 58% |
| the same, LIGHT / LIGHTEST | 3541 / 3401 | ≈ 57 / 55% |

Optimisation history: 89% → 65% (oscillators as magic circles, a shared formant bank, chunk envelopes) → 51% → 59%
(the worst case had been undercounted: a drone bug kept it silent in the benchmark; fixed and re-measured) → 59%.

## The hardware plan (to do on your FM-1)

Do these in order, and record the results.

1. **Build.** Push to GitHub. The workflow must pass. Its first run is the first compile for pi32v2: fix what it
   reports (fm1-base's build checks: no doubles, no hardware access outside the base, RAM code rules, the 64-bit
   multiply check).
2. **Back up first:** see DEPLOY.md.
3. **Install** with the installer, then let it boot. Pass: the splash (pink Ø, HALO), then HOME.
4. **Idle load:** SETTINGS shows AUDIO LOAD. Note it with nothing playing.
5. **Typical load:** preset 01, PLAY, 3 minutes. Note the load and XRUNS (expect about 47%; pass if under 75% and
   0 xruns).
6. **Worst case:** QUALITY HIGH; set DRONE DENSITY 100 and GRAIN 100, ECHO SUSTAIN 100; play 4 keys on SYNTH, hold
   ENV, play 3 on ARIA and 4 on ECHO, CHORUS, PHASER, TREMOLO and NOISE up. Note the load. If it exceeds 90%, AUTO
   must step down (ENGINES: LIGHT) and the audio must keep running.
7. **Long run:** INFINITY for 2 hours. Listen for any drift, a stuck note, a growing tail. Check XRUNS stays 0.
8. **Controls:** every button and encoder per MANUAL.md. Velocity on and off. The MASTER pot.
9. **Storage:** SAVE, power off, power on: the sound, the song and the settings return.
10. **Backup:** back up over USB, change things, restore; the old state returns.
11. **MIDI:** USB notes on channels 1–5, Start and Stop, CC 91 / 1 / 74 / 71; the TRS input.
12. **Return path:** install the official firmware with the installer's "back to official" once. Confirm the FM-1
    works on stock, then reinstall HALO.

Anything that fails: note the step and what the screen said. A crash report is readable with
`base/tools/fm1_remote.py` (fm1-base README, Development over USB).
