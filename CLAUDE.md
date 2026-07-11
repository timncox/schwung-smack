---
status: active
last_touched: 2026-07-10
---

# Smack

Schwung module for the Ableton Move: records incoming audio (mic/line or
whatever is upstream in the Signal Chain), loops it quantized (1 step to 16
bars), auto-slices at a chosen resolution, and assigns random playback FX to
slices at a chosen density — retrigger, reverse, varispeed pitch, half/double
speed, gate-chop, buzz glitch, bitcrush — plus random slice reordering.
The pattern is seeded and stays fixed across loop repeats until re-rolled.
A/B switches between the clean loop and the pattern.

Schwung = charlesvestal/schwung, the Shadow-UI sidecar framework for the Move
(LD_PRELOAD shim + QuickJS shadow UI + native ARM DSP modules).

## Two builds, one core

- `src/smack_core.c` — all engine logic (ring buffer, MIDI-clock tracking,
  capture, slicing, pattern, FX render). Non-allocating render path.
- `src/smack_fx.c` → **smack** (`audio_fx`): works in chain slots AND Master
  FX slots (master FX loads the same audio_fx plugins). The .so MUST be named
  `smack.so` — the chain host loads `modules/audio_fx/<id>/<id>.so` without
  reading module.json. **Critical ABI gotcha:** the chain host delivers MIDI
  to audio FX via `dlsym("move_audio_fx_on_midi")`, NOT the struct's on_midi
  field — the exported wrapper in smack_fx.c is what makes clock sync work
  (pattern verified in ducker, pushnpull, punchfx; leave struct field NULL
  for old-host ABI safety).
- `src/smack_gen.c` → **smack-in** (`sound_generator`, `audio_in: true`):
  standalone mic/line looper reading the host mailbox input like linein does.

## Build / test / deploy

- `make test` — native compile of the core + `test/host_sim.c` (simulated
  clock + capture/AB/reroll/arm assertions). No hardware needed.
- `make arm` (= `scripts/build.sh`) — Docker cross-compile for aarch64 using
  debian:bookworm + gcc-aarch64-linux-gnu (same toolchain family as schwung
  itself). Produces module tarballs under `build/`.
- `scripts/deploy.sh` — scp both modules to `ableton@move.local:` under
  `/data/UserData/schwung/modules/`, then rescan modules on-device.

`include/*.h` are vendored copies of schwung's stable ABI headers
(`src/host/plugin_api_v1.h`, `src/host/audio_fx_api_v2.h`).

## Timing model

4/4, 16th-note steps, MIDI clock 24 ppqn (6 ticks/step, 96/bar) arriving via
`on_midi` (source 3 = host). Free-run fallback derives phase from
`host->get_bpm()`. Retro capture aligns to the last half-step boundary.

## v1 limitations (deliberate, revisit)

- Ring recording pauses while LOOPING (no incremental copy-out); re-grab
  while looping re-slices the frozen history.
- Arm/record starts on a block boundary (≤2.9 ms off), not sample-accurate.
- Bar-phase (downbeat) alignment for ≥1-bar grabs assumes 0xFA start resets
  the grid — VERIFY ON DEVICE.
- Varispeed = linear interp, no anti-aliasing (lo-fi is part of the charm).

## Setup prerequisite (from schwung manual, manual.html#limitations)

Modules only receive MIDI clock when **Move's MIDI Clock is set to Out**
(same as the Arp). Without it Smack free-runs from get_bpm() project tempo —
functional but not phase-locked to Move's transport. The manual confirms
this clock→project-tempo fallback is the house convention (the built-in
Quantized Sampler does the same). Also: Master FX has two LFOs that can
target any loaded FX param — e.g. LFO on Smack's fx_density/order_density.

## Clock findings from shipped modules (verified in repos 2026-07-10)

- MIDI clock IS broadcast to audio-FX slots (pushnpull clock.c: "we lock to
  the MIDI clock the host broadcasts to audio-FX slots").
- Downbeat = 0xFA Start / 0xFB Continue resets beat phase to 0; phase resyncs
  from tick_count each block (pushnpull convention; smack_core mirrors it).
- Trigger params must only fire on non-zero values and read back "0"
  (UI init / autosave restore sends "0"; autosave must never re-fire them).

## Not yet verified on hardware

1. End-to-end smoke test (clock arrival, capture alignment, audio quality).
2. Enum "trigger" params (Capture/Arm/Re-Roll/Clear as knob enums) UX — the
   real answer is a ui_chain.js with a punch pad + step-LED pattern display.

## Chain UI (src/ui_chain.js, shipped in both tarballs)

Loads when Smack's component editor is opened inside a slot's Signal Chain.
**The Master FX editor never loads custom module UIs** (verified:
shadow_ui_master_fx.mjs only uses hierarchy knob pages) — on master, knob
pages are the interface. Pad map: 68 Capture / 69 Arm / 70 A-B / 71 Re-Roll
/ 76 Clear; hardware Capture button (CC 52) = retro grab; step buttons 1-16
show slice FX colors + playhead chase, press = mute slice fx, again =
restore seeded fx (lock_slice_<i>, -1 unlocks). Knobs 1-8 = FX / Order /
Len / Res / Wet / Pitch / Qnt / Seed.

## Next steps

- On-device: verify chain UI (LED colors on steps, pad consumption,
  playhead chase rate) — untested on hardware.
- Momentary A/B punch (hold pad = temporary flip, release = back) — needs
  press-duration tracking in ui_chain.js.
- v2 FX: granular freeze/spray, tape-stop, time-preserving pitchshift.
- Module Store distribution: release.json + tarballs already produced by
  build.sh.
