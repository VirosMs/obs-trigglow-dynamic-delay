*[Versión en español](ROADMAP-es.md)*

# Roadmap — Trigglow Dynamic Delay

## v0.2.0 (MVP — this release)
- No-reconnect **buffer mode**: the streaming output is never touched, at any point, for any
  reason — the plugin delays video and audio by buffering them in system RAM and switching OBS's
  Program internally between the live scene, an optional loading scene, and a delayed wrapper
  scene.
- Enable / Disable / Toggle Delay, with the live scene required and the loading scene optional.
- Selectable minimum quality (480p/720p/1080p); requested duration shortens instead of ever
  lowering it, if the RAM budget doesn't fit both.
- Live buffer-fit estimate and detected RAM budget shown in the dock, updated as the user adjusts
  seconds or quality, with a non-blocking warning before Enable if the full request won't fit.
- Video and audio delayed together, in sync.
- RAM buffer freed automatically on Disable.
- `Inactive` / `Filling` (with a live countdown) / `Active` / `Error` status visible in a native
  OBS dock.
- 3 native OBS hotkeys (Toggle mandatory, Enable/Disable optional), directly usable from Stream
  Deck via its own "System: Hotkey" action.
- Settings persistence per OBS profile.
- Crash-free error handling (no live scene chosen, live scene resolves to nothing, etc.).
- Windows as the priority platform; macOS/Linux build green in CI but haven't been run live yet.

## v0.3.0 (proposed)
- Real buffer **compression**. The ring currently stores uncompressed NV12 frames; this investigates
  an FFmpeg-based encode/decode pipeline (vendoring FFmpeg for both directions, since
  `obs_video_encoder_create` only feeds OBS's own output pipeline and libobs exposes no public
  decoder API). A GPU-compute-shader path was already investigated and ruled out for v0.2.0 —
  libobs has no compute/dispatch API in its public graphics interface — so this is squarely a CPU
  encode/decode effort. Expected to be multi-session work given the vendoring involved.

## v0.4.0 (proposed)
- Saved delay presets (e.g. "Short delay 5s", "Long delay 30s"), selectable from the dock and from
  additional hotkeys.
- macOS/Linux **live verification** — both already build green in CI; this is about actually
  running and confirming the plugin on those platforms, not new build-system work.

## Later
- Signed installers for Windows/macOS (the current Windows installer is unsigned — Early Access).
- A dedicated Trigglow Stream Deck plugin, in addition to the native-hotkey path (which always
  remains available as a dependency-free option) — mainly to show ON/OFF/Filling status with color
  directly on the Stream Deck button itself.

## Out of scope for now
- Per-scene delay.
- Delay for the Replay Buffer or local recording as a separate control from streaming (both follow
  the same Program output as the stream today — see `docs/FAQ.md`).
- External web panel as the main way to control it (product decision: everything lives natively
  in OBS).
