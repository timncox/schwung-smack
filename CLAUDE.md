---
status: active
last_touched: 2026-07-11
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

## Three builds, one core

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
- **oversmack** (`component_type: overtake`, v0.1.0): the SAME dsp.so as
  smack-in (build.sh copies it) + `src/ui_overtake.js` as a full-surface UI.
  Verified in schwung source (2026-07-11): loadOvertakeModule auto-loads the
  module.json `dsp` into the single-tenant slot-0 overtake DSP BEFORE
  evaling ui.js, and shims host_module_set_param/get_param to
  `shadow_set_param(0, "overtake_dsp:"+key, v)` — same call names as the
  chain UI, so engine code carries over. Clock reaches the DSP via the
  plugin STRUCT's on_midi on this path (no dlsym gotcha — that's chain-host
  audio_fx only). ui.js contract: set globalThis.init/tick/
  onMidiMessageInternal (+ optional onUnload/onResume); the host captures
  and restores them. `suspend_keeps_js`: Back hides the UI, DSP keeps
  running; onResume must force-repaint LEDs (setLED(..., true)) because
  they're cleared while suspended. `button_passthrough: [85]` = MovePlay
  reaches Move firmware, so transport/clock work under the UI.
  Editing model: steps select a slice; palette pads (76 + fx code, 0-22;
  pad 99 = unlock) pin an effect via `lock_slice_<i>` — pinning a DIFFERENT
  effect sets default_fxp(f), re-pinning the SAME effect keeps the rolled
  fxp (lock-as-is), -1 unlocks. Locks serialize as `i:f:p` triplets in the
  preset blob (parser accepts legacy `i:f`).

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
3. Fill pad momentary hold (v0.5.0): needs pad **release** events reaching
   onMidiMessageInternal (0x80, or 0x90 vel 0). If the host filters
   note-offs, tap-to-latch still works but hold-release won't auto-off.

## Help + accessibility (added v0.4.1)

- `src/help.json` (smack) / `src/help_smack_in.json` (smack-in) — on-device
  Help viewer content (Shift+Vol+Menu → Help → Modules). Host auto-discovers
  `help.json` in each module dir; build.sh copies them in. Format: tree of
  {title, children|lines}, lines pre-wrapped ≤20 chars (128x64 display).
- ui_chain.js imports `shared/screen_reader.mjs` (same pattern as the store/
  song-mode/file-browser UIs) and announces: view on init, pad actions,
  slice mute/restore, knob changes (speech-friendly names + option labels,
  not the screen abbreviations), and async state transitions from the tick
  refresh. A/B announces at press time only — get_param("ab") returns the
  APPLIED value, which lags the pad while a quantized flip is pending.
- Master FX knob pages announce automatically via shadow_ui.js
  (announceParameter on knob change) using module.json names/options —
  which is why those are full words ("1/2 Step", "Instant"), and why the
  trigger enums must stay literally ["idle","trigger"] (host idiom).
- `host_send_screenreader` writes to shared memory unconditionally; the TTS
  side decides whether to speak. Announcing unconditionally is the house
  pattern (shadow_ui.js does the same).

## Chain UI (src/ui_chain.js, shipped in both tarballs)

Loads when Smack's component editor is opened inside a slot's Signal Chain.
**The Master FX editor never loads custom module UIs** (verified:
shadow_ui_master_fx.mjs only uses hierarchy knob pages) — on master, knob
pages are the interface. Pad map: 68 Capture / 69 Arm / 70 A-B / 71 Re-Roll
/ 76 Clear / 77 Fill (tap = latch toggle, hold ≥350 ms = momentary);
hardware Capture button (CC 52) = retro grab; step buttons 1-16
show slice FX colors + playhead chase (fill pattern while Fill is active),
press = mute slice fx, again = restore seeded fx (lock_slice_<i>, -1
unlocks). Knobs 1-8 = FX / Order / Len / Res / Wet / Pitch / Fill Amt /
Seed (Quantize lives on the Setup knob page).

## Fill (added v0.5.0)

Elektron-style temporary variation: a second seeded pattern layer (own
order + fx rolls) that sounds only while fill is active — over either A/B
side; the normal layers are its "not fill" counterpart. Design points:

- Rolled in roll_pattern from an **independent RNG stream**
  (`seed ^ 0xF111F111 ^ nonce*2246822519`) so main-layer locks (which skip
  draws) never perturb the fill and vice versa. Same seed/nonce = same fill.
- 50% of fill fx picks come from a stutter-bias subset (retrig, reverse,
  speed, gate, buzz, repeat, revafter, tapestop) so fills read as fills.
  Order-shuffle density = fill_amt * 0.5, fx density = fill_amt.
- Params: `fill` (0/1 state, quantize-gated via fill_pending like
  ab_pending), `fill_once` (trigger: on now, auto-off at loop wrap — the
  wrap handler only clears it when fill is APPLIED, so a loop-quantized
  pending fill survives its first wrap and plays the full next pass),
  `fill_amt` (0-100, preset-saved). `fill` itself is deliberately NOT in
  the state blob (Elektron doesn't persist fill either); `fill_pattern`
  mirrors `pattern` for the LEDs. Clear drops fill with the loop.
- roll_fxp() is the factored per-effect parameter roller — **draw counts
  per effect must never change** or every saved seed re-sounds.

## Next steps

- On-device: verify chain UI (LED colors on steps, pad consumption,
  playhead chase rate) — untested on hardware. Same for oversmack (LED
  queue pacing, palette pads, suspend/resume repaint, play passthrough,
  and whether standalone DSP output sums into Move's mix at sane gain).
- **Dual-mono mode (Tim, 2026-07-11)**: treat incoming L and R as two
  independent mono lanes — each lane gets its own slice pattern/effects,
  plus a pan knob per lane to place them in the stereo field (two mono
  synths into Move's stereo line-in). Engine-level: dual fx/order/lock
  lanes, per-lane render position (order differs), pan_l/pan_r params,
  lane-select in the editing UIs, locks serialization gains a lane. Seed
  determinism: lane B continues the same rng stream. Benefits all three
  builds; oversmack UI has the room for a lane toggle pad.
- Momentary A/B punch (hold pad = temporary flip, release = back) — needs
  press-duration tracking in ui_chain.js.
- v2 FX: time-preserving pitchshift.
## Release process (three repos, one source)

Source + `smack` distribution: this repo (timncox/schwung-smack).
`smack-in` / `oversmack` distributions: timncox/schwung-smack-in and
timncox/schwung-oversmack — THIN repos holding only README + release.json
+ release tarballs. (store_utils.mjs does support a multi-module
release.json format `{modules:{id:{version,download_url}}}`, but no
catalog module uses it and older installers may predate it — sticking
with one repo per catalog id.)

To ship version X: bump the module.json versions + root release.json here,
`make arm`, commit/push, `gh release create vX` here with ALL tarballs,
then on each thin repo: update its release.json via API and
`gh release create` with that module's tarball. oversmack versions
independently (started 0.1.0).
Catalog PR: charlesvestal/schwung#156 (smack + smack-in entries;
oversmack needs a third entry once hardware-tested).
