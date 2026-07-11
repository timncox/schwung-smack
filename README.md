# Smack

A [schwung](https://github.com/charlesvestal/schwung) module for the Ableton
Move: grab a quantized loop of live audio, auto-slice it, and let seeded
randomness assign glitch effects and a new play order to the slices.
Inspired by Sugar Bytes Looperator and dblue Glitch, built for hands-on
hardware use.

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
- **18 effects**: retrigger, reverse, pitch (±12 st varispeed), half/double
  speed, gate, buzz, bitcrush, repeat-after-split, reverse-after-split,
  tape stop, tape start, vinyl scratch, envelope shapes, pan tricks, LP/HP
  filter sweeps, morphing vowel filter, tonal delay, granular freeze.
- **Presets**: full settings snapshot via schwung's module-preset system;
  transport follow (Move's stop pauses the loop, play restarts it).

Two builds from one core:

| module | type | where it runs |
|---|---|---|
| `smack` | audio_fx | Signal Chain slots and Master FX (glitch the whole Move mix) |
| `smack-in` | sound_generator | standalone, reads mic/line input directly |

## Install

- **Module Store**: install `smack` / `smack-in` from the schwung catalog
  (if listed).
- **From GitHub**: schwung-manager → Install Custom Module →
  `timncox/schwung-smack` (chain/master FX) or `timncox/schwung-smack-in`
  (standalone line-in looper — a thin distribution repo; source lives here).

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
