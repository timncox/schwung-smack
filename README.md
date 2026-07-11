# Smack

A [schwung](https://github.com/charlesvestal/schwung) module for the Ableton
Move: grab a quantized loop of live audio, auto-slice it, and let seeded
randomness assign glitch effects and a new play order to the slices.

- **Capture**: retroactive grab (loop the last N steps/bars you just played)
  or arm-and-record. Lengths from 1 step to 16 bars, clock-synced.
- **Slice**: grid slicing at 1/2-step to 4-step resolution.
- **Pattern**: per-slice FX rolled at a set density — retrigger, reverse,
  varispeed pitch (±12 st), half/double speed, gate, buzz glitch, bitcrush —
  plus a separate slice-reorder density. Patterns are seeded: they repeat
  identically every loop pass until you re-roll.
- **A/B**: punch between the clean loop (A) and the pattern (B), quantized
  to slice or loop boundaries.

Two builds from one core:

| module | type | where it runs |
|---|---|---|
| `smack` | audio_fx | Signal Chain slots and Master FX (glitch the whole Move mix) |
| `smack-in` | sound_generator | standalone, reads mic/line input directly |

## Build

```bash
make test   # native sanity tests, no hardware
make arm    # Docker cross-compile for the Move (aarch64)
scripts/deploy.sh   # scp to move.local + rescan
```

Status: scaffold — compiles and passes host-simulation tests; not yet
verified on hardware.
