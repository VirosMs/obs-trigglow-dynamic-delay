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

## v0.3.0 (shipped — 2026-08-27)
- Real buffer **compression**: each ring slot is now MJPEG-encoded (all-intra) on capture and
  decoded on playback, vendoring FFmpeg for both directions (`obs_video_encoder_create` only feeds
  OBS's own output pipeline, and libobs exposes no public decoder API — a GPU-compute-shader path
  was also investigated and ruled out for v0.2.0, since libobs has no compute/dispatch API in its
  public graphics interface). Automatic fallback to uncompressed NV12 whenever the codec isn't
  available or a frame fails.
- Ring slots reserve RAM based on a budgeted (compressed) size rather than the full raw ceiling,
  growing only as individual frames need it. Measured live at 30s@1080p: ~2.9GB with the buffer
  active, down from ~6.3GB pre-compression.
- Windows + Linux only for now — macOS has no equivalent trusted static FFmpeg build available and
  keeps working exactly as in v0.2.0 (uncompressed).
- Still open: measuring the real compression ratio against sustained gameplay content (today's
  only data point came from a static loading scene, not representative); a slot's RAM growth is
  permanent for the session once it happens (never shrinks back until Disable()).
- `main` is now a protected branch (PR required, even for the maintainer); `develop` is where
  ongoing work happens.

## v0.4.0 (shipped — 2026-09-07)
- **Report a problem**: a native dock dialog that creates a real trigglow.com support ticket
  (category `dynamic_delay`) and attaches the current OBS log automatically — no manual log
  hunting for the user. New `HttpsPostMultipartFile()` on the existing WinHTTP client
  (`src/win-http.cpp`) for the attachment upload.
- **In-app update check**: on load, compares this repo's latest GitHub release against the running
  version (public, unauthenticated) and shows a dismissible-by-design dock notice if newer —
  silent on failure or when already current, never an error.
- **Real localization**: every user-facing string (dock, report-a-problem dialog, filter names,
  hotkey descriptions, buffer status messages) now goes through OBS's own `obs_module_text()`
  locale system instead of hardcoded Spanish literals — `data/locale/en-US.ini`/`es-ES.ini` are
  now fully translated rather than holding a single `PluginName` key each. Previously a user
  running OBS in any language other than Spanish still saw a 100% Spanish plugin.
- **Dock redesign**: card-grouped sections, a consistent semantic status-color palette, and
  side-by-side delay/quality controls.
- **RAM measured for the first time, not assumed**: a `compression check` log line
  (`src/video-delay-filter.cpp`) exposed a real ~11-15x MJPEG compression ratio on live gameplay,
  well above the 3x conservatively assumed since v0.3.0 — `kAssumedCompressionRatio` retuned to
  5.0 on that data, plus removed a per-tick encode-frame allocation. Measured live at 30s/1080p60:
  total OBS process memory went from ~2.8GB to ~2.1GB.

## v0.5.0 (shipped — 2026-10-04)
- **"Delay Ns" overlay** (logo badge + text, 40% size) shown over the delayed output while
  Active, with an on/off checkbox and a four-corner selector in the dock.
- **Single Enable/Disable button** and a **responsive dock** (scroll area + rows that stack when
  narrow).
- **Video delay by wall clock.** A production log showed the frame-count ring running 48-90
  slots/s at 60 fps (delay really ~20-38s vs audio exactly 30s). Capture is now one slot per
  1/fps cell and playback picks the slot by capture time; audio also follows timestamps.
- **Wrapper scene always holds the selected live scene**, and delay filters left enabled by an
  OBS close while Active are switched off on load.
