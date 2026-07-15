---
status: active
last_touched: 2026-07-15
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
`host->get_bpm()`. Retro capture aligns its buffer to the last half-step
boundary, then starts at the elapsed grid phase instead of restarting late at
frame zero. MIDI-clock tempo uses a 96-tick regression window to remove the
host's 128-frame callback quantization; playback varispeeds by the residual
capture/current-clock ratio so the error cannot accumulate over long loops.

## v1 limitations (deliberate, revisit)

- ~~Ring recording pauses while LOOPING~~ FIXED v0.9.0: the ring keeps
  recording during playback, so Capture re-grabs NEW audio. It only
  freezes when the write head would run into the playing loop region
  (70 s ring minus loop length = the re-grab window; each capture
  resets it — only near-ring-filling loops ever hit the freeze).
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

0. USB-C audio as input: the XMOS muxes mic/line/USB-C into the single
   SPI-mailbox Audio-IN region the modules read (Tim, 2026-07-11 — the
   CPU just listens to "an audio source"). If so, Smack processes USB-C
   audio with no code changes; verify by selecting USB as Move's input
   and capturing in smack-in/oversmack.
1. End-to-end smoke test (clock arrival, capture alignment, audio quality).
2. Schwung 0.11.4 latched enum "trigger" knobs and held the "Triggered"
   overlay for about four seconds. The custom Schwung build dated 2026-07-15
   adds short, self-clearing trigger feedback, so the Perform hierarchy maps
   Capture/Arm/A-B/Re-Roll to knobs 1-4. Clear remains list-only to avoid an
   accidental loop wipe. Clockwise fires; counter-clockwise explicitly returns
   idle and re-arms the gesture. The chain/overtake UIs still provide pads.

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

## Dual mono (v0.5.0, engine-level)

- `channel_mode` 0 Stereo / 1 Dual Mono; `pan_l`/`pan_r` 0-100 equal-power
  (defaults 0/100 = keep input sides). In dual mode the B side renders
  lane 0 from the LEFT input channel and lane 1 from the RIGHT, each
  through its own pattern + per-effect runtime (`smack_lane_t`: fx/order/
  locks + filter/vowel/delay/phaser/verb state), then pans the two lane
  outputs into the field. The A side always plays the raw stereo loop.
- Determinism: one rng stream — lane 1's roll continues after lane 0's,
  so seed+nonce fully determine both lanes, and lane 0 always equals the
  stereo-mode pattern.
- Params: `pattern_r`, `lock_slice_r_<i>`; state blob gains `chan`,
  `pan_l`, `pan_r`, `locks_r`. Old presets (no `chan`) restore as stereo.
- UIs (chain + oversmack, same pads): pad 74 = mode toggle, pad 75 =
  lane select (tap) / hold + knob 1-2 = Pan L / Pan R. Steps + locks edit
  the selected lane. Setup hierarchy page adds Channels/Pan L/Pan R
  (master-FX LFO can target the pans!).
- ⚠️ Parallel work: branch `feat/fill` (draft PR timncox/schwung-smack#1,
  from another session) predates this refactor — it must REBASE onto the
  lane refactor (roll_pattern/render/params all restructured) and bump to
  0.6.0 (0.5.0 is taken by dual mono).

## Feedback guard (v0.5.1)

- The HOST's guard (shadow_ui.js reconcileFeedbackHolds + feedback_gate.mjs)
  only walks chain SLOTS and keys off `consumesLineInput(metadata)`
  (capabilities.audio_in + component_type != audio_fx/midi_fx). It covers
  smack-in as a plain slot synth, but is BLIND to (a) overtake modules and
  (b) smack-in nested inside a chain patch (chain/module.json declares no
  audio_in — upstream schwung gap).
- Ours: core `monitor` param (0 = live input muted at the output; ring
  keeps recording, loop playback stays audible; never preset-saved) +
  `hw_input` flag set by the gen wrapper so the SHARED chain UI only arms
  the guard for smack-in (the audio_fx build takes upstream chain audio
  and must never auto-mute). Both UIs poll host_speaker_active() &&
  !host_line_in_connected() ~2x/s: risk -> auto-mute + announce; safe ->
  auto-restore; Monitor pad (73 in both UIs, green/red) toggles manually
  and overrides during risk.
