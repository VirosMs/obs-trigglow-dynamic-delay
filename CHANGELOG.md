*[Versión en español](CHANGELOG-es.md)*

# Changelog — Trigglow Dynamic Delay for OBS

## v0.5.1 — 2026-10-18 (Early Access)

Performance hotfix. With a canvas larger than the output (reported live: 2560x1440 canvas,
1920x1080 output at 60 fps), OBS showed **"Encoding overloaded"** as soon as the delay was enabled.

**Fixed / Performance:**
- **"Encoding overloaded" with the delay on when the canvas is larger than the output.** The video
  buffer was stored at canvas resolution, so with a 1440p canvas streamed at 1080p the plugin
  MJPEG-compressed and decompressed every buffered frame at 1440p inside OBS's graphics thread:
  14.3/17.4 ms p50/p95 per frame in an offline benchmark of the same pipeline, most of the
  16.7 ms a 60 fps frame has, and OBS fell behind.
  Those extra pixels were discarded by OBS's own output rescale anyway. The buffer now never
  exceeds OBS's output resolution (Settings → Video → Output (Scaled) Resolution), and together
  with the multi-threaded compression below the same setup costs ~5.2/5.9 ms p50/p95 (same
  benchmark). You can go back to a 2560x1440 canvas with a 1920x1080 output. `EnsureRingSized()`
  and the dock estimate
  both go through the new pure sizing function `bufferfit::Compute()` (`src/buffer-fit.{hpp,cpp}`,
  no libobs/FFmpeg dependency) with the canvas (`obs_video_info::base_*`) and output (`output_*`)
  geometry. The ceiling is a uniform scale, `min(1, max(outW/baseW, outH/baseH))`, which keeps
  the aspect ratio and never goes below the output on either axis; it only applies when the
  buffered target is canvas-sized (the live scene). See `docs/SPEC.md` §3.5.
- **Multi-threaded MJPEG compression.** The encoder splits each frame into slices compressed in
  parallel, up to 4 threads depending on the CPU (one per 4 logical CPUs: 4 on an 8-core/16-thread
  CPU, 1 on a 4-thread one). At 1080p the plugin's cost on OBS's graphics thread drops from
  8.6/10.4 to 5.2/5.9 ms p50/p95 per frame. `thread_type = FF_THREAD_SLICE` is set explicitly: with
  only `thread_count > 1`, FFmpeg picks frame threading for MJPEG, `receive_packet()` returns
  `EAGAIN` within the same tick and every frame would be stored raw. If FFmpeg still comes up
  frame-threaded, the encoder is reopened single-threaded with a WARN. Encode path alone at 1080p:
  5.1 → 1.8 ms p50, same total CPU, ~3.5% bigger frames.
- **Each delayed frame is decoded and uploaded once, however many views are open.** OBS renders
  the scene once per view (Program, preview, projectors, multiview), and every one of those calls
  used to decompress and upload the same delayed frame to the GPU again. Now it happens at most
  once per frame; extra views just redraw it. If a frame can't be shown (never captured, or it
  fails to decode), the last one is repeated instead of drawing black: a one-frame stutter that
  never stretches the delay. The cache is keyed by slot index + capture timestamp and invalidated
  in `ReleaseRing()`/`ReleaseSizedGpuObjects()`, so a repeated frame never comes from a previous
  cycle.
- **No freeze or RAM spike when the delay starts.** Sizing the buffer zero-filled every slot on
  OBS's graphics thread, which touched all of its memory at once: at 1440p60 that was ~1.9 GB
  resident in one go and a ~381 ms stall (about 23 dropped frames) on the first frame after Enable.
  Slots are now allocated without initializing them (`Slot::pixels` is a default-initialized
  `std::unique_ptr<uint8_t[]>`), so memory only becomes resident as frames are written: in the
  offline simulation 651-896 MB instead of 1923 MB at 1440p60, with sizing taking ~5 ms and freeing
  the buffer on Disable also faster (87 → 30 ms). The reserved (committed) memory, and the `ring`
  figure in `perf check`, don't change. Nothing reads beyond the bytes each frame wrote.
