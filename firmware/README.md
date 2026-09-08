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

## Flashing — and a warning about headroom

Builds `BOOT_NONE`, which **just fits**: 123,408 B of 131,072 B, **94.15%
used, 7,664 B free**.

That means it flashes with the STM32 ROM DFU baked into the silicon — hold
**BOOT**, tap **RESET**, release BOOT, and the board sits in DFU indefinitely
as `0483:df11`. No bootloader install, no QSPI write, and none of the
~2000 ms bootloader DFU window smack-versio has to race.

**But 7.6 KB is not much.** smack-versio hit exactly this wall — its Makefile
records BOOT_NONE at `-O3` having "4,056 bytes of headroom and could not take
another feature." Expect the next feature here to push it over. When it does,
switch to `APP_TYPE=BOOT_SRAM` and follow smack-versio's `FLASHING.md`.

This build fits where smack-versio's ~137 KB did not, largely because it does
not link the USB serial logger — 7.1 KB on its own in smack-versio's measured
breakdown.

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
| Audio Out 3/4 | Silent |
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

## Known gaps

- **Outs 3/4 are silent.**
- **No persistence.** smack-versio keeps settings in the last QSPI sector via
  `PersistentStorage`; that is not wired up here. Params reset on power-up.
- **CV outs and the SD card are unused.**
- **Nothing has been heard on hardware.** Clean build, measured memory map;
  that is all that is known.
