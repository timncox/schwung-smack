# Smack — Daisy Patch firmware

Smack's engine ported to the Electrosmith **Daisy Patch**, a 20 HP Eurorack
module built on the Daisy Seed. This directory is the Daisy shim; it is the
counterpart of `src/smack_fx.c`, which is the Move shim.

The engine in `vendor/` is vendored from this repo, not forked — see the header
comment in `vendor/smack_core.h` for the two constants that differ and why.
Fix engine bugs upstream in `src/` and re-vendor; do not patch `vendor/`.

## Build

```sh
make -C firmware                 # -> firmware/build/smack_patch.bin
make -C firmware program-dfu     # flash over USB
```

Needs libDaisy built at `~/tim-os/daisy-sdk/libDaisy` (override with
`LIBDAISY_DIR`). DaisySP is not used.

## Flashing — this one needs the bootloader

Builds **`BOOT_SRAM`**, unlike Belt and Mark. The history is the warning:

| | Size | |
|---|---|---|
| First cut, `BOOT_NONE` | 123,408 B / 131,072 B | 94.15% — Makefile said "the next feature pushes it over" |
| After `-u _printf_float` + CV outs | **128,296 B** | **97.88%, 2,776 B free** |
| **`BOOT_SRAM`** | 128,152 B of 480 KB SRAM | **26.07%** |

The Makefile predicted it and the very next commit proved it. 2,776 B is not a
margin, it is a tripwire for whoever edits this next.

**What that costs you:** the module needs the Daisy bootloader in internal
flash once (see smack-versio's `FLASHING.md`), and the bootloader's DFU window
is only **~2000 ms** after power-up — arm `dfu-util … -w` **first**, then
power-cycle **without** touching BOOT/RESET. Installing the bootloader is
itself done over USB from ST ROM DFU, so nothing extra is needed to get there,
and it is reversible: flashing any `BOOT_NONE` image over internal flash
removes it again.

Belt and Mark stay `BOOT_NONE` at 72% and 77%, so they still flash straight
from ST ROM DFU with no bootloader.

## Relationship to smack-versio

Same engine, same silicon, different panel. The Versio has 7 knobs, two
three-position switches, a button, one gate and 4 LEDs; the Patch trades most
of that for an OLED and an encoder.

What carried over **verbatim, deliberately** is the knob deadband/hysteresis
logic — it was learned by playing the module, not reasoned out. The pot sums
with its CV jack in analog hardware, so a knob parked on a step boundary
chatters; and `seed` gets a band of exactly one step, because a single step of
it re-rolls the whole pattern while `HYST` (2% of travel, ~2.5 seeds) would
skip most of the seed space.

What the screen changes: on the Versio the run state and CPU load had to be
replayed on LEDs at the *next* power-up, because "LED 3's live alarm can't be
read by someone playing with both hands." Here they are on screen while you
play.

## Panel

| Control | Function |
|---|---|
| Encoder turn | Page: `FX` / `LOOP` / `CLOCK` |
| Encoder tap | **Re-roll** — new pattern, same loop |
| Encoder hold > 0.6 s | **Capture** |
| Encoder hold > 2 s | **Clear** |
| Encoder double-tap | **Live** toggle |
| Knobs (page FX) | `fx_density` `order_density` `wet` `seed` |
| Knobs (page LOOP) | `loop_len` `slice_res` `pitch_range` |
| Knobs (page CLOCK) | 1 = ratio `/2 =1 x2`, 2 = mode `EXT INFER AUTO` |
| Gate In 1 | Clock / trigger (as on the Versio) |
| Gate In 2 | Direct capture trigger — footswitchable |
| Audio In 1/2 | Stereo source |
| Audio Out 1/2 | Processed out |
| Audio Out 3/4 | **Dry thru** — clean signal for downstream crossfading |
| **CV Out 1** | **Playhead through the captured loop, 0–5 V ramp per pass** |
| **CV Out 2** | **Wet amount, 0–5 V** |
| **Gate Out** | **Pulse on every loop wrap** |
| MIDI In | CC to the engine |

The gesture mapping is the one arrived at by playing smack-versio — tap is
re-roll because re-roll is used constantly; hold is capture because capture is
deliberate and happens once. It was reversed from the original for that reason;
don't reverse it back without playing it.

Display shows page, run state, inferred BPM and a `?` until the clock locks.
Knobs that have not yet picked up are marked `*`.

## Port decisions

**Knob pickup.** Seven params across pages on four absolute knobs — without
pickup, changing page slams whatever is under the pot into the engine. A knob
is inert until it crosses the value it takes over. The Versio never needed this
because its knobs were one-to-one with params.

**Block size 128 is not a preference.** The engine's clock regression was built
for 128-frame callbacks; at Daisy's default 48 the retro-capture phase
alignment fails. Reproduced at 44.1 kHz too, so it is block size, not rate.

**16 MB SDRAM pool.** `SMACK_RING_FRAMES` is 3,360,000 frames — 13.44 MB
stereo int16 — plus the per-lane delay and reverb lines. Same pool size
smack-versio uses.

## Why the gate output matters

Smack's loop length is whatever the player captured. The gate output pulses on
every wrap, so it is a clock at the musical period *actually in the buffer* —
something no division of a master clock knows. The Versio has four LEDs; the
Patch can tell the rest of the rack.

This requires `-u _printf_float`: the engine formats `play_frame` as `"%.0f"`,
and without the flag it reads `""`, the ramp sits at zero and the gate never
fires. That is the exact silent failure that broke smack-versio's LIVE mode.
**If the ramp is dead on hardware, check the ELF for `_printf_float` before
suspecting anything else.**

## Known gaps
- **No persistence.** smack-versio keeps settings in the last QSPI sector via
  `PersistentStorage`; that is not wired up here. Params reset on power-up.
- **The SD card is unused.** It is the obvious home for persistence.
- **Nothing has been heard on hardware.** Clean build, measured memory map;
  that is all that is known.
