# HALO factory presets

Each preset is a complete instrument state: the five engines, the effects, the mix, and the composer's mood,
key, scale, tempo and rhythm, so pressing PLAY in INFINITY mode plays the atmosphere the preset is named for. They
are defined in `firmware/dsp/halo_params.c` as changes to the defaults in `halo_params.h`. Presets 21–24 are the
Minecraft-era palette (docs/ARCHITECTURE.md): techniques, no borrowed melodies.

| # | Name | Character | Mood | Key / scale | Tempo | Echo | Reverb |
|---|---|---|---|---|---|---|---|
| 01 | Pale Horizon | Airy lydian pads, glass bells, a wide hall | Dream | D Lydian | 70 | FM | Hall |
| 02 | Glass Cathedral | Bells and shimmer in a vast stone room | Celestial | E Major | 60 | FM | Cathedral |
| 03 | Infinite Bloom | A frozen, shimmering tail that never ends | Celestial | G Lydian | 54 | FM | Infinite |
| 04 | Distant Voices | A wordless choir, far away, oh to ah | Melancholy | A Minor | 60 | FM | Cathedral |
| 05 | Velvet Atmosphere | Warm analog pads, chorus and soft tape | Nostalgic | F Major | 76 | FM | Hall |
| 06 | Celestial Drift | Sus2 harmony that re-voices itself slowly | Celestial | C Major | 58 | FM | Shimmer |
| 07 | Hollow Moon | Triangle pads, a fifth, a slow phaser | Mysterious | B Dorian | 56 | FM | Dark |
| 08 | Dream Sequence | An arpeggio through ping-pong echoes | Dream | D Lydian | 92 | FM | Hall |
| 09 | Pink Nebula | Lydian shimmer, chorused air, a soft hiss | Dream | G# Lydian | 66 | FM | Shimmer |
| 10 | Frozen Memories | Glassy minor pads, wow and flutter, dust | Melancholy | C# Minor | 62 | FM | Hall |
| 11 | Soft Collapse | A dark drone that breathes and buckles | Dark | E Phrygian | 50 | FM | Dark |
| 12 | Lunar Choir | A hushed ensemble: oo drifting toward ah | Serene | G Pent. major | 58 | FM | Cathedral |
| 13 | Endless Rain | Minor plucks through tape, a rain layer | Melancholy | D Pent. minor | 82 | FM | Hall |
| 14 | Harmonic Dust | Grains and sparks above a slow chord | Mysterious | A Dorian | 64 | FM | Hall |
| 15 | Ghost Signal | Metal resonance, tremolo, a far signal | Dark | F# Harmonic minor | 70 | FM | Dark |
| 16 | Floating Gardens | Pentatonic bells, ping-pong, light as air | Serene | F Pent. major | 78 | FM | Hall |
| 17 | Midnight Aria | A solo voice on ah over a dark minor hall | Melancholy | B Minor | 56 | FM | Dark |
| 18 | Silent Geometry | Quartal harmony, euclidean polyrhythms | Mysterious | C Dorian | 88 | FM | Hall |
| 19 | Lost Frequencies | Harmonic minor tension, grit and static | Dark | D# Harmonic minor | 60 | FM | Dark |
| 20 | Afterglow | Warm maj7 chords, tape echoes, last light | Nostalgic | A# Major | 74 | FM | Hall |
| 21 | Warm Circuit | An analog lead gliding over minor chords | Analog | E Minor | 66 | Lead | Hall |
| 22 | Paper Lanterns | Turning pentatonic bells, pads, a choir | Lantern | C# Pent. major | 80 | FM | Hall |
| 23 | River Lights | Soft plucks, a quiet beat, warm pads | Lantern | D# Pent. minor | 90 | FM | Hall |
| 24 | Soft Keys | Felt piano, plain chords, long silences | Wistful | G Major | 74 | Piano | Hall |

Every value behind a preset is editable on the device and saved with SAVE.
