# HALO manual

HALO turns the M-VAVE FM-1 into an ambient workstation: five sound engines on five tracks, a step sequencer with a
chord generator, and **INFINITY**, which composes by itself in a mood you choose.

## Ten seconds to sound

1. Turn **PRESET** to choose an atmosphere (it loads as you turn).
2. Press **PLAY**. INFINITY begins: first a drone and a chord, then shimmer, then melody and choir.
3. Turn **K1–K4**: SPACE, MOTION, SHIMMER, DEPTH.
4. Play the keys over it. Hold **LFO** to freeze the reverb.

## The screen

```
 Ø HALO  PAGE            ▶ 70      header: page, transport, tempo, REC dot; a dot per page beneath
 ┌──────────────────────────────┐
 │  the page: what it controls  │   main area
 └──────────────────────────────┘
 K1 LABEL  K2 …   K3 …   K4 …      the four knobs' values and bars; the row's name and position beneath
```

## Controls

| Control | Everywhere | On particular pages |
|---|---|---|
| SELECT (turn) | page: HOME, SYNTH, DRONE, SHIMMER, ARIA, ECHO, SEQUENCE, INFINITY, FX, PRESETS, SETTINGS | |
| ALGO (turn) | row of the page (each row is four values for K1–K4) | |
| PRESET (turn) | the page's track volume | HOME: presets, loaded as you turn · PRESETS: browse · SEQUENCE: step cursor · INFINITY: mood |
| K1–K4 | the four values of the row | HOME row 1: the macros |
| HOME | HOME page | held: white keys 1–5 solo, black keys 1–5 mute tracks |
| EDIT | next engine page (SYNTH → DRONE → SHIMMER → ARIA → ECHO) | |
| SEQ / GLO / FX / SEL | SEQUENCE / INFINITY / FX / PRESETS page | PRESETS: SEL loads the highlighted preset |
| ENV | DRONE HOLD on/off (lit when on) | |
| LFO (hold) | FREEZE: the reverb holds its tail | |
| ARP | the ECHO arpeggiator on/off | |
| PLAY | start / stop (INFINITY or SEQUENCE, by MODE) | |
| REC | record: notes played go into the track's steps (from the cursor when stopped, at the playing step when running) | |
| SAVE | save sound, song and settings (lit when there are unsaved changes; the audio pauses for a moment) | |
| OCT− / OCT+ | keyboard octave | SEQUENCE: steps 1–16 / 17–32 |
| MASTER | output volume | |

The keys play the page's track. On HOME, FX, INFINITY, PRESETS and SETTINGS they play the last selected track.
SCALE LOCK (SETTINGS) snaps them to the key and scale.

### Macros (HOME, K1–K4; 0 is neutral, −63 … +63)

| Macro | Moves |
|---|---|
| SPACE | reverb size and decay, reverb sends, delay feedback |
| MOTION | every LFO and drift rate, drone movement, vowel drift, reverb modulation, chorus speed |
| SHIMMER | shimmer level and brightness, the pad's air, the reverb's shimmer, exciter, pad brightness |
| DEPTH | drone level, density and sub, the pad's sub, darker pad and drone filters, darker reverb |

MIDI CC 91, 1, 74 and 71 drive SPACE, MOTION, SHIMMER and DEPTH.

## Pages

**SYNTH (ATMOS):** oscillators (SHAPE morphs sine → triangle → saw; DETUNE; AIR, an octave partial; SUB), filter
(CUTOFF, RESO, ENV>F, DRIFT), envelope, movement (LFO HZ, LFO>F, MORPH: the shape wanders; WIDTH), and mix (volume,
pan, reverb and delay sends). One key plays a chord when CHORD (INFINITY page) is not NOTES.

**DRONE:** ROOT, DENSITY (how many harmonics), SUB, FIFTH · MOTION, TONE, BEAT (slow beating), GLIDE · GRAIN,
GRIT, CUTOFF, WOBBLE · HOLD, FADE IN, FADE OUT, FOLLOW (follows the key's tonic while playing). A key sets the root;
with HOLD on, it keeps sounding after release. The screen shows the spectrum and the root now.

**SHIMMER:** INTENSITY, BRIGHT, SPARKLE, DIFFUSE · ATTACK, RELEASE, OCTAVE, FEEDBACK (the reverb's shimmer). It
follows the pad's chords automatically; its own keys and steps add notes.

**ARIA:** VOWEL (AH EH EE OH OO), V.DRIFT (how far the vowel swings), V.RATE, BREATH · VIBRATO, V.SPEED, ENSEMBLE,
FORMANT (lower: darker, a male choir) · ATTACK, RELEASE, OCTAVE, BRIGHT.

**ECHO:** RATIO (0.5 to 7: soft plucks, glass, metal), INDEX, I.DECAY, TONE · ADSR · ARP (UP, DOWN, UP DN,
RANDOM, CHORD: arpeggiates the pad's chord), RATE, OCTS, VEL (velocity response).

**SEQUENCE:** the white keys are steps 1–16 (OCT+ for 17–32). Tap a key to switch a step on or off; hold it and
turn K1–K4 to set its NOTE, CHANCE, LENGTH. Black keys 1–5 choose the track; black keys 6–9 choose the pattern
(A–D). Rows: STEP · TRACK (STEPS: length, RATE, CHAIN: which patterns play in turn, GENERATE: steps wander to
neighbouring scale tones) · PATTERN (the pattern edited, AUTO: a parameter automated across the pattern, POINT: its
value at the cursor, TEMPO). On the pad track a step plays a chord on its note.

**INFINITY:** MOOD (DREAM, CELESTIAL, MELANCHOLY, DARK, SERENE, MYSTERIOUS, NOSTALGIC; each brings a scale and tempo,
both still editable), KEY, SCALE, TEMPO · COMPLEXITY (richer chords, wider leaps, a polyrhythmic second melody),
DENSITY, EVOLVE (how often the chords and sections change), RANDOM · MODE (SEQUENCE or INFINITY: what PLAY runs),
CHORD (a fixed colour, or AUTO for the mood's), H.DRIFT (harmonic drift), HUMAN (velocity and timing life).

**FX:** reverb (TYPE: HALL, CATHEDRAL, INFINITE, DARK, SHIMMER; SIZE, DECAY, DAMP, MOD, SHIMMER, LEVEL, LOCUT) ·
delay (TIME in beats, FEEDBACK, TONE, WOW, MODE: TAPE or PINGPONG, LEVEL, TO REV) · CHORUS, PHASER, TREMOLO ·
SATURATION, EXCITER, NOISE (with N.TONE and N.MOVE), OUTPUT.

**PRESETS:** 20 factory presets (docs/PRESETS.md). PRESET or K1 browse; SEL loads.

**SETTINGS:** SCALE LOCK, VELOCITY, OCTAVE, MIDI IN (channels 1–5 to tracks 1–5, or OMNI to the selected track) ·
QUALITY (AUTO lightens the engines under load; HIGH; ECO), CLEAR SONG and INIT SOUND (turn twice). The audio load
is shown here.

## MIDI

In over USB and TRS. Notes on channels 1–5 play tracks 1–5 (or every channel plays the selected track: OMNI). Start
and Stop run the transport. CC 123 silences everything. The keys' notes go out over USB on the track's channel.

## Saving

SAVE writes the sound (every parameter, mutes and solos), the song (all patterns) and the settings. They load at
power-on. Loading a preset replaces the sound but keeps the song and MODE.