- ⚠️ oversmack does NOT declare capabilities.audio_in (dropped in 0.5.4):
  schwung's feedback gate raises a launch confirmation modal for
  audio-in tools (speakers on, no line-in), and on the v0.11.x shim
  that sequence ends in a NATIVE crash (hardware-observed 2026-07-12).
  Safe to drop: the shim feeds overtake DSPs raw hardware input
  unconditionally — the flag only drives the gate heuristic. RESTORE
  the flag once schwung ships a release newer than v0.11.4 (main's
  gate is display-mode aware).

Loads when Smack's component editor is opened inside a slot's Signal Chain.
**The Master FX editor never loads custom module UIs** (verified:
shadow_ui_master_fx.mjs only uses hierarchy knob pages) — on master, knob
pages are the interface. Pad map: 68 Capture / 69 Arm / 70 A-B / 71 Re-Roll
/ 76 Clear; hardware Capture button (CC 52) = retro grab; step buttons 1-16
show slice FX colors + playhead chase, press = mute slice fx, again =
restore seeded fx (lock_slice_<i>, -1 unlocks). Knobs 1-8 = FX / Order /
Len / Res / Wet / Pitch / Qnt / Seed.

## BPM detection + step paging (v0.6.0)

- Shift+Capture (pad or hardware button; MoveShift = CC 49) triggers
  `detect_bpm`: onset-strength envelope (|L|+|R|, 512-frame hop) over the
  last 8 s of ring, autocorrelated across 60-180 BPM, spread incrementally
  on the audio thread (16384 ring frames/block, ~21 blocks). Parabolic
  peak refinement; octave candidates {x, 2x, x/2} biased to host
  get_bpm() by log distance; confidence gate (peak < 2x mean |r| = "no
  clear tempo" — pads/drones report 0, sim-tested). Result auto-applies
  as `bpm_override` (free-run only — real MIDI clock ALWAYS wins in
  frames_per_tick_now). `detected_bpm` reads -1 scanning / 0 none / BPM.
  Title shows @BPM. Ableton Link needs no code: Move's tempo follows
  Link, so get_bpm() and clock-out both track it.
- Step paging for loops > 16 slices: oversmack = jog wheel (MoveMainKnob
  CC 14, decodeDelta); chain UI = pad 72 cycles pages. Step press maps
  through `stepPage*16`; footer/summary show p N/M; page clamps on
  pattern refresh, resets when the loop ends.

## Web editor + per-slice tweaking (v0.7.0 / oversmack 0.4.0)

- `src/web_ui.html` ships in the smack-in + oversmack tarballs; schwung-manager
  auto-serves it (Remote UI iframe for the smack-in synth slot, Tool tab for
  oversmack; pop-out standalone works via `?schwungStandalone=1[&tool=1]`).
  The chain audio_fx build CANNOT have one — remote_ui.go only wires custom
  web UIs for slot synths and overtake tools.
- Protocol (all verified in schwung-manager source): browser cache seeds from
  the module's `state` JSON via fetchAllParams (value must start `{`);
  device→browser sync is rev-gated on `rui_poll` = "rev:on:tick:bpm"
  (`edit_rev` bumps on every content edit EXCEPT punch_pressure; playhead
  rides `rui_play` pushes, slot mode falls back to `ps` in state). The DSP
  must answer `module_id` (gen wrapper returns "oversmack") or the manager
  thinks no tool is loaded.
- `state` JSON now carries read-only display fields (run/nsl/mon/ps/det/pfx/
  bpmo + pat/fxp/ord csv per lane) — the restore parser ignores them, old
  presets still load.
- `lock_slice_<i>` (and `_r_`) accepts "f:p" to pin an effect WITH a chosen
  parameter; bare "f" keeps pad-UI semantics.
- roll_lane now CONSUMES rng draws for locked slices (computed, discarded):
  unlocked slices roll identically with or without pins, so preset/layout
  restores reproduce exactly what was heard when the pin was placed live.
  (Before this, a pin shifted every later slice on the next roll.)
- Layouts need no engine support: a layout = partial state blob
  {slice_res, densities, pitch_range, seed, nonce, chan, locks, locks_r};
  applying = one `state` SET (absent keys keep current values; audio
  untouched). Effect presets = (f,p) pairs. Both live in browser
  localStorage with export/import in the editor.
- ⚠️ Released schwung (v0.11.4, 2026-06-25) predates ALL of this
  manager-side: no Tool tab (oversmack editor unreachable), no
  resubscribe/param-poll (editor shows a banner; reload to re-sync),
  master-FX Remote UI values render "?". The smack-in slot iframe DOES
  work there (verified on Tim's device). Everything lights up when
  schwung ships > v0.11.4.

## Extended effect options (v0.8.0)

Seeded rolls keep the old ranges (same seed = same pattern,
bit-identical — Freeze/Verb rolled values 0-3 special-cased), but the
editable fxp space per effect is much deeper, reachable from the web
editor and swept by punch pressure (pitch punch ±24): Retrig to 1/64,
Crush 2-24, Buzz 6 windows, Scratch 8 cycles, Speed ×1.5/×¾, Gate
4/8/16/swing, TapeStop sag/slam, Repeat/RevAfter 7 splits, Env 8
shapes, Pan pingpong×8 + sweeps, Filter resonant (bq_set_q Q=4),
Vowel 8 pairs, TonalDelay 8 curves, Delay 32nd/triplet/64th/
pingpong-8th (quarter impossible: SMACK_DLY_LEN 8192 < step×4 @120),
Dist hot (+4 drive), Phaser deep/static notches, Verb decay to 0.96.
fp is int8 (±127) — every render case clamps/masks, never trusts
range. Sim runs an extreme-min/max render sweep per effect.

## Chain-UI swap + pad-grid reality (v0.12.1, hardware-verified 2026-07-13)

- HARDWARE FINDING (closes the oldest "not yet verified" item): in a
  slot's component editor, Move firmware KEEPS the pad grid (pads play
  notes into the synth) — chain module UIs get screen + knobs + jog +
  hardware buttons, but their pad-LED writes lose to firmware (Tim:
  "the grid isn't switching for smack-in"). Full pad ownership =
  overtake mode only. ui_chain.js pad handlers kept (schwung routes
  all MIDI to the module UI in COMPONENT_EDIT), but the pad map is an
  oversmack feature on hardware; slot-mode grid editing = web editor.
- Shift+jog-click in Smack's chain UI = swap module, via schwung
  main's `host_swap_module()` shim (setupModuleParamShims — unloads
  the module UI, enters component select). Mirrors the chain list's
  handleShiftSelect gesture. Guarded with typeof for old hosts
  ("Swap needs a newer schwung"). Footer shows "click=swap" on Shift.
