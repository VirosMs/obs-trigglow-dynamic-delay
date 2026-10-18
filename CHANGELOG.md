*[Versión en español](CHANGELOG-es.md)*

# Changelog — Trigglow Dynamic Delay for OBS

## v0.6.0 — 2026-10-18 (Early Access)

The delay no longer competes with OBS for its graphics thread, it adapts to what your computer can
do without ever cutting, and the plugin takes the look of trigglow.com. A v0.5.1 performance hotfix
was prepared but never published: everything in it ships here.

**What changes for you:**
- **No more "Encoding overloaded" as soon as the delay is on with a canvas larger than the
  output.** Reported live with a 2560x1440 canvas and a 1920x1080 output at 60 fps. The delay
  buffer is now stored at OBS's output resolution at most (Settings → Video → Output (Scaled)
  Resolution), so you can go back to your larger canvas.
- **1440p output at 60 fps is supported.** Compressing and decompressing the buffered frames no
  longer happens on OBS's graphics thread (the one that has to finish every frame of your stream in
  time) but on the plugin's own threads. At 1440p60 the plugin's work on that thread drops from
  ~14/17 ms to ~1.2–1.7/1.6–2.1 ms per frame (p50/p95, offline microbenchmark on an
  8-core/16-thread CPU).
- **If your computer can't keep up, the delay still never cuts.** The plugin first adds compression
  threads, then buffers at a lower resolution (never below your minimum quality), and only then
  captures at half the frame rate; it goes back up by itself once there's room. Meanwhile the dock
  shows an amber notice saying what was lowered and why. The delay in seconds never changes.
- **Less RAM and no freeze when the delay starts.** About 0.6–0.8 GB instead of ~1.9 GB for 30 s at
  1440p60 (offline simulation). Memory is taken as frames arrive instead of all at once, and turning
  the delay off frees it away from OBS's graphics thread, together with the codecs and the video
  memory the delay used.
- **Audio and video always share the same delay**, in whole seconds, also when RAM can't hold the
  full delay (they could be up to ~1 s apart). The dock and the live overlay show that same figure.
- **The delay seconds are locked while the delay runs**, like the scenes and the minimum quality:
  "Turn the delay off to change it."
- **New design** of the dock, the report dialog and the live overlay, aligned with trigglow.com, on
  top of your OBS theme (dark, light, high contrast), with AA contrast and visible keyboard focus.
- **Fixes**, among others: delay filters left on by closing OBS while Active are switched off at
  startup and when you switch scene collection; 59.94 fps no longer loses a frame per second;
  changing the delay while it runs shows the loading scene instead of black.

**Before you update:**
- **The objects the plugin manages are renamed from "(no tocar)" to "(auto)"**, automatically.
  **Point any Stream Deck, obs-websocket or other remote-control action that uses the old names at
  the new ones** (list below).
- **Going back to v0.5.0 or earlier after updating duplicates those objects** (see "Names" below).

**Delay buffer pipeline (technical detail):**
- **Output-resolution cap.** Up to v0.5.0 the video buffer was stored at canvas resolution, so with
  a 1440p canvas streamed at 1080p the plugin MJPEG-compressed and decompressed every buffered frame
  at 1440p inside OBS's graphics thread: 14.3/17.4 ms p50/p95 per frame in an offline benchmark of
  the same pipeline, most of the 16.7 ms a 60 fps frame has. Those extra pixels were discarded by
  OBS's own output rescale anyway. `EnsureRingSized()` and the dock estimate both go through the new
  pure sizing function `bufferfit::Compute()` (`src/buffer-fit.{hpp,cpp}`, no libobs/FFmpeg
  dependency) with the canvas (`obs_video_info::base_*`) and output (`output_*`) geometry. The
  ceiling is a uniform scale, `min(1, max(outW/baseW, outH/baseH))`, which keeps the aspect ratio
  and never goes below the output on either axis; it only applies when the buffered target is
  canvas-sized (the live scene). Buffer widths are multiples of 8 and heights even, so every I420
  plane row is a whole number of 4-byte words (OpenGL pads readback rows to 4 bytes on Linux and
  macOS). See `docs/SPEC.md` §3.5.
