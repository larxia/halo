# HALO — hardware verification and architecture

## Phase 1: the hardware, verified

Everything below comes from M-VAVE's own open-source release and from the code of firmwares that run on FM-1s today.
Each line names where it comes from. Nothing here is assumed.

| What | Verified value | Source |
|---|---|---|
| SoC | JieLi AC791N / WL82, CPU `pi32v2`, run at 360 MHz by the boot loader | AL-255/FM-1-RE (reverse engineering); felucca-bsp `docs/CPU.md`; fm1-base `plat.h` (`plat_cpu_khz`) |
| FPU | single-precision hardware float (the SDK's own flags; fm1-base builds with it, minimp3 runs on it) | fm1-base `tools/build.py` |
| Second core | present; usable through fm1-base, marked **beta** | felucca-bsp `docs/EXPERIMENTAL.md`; fm1-base `plat_cpu2_*` |
| Flash for an app | 581 KB app slot, plus a data area (0x97000–0xDFFFF) for saved objects | fm1-base README, "What each costs" |
| RAM | 96 KB for `.data`/`.bss`, plus a 336 KB pool for big buffers (no external RAM) | fm1-base README |
| Audio | 44.1 kHz stereo, rendered in blocks (HALO: 256 frames = 5.8 ms), 24-bit samples to the codec | fm1-base `template/app.h`, `plat.h` |
| Display | 240 × 240 ST7789-class **colour** panel, RGB565, on SPI | fm1-base `device/lcd.c` |
| Keys | 27 (F3–G5); velocity and pressure readable on the analog rows | fm1-base `plat.h`, `lib/touch.h` |
| Encoders | 7: SELECT, ALGO, PRESET, K1–K4 (detents, no push) | fm1-base `plat.h` |
| Buttons | 14: FX, SEL, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ, PLAY, REC, OCT−, OCT+ | fm1-base `plat.h` |
| Other | MASTER pot, LEDs under buttons and keys, headphone jack switch, battery reading | fm1-base `plat.h` |
| MIDI | in: USB, TRS, Bluetooth LE; out: USB, Bluetooth LE (no MIDI out jack) | fm1-base `plat.h`; FM1 Open README |
| Storage | 3,840-byte objects in A/B flash sectors with CRC (torn-write safe); a flash write pauses audio | fm1-base `plat.h` (`plat_store_*`) |
| Firmware format | `.fwsc` package (app + update loader), installed over USB-MIDI | felucca-bsp `tools/fm1pkg.py`; Felucca BUILDING.md |
| Official route | M-VAVE publishes FM1 Open (GPLv3 app, binary engine SDK) with a browser workbench that can restore stock V15 | github.com/m-vave/fm1_open |
| Known CPU fault | inlined 64-bit multiplies next to 16-bit loads corrupt registers now and then | felucca-bsp `docs/CPU.md` |

**Is custom firmware feasible and safe?** Yes, with care. Dozens of community firmwares install and run, and M-VAVE
now supports the practice officially. Every fm1-base firmware has an update entry. The web installers offer
"return to official V15". There is a rescue path for a unit stuck in boot mode, though it needs a Seeed XIAO RP2040
wired to the USB lines (FM-1 Transporter). One honest limit: the reverse-engineering project notes that the update
protocol is not a proven recovery mechanism, because an interrupted write is not guaranteed safe. See DEPLOY.md.

**What this means for the design.**

| Requested | On the FM-1 | HALO's approach |
|---|---|---|
| Fixed-point DSP "where beneficial" | float is native, and 64-bit fixed-point multiplies are hazardous here | float32 throughout, no doubles, no 64-bit multiplies |
| Five simultaneous engines | yes, with a fixed voice budget | 4 + 1 + 4 + 3 + 4 voices, no dynamic allocation |
| True pitch-shifted shimmer reverb | too costly next to five engines | harmonic synthesis + a rectifier octave in the reverb loop |
| Granular textures | no sample RAM to spare | synthesised grains: short windowed sines on the drone's harmonics |
| Wavetable synthesis | no tables worth the flash | a continuous sine–triangle–saw morph that wanders (MORPH) |
| Stereo width | yes | per-voice panning, stereo filters, ping-pong delay, stereo reverb |
| 4 macro controls | K1–K4 exist | mapped one-to-one, no paging needed |
| Pink and black interface | colour panel | the identity as specified, not a monochrome translation |
| Bluetooth MIDI | possible, but costs pool RAM the effects use | off in v0.1; USB + TRS MIDI |

## Phase 2: architecture

```
 keys / MIDI / encoders  ──>  app.c (UI, 60 fps)  ──comp_post()──>  lock-free queue
                                                                        │
 audio interrupt (every 256 frames) ── halo_render() ───────────────────┘
     for each 32-frame chunk:
        clock (24 PPQ) ─> sequencer  |  INFINITY director  |  arpeggiator  |  pending note-offs
                    └──> dsp_note_on / dsp_pad_chord ...
        dsp_process():  cook params (P[] + macros) ─> 5 engines ─> mixer ─> delay ─> reverb ─> master
```

* **One state array.** Every value a knob can reach is a byte in `P[]` (129 of them, `dsp/halo_params.h`). The UI
  writes single bytes. The DSP reads them once a chunk and "cooks" them into coefficients. Presets are lists of
  changes to the defaults. A saved sound is that array.
* **No locks on the audio path.** The main loop never calls into the engines. Notes go through a 64-entry
  single-producer queue, drained at the start of each block.
* **Chunk processing.** 32 frames (0.7 ms) is the unit for parameter cooking, envelopes of slow engines, LFOs and the
  clock. Within a chunk, levels are ramped linearly, so a parameter change never clicks.

### Engines (`firmware/dsp/halo_dsp.c`)

| Engine | Voices | Synthesis |
|---|---|---|
| ATMOS (pad) | 4 | 3 detuned oscillators per voice morphing sine → triangle → band-limited saw (polyBLEP); an octave "air" partial and a sub; a trapezoidal SVF low-pass per voice with envelope and LFO; slow per-voice pitch drift; chords glide between voicings (voice leading) instead of re-attacking |
| DRONE | 1 (13 partials) | 8 harmonic partials (1–10×) with random-walk amplitudes, 5 detuned twins for slow beating, sub octave and fifths (only where audible), optional band-limited saw "grit", up to 4 synthesised grains on the harmonics, a wandering stereo filter, HOLD, glide between roots |
| SHIMMER | 4 slots × 3 partials | 2, 2½ and 3 octaves above the harmony (±1 octave), each partial twinkling on its own random envelope, two cross-coupled allpass diffusers, sent hard into the reverb |
| ARIA (choir) | 3 | two detuned band-limited saws plus breath noise per voice, one shared stereo formant bank (three band-passes) morphing through AH, EH, EE, OH and OO, delayed-onset vibrato, ensemble detune |
| ECHO | 6 (4 at lighter quality; 1 as LEAD) | three models. **FM:** two operators, 8 ratios, decaying index. **PIANO:** two detuned strings (FM at ratio 1 for hammer brightness), felt-hammer noise, key-tracked decay, brightness fading before level, low-to-high panning. **LEAD:** mono, saw-to-square oscillators plus a sharp saw, soft drive, two-stage resonant SVF (4-pole), filter envelope, glide, delayed vibrato, last-note priority with a 6-key stack. ADSR, velocity, tone filter, arpeggiator, scale lock |
| BEAT | 3 sounds | a sine kick falling to 45 Hz, a bright noise tick, a swelling noise shaker; INFINITY's RHYTHM row drives it |

All fixed-pitch sines (partials, sub, air, grains, the FM modulator) are "magic circle" oscillators: two multiplies
and two adds a sample. They are renormalised once a chunk, so they never drift over hours.

### Effects

| Effect | Implementation | Memory |
|---|---|---|
| Reverb | 4-line FDN, Hadamard mix, 4 input diffusers, two modulated lines, damping per line, low cut, rectifier shimmer in the loop; types HALL, CATHEDRAL, INFINITE, DARK, SHIMMER; LFO-button FREEZE | 80 KB pool |
| Delay | tape / ping-pong, tempo-synced, stored at 22.05 kHz in 16 bits (1.49 s a side), wow and flutter, saturation and filtering in the feedback | 128 KB pool |
| Master | chorus, 4-stage phaser, tremolo, moving noise layer (air or rain), exciter, DC blocker, saturation, tanh limiter | 8 KB pool |
| Tape warble | a 0.55 Hz wow and 4.7 Hz flutter added to every engine's pitch (WARBLE), a few cents | none |

### Composer (`firmware/dsp/halo_comp.c`)

* **Clock:** 24 PPQ, tempo 30–157 BPM, chunk-accurate timing.
* **Sequencer:** 5 tracks, 4 patterns × 32 steps each. Every step has a note, a probability (8 levels) and a length
  (1–32 steps). Per track: length (polymeter), rate (1/16 to 2 bars), chain (any pattern order), GENERATE
  (steps wander to neighbouring scale tones), and one automation lane of 16 points. Notes are scale-locked.
  The pad track plays chords from the chord generator.
* **Chord generator:** 10 scales, chord types AUTO (the scale's own sevenths), MAJ7, MIN7, ADD9, SUS2, SUS4, MIN9,
  OPEN 5TH, QUARTAL and NOTES. Voice leading searches all 24 voice assignments for the least movement, penalising
  doubled notes and low clusters. **HARMONIC DRIFT** moves one voice at a time to a colour tone of the chord (its
  9th, 11th or 6th, if the scale has it), never into a semitone rub except in DARK.
* **INFINITY:** per mood, a weighted transition table between scale degrees, a palette of chord colours, a melodic
  register, a choir frequency and a drone "lift". It runs an arrangement arc (intro → bloom → song → thinning →
  swell) with an energy level that gates layers in and out. Melodies are Euclidean rhythms carrying a motif that
  repeats with mutations, leaning on chord tones on strong beats. Above 50% complexity a second polyrhythmic layer
  (5, 7 or 12 steps) is added. A slow "breath" varies note probability. RANDOM flattens the transition weights,
  mutates the motif and moves section boundaries; it never leaves the scale.
* **RHYTHM (0.2):** PULSE plays an ostinato, a figure of 8 or 16 indexes into the current voicing (rising and
  turning shapes), so it follows every chord change and voice-leading move. REST gates the melody into 2–4-bar
  phrases with 1–5-bar silences; above 50% the pad sits out some chords. With the PIANO model a left hand plays a
  low root each bar and a chord tone a beat or two later. BEAT puts a kick on the bar (and the half, with density),
  ticks on a Euclidean figure and a shaker off the beat, once the arrangement's energy passes the intro.

### Memory budget (HALO's own; the platform's comes on top)

| | Size | Where |
|---|---|---|
| Reverb lines + diffusers | 80 KB | pool |
| Delay (2 × 32,768 × int16) | 128 KB | pool |
| Chorus | 8 KB | pool |
| Screen canvas (fm1-base) | 68 KB | pool |
| **Pool total** | **≈ 284 KB of 336 KB** | |
| Sine table, diffusers, track buffers, song, settings | ≈ 13 KB | RAM (of 96 KB) |
| Saved objects | settings, sound (≈ 140 B), song (1.6 KB) | flash, 3 objects |

### CPU budget

Estimated with `firmware/tests/bench/cpu_estimate.sh`. Instructions per sample are counted on x86 with valgrind and
scaled against a figure measured on a real FM-1 (Felucca's ANALOG engine, 8 voices = 17%):

| Case | Estimated load |
|---|---|
| Typical INFINITY, presets 01 / 21 / 22 / 23 / 24 (pad chord, drone, shimmer, choir, 3 echo voices, beat) | ≈ 49 / 47 / 51 / 51 / 47% |
| Worst case: every voice of every engine (6 PIANO voices), every effect, the beat; quality FULL | ≈ 69% |
| The same with FM / LEAD echo | ≈ 63 / 58% |
| Worst case, quality LIGHT / LIGHTEST (AUTO steps down to these above 90%) | ≈ 57 / 55% |

This is an **estimate, not a measurement**. HALO is float and the calibration engine is fixed-point. The FM-1 runs
code from a flash cache, and the target compiler is different. The SETTINGS page shows the real audio load on a
device. In AUTO quality, `app_overload` (the platform calls it above 90%) steps the engines down, and they come back
8 seconds after the load allows. Next steps if the device measures higher: run the audio loops from RAM
(`PLAT_RAM_CODE`), and give the pad and drone to the second core.

## Sound design: the reference

The reference atmosphere (soft, slowly changing harmony; synthetic melodies; high harmonics; long reverb; drones) was
analysed for its *techniques*, not its notes. Nothing of any existing recording, melody or arrangement is reproduced.

* **Harmony that moves by voice leading, not by jumps:** chords glide voice by voice (`dsp_pad_chord`), and HARMONIC
  DRIFT re-colours a held chord, so the harmony "breathes" between changes.
* **Lydian and major-seventh colour:** the DREAM mood favours lydian, maj7 and add9, and I–vi–IV gravitation.
* **Upper harmonics as a separate layer:** SHIMMER puts partials 2–3 octaves above the harmony and lets them twinkle
  independently. The reverb's rectifier adds still more octaves.
* **Space as an instrument:** high sends, long modulated tails and the SPACE macro moving size, decay and sends
  together. FREEZE holds the room.
* **A bed underneath:** the drone holds the tonic, lifting to chord roots now and then, and fades over seconds.

## Sound design: the Minecraft-era palette (0.2)

Requested later: music in the manner of C418's Volume Alpha and Beta, naming *Moog City*, *Haunt Muskie* and *Aria
Math*. Published descriptions: Moog City was made with a Moog Voyager synthesizer and sits in E minor; Haunt Muskie is
described as an upbeat, ambient, rhythmic track; C418 hears an Asian influence in Aria Math and Haunt Muskie, and
Volume Beta leans on lush ambient synths. HALO takes the *techniques* (a warm mono analog lead with glide and
vibrato, rhythmic pentatonic figures over lush pads, a soft felt piano with space, a little tape wobble) and none of
the melodies, chord sequences or recordings. The presets WARM CIRCUIT, PAPER LANTERNS, RIVER LIGHTS and SOFT KEYS,
and the moods ANALOG, LANTERN and WISTFUL, are where these meet.