- host_list_modules() is registered on the QuickJS global (both host
  + shadow contexts) — [{id,name,version}] — available if a UI ever
  needs a module list without fetch.

## Per-slice depth + mix (v0.12.0 / oversmack 0.9.0)

- Every slice now carries fxp2 (per-effect depth 0-100, -1 = engine
  default) and fxmix (0-100 wet blend vs the CLEAN slice in pattern
  order, -1 = full wet). Rolls NEVER set them (edit-only; seeds
  unchanged from 0.11.0). Full token form everywhere: "f[:p[:p2[:m]]]"
  — lock_slice / set_slice / punch_fx / palette pads all accept it
  (parse_fx_token; clamp_pct −1-sentinel). Locks serialize i:f:p or
  i:f:p:p2:m; blob adds fx2/mix csvs (display); palette tokens carry
  4 fields; punch gets punch_fxp2/punch_mix.
- Depth meanings: Retrig/Repeat decay, Pitch glide, Gate floor, Buzz
  fade, Crush smash (variable bit shift 1-8), Scratch throw, Pan
  width, Filter rez (Q .7-8), Vowel sharpness, TonalDly/Delay/Comb
  feedback (.15-.87, overrides variants), Freeze/PShift grain size,
  Dist drive (1-15, overrides hot), Phaser depth, Verb tail, RingMod
  tune trim (±1 oct), Scatter chaos %. No depth: Reverse/Speed/
  RevAfter/TapeStop/TapeStart/Env (Mix covers them).
- Render refactor: edge/loop fades moved from g into gedge so the dry
  tap of the mix blend gets fades but not effect gains; blend happens
  after CRUSH quantize; dry tap = same source slice unwarped
  (src_dry), so Mix = effect intensity, order preserved.
- Web editor: Depth + Mix sliders (× = back to default) on slice,
  brush, presets, AND palette pads (renderPadCtl); labels from FX
  meta `x:` field. Sim gotcha that cost 20 min: energy assertions
  MUST set monitor=0 first or the live saw input drowns the loop.

## Variant pads + 4 new effects (v0.11.0 / oversmack 0.8.0)