- **Codec worker threads** (`src/codec-pipeline.{hpp,cpp}`, pure C++ with the codec injected). The
  graphics thread now only renders the capture, converts it, reads it back (three staging slots,
  mapped two captures later so it never stalls) and uploads the frame due, only when it changes. One
  or two encode workers (`trigglow-enc-N`, each with its own MJPEG context) take frames from a
  bounded queue; one decode worker (`trigglow-dec`) decodes up to 3 frames ahead of what's on screen,
  jumps straight to the frame due when it falls behind, and starts on the oldest frames during the
  last quarter second of the fill window, so the first delayed frame is ready when it's due. A
  reaper thread (`trigglow-reap`) frees rings, frame pools and arena blocks: the graphics thread
  never frees hundreds of MB nor waits for a worker. When the encoders fall behind, the newest
  capture is dropped (the delayed view repeats a frame); the queue never grows. Measured offline at
  1440p60 (Ryzen 7 7700X, vendored FFmpeg n9.0.2): graphics thread ~1.2–1.7/1.6–2.1 ms p50/p95 per
  frame, encode ~8.8/10.4 ms and decode ~6.2/7.5 ms on their own threads (about 53% and 37% busy,
  under one core in total). See `docs/SPEC.md` §4.6.
- **Color conversion to I420 on the GPU.** The shader writes Y, U and V as three separate planes
  (BT.601, studio range), so the CPU no longer de-interleaves NV12 in either direction: about
  1.1–1.3 ms less CPU per frame at 1440p. The MJPEG frames are now labelled with the studio range
  they hold (`AVCOL_RANGE_MPEG`; metadata only, the decoded planes are identical).
- **Ring with a slot state machine and a block arena.** The ring (`src/frame-ring.*`) is a fixed
  set of slot headers, each one Free, Reserved, Encoding, Ready or Dropped, with its capture time
  assigned when the slot is reserved, so timestamps stay in order whatever order the encoders finish
  in. Payloads live in a block arena (`src/frame-arena.*`): packets packed back to back in blocks of
  4–32 MiB, never zero-filled; a block is recycled whole once the ring has overwritten every frame
  it held, and only empty blocks beyond two spares go back to the OS, on the reaper. Committed and
  resident memory follow what the frames really weigh: ~576–768 MB for 30 s at 1440p60 in the
  offline simulation, against ~1.9 GB resident and a ~381 ms stall (about 23 dropped frames) on the
  first frame after Enable when every slot was zero-filled up front. The arena never grows past the
  RAM budget: a long run of frames that compress far worse than assumed drops frames (a repeated
  frame on screen) instead of growing memory.
- **Disable frees everything the delay used.** The ring and the frame pool go to the reaper, every
  worker closes its codec, and the GPU objects are freed (~100 MB of RAM measured and ~37 MB of VRAM
  estimated at 1440p while the delay is off); reopening them on the next Enable costs about a
  millisecond.
