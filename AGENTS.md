---
status: active
last_touched: 2026-07-18
deploy: scripts/deploy.sh
---

# Smack

Schwung Shadow UI looper and slice-FX module for Ableton Move. It ships as
the `smack` audio effect, `smack-in` input generator, and `oversmack`
full-surface tool. All three share `src/smack_core.c`.

## Work safely

- Run `make test` for the native core simulator after DSP or state changes.
- Run the same test with AddressSanitizer and UBSan before release.
- `make arm` stages and cross-compiles all module archives with Docker.
- `scripts/deploy.sh` writes to Move hardware. Do not run it without explicit
  deployment authorization.
- Keep trigger params inert for zero/`idle` values; autosave restores those.
- Preserve the audio callback's non-allocating design.
- The audio-FX wrapper must continue exporting `move_audio_fx_on_midi` for
  chain-host clock delivery.

See `CLAUDE.md` for the current architecture, UI protocol, hardware findings,
release process, and on-device verification backlog.