- SMACK_FX_COUNT 23 → 27: PSHIFT (time-preserving granular pitch shift,
  1024-frame grains, two-tap FREEZE-style crossfade, fxp = semitones),
  RINGMOD (sine ring mod, 0-5 fixed 50-1760 Hz, 6/7 = 4-octave chirps
  with exact integral phase), COMB (feedback comb via the SHARED
  ln->dly line — joined the TONALDELAY/DELAY branch so the else-reset
  doesn't clear it every frame; 0-3 static 55/110/220/440 Hz, 4/5
  sweeps, 6/7 hot-feedback screamers), SCATTER (hash-shuffled grains
  within the slice, seed+slice+grain deterministic, no rng state;
  4-7 sparse dropouts). All four JOIN THE ROLL POOL — old seeds now
  produce different patterns (Tim explicitly OK'd: no saved seeds).
- Palette entries are now `f` or `f:p` (SMACK_PALETTE_SLOTS=23 pads
  select from the 27-effect pool; duplicates + subsets legal; parser
  still all-or-nothing). `punch_fx` and `set_slice_<i>` accept "f:p";
  variant pads assign/pin/punch with their pinned parameter.
- Web editor arrange mode: click a pad → renderPadCtl (effect <select>
  swaps the pad's effect, option buttons/slider set its variant,
  "Default param" clears); click a second pad to swap positions.
  Pad buttons show "•" when a variant is pinned.

## Custom palette layout (v0.10.0 / oversmack 0.7.0)

- `palette` param + `pal` state-blob key: position (pad offset 0-22) →
  fx code, engine-persisted so oversmack + the web editor share one
  arrangement and presets restore it. parse_palette accepts ONLY a full
  23-code permutation (truncated blobs / dupes / out-of-range never
  half-apply); old presets without `pal` keep the current layout.
- Web editor "Arrange pads": click two pads to swap (sends full CSV),
  "Factory layout" resets; UNLK (pad 99) is fixed and disabled while
  arranging. Reads M.pal (blob key), writes `palette` (param key) —
  same asymmetry as locks / lock_slice_<i>.
- ui_overtake polls `palette` in fetchAll (%12 tick) → repaints palette
  LEDs when it changes. PALETTE_PAD_OF_CODE was dead code, removed.
- The engine never reads the palette — pure UI preference stored in
  engine state. Manual site documents it (pinning + web-editor sections).

## Pad play — notes trigger slices (v0.13.0 / oversmack 0.10.0)

- MIDI note-on/off now reach the engine: with `pad_play` on, a note
  triggers pattern cell (note % n_slices), QUANTIZED to the next
  `pad_rate` boundary (1/16, 1/8, 1/4 dflt, 1/2, 1 bar — loop-grid
  domain, derived from slice_frames so post-capture tempo drift can't
  skew it) and RETRIGGERS there while held. Velocity → sqrt gain;
  between repeats (slice shorter than rate) the pad owns the output:
  silence, not the loop. Release = back to the still-advancing loop
  timeline instantly. Last-note priority, 10-deep held stack;
  note-offs always drain it (toggling pad_play can't stick a note);
  0xFA/0xFB clears it (transport stuck-note safety).
- Render: trig forces the pattern side with oslice/p overridden (cell +
  repeat-local pos) and exactly ONE render_lane call per frame while
  trig — routing it through the normal w-blend would double-advance
  per-effect runtimes (filters/delays) when 0<wet<1.
- `pad_note` param ("n:v" fire / "n:0" release) bypasses the pad_play
  gate for UIs; excluded from edit_rev like punch_pressure.
  `pad_state` reads "active:step". Blob keys pp/pr (settings only).
- Defaults: engine 0. smack_gen create() sets pad_play=1 — in a slot
  editor the firmware pads play notes into the synth, and smack-in IS
  the synth, so the stock grid becomes slice-repeat pads with zero UI.
  ui_overtake init forces 0 (overtake pads are editors; whether note
  MIDI even reaches the DSP there is unverified). smack audio_fx:
  default 0 — notes-to-fx-slot delivery UNVERIFIED on hardware.

## Next steps

- On-device: verify chain UI (LED colors on steps, pad consumption,
  playhead chase rate) — untested on hardware. Same for oversmack (LED
  queue pacing, palette pads, suspend/resume repaint, play passthrough,
  and whether standalone DSP output sums into Move's mix at sane gain).
- Pad play (v0.13.0) on-device: which note numbers Move's grid emits
  per scale/octave (mod mapping absorbs any base), velocity delivery,
  and whether notes reach audio_fx slots / the overtake DSP.
- Dual-mono mode: BUILT (v0.5.0, 2026-07-11) — see "Dual mono" section
  below. On-device verification pending like the rest.
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
