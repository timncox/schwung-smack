# Smack

A [schwung](https://github.com/charlesvestal/schwung) module for the Ableton
Move: grab a quantized loop of live audio, auto-slice it, and let seeded
randomness assign glitch effects and a new play order to the slices.
Inspired by Sugar Bytes Looperator and dblue Glitch, built for hands-on
hardware use.

(It's basically how the artist Honeysmack uses the Octatrack to process
modular. All hail Honeysmack!)

**📖 [Operation manual](https://timncox.github.io/schwung-smack/)** — every
pad, knob, and effect, with an interactive surface map.

- **Capture**: retroactive grab (loop the last N steps/bars you just played —
  also on the hardware Capture button) or arm-and-record. Lengths from
  1 step to 16 bars, clock-synced, with project-tempo free-run fallback.
- **Slice**: grid slicing at 1/2-step to 4-step resolution.
- **Pattern**: per-slice effects rolled at a set density, plus a separate
  slice-reorder density. Patterns are **seeded**: they repeat identically
  every loop pass until you re-roll, and the Seed knob browses patterns by
  number. Step buttons show the pattern (color per effect) with a playhead
  chase; press a step to mute that slice's effect.
- **A/B**: punch between the clean loop (A) and the pattern (B), quantized
  to slice or loop boundaries.
- **22 effects**: retrigger, reverse, pitch (±24 st varispeed), half/double
  speed, gate, buzz, bitcrush, repeat-after-split, reverse-after-split,
  tape stop, tape start, vinyl scratch, envelope shapes, pan tricks, LP/HP
  filter sweeps, morphing vowel filter, tonal delay, granular freeze,
  tempo-synced delay (incl. ping-pong), distortion (soft/hard/fold/gnash),
  phaser sweeps, and gated reverb bursts.
- **Dual mono**: treat the L and R inputs as two independent mono signals —
  each gets its own effect pattern and slice order, then Pan L / Pan R
  place them in the stereo field (two mono synths into Move's stereo
  line-in). Lanes share the seed stream, so patterns stay reproducible.
- **Presets**: full settings snapshot via schwung's module-preset system;
  transport follow (Move's stop pauses the loop, play restarts it).
- **On-device help**: full manual in schwung's Help viewer
  (Shift+Vol+Menu → Help → Modules).
- **Accessible**: with schwung's screen reader enabled, the chain editor
  announces pads, knob changes, and loop state; the Master FX knob pages
  are announced by the host.

Three builds from one core:

| module | type | where it runs |
|---|---|---|
| `smack` | audio_fx | Signal Chain slots and Master FX (glitch the whole Move mix) |
| `smack-in` | sound_generator | standalone, reads Move's selected input (mic/line/USB-C) directly |
| `oversmack` | overtake | full-surface looper (mic/line/USB-C) with the whole pad grid as a step-FX editor |

**Oversmack** takes over the entire Move surface (launch from the overtake
menu, Shift+Vol+Jog-Click): steps show the pattern and select a slice, the
upper three pad rows are an effect palette (tap to pin any of the 22
effects — or Clean, or Unlock — to the selected slice; pins survive
Re-Roll), the bottom pad row is transport, and the Play button passes
through to Move so clock keeps running. Back hides the UI while the audio
keeps processing.

## Install

- **Module Store**: install `smack` / `smack-in` from the schwung catalog
  (if listed).
- **From GitHub**: schwung-manager → Install Custom Module →
  `timncox/schwung-smack` (chain/master FX), `timncox/schwung-smack-in`
  (standalone line-in looper), or `timncox/schwung-oversmack` (full-surface
  editor). The -in and over- repos are thin distribution repos; all source
  lives here.

**Sync note:** set Move's **MIDI Clock to Out** so Smack locks to the
transport (same requirement as the Arp). Without it, Smack free-runs at the
project tempo.

## Build from source

```bash
make test   # native sanity tests, no hardware
make arm    # Docker cross-compile for the Move (aarch64), makes tarballs
scripts/deploy.sh   # scp to move.local (dev loop)
```

## Credits

Built on [schwung](https://github.com/charlesvestal/schwung) by Charles
Vestal (vendored API headers, MIT). Effect vocabulary inspired by Sugar
Bytes Looperator. Heavily written by coding agents, with human supervision.
