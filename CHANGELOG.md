*[Versión en español](CHANGELOG-es.md)*

# Changelog — Trigglow Dynamic Delay for OBS

## v0.2.0 — 2026-08-26 (MVP / Early Access)

First release of the actual, shipped design. Native OBS Studio plugin (C++, official
`obsproject/obs-plugintemplate` template) that delays your stream's video and audio together,
without ever touching the streaming output — no reconnection, no cut, at any point, for any
reason.

A `0.1.0` had been published earlier (2026-08-23) describing a **reconnect-based** delay that was
fully abandoned during development in favor of the no-reconnect buffer mode below — see
`docs/SPEC.md` §0/§2 for why. `0.1.0`'s release assets predate this rewrite entirely; `0.2.0` is
the first release that actually reflects what ships.

**Included:**
- No-reconnect **buffer mode**: two OBS filters buffer video and audio in a system-RAM ring, and
  the plugin switches OBS's Program between the live scene, an optional loading scene, and a
  delayed wrapper scene internally. The streaming output's own delay setting is never touched.
- Enable / Disable / Toggle from a native OBS dock, with visible status and a live countdown while
  the buffer fills: `Inactive` / `Filling (Ns restantes)` / `Active` / `Error`.
- Video and audio delayed together, in sync, using the same configured number of seconds.
- Selectable minimum quality floor (480p/720p/1080p) for the delayed video. If the requested delay
  and quality don't both fit the RAM budget detected on the machine, the plugin shortens the actual
  buffered duration instead of ever dropping below the chosen quality.
- Live, non-blocking buffer-fit estimate and detected RAM budget shown in the dock, updated as the
  user adjusts seconds or quality — before Enable is ever pressed.
- RAM buffer freed automatically when the user presses Disable.
- 3 native OBS hotkeys (Toggle, Enable, Disable) — directly usable from Stream Deck via its own
  "System: Hotkey" action, with no dedicated Stream Deck plugin needed.
- Settings persistence per OBS profile.
- Crash-free error handling (no live scene chosen, live scene resolves to nothing, etc. → `Error`
  state with a clear dock message, never a crash).

**Known limitations (not bugs):**
- The ring buffer stores uncompressed NV12 frames — real video/audio compression (an FFmpeg-based
  encode/decode pipeline) was investigated and explicitly deferred as future work; see
  `docs/SPEC.md` §4.
- An earlier one-GPU-texture-per-buffered-frame design produced a real "Device Remove/Reset" GPU
  crash on Disable during live testing. The current RAM/NV12 rewrite uses a small, fixed pool of
  GPU objects instead and plausibly reduces this risk, but it has not yet been specifically
  re-tested for recurrence — see `docs/SPEC.md` §6.

**Out of scope in v0.2.0:** real buffer compression, saved delay presets, choosing an individual
source instead of a whole scene, a dedicated Trigglow Stream Deck plugin, signed installers,
macOS/Linux live-testing (both build green in CI but haven't been run live). See `docs/ROADMAP.md`.