- **Faster compression on CPUs with fewer than 8 logical cores.** The MJPEG encoder now uses the
  standard JPEG Huffman tables (`huffman=default`) instead of FFmpeg's per-frame optimal ones, which
  cost a second pass over every frame. With slices FFmpeg already did this, so it changes the
  single-threaded encoder: 10-17% less encode time at 1440p for ~3% bigger frames, and the decoded
  picture is identical (Huffman coding is lossless). The `MJPEG encoder/decoder open` log line now
  says `huffman=default` (or why it couldn't be set).
- **Exact frame interval at fractional frame rates.** At 59.94 fps the capture interval was
  `1e9 / 59` ns, so almost one real frame per second was lost. It is now `1e9 × fps_den / fps_num`
  (16,683,333 ns at 59.94), used for capture, the ring span and its sizing. The effective delay
  that keeps audio in sync (`get_effective_delay_seconds`) uses rational arithmetic as well:
  multiplying by the truncated interval could floor a span of exactly 1800 frames at 60 fps (30 s)
  to 29 s.
- **Delay stuck on after closing OBS while Active (the v0.5.0 fix didn't work).** If OBS was closed
  with the delay on, the next start left the live scene delayed for good while the dock said
  "Inactive". v0.5.0 tried to switch those filters off on load, but it enumerated with
  `obs_enum_sources()`, which never returns filters, so it disabled nothing. It now walks the
  filters (`obs_source_enum_filters`) of inputs, groups, scenes and the global audio sources, with
  every filter switched off outside libobs locks. Known limitation: this runs once, when OBS
  finishes loading, not when you switch scene collection without restarting OBS.
- **Dock tooltip and filter property text for the minimum quality corrected.** They said the
  delayed video "never drops below" that resolution, which stopped being true with the output
  ceiling (see Changed).
- **References to the delay filter leaked on every Enable.** `obs_source_get_filter_by_name()`
  returns a new reference, but the plugin treated it as borrowed and never released it (video
  filter: several per Enable; audio filters: one per audio source and call), so the video filter
  was never destroyed. Every lookup now releases its reference (`OBSSourceAutoRelease`), and the
  filter's destructor, which libobs runs without a graphics context, now enters one to free its GPU
  objects.
- **A settings file with invalid JSON is no longer overwritten.** If `buffer-mode-settings.json`
  couldn't be parsed (for example after a hand edit with a missing comma), the plugin loaded the
  defaults and wrote them over the file when OBS closed, losing what was there. It is now moved
  aside as `buffer-mode-settings.json.<yyyyMMdd-HHmmss>.invalid` (copied if it can't be renamed)
  with a WARN giving both paths; if neither works, the file is left untouched at exit.

**Changed:**
- **"Minimum quality" is a floor for the RAM reduction, bounded by OBS's output.** When RAM is
  short it still decides how far the buffer may shrink (rather than go below it, fewer seconds are
  stored), but it no longer raises the buffer above the output resolution: with a 720p output the
  buffer is 720p even with a 1080p minimum. Storing more rows than OBS emits bought nothing.
- **The `compression check` log line is replaced by `perf check`** (below), which carries the same
  compression numbers.

**Added:**
- **`perf check` line in the OBS log.** Every 10 s while the delay is on (Filling and Active), plus
  a last one when the delay is disabled or OBS closes. It reports the buffer resolution and whether
  it is capped (`capped to OBS output …`, `not capped …`, `cap off …`), p50/p95/max timings per
  phase (whole render, readback, encode, decode, upload), calls/captures/uploads per second,
  repeated frames (`holds`), raw fallbacks, OBS's own rendered/lagged/skipped frame counters over
  the same window, compression ratio and ring memory. It's what support reads when the delay seems
  to cost too much; field-by-field guide in `docs/TROUBLESHOOTING.md`. Fixed 4096-sample arrays per
  phase, no allocation per frame.
- **Warning when an output encodes above the buffer.** If an active output has a video encoder
  of the main canvas above the buffer's resolution (for example a recording rescaled to canvas
  resolution with Advanced output's Rescale output, or Enhanced Broadcasting tracks above the
  output), the log warns once per encoder, size and delay cycle that its delayed video is upscaled,
  with both resolutions, the size the buffer would have without the cap and how to lift it, adding
  that lifting it costs CPU and RAM and can shorten the delay if RAM runs short (with the figure
  when the uncapped buffer wouldn't hold the full delay). Checked on Enable, when the buffer fills,
  and when streaming, recording or the Replay Buffer start; only when lifting the cap would actually
  change the buffer. Encoders of other named canvases (e.g. a vertical canvas) are recognized and
  skipped. GPU-rescaled encoders, which libobs moves to a mix it doesn't expose, are attributed to
  the main canvas when they have its aspect ratio (or the output's), and the WARN says so; a
  same-shaped second canvas could be mistaken for the main one, and a main-canvas encoder rescaled
  to another aspect ratio is not detected. Decision in `src/encoder-check.{hpp,cpp}`, details in
  `docs/SPEC.md` §3.5.
- **Hidden `buffer_output_cap` setting, for support.** `"buffer_output_cap": false` in the
  plugin's `buffer-mode-settings.json` brings back v0.5.0's way of sizing (source/canvas
  resolution, only reduced for RAM) for someone who records at canvas resolution and prefers that
  quality at the cost of CPU. Only a literal JSON `false` turns the cap off: any other value
  (`"false"` in quotes, `0`...) keeps it on and logs a WARN. No dock control on purpose; read only
  when OBS loads, and confirmed in the log with `output_cap=off`. Steps in
  `docs/TROUBLESHOOTING.md`.
- **Unit tests for the buffer sizing math** (`tests/buffer_fit_tests.cpp`, off by default:
  configure with `-DTRIGGLOW_BUILD_TESTS=ON`, or standalone with `cmake -S tests -B build_tests`
  without any OBS dependency; run with `ctest`). They cover canvas = output, 1440p → 1080p,
  4K → 1080p, output above canvas, mismatched aspect ratios, a floor above the output, RAM
  reduction, rounding, odd dimensions, degenerate input, 59.94 fps, and budget/output sweeps. A
  second test program, `tests/encoder_check_tests.cpp`, covers the encoder warning's decision: a
  canvas encoder with and without scaling, GPU-rescaled encoders above and below the buffer,
  Enhanced Broadcasting tracks, vertical canvases (720x1280, 1080x1920) and a buffer the cap
  doesn't limit.

**Build / CI:**
- **Windows FFmpeg re-pinned** to BtbN's month-end build `autobuild-2026-09-30-13-08`
  (`ffmpeg-n9.0.2-17-g2a571b6068`, same 9.0 branch and DLLs: avcodec-63, avutil-61, swresample-7).
  The previous pin, `autobuild-2026-10-03-18-14` (`n9.0.2-22`), was deleted upstream and returned
  404, which broke the Windows build at configure. BtbN deletes its daily autobuilds after about two
  weeks but keeps the month-end ones, so only month-end builds are pinned from now on
  (`buildspec.json`, `cmake/windows/buildspec.cmake`).
- **CI builds and runs the unit tests** on Ubuntu, Windows and macOS: new `unit-tests` job in
  `.github/workflows/build-project.yaml`, using the new `tests/CMakeLists.txt`, which configures on
  its own without downloading OBS, Qt or FFmpeg (`cmake -S tests -B build_tests`) and also backs the
  top-level `-DTRIGGLOW_BUILD_TESTS=ON` path. It runs on PRs and pushes, and gates the release job.
- Version bumped to 0.5.1 in `buildspec.json` and the Windows installer script.

**Known limitations:**
- **1440p output at 60 fps.** With a 2560x1440 *output* (not just canvas) the buffer is 1440p too,
  and compression still runs inside OBS's graphics thread: at 30 fps it should work, at 60 fps it
  may still overload OBS. Moving compression off the graphics thread (target 1440p60) is the plan
  for v0.6.0 — see `docs/ROADMAP.md`.
- When the RAM budget trims the ring, video is delayed by the exact (fractional) span and audio by
  whole seconds, so they can be up to ~1 s apart. Aligning them is also planned for v0.6.0.

## v0.5.0 — 2026-10-04 (Early Access)

**Added:**
- **"Delay Ns" overlay.** While the delay is Active, a small badge with the Dynamic Delay logo and
  the real delay ("Delay 30s") is drawn over the delayed output; it disappears the moment you
  disable the delay. The dock has a **"Show overlay"** checkbox and a **corner selector** (top
  left, top right, bottom left, bottom right; top left by default) so it never covers your layout.
  It is a regular OBS source inside the plugin's wrapper scene (`data/images/delay-badge.png` +
  a text source), shown at 40% size. If the RAM budget shortens the video delay, the overlay shows
  the delay actually in effect, not the requested one.

**Changed:**
- **One Enable/Disable button.** The dock now shows only the button that makes sense: **Enable**
  while inactive, **Disable** from the moment it starts filling until it is switched off.
- **Responsive dock.** The panel lives in a scroll area, so shrinking it no longer squashes
  controls on top of each other; side-by-side rows (account, delay/quality, buttons) stack into
  columns when the dock is narrow. Fixed clipped text in the delay/quality fields and tightened
  margins and paddings.

**Fixed:**
- **Video delay drifted from the real time (measured from a production log).** The video ring wrote
  one slot per `Render()` call and assumed that was exactly `fps` per second; live it ran 48-90
  slots/s at 60 fps, so a "30s" video delay was really ~20-38s while the audio was exactly 30s.
  The ring is now captured at most once per 1/fps wall-clock cell and played back by capture time,
  so video holds the configured delay and stays in sync with audio.
- **Audio delay now follows the wall clock too.** Sources that stop sending audio while idle
  (loopback/application capture, paused media, browsers) no longer stretch the audio delay: gaps
  are filled with silence and a rewound or restarted source resets its history. Stale audio is
  flushed on every Enable, the first output after warm-up fades in (no click), missing audio
  planes write silence, and the delay setting is now thread-safe.
- **The chosen live scene was ignored when the wrapper scene was reused.** After changing the live
  scene (e.g. "Talk" to "In Game") the wrapper kept showing the old one, so the delay filter never
  buffered what was on screen and the delayed output looked frozen. The wrapper now always holds
  exactly the selected live scene.
- **Delay left running after closing OBS while Active.** The filters are saved enabled in the scene
  collection; they are now switched off when OBS finishes loading if the plugin is Inactive.
- **The Windows installer never shipped `es-ES.ini`**, so Spanish OBS installs showed the plugin in
  English. It now ships both locales, plus the overlay image.

## v0.4.0 — 2026-09-07 (Early Access)

**Added:**
- **"Report a problem" button in the dock.** Opens a small native dialog (name, email, a short
  description) that creates a real trigglow.com support ticket (category `dynamic_delay`) and
  attaches the current OBS log to it automatically — the user never has to find OBS's log file
  themselves. Built on the existing `src/win-http.cpp` WinHTTP client, extended with a new
  `HttpsPostMultipartFile()` for the attachment upload; new files `src/bug-report.{hpp,cpp}`
  (locates/copies the current OBS log, Windows only for now) and
  `src/report-bug-dialog.{hpp,cpp}` (the dialog itself). New outbound network use — see
  `docs/OBS_SUBMISSION_CHECKLIST.md`'s disclosure note.
- **In-app update check.** On load, the plugin checks this repo's latest published GitHub release
  against its own version (a public, unauthenticated GET, no new trigglow.com backend endpoint
  needed) and shows a small dismissible-by-nature notice at the top of the dock if a newer version
  exists, linking straight to the release page. Silent on any failure or when already up to date —
  never surfaced as an error. New file `src/update-checker.{hpp,cpp}`. New outbound network use —
  see `docs/OBS_SUBMISSION_CHECKLIST.md`.
- **Real localization.** Every user-facing string (the dock, the report-a-problem dialog, both
  filters' names in OBS's own Filters dialog, the 3 hotkey descriptions, and the buffer's status
  messages) now goes through OBS's own locale system (`obs_module_text()`, via a new
  `src/i18n.hpp` `Str()` helper) instead of being a hardcoded Spanish literal in the C++ source.
  `data/locale/en-US.ini` and `es-ES.ini` went from a single `PluginName` key each (all that was
  ever actually used before this) to the plugin's full real translation. Previously, a user running
  OBS in English (or anything other than Spanish) still got a 100% Spanish plugin UI regardless of
  their OBS language.

**Changed:**
- **Dock redesign**: controls now group into cards (status / account / configuration) instead of
  one long column of rows, with a consistent semantic color palette (green/amber/red/blue for
  success/warning/error/accent, independent of the user's OBS theme — everything else still follows
  `palette(...)` so the dock matches whatever theme is active) and Enable/Disable colored to read at
  a glance. Delay and minimum quality now sit side by side instead of stacking. Font sizes moved
  from fixed pixels to points so the dock scales correctly with Windows display scaling.

**Fixed / Performance:**
- **Real-world RAM usage at 30s/1080p measured live for the first time** (`compression check` log
  line, `src/video-delay-filter.cpp`) instead of assumed: a full session held a steady ~11-15x MJPEG
  compression ratio on real gameplay, well above the 3x this had been conservatively assumed at
  since v0.3.0. Retuned `kAssumedCompressionRatio` from 3.0 to 5.0 on that real data — the ring's
  own memory dropped from ~1.78GB to an estimated ~1.0GB at that setting, and total OBS process
  memory in testing went from ~2.8GB to ~2.1GB. A new per-slot growth counter
  (`encodeGrowthCount_`) logs alongside the ratio so a future retune has real growth-frequency data
  to check against, not another guess.
- Removed a real per-tick allocation: `EncodeScratchNv12Into()` used to `av_frame_unref()` +
  `av_frame_get_buffer()` (a full malloc+free of the encode frame's pixel buffer) on every single
  encoded frame; the buffer is now reserved once when the codec context opens and reused via
  `av_frame_make_writable()` every tick. Doesn't change the ring's own memory (unrelated to the
  retune above), but removes heap churn/fragmentation on the encode path across a long session.

## v0.3.3 — 2026-09-02 (Early Access)

**Added:**
- Enable (dock button, hotkey, and Stream Deck) now requires having signed in to a free
  trigglow.com account once — see `docs/ACCOUNT_GATE.md`. The plugin stays 100% free; this is a
  discovery gate, not a paywall. Login happens entirely in the user's system browser (the same
  login the web app already uses), never inside OBS itself — the plugin only ever holds an opaque
  session token afterward. New outbound network use: previously none, now a handful of HTTPS calls
  to trigglow.com around sign-in (see `docs/OBS_SUBMISSION_CHECKLIST.md`'s disclosure note), made
  via a native WinHTTP client (`src/win-http.cpp`) rather than Qt's own networking — see that
  file's comment for why (Qt's TLS backend plugin is tied to the exact Qt build already loaded by
  OBS itself, which doesn't match what this plugin builds against).

**Changed:**
- New dedicated icon for the Windows installer branding (previously reused the general Trigglow
  logo) — see `installers/windows/branding/generate-assets.py`.

**Known issue:**
- The Windows installer has been flagged outright by Microsoft Defender
  (`Trojan:Win32/Wacatac.B!ml`) as a detected threat, confirmed on the official v0.3.2 release
  asset itself, not just unofficial builds — see
  [#10](https://github.com/VirosMs/obs-trigglow-dynamic-delay/issues/10). Almost certainly a false
  positive (fresh, zero-reputation binary). CI now signs the installer with a **self-signed**
  certificate as a stopgap (proves the file wasn't tampered with after this project's own build
  produced it) — this does **not** remove the SmartScreen warning or stop the Defender false
  positive, both of which need a CA-vetted identity; a SignPath Foundation application (free,
  real CA signing for qualifying open-source projects) is in progress as the actual fix.

## v0.3.2 — 2026-08-27 (Early Access)

**Fixed:**
- Audio could noticeably lag behind video on long and/or high-quality delays
  (30s+, 1080p) — reported live as "audio comes out after the video action."
  Root cause: when the RAM budget doesn't fit the full requested delay at
  the chosen quality, `VideoDelayFilter` silently shortens its own actual
  buffered duration (it always has, since before compression even existed —
  this is by design, quality always wins over duration), but
  `AudioDelayFilter` always buffers the full requested seconds exactly
  (cheap PCM, no RAM budget applied to it) — so whenever video had to
  shorten, audio kept delaying longer than video actually did, and the gap
  only grew with longer/higher-quality requests. Fixed by having
  `BufferModeController` ask the video filter what it's actually able to
  buffer (a new `get_effective_delay_seconds` query) once the buffer
  actually fills, and clamping audio's delay down to match whenever video
  had to shorten. A one-time side effect: if this correction fires, audio's
  ring resets and briefly goes silent (well under a second) right as the
  buffer switches to `Active`, while it re-fills to the corrected duration —
  a good trade for not drifting out of sync for the rest of the session.

## v0.3.1 — 2026-08-27 (Early Access)

**Fixed:**
- The dock's live/loading scene selectors came up blank on every fresh OBS
  start (and every plugin reinstall), even though the saved scene names
  were loading correctly behind the scenes — making it look like the
  plugin had "forgotten" the configuration and forcing a reselect every
  time. Root cause: `obs_module_load()` (and therefore the dock's first
  population of those dropdowns) runs before OBS has finished loading the
  scene collection, so the very first attempt to list scenes always came
  back empty. The dock now also repopulates both dropdowns once OBS
  actually finishes loading, re-selecting whatever was saved.

**Changed:**
- The Windows installer now uses real Trigglow branding — a custom
  `.exe`/taskbar icon and wizard banner/logo, generated from the same mark
  used on trigglow.virosms.com — instead of Inno Setup's generic defaults.

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
