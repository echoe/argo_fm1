# Argo

## Overview

Multi-engine synthesizer firmware for the M-VAVE FM-1 that is a branch off of Felucca, removing the samples and adding in different engines -
one SHDRUM, unique to this firmware, three engines from Mini-tone, and the drum engine from Jiant.
Connect the FM-1 to a computer by USB, open the
[web installer](https://echoe.github.io/argo_fm1/) in Chrome or Edge and press Install; no extra hardware is needed.
Installing is at your own risk: M-VAVE's updater or the installer's **Return to official V15** takes you back.

- Install: [web installer](https://echoe.github.io/argo_fm1/)
- Edit: [web editor](https://echoe.github.io/argo_fm1/webapp/editor/)
- Build: [BUILDING.md](BUILDING.md)
- Related: [Felucca-Engines](https://github.com/hugelton/Felucca-Engines) (the ANALOG, LOFI, TRIO, WHEEL, NOISE and PHASE engines, MIT; still maintained) ·
  [felucca-bsp](https://github.com/hugelton/felucca-bsp) (the FM-1 hardware layer, MPL-2.0; Felucca, and by extension Argo, builds on it from 1.7.0)
  [Mini-tone](https://github.com/eldontyrellcorp/Mini-Tone) (the SWARM, FM-TONE, and WAVETONE engines)
  [Jiant](https://github.com/juanjiant-bit/JIANT) (the DRUM-X engine)

## AI disclaimer

Argo is built fully with generative AI, piggybacking off of the work of Felucca. As is also true with Felucca, the
design, features, sound design and direction are the maintainer's or the community's, and no generative AI is used for music,
icons or artwork. This README was initially written by AI as it edited the repo; it was then edited for clarity. (If you see something that reads in a confusing way, please feel free to open an issue.)

## Credits and licence

Free software: [GPL-3.0-only](LICENSE); the Hügelton Sample Pack is CC0 1.0, but should be fully removed; the engines in `firmware/engines` are MIT
([Felucca-Engines](https://github.com/hugelton/Felucca-Engines)); the hardware layer in `firmware/bsp` is MPL-2.0
([felucca-bsp](https://github.com/hugelton/felucca-bsp)); the bundled fonts and ported DSP keep their own licences
([LICENSES/](LICENSES/), [LICENSING.md](LICENSING.md)). Additional engines are as listed in the overview.
Carried over from Felucca, thanks to everyone whose work and contributions are in Felucca as listed here: [CREDITS.md](CREDITS.md).
This includes the engines:
VOICE engine: after klattsch by Tony Gies (MIT); formant data from Klatt (1980) and Hillenbrand et al. (1995)
PHYS engine: models ported from DaisySP by Electrosmith and Emilie Gillet (MIT) and from Emilie Gillet's eurorack code (MIT)
FM6 engine: msfa from Dexed by Google Inc. and Pascal Gauthier (Apache-2.0)

M-VAVE and FM-1 are trademarks of their respective owners. Argo is not affiliated with or endorsed by them.

# The differences between this and Felucca

## No samples or engines that use samples are in this firmware

No samples or engines with samples are used. This frees up space for other features; I also wanted to focus on synth sounds specifically.

## Five new synth engines; one unique to this firmware, four from other firmwares

The synths in common with Felucca are:

- ANALOG, FM6, PHASE, LOFI, VOCAL, TRIO, WHEEL, PHYS, NOISE: default Felucca synths.

Synths from other firmwares are:
- SWARM, FM-TONE, WAVETONE: Engines from the Mini-tone firmware, modeled after the Digitone II's synth engines.
- DRUM-X: A drum engine from the Jiant firmware, modeled after Microtonic.

The unique synth in this firmware is:
- SH-TONE: A drum engine unique to this firmware, modeled after the SH-4d's drum engine. There is both a drum engine around this with several presets, and a synth engine where you can change every setting. The architecture is:
  - Two oscillators that can be SIN, TRI, SAW, SQR, NOISE, and PULSE.
  - Either MIX, FM, RING, or SYNC the two oscillators.
  - Sweep pitch and time, Osc B pitch, and Drive.
  - Individual AD envelope, leve, and Pulse Width (if applicable).
  - Drive/Click/Fold/Crush effects.
  - An ending LP/BP/HP filter.

## Added sequences (eight per track)

Each track plays one of eight sequences. A sequence has its own steps, its own LEN and its own automation and parameter
locks (up to 64 records while it is not the one playing); DIV, SWING, GATE, QUANTIZE and the sound stay the track's.

- **SEQ > SEQUENCE** (the ending SEQ page: KNOB 1..4 pick the sequence, 1..8, of tracks 1..4.
- **MENU > SYSTEM > SEQ SWITCH**: NOW (the default) switches patterns at once; END waits for the last step of the sequence playing.
- **CHAIN** (SEQ > SEQUENCE, PRESETS): turn PRESETS right to chain the selected track's sequence (the one playing, or the one
  waiting), left to take it out of the chain. The sequences you chain play one after the other in order. A sequence you did not chain loops by itself.
  A chained cell shows a bar at its left, the card "x3" (how many are chained), and the footer, where the sound's name is
  (PRESETS is not the sound on this page), "CHAIN ON" / "CHAIN OFF" for the selected sequence.
- **SONG**: PRESETS turns the sequence the selected cell plays: 0 (as saved: the one the project was saved on, as every song
  before) or 1..8 of that project's track. The PAT card says "S3", the line under the grid "NAME SEQ 3", and the footer says
  what PRESETS does ("PRESETS SEQ 3", or "SAVED"). A song holds up to six such chosen sequences ("TOO MANY SEQ" at PLAY
  otherwise). Older firmware reads a song with a sequence chosen as no song.
- SAVE keeps all of a project's sequences (the record holds the one playing, so an older firmware still opens it); RESTORE LAST
  keeps the music's. They live in the flash the user sample slots had, which builds with `FELUCCA_PATBANK=1` (the default) do not
  have. A song plays each project's sequence as it was saved.

## A generative mode within ARP

The arp MODE has a last value, **GEN**: the track plays by itself while the music plays (PLAY starts it on its first step, STOP silences it).
This doesn't affect the current sequence so the generated notes will play at the same time as sequence notes if the sequence is filled.
Everything that affects the ARP also affects this generative mode - for instance, it only plays notes as set in the SCL mode you can access in the SEL button menu.

**ARP > GEN** (the third ARP page) has these knobs for the selected track:
- **DENS** (KNOB 1): This controls how many note events are created. Turning it up adds notes to the same loop and turning it down takes them away; 0 plays nothing, 100 % makes a note on every step.
- **CHORD** (KNOB 2): 0 plays single notes. From 1 a growing share of the steps are chords. All of the notes are chords from halfway through.
  The chords get richer after that: a fifth or a third, then triads and their suspensions, sevenths, then 9ths and quartal stacks (up to four notes).
- **EVOLVE** (KNOB 3): how much the loop changes after each play. 0 repeats it exactly; a little moves a few notes a step or two in the scale (now and then a new chance, chord or loudness); 100 % changes nearly every step each loop.
- **ALGO** (KNOB 4): The algorithm the generative engine uses. It starts with no algorithm (full randomness). At 33% it has a 'Fifths' algorithm, at 67% a 'Bounce' algorithm, and at 100% it's a 'DRUMS' algorithm.

- To change the length of the generated loop, you need to change the length of the underlying sequence. Hold SEQ and you can change LEN there - it only generates up to 32 steps though, at the arp's RATE. SWG, GATE and PROB apply too.
- The knobs are saved with the sequence banks (project SAVE, RESTORE LAST); the loop itself is made when GEN starts and when a project loads, and keeps through STOP and PLAY.
- Record-arm a track to record the generated notes into the underlying sequence if it makes something you really like!

## Chord Improvements

Multiple chord-playing methods have been added to this firmware.

The CHORD page's CHRD has three new values. Two of them, **STR3** and **STR7**, use the black keys to select a chord, which changes what the white keys are.
With that black key held, you can play the white keys as all of the notes in that chord. The LLM says it this way:

- **Black keys** (F#3 .. F#5, eleven) choose the chord and make no sound: the diatonic triad (STR3) or seventh (STR7) on the scale's
  1st, 2nd, 3rd ... degree (the track's ROOT and SCALE; a chromatic scale uses the major scale of ROOT), the keys past the scale's
  last degree the same chords an octave up. The chosen key lights, and the CHORD page names the chord ("G7").
- **White keys** (F3 .. G5, sixteen) are the strings of that chord: the chord's notes in order from the left, then the same an
  octave up, and so on, so sliding a finger over the keys strums up the chord, and a key held sounds its string until it is let go.
- **VOIC** voices the chord (CLOSE, OPEN, INV1, INV2, +OCT's bass string), **TRN** and the octave buttons move it. A MONO, LEGATO or
  UNISON part plays the strings one at a time. MIDI notes, the arp and kits (DRUM, slices) play as with CHRD OFF.

There is also a third new value for CHRD, **PLAY**, inspired by a popular chord engine VST. Every white key plays its chord, and the 
**black keys held** with it shape the chord. The black keys make no sound by themselves. You can also set what each black key does, to set it up to your liking.
With the built in POP pack, for example, keys are set like this: 
key 1 MIN, key 2 MIN7, key 3 7, key 4 MAJ7, key 5 SUS4, key 6 SUS2,
key 7 DIM, key 8 +9, key 9 6, key 10 STRM, key 11 ARP. 
When you hold a black key and then hit a white key note, it will play that note as a chord, modified by whatever black notes you hit. So you can strum a minor 7 C chord by holding black key 1, 3, and 10, and then hitting the white C note.

The actions (lowest black key first, so a later one changes what an earlier one set: MIN then 7 is a minor seventh; 7, +9 and +11
stack):

- **quality** MIN, DIM, AUG, SUS2, SUS4, POW; **seventh** 6, 7, MAJ7, and MIN7, DIM7, M7B5, MMAJ7 (a quality with it); 9, MAJ9, MIN9
  (with the ninth); **extras** +9, +11, +13 (added tones), INV (the root up an octave), BASS (the root an octave down).
- **STRM**: the notes of the chord one after the other, low to high, held until the key is let go. How fast is the **SPEED** below.
- **ARP**: one note at a time, round the chord, at the track's ARP RATE and GATE, until the key is let go.
- Up to six tones: past that the fifth goes first, then the highest. VOIC voices the chord as it does the other chord modes.

**LATCH** (SCL > CHORDS, PRESETS: right is LATCH, left is HOLD): with LATCH on a black key pressed toggles its action on and a second
press turns it off, so the chord keeps its shape with no key held; the keys whose action is on stay lit. The chord is made from the latched
actions (a key still held does not count twice). Turning LATCH off, loading a pack, or loading the music clears them. LATCH settings are part
of the pack, saved and loaded with it.

**SCL > CHORDS** (the SCL button's third page, after SCL and CHORD) is where the black keys described above are actually set: KNOB 1 PACK, 2 KEY (which black key you're editing), 3 ACT (the action: what it
does), 4 SPEED (for strum, 0..1000 ms between notes). The page shows all eleven keys. 
Packs are what the black keys do and the strum speed together: six are built in (POP, JAZZ, SUS, EXT, EASY, HARP).
USER 1 through USER 4 can be changed and saved by the user: turning KNOB 1 loads a pack at once (a "*" says it was changed since).
**OCT+ saves** what the black keys do now into the user pack you pick. 
The packs and the keys' setting are saved with the music (project SAVE, RESTORE LAST), in the sequence bank's flash.

## Additional INSERT effects

We've added a GRAIN effect and a TAPE effect. This is to make up for the GRANULAR engine being removed.

## Preset changing per step

You can set a different preset per step. The preset needs to be of the same synth engine as the track is set to.
To do this go to SEQ, then the DETAIL 2 page (3/5), and you can see an option for the step you selected on page 1. You can also set the specific action individually on the 4th page of the SEQ menu.
This is an optional setting that is processed as though there is a settings change for the step.
... There are also other things you can tweak per step on that page that aren't the presets, but the reason the page is there is to have a quicker way to swap presets.

## 48 user presets

We've removed the saved patterns from presets in order to have more space to add additional presets. 
Note: The last 16 presets can't hold FM6 patches.

## Todo

-Manual editing of the slicer patterns instead of picking one of 16.
-Removing phrases, if possible.
-Figure out how to put on the screen that you delete a step by pressing the edit button, in the SEQ screen. It took me forever to find that in Felucca and it would be good.
-Make this change https://github.com/hugelton/Felucca/commit/743551712c36b3984392eea1dbe13a309907e17a