- **Standard JPEG Huffman tables** (`huffman=default`) instead of FFmpeg's per-frame optimal ones,
  which cost a second pass over every frame: 10–17% less encode time at 1440p for 3–3.5% bigger
  frames; the decoded picture is identical (Huffman coding is lossless). The `MJPEG encoder open`
  log line says `huffman=default` (or why it couldn't be set).
- **Each delayed frame is decoded and uploaded once, however many views are open** (Program,
  preview, projectors, multiview). If the frame due isn't ready (dropped, or not decoded in time),
  the last one is repeated instead of drawing black: a one-frame stutter that never stretches the
  delay. Right after a pause in rendering, frames decoded ahead before the pause are dropped rather
  than shown late.
- **Exact frame interval at fractional frame rates.** At 59.94 fps the capture interval was
  `1e9 / 59` ns, so almost one real frame per second was lost. It is now `1e9 × fps_den / fps_num`
  (16,683,333 ns at 59.94), used for capture, the ring span and its sizing.
- **Sync mode** (the `codec_workers` kill switch, below): the codec runs on OBS's graphics thread
  as in v0.5.0, but with the slice-threaded encoder (`thread_type = FF_THREAD_SLICE`,
  `hardware_concurrency() / 4` threads, 1 to 4): 1080p went from 8.6/10.4 to 5.2/5.9 ms p50/p95 on
  that thread. `FF_THREAD_SLICE` has to be explicit: with only `thread_count > 1` FFmpeg picks frame
  threading for MJPEG, `receive_packet()` returns `EAGAIN` and every frame would be stored raw; if
  it still comes up frame-threaded, the encoder is reopened single-threaded with a WARN.

**Adaptive degradation (technical detail, `src/degradation-policy.*`):**
- Evaluated every 250 ms over the trailing 2 s, at least 2 s after the previous change. It looks at
  the plugin's own work on the graphics thread (conversion, readback and copy, upload; not rendering
  the scene, which OBS does anyway), the encode and decode time per frame, dropped captures and
  frames the decoder didn't have ready in time.
- **Down**, one step at a time, when: the graphics-thread p95 goes above 40% of the frame interval
  (or above 25% while OBS is lagging frames, or its p50 over the last 0.5 s above 50%); the encode
  p95 goes above 80% of what the encode contexts have per frame, or more than 2% of captures are
  dropped; the decode p95 goes above 80% of the capture interval, or more than 5% of the frames due
  aren't decoded in time.