- **Windows installer packaging fix:**
- **The Windows installer never shipped `es-ES.ini`.** Found live testing on an OBS install set to
  Spanish: the plugin appeared stuck in English regardless of OBS's language setting.
  `installers/windows/trigglow-dynamic-delay-setup.iss`'s `[Files]` section only ever listed
  `en-US.ini` — a packaging oversight predating this session, invisible since v0.4.0 first made
  `es-ES.ini` a real translation (before that it held a single placeholder key, so a missing file
  changed nothing user-visible). `obs_module_text()` falls back to the default locale whenever the
  requested language's `.ini` isn't present on disk at all, which looks identical to "this plugin
  doesn't support Spanish" from the user's side. Now ships both. This is the next release after
  v0.4.0 (currently in production) — small enough on its own that it doesn't need to wait on the
  unrelated features below.

## v0.6.0 (proposed)
- Saved delay presets (e.g. "Short delay 5s", "Long delay 30s"), selectable from the dock and from
  additional hotkeys.
- macOS/Linux **live verification** — both already build green in CI; this is about actually
  running and confirming the plugin on those platforms, not new build-system work.
- Measure the v0.4.0 RAM retune (`kAssumedCompressionRatio = 5.0`) against more sessions/games
  using the new per-slot growth counter (`encodeGrowthCount_`) before considering raising it
  further — see that constant's comment in `src/video-delay-filter.cpp`.

