# HALO factory presets

Each preset is a complete instrument state: the five engines, the effects, the mix, and the composer's mood,
key, scale and tempo, so pressing PLAY in INFINITY mode plays the atmosphere the preset is named for. They are
defined in `firmware/dsp/halo_params.c` as changes to the defaults in `halo_params.h`.

| # | Name | Character | Mood | Key / scale | Tempo | Reverb | Changes from default |
|---|---|---|---|---|---|---|---|
| 01 | Pale Horizon | Airy lydian pads, glass bells, a wide hall | Dream | D Lydian | 70 | Hall | 18 |
| 02 | Glass Cathedral | Bells and shimmer in a vast stone room | Celestial | E Major | 60 | Cathedral | 19 |
| 03 | Infinite Bloom | A frozen, shimmering tail that never ends | Celestial | G Lydian | 54 | Infinite | 16 |
| 04 | Distant Voices | A wordless choir, far away, oh to ah | Melancholy | A Minor | 60 | Cathedral | 15 |
| 05 | Velvet Atmosphere | Warm analog pads, chorus and soft tape | Nostalgic | F Major | 76 | Hall | 16 |
| 06 | Celestial Drift | Sus2 harmony that re-voices itself slowly | Celestial | C Major | 58 | Shimmer | 12 |
| 07 | Hollow Moon | Triangle pads, a fifth in the drone, a slow phaser | Mysterious | B Dorian | 56 | Dark | 13 |
| 08 | Dream Sequence | An up-down arpeggio through ping-pong echoes | Dream | D Lydian | 92 | Hall | 14 |
| 09 | Pink Nebula | Lydian shimmer, chorused air, a soft hiss | Dream | G# Lydian | 66 | Shimmer | 15 |
| 10 | Frozen Memories | Glassy minor pads, wow and flutter, dust | Melancholy | C# Minor | 62 | Hall | 13 |
| 11 | Soft Collapse | A dark drone that breathes and buckles | Dark | E Phrygian | 50 | Dark | 15 |
| 12 | Lunar Choir | A hushed ensemble: oo drifting toward ah | Serene | G Pent. major | 58 | Cathedral | 12 |
| 13 | Endless Rain | Plucked minor figures through tape, a rain layer | Melancholy | D Pent. minor | 82 | Hall | 14 |
| 14 | Harmonic Dust | Grains and sparks above a slow chord | Mysterious | A Dorian | 64 | Hall | 12 |
| 15 | Ghost Signal | Metallic resonance, tremolo, a distant transmission | Dark | F# Harmonic minor | 70 | Dark | 16 |
| 16 | Floating Gardens | Pentatonic bells, ping-pong, light as air | Serene | F Pent. major | 78 | Hall | 12 |
| 17 | Midnight Aria | A solo voice on ah over a dark minor hall | Melancholy | B Minor | 56 | Dark | 13 |
| 18 | Silent Geometry | Quartal harmony, euclidean and polyrhythmic lines | Mysterious | C Dorian | 88 | Hall | 13 |
| 19 | Lost Frequencies | Harmonic minor tension, grit and static | Dark | D# Harmonic minor | 60 | Dark | 15 |
| 20 | Afterglow | Warm maj7 chords, tape echoes, the last light | Nostalgic | A# Major | 74 | Hall | 12 |

The synthesis parameters behind each one are listed in full in the source; every one of them is editable on the
device and saved with SAVE.