- **The ladder.** Encode load adds parallelism first (encode contexts × slice threads: 1×1, 2×1,
  2×2, 2×4), then lowers the rung; decode or graphics-thread load lowers the rung directly (the
  MJPEG decoder has no threading, and what's left on the graphics thread scales with pixels). The
  rungs: the full buffer size, then steps of ×0.83 down to your minimum quality (never below it),
  then that floor captured at half the frame rate (only when the output runs at 48 fps or more).
  Every rung keeps the same ring and slot count: the delay never moves and the buffer never empties.
- **Up**, one step, only when the load predicted one step up stays under target (graphics thread
  ≤ 25% of the frame interval, workers ≤ 50%) for 30 s in a row; quality comes back before threads
  are given back. Decode metrics only count once the decoder works on frames captured at the
  current rung (it runs `delay` seconds behind the capture).
- The starting point is predicted from the resolution: 1440p60 starts with one encode context and
  one slice, 4K60 with two contexts.
- Every rung change is a WARN in the log with its cause and the last 2 s of metrics, ending in "The
  delay itself is unchanged."; thread changes are INFO lines. The dock reads the state once a second
  through the filter's `get_pipeline_state` proc, which never waits on the graphics thread.

**Fixed:**
- **Audio and video share one whole-second delay.** When RAM trimmed the ring, video was delayed
  by the exact fractional span and audio by whole seconds, so they could be up to ~1 s apart.
  `bufferfit::Compute()` now returns one whole-second effective delay, keeping the quarter-second
  margin against jitter, and video and audio both use it. The video filter's
  `get_effective_delay_seconds` computes it from the filter's current settings on the calling
  thread (no hop to the graphics thread), so audio gets the new value the moment the dock changes a
  setting, from Enable on. The dock's status card shows that same figure as the live overlay: with
  60 s requested and 35 s fitting, the chip reads "35 / 60 s" in amber and the detail line says
  only 35 of the 60 s fit in RAM.
- **Changing the delay while it runs no longer shows black.** A change of the delay or the minimum
  quality that resizes the buffer throws the buffered frames away. The filter now signals it
  (`buffer_restarted`) and Program goes back to the loading scene, with audio re-synced, while the
  buffer refills, instead of a blank delayed view for `delay` seconds. Same after a reset of OBS's
  video settings. With no loading scene set, Program stays on the (blank) wrapper scene, never on
  the undelayed live scene. The dock now locks those fields while the delay runs, so in practice
  this covers changes made from the filter's own properties and video resets.
- **A 0 s delay** (only possible from the filter's own properties; the dock's minimum is 1) passes
  the video through undelayed, with no buffer and no codec. Before, it encoded every frame into a
  one-slot ring and never drew it: black output at full CPU cost.
- **Delay stuck on after closing OBS while Active.** If OBS was closed with the delay on, the next
  start left the live scene delayed for good while the dock said "Inactive". v0.5.0 tried to switch
  those filters off on load, but it enumerated with `obs_enum_sources()`, which never returns
  filters, so it disabled nothing. It now walks the filters (`obs_source_enum_filters`) of inputs,
  groups, scenes and the global audio sources, with every filter switched off outside libobs locks,
  when OBS finishes loading and also when you switch to another scene collection (both only while
  the delay is off).
- **References to the delay filter leaked on every Enable.** `obs_source_get_filter_by_name()`
  returns a new reference, but the plugin treated it as borrowed and never released it, so the video
  filter was never destroyed. Every lookup now releases its reference (`OBSSourceAutoRelease`), and
  the filter's destructor, which libobs runs without a graphics context, now enters one to free its
  GPU objects (after joining the codec threads).
- **A settings file with invalid JSON is no longer overwritten.** If `buffer-mode-settings.json`
  couldn't be parsed (for example after a hand edit with a missing comma), the plugin loaded the
  defaults and wrote them over the file when OBS closed. It is now moved aside as
  `buffer-mode-settings.json.<yyyyMMdd-HHmmss>.invalid` (copied if it can't be renamed) with a WARN
  giving both paths; if neither works, the file is left untouched at exit.
- **Dock tooltip and filter property text for the minimum quality corrected.** They said the
  delayed video "never drops below" that resolution, which stopped being true with the output cap.

**Changed:**
- **"Minimum quality" is a floor for the RAM reduction and for the degradation, bounded by OBS's
  output.** When RAM is short it still decides how far the buffer may shrink (rather than go below
  it, fewer seconds are stored), and the degradation never goes below it either; but it no longer
  raises the buffer above the output resolution: with a 720p output the buffer is 720p even with a
  1080p minimum.
- **The `compression check` log line is replaced by `perf check` and `pipeline check`** (below).

**Changed / Dock:**
- **Brand look on top of your OBS theme.** Cards, fields and backgrounds still follow the active OBS
  theme (dark, light, Windows high contrast); the brand comes in through square cards with
  hairlines, Archivo headlines and Geist Mono labels and figures (bundled, see Packaging), SVG icons
  instead of text glyphs, and the cyan-to-magenta gradient only on the main action, the checked box
  and the Active state. Colors are derived from the theme's palette in one place
  (`src/ui-tokens.*`, `src/ui-theme.*`): every text, placeholders included, reaches WCAG AA (4.5:1)
  and every field border and focus ring 3:1 on the palettes of OBS 31's themes (Yami and its Grey,
  Light, Classic, Acri and Rachni variants), checked by `tests/ui_tokens_tests.cpp`. Keyboard focus
  is visible on every control, and the dock's scroll area is no longer an invisible first tab stop.
- **Status card**: icon, headline and a seconds chip per state, never color alone. Preparing delay
  shows a progress bar with "12 / 30 s" and "18 s left"; Delay on is cyan with a shield (the green
  is gone, the web has no green); errors get a red rule and an alert icon. Delay on shows the
  seconds viewers really get, the same as the live overlay (see Fixed).
- **"Buffer quality lowered" notice** at the bottom of the status card while the buffer runs below
  full quality: a sliders icon and the title in amber, then what was lowered (resolution against the
  full size, capture fps, or both), why (OBS's graphics thread under heavy load, compressing or
  decompressing the frames) and that the delay is unchanged; repeated and dropped frames per minute
  follow when above zero. It goes away as soon as the buffer is back at full quality, and nothing is
  polled while the delay is off. Screen readers hear it appear and go.
- **Locked while the delay runs:** the live and loading scene pickers, the delay seconds and the
  minimum quality. The seconds used to stay editable while Active and applied on every keystroke,
  resizing the buffer under a live stream. Locked fields lose their arrows, take the card's
  background and show a lock; a line underneath, "Turn the delay off to change it.", is also their
  accessible description; they are out of the tab order until the delay is off, and their value
  stays readable (muted text, AA in every theme).
- **Enable delay explains itself**: while it's disabled, a checklist shows whether the free account
  and the live scene are missing.
- **Live scene picker**: nothing is preselected ("Choose a scene…") until you pick a scene, so the
  first one can be chosen too and the field never shows a scene the checklist says is missing; a
  saved scene that no longer exists counts as not chosen. The plugin's helper scene is no longer
  offered as a live or loading scene.
- **Sign-in failures are no longer silent**: the account card says when trigglow.com can't be
  reached, the link expired, the browser didn't open (with **Copy link**) or the session ended, and
  the wait can be cancelled.
- The mouse wheel over a scene, seconds or quality control no longer changes it unless it has focus;
  it scrolls the dock instead. The seconds box draws its own arrows, so themes without spin box
  styling (OBS's System theme) no longer show two dots.
- Numbers follow OBS's language (`15.910 MB`, `41,2 s` in Spanish), resolutions use `×`, units a
  no-break space (`30 s`). The narrow dock (below 300 px) stacks the account button and the footer.
- **Report a problem**: header, labels above their fields, errors under the field with focus moved
  to it, network errors in a banner, and an attachment row that says whether the OBS log goes with
  the ticket. Raw transport errors stay in the OBS log. It opens 460 px wide and can be narrowed to
  340 px: Name and Email stack below 400 px and the browser note moves above the buttons; the
  email's help line sits under the email field.
- **Texts**: English and Spanish microcopy rewritten (Spanish accents, no `--`, no glyphs, shorter
  tooltips). In Spanish the hotkeys read "Trigglow: Activar/desactivar Dynamic Delay", "Trigglow:
  Activar Dynamic Delay" and "Trigglow: Desactivar Dynamic Delay"; existing hotkey bindings are
  kept.

**Changed / Live overlay:**
- The "DELAY 30 s" overlay is now **one image the plugin renders itself** (ink plate, brand rule,
  Dynamic Delay mark, Archivo), written to the plugin's config folder (`overlay/delay-badge-30s.png`)
  and shown by a single image source. It looks the same on Windows, macOS and Linux (the old text
  used Segoe UI, Windows only) and moves in one piece. It sits 2.2% of the canvas's short side away
  from the chosen corner and keeps its size relative to that side: the same as before on landscape
  canvases (200x58 at 1080p), and on a 1080x1920 vertical canvas the 1080p size instead of a third
  of the frame's width. It shows the delay really in effect.
- **Move or resize it in OBS and it stays that way.** The plugin places it in the chosen corner the
  first time and again only when you pick another corner in the dock (or when the canvas size
  changes, as long as you haven't moved or resized it). An existing image source already called
  "Trigglow Delay Badge (auto)" is reused instead of adding "... 2" copies.
- The old text source ("Trigglow Delay Overlay Text (auto, no tocar)") is removed automatically.
  Its text can no longer be edited: a custom font or color applied to it is lost; the image can be
  moved or scaled and keeps that.

**Changed / Names of the objects the plugin manages:**
- "(no tocar)" is gone from every language: the scene is now **Trigglow Delay Buffer (auto)**, the
  filters **Trigglow Buffer Mode Delay (auto)** and `Trigglow Audio Delay (auto) - <source>`, the
  overlay **Trigglow Delay Badge (auto)**. Existing collections are renamed once, in place, when they
  load (position, filters and settings kept); if a new name is already taken the old one stays and
  keeps working.
- **Anything that refers to these objects by name has to be updated**: a Stream Deck,
  obs-websocket or other remote-control action that switches to the old scene name, or toggles the
  old filters or overlay by name, must be pointed at the "(auto)" names.
- **Going back to v0.5.0 or earlier after the rename duplicates them**: the older version doesn't
  know the "(auto)" names, so it creates its "(no tocar)" helper scene, delay filters and overlay
  again next to them (it switches the "(auto)" filters off when the collection loads, so they stay
  inert). Remove the set you don't use; back on 0.6.0, the "(auto)" set is used and the leftover
  "(no tocar)" one isn't renamed while its new name is taken.

**Added (support and diagnostics):**
- **`perf check` line in the OBS log**, every 10 s while the delay is on (Filling and Active), plus
  a last one when the delay is disabled, set to 0 s, or OBS closes. It reports the buffer resolution
  and whether it is capped (`capped to OBS output …`, `not capped …`, `cap off …`), p50/p95/max of
  the whole render call, of the plugin's own part of it (`own`), of the readback and of the upload,
  calls/captures/uploads per second, repeated frames (`holds`, split into late decode, dropped and
  still encoding) and OBS's own rendered/lagged/skipped frame counters over the same window. Fixed
  4096-sample arrays per phase, no allocation per frame.
- **`pipeline check` line** right after it: mode (`workers` or `sync`), degradation rung, capture
  size and fps, encode contexts × slice threads, decode-ahead and staging depth, encode and decode
  p50/p95/max with how busy each thread was, drops (reserve, backpressure, arena, capture), raw
  fallbacks, codec failures, stale encodes, decoder jumps, stale decoded frames, encode queue depth,
  frames decoded ahead, compression ratio, arena memory (committed, live, peak, blocks, limit) and
  slot states. Each cycle also starts with a `delay pipeline: …` line with the configuration chosen
  and the degradation ladder. Field-by-field guide in `docs/TROUBLESHOOTING.md`.
- **Warning when an output encodes above the buffer.** If an active output has a video encoder of
  the main canvas above the buffer's resolution (for example a recording rescaled to canvas
  resolution with Advanced output's Rescale output, or Enhanced Broadcasting tracks above the
  output), the log warns once per encoder, size and delay cycle that its delayed video is upscaled,
  with both resolutions, the size the buffer would have without the cap and how to lift it, adding
  that lifting it costs CPU and RAM and can shorten the delay if RAM runs short (with the figure when
  the uncapped buffer wouldn't hold the full delay). Checked on Enable, when the buffer fills, and
  when streaming, recording or the Replay Buffer start; only when lifting the cap would actually
  change the buffer. Encoders of other named canvases (e.g. a vertical canvas) are recognized and
  skipped. GPU-rescaled encoders, which libobs moves to a mix it doesn't expose, are attributed to
  the main canvas when they have its aspect ratio (or the output's), and the WARN says so; a
  same-shaped second canvas could be mistaken for the main one, and a main-canvas encoder rescaled
  to another aspect ratio is not detected. Decision in `src/encoder-check.{hpp,cpp}`, details in
  `docs/SPEC.md` §3.5.
- **Hidden `buffer_output_cap` setting, for support.** `"buffer_output_cap": false` in the plugin's
  `buffer-mode-settings.json` brings back v0.5.0's way of sizing (source/canvas resolution, only
  reduced for RAM) for someone who records at canvas resolution and prefers that quality at the
  cost of CPU and RAM. Confirmed in the log with `output_cap=off`.
- **Hidden `codec_workers` kill switch, for support.** `"codec_workers": false` in the same file
  runs encode and decode on OBS's graphics thread again, with no codec worker threads (the reaper
  still frees memory off that thread, and the degradation still runs, adding slice threads first).
  It exists to rule the worker threads in or out of a problem; it brings back the graphics-thread
  cost. Confirmed with `codec_workers=off` in the `settings loaded` and `enabled` lines and
  `mode=sync` in `pipeline check`.
- Both hidden settings have no dock control on purpose, are read only when OBS starts, and must be
  edited with OBS closed (OBS rewrites the file on exit). Only a literal JSON `false` turns one off:
  any other value (`"false"` in quotes, `0`...) keeps it on and logs a WARN. Steps in
  `docs/TROUBLESHOOTING.md`.

**Packaging / Build / CI:**
- New `data/fonts/` (Archivo SemiBold/Bold, Geist Mono Medium subsets, SIL OFL 1.1 with their
  `OFL-*.txt`) and `data/licenses/` (Lucide icons, ISC), both in the installer, which now fails to
  build without them. If they go missing from an install, the plugin falls back to OBS's UI font
  and logs it once. The plugin now links `Qt6::Svg`.
- **Windows FFmpeg re-pinned** to BtbN's month-end build `autobuild-2026-09-30-13-08`
  (`ffmpeg-n9.0.2-17-g2a571b6068`, same 9.0 branch and DLLs: avcodec-63, avutil-61, swresample-7).
  The previous pin, `autobuild-2026-10-03-18-14` (`n9.0.2-22`), was deleted upstream and returned
  404, which broke the Windows build at configure. BtbN deletes its daily autobuilds after about two
  weeks but keeps the month-end ones, so only month-end builds are pinned from now on
  (`buildspec.json`, `cmake/windows/buildspec.cmake`).
- **CI builds and runs the unit tests** on Ubuntu, Windows and macOS: new `unit-tests` job in
  `.github/workflows/build-project.yaml`, using `tests/CMakeLists.txt`, which configures on its own
  without downloading OBS, Qt or FFmpeg (`cmake -S tests -B build_tests`) and also backs the
  top-level `-DTRIGGLOW_BUILD_TESTS=ON` path. It runs on PRs and pushes, and gates the release job.
- **ThreadSanitizer job** (`unit-tests-tsan`, Ubuntu, GCC): builds `frame_arena_tests`,
  `frame_ring_tests` and `pipeline_stress_tests` with `-fsanitize=thread` and runs them. For now it
  only reports (warning annotation and job summary) instead of blocking the release, since TSAN can
  fail without a race in this code (uninstrumented libstdc++, its slowdown tripping the stress
  test's timing checks); the build step does block.
- Version 0.6.0 in `buildspec.json` and the Windows installer script.

**Tests** (9 suites, no OBS needed; `ctest`):
- `buffer_fit_tests` (sizing: canvas = output, 1440p → 1080p, 4K → 1080p, output above canvas,
  mismatched aspect ratios, a floor above the output, RAM reduction, rounding, widths multiple of
  8, odd dimensions, degenerate input, 59.94 fps, the whole-second effective delay, budget/output
  sweeps), `encoder_check_tests` (the encoder WARN's decision), `ui_tokens_tests` (contrast on OBS
  31's palettes), `auto_names_tests` (names and their migration), `overlay_layout_tests` (overlay
  placement), `frame_arena_tests`, `frame_ring_tests` (slot state machine, generations, age-based
  selection), `degradation_policy_tests` (ladder, every trigger, cooldown, 30 s hysteresis, no
  oscillation) and `pipeline_stress_tests` (ring + arena + codec pipeline under random detaches,
  resizes, rung and rate changes: the delay never shrinks, frames are never torn or shown out of
  order, and no ring is ever freed on the simulated graphics thread).

**Known limitations:**
- **Changing the delay from the filter's own properties** (instead of the dock) while it runs: the
  buffer restarts behind the loading scene, but audio and the dock keep the dock's delay, and only
  follow when the filter's value is lower. Change the delay from the dock, with the delay off.
- Captures are deduplicated by a wall-clock cell per frame interval, not by OBS's frame time, so
  heavy render jitter can occasionally cost a captured frame (a repeated frame, never a shift of the
  delay). Planned next.
- While the buffer fills, keeping the live scene rendering still uses a canvas-sized render target
  whose content is never used (GPU only). Planned next.
- With a canvas larger than the output, the delayed frame is drawn back at canvas size and OBS
  scales it down again, which can look slightly softer than the live picture. If you don't need the
  larger canvas, setting canvas = output avoids it.
- macOS still has no FFmpeg build: the buffer is stored uncompressed (raw I420) there.

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
