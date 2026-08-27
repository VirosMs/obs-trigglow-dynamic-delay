*[Versión en español](CHANGELOG-es.md)*

# Changelog — Trigglow Dynamic Delay for OBS

## v0.3.0 — 2026-08-27 (Early Access)

Real buffer compression. The ring buffer now encodes each frame with MJPEG (all-intra, matching
the ring's abrupt resets and index-based reads) before storing it, and decodes it back on
playback — cutting real RAM usage on top of the v0.2.0 NV12 storage, without lowering the quality
floor you choose.

**Included:**
- FFmpeg (avcodec/avutil) vendored and linked on Windows + Linux; each ring slot is MJPEG-encoded
  on capture and decoded on playback.
- Automatic, safe fallback to uncompressed NV12 (identical to v0.2.0) whenever the codec can't
  open or a specific frame fails to encode/decode — compression is never a hard requirement for
  the buffer to work.
- Ring slots now reserve RAM based on a budgeted (compressed) size instead of the full raw
  ceiling, growing a given slot only when a specific frame genuinely needs more room. Measured
  live at 30s@1080p: **~2.9GB total with the buffer active, down from ~6.3GB** before this
  release — real gameplay content compresses somewhat worse than the assumed ratio, so this
  varies with what's on screen.
- macOS is not part of this release yet — no equivalent trusted static FFmpeg build is available
  there today (see `docs/ROADMAP.md`); the plugin keeps working exactly as in v0.2.0 on macOS,
  uncompressed.

**Known limitations (not bugs):**
- The real compression ratio hasn't been measured against sustained real gameplay yet — the one
  number logged so far (very high) came from a mostly-static loading scene and isn't
  representative. RAM usage in practice may be higher or lower than the ~2.9GB measured here
  depending on content.
- A slot that once needed more room than its budgeted allocation keeps that larger capacity for
  the rest of the session (by design, to avoid a reallocation every single frame) — RAM can only
  grow during a session, never shrink back down, until Disable() releases the whole buffer.

**Repo changes:** `main` is now a protected branch (pull request required, even for the
maintainer) — development happens on `develop`, merged via PR. See `README.md` if you're
contributing.

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
  `docs/SPEC.md` §5 (video compression shipped in v0.3.0 below — see `docs/SPEC.md` §4 — audio
  compression remains deferred).
- An earlier one-GPU-texture-per-buffered-frame design produced a real "Device Remove/Reset" GPU
  crash on Disable during live testing. The current RAM/NV12 rewrite uses a small, fixed pool of
  GPU objects instead and plausibly reduces this risk, but it has not yet been specifically
  re-tested for recurrence — see `docs/SPEC.md` §7.

**Out of scope in v0.2.0:** real buffer compression, saved delay presets, choosing an individual
source instead of a whole scene, a dedicated Trigglow Stream Deck plugin, signed installers,
macOS/Linux live-testing (both build green in CI but haven't been run live). See `docs/ROADMAP.md`.