## v0.7.0 (paused — scoped, not started, 2026-09-08)
Goal: a "true-live" Replay Buffer (dedicated to Trigglow, saved via its own hotkey — OBS's native
"Save Replay" hotkey can't be redirected to a custom output, so this was never going to reuse it)
that keeps working while buffer mode is Active, instead of capturing the delayed wrapper scene like
the streaming output does. Investigated 2026-09-08, deliberately paused before writing any code —
see below.

- **First-pass idea, and why it's wrong.** This entry originally proposed the
  `exeldro/obs-source-record` "Branch Output" pattern verbatim: a dedicated `obs_view` +
  encoder + `"replay_buffer"` output (all public OBS API — `obs_view_create`/`obs_view_add2`,
  `obs_video_encoder_create`, `obs_output_create("replay_buffer", ...)`, source read from Source
  Record's actual implementation) pointed at the real live scene, running alongside the streaming
  output that keeps showing the delayed wrapper. That's wrong for THIS plugin specifically: filters
  are a property of the source object itself, not of whoever's rendering/reading it — verified
  directly against `libobs/obs-source.c` (`obs_source_output_audio()` runs the full filter chain via
  `filter_async_audio()` *before* `source_signal_audio_data()` reaches ANY capture callback or
  consumer; `obs_source_default_render()` works the same way for video). Since `VideoDelayFilter`/
  `AudioDelayFilter` are attached directly to the live scene's/leaf sources' own objects (nesting
  never duplicates them — see `EnsureBufferWrapperScene`'s header comment), a brand-new `obs_view` or
  encoder pointed at "the real live scene" would see the SAME already-delayed output the streaming
  side does, the moment buffer mode goes Active — exactly the state where a true-live replay would
  matter most. Source Record itself never hits this, since its whole point is capturing a source
  *with* whatever filters it has.
- **What actually fixing this would take.** `VideoDelayFilter`/`AudioDelayFilter` already capture an
  undelayed copy of every frame/audio buffer each tick, before writing it into their delay ring — the
  correct fix is having them ALSO push that same undelayed copy into a dedicated `video_output_t`/
  `audio_output_t` (`video_output_open()`/`video_output_lock_frame()`, mirrored for audio) that a new
  replay-buffer encoder+output reads from, instead of building anything as a separate, isolated
  addition. That means changing the internals of the two files this plugin's core delay guarantee
  depends on — not a self-contained new feature next to them.
- **Why paused rather than attempted now.** `docs/SPEC.md`'s own history (§3.3's v0.3.1/v0.3.2
  entries, §7's unresolved GPU crash) shows this exact pair of files has repeatedly surfaced subtle,
  only-found-live bugs even from focused, delay-specific changes. Touching their internals for a
  second, unrelated capture path risks the core "never cuts, ever" guarantee for a feature that's
  additive, not essential. Picking this back up should start from "how do we validate the existing
  delay path is unaffected," not just "add the second tap."

## Later
- **Output-level delay redesign** (studied 2026-09-08, not started). `obsdelay.com` buffers the
  already-encoded bitstream and re-sends it through its own multi-destination RTMP sender, so
  Program, scene switching, the Replay Buffer, and recording are never touched at all — with a much
  smaller memory footprint than our NV12/MJPEG frame ring, since a compressed bitstream is far
  smaller than decoded frames. Architecturally the cleanest solution to v0.7.0's goal AND to
  seamless multi-scene switching (see "Out of scope for now" below) at once, but it means replacing
  OBS's native RTMP output with a custom one (reconnection, keyframe handling, multi-destination
  sending) — multiple sessions of work and meaningfully higher risk. Only worth prioritizing if
  v0.7.0's narrower fix turns out not to cover real usage once it ships.
- **CA-issued signing for Windows/macOS** (real fix for the SmartScreen/Defender false positive
  in [#10](https://github.com/VirosMs/obs-trigglow-dynamic-delay/issues/10)) — application to
  [SignPath Foundation](https://signpath.org/apply)'s free code-signing program for qualifying
  open-source projects is in progress. Only a CA-vetted identity actually feeds SmartScreen's and
  Defender's reputation systems.
  - **Stopgap in place as of v0.3.3+**: the Windows installer is now signed with a *self-signed*
    certificate (`.github/workflows/build-project.yaml`'s "Sign Windows Installer (self-signed)"
    step, gated on the `WINDOWS_SELFSIGN_PFX_BASE64`/`WINDOWS_SELFSIGN_PFX_PASSWORD` repo secrets).
    This only proves the installer wasn't tampered with after this project's own CI produced it —
    it does **not** remove the "Unknown publisher" SmartScreen warning and does **not** stop
    Defender's `Wacatac.B!ml` false positive, since neither trusts a self-issued identity. Keep
    this step even after a real certificate lands, as a fallback for any build path that runs
    without the paid/free CA cert configured.
- A dedicated Trigglow Stream Deck plugin, in addition to the native-hotkey path (which always
  remains available as a dependency-free option) — mainly to show ON/OFF/Filling status with color
  directly on the Stream Deck button itself.

## Out of scope for now
- Per-scene delay — a *different* delay duration configured per individual scene.
- **Following the streamer's scene switches while delayed, without a refill gap** (investigated and
  rejected, 2026-09-08). A first attempt made the wrapper scene retarget to whatever scene Program
  switched to (dock-driven, then auto-following `OBS_FRONTEND_EVENT_SCENE_CHANGED`) — technically
  correct, but every switch necessarily re-enters `Filling` first, since the newly-selected scene's
  ring buffer starts empty: our video/audio filters only ever capture ONE source's frames at a time,
  so "the buffer already has history for the scene you just switched to" isn't possible without
  continuously pre-buffering every scene the user might switch to (multiplying RAM/GPU cost by scene
  count for no benefit the rest of the time). That refill is directly at odds with this plugin's core
  promise — "never cuts, ever" — so it was reverted rather than shipped as a compromise. Checked
  against the reference plugins for a way around this: `exeldro/obs-dynamic-delay`'s per-source
  filter has the identical limitation (its own issue tracker has a standing feature request for
  buffering an invisible source's parent, i.e. exactly this gap) and `ne0lines/comp-delay-for-obs`
  uses the same source/transition/delay 3-scene architecture we do, with no indication it solves
  this either. Only `obsdelay.com` avoids it, and only because it never delays a specific scene's
  frames at all — it buffers the stream's own already-composited, already-encoded bitstream
  downstream of Program, so a scene switch is just part of that one continuous signal, never a
  "different buffer with no history." That's the "Output-level delay redesign" already listed under
  Later — worth revisiting only as part of that larger redesign, not as a standalone addition to the
  current per-scene-filter architecture.
- External web panel as the main way to control it (product decision: everything lives natively
  in OBS).
