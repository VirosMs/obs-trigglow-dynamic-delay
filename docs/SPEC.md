*[Versión en español](SPEC-es.md)*

# Technical specification — Trigglow Dynamic Delay for OBS

Status: **MVP / v0.5.1 — Early Access**
Last updated: 2026-10-18 (v0.5.1: §3.3, §3.5, §4.2–§4.5, §8; rewritten from scratch on 2026-08-26 —
see §0)
Plugin machine name: `obs-trigglow-dynamic-delay`
Website: https://www.trigglow.com/dynamic-delay

---

## 0. Why this document was rewritten

The original v0.1.0 spec (dated 2026-08-23, preserved in git history) described a **reconnect-based**
delay: change the value, and if you're already live, OBS briefly reconnects the stream to apply it.
That design was built, then abandoned entirely on 2026-08-24 after live testing made the reconnection
itself feel unacceptable for real streaming use, in favor of **buffer mode**: a delay that never
touches the streaming output, ever, at any point. Buffer mode is what v0.2.0 actually ships today.
The old spec kept describing the abandoned mechanism (including labeling buffer mode "a v2/v3
candidate, not fit for a reliable MVP") straight through the entire buffer-mode development — this
document replaces it with what's actually in the codebase.

## 1. Product summary

Trigglow Dynamic Delay is a native OBS Studio plugin (C++, CMake, official
[`obsproject/obs-plugintemplate`](https://github.com/obsproject/obs-plugintemplate)) that delays your
stream's video and audio by a configurable number of seconds — with a button, a native OBS hotkey, or
a Stream Deck button mapped to that hotkey — **without the streaming output ever being touched**. No
reconnection, no cut, at any point, for any reason. No external app, no web panel, no separate
process: everything lives inside the OBS process.

## 2. Why reconnection was rejected

Before building anything, we verified OBS's real behavior by reading `libobs` source directly (not
just documentation, which is incomplete on this point), cross-checked against the OBS forum thread
["how to change stream delay without restarting OBS while live"](https://obsproject.com/forum/threads/how-to-change-stream-delay-without-restarting-obs-while-live.194645/)
and the still-unresolved feature request
["Allow adding/removing Output Delay while live"](https://ideas.obsproject.com/posts/541/allow-adding-removing-output-delay-while-live).

**Key finding (verified in `libobs/obs-output.c`, `hook_data_capture()`):**

```c
if (output->delay_sec) {
    output->active_delay_ns = (uint64_t)output->delay_sec * 1000000000ULL;
    ...
    os_atomic_set_bool(&output->delay_active, true);
}
```

`obs_output_set_delay(output, delay_sec, flags)` is a plain setter: it stores `delay_sec`/`flags` on
the output struct but **never touches the buffer already in flight**. The value is only read and
converted into the real delay (`active_delay_ns`) **once, at the moment the output starts** —
`hook_data_capture()` runs on stream start, not on every setter call. This confirms, from source code
rather than forum folklore:

> **OBS locks the delay value in the instant the stream starts. Calling `obs_output_set_delay()`
> while the stream is already active does not change what's happening right now — it only "arms" for
> the next time the output starts.** This is a `libobs` architectural constraint, not something fixable
> from the Frontend API or from any plugin — the same limitation OBS's own native delay setting has.

An earlier version of this plugin (v0.1.0's first build, since abandoned) worked around this by
calling `obs_frontend_streaming_stop()` + an automatic restart to re-arm the value — a real, working
approach, but one that causes a brief, visible reconnection on the destination platform every time the
delay changes while live. Live testing made clear that reconnecting on every change wasn't an
acceptable user experience for a "streamer can react instantly" tool, so it was dropped entirely
in favor of buffer mode below. (The `delay-controller.*`/`hotkeys.*` reconnect-mode code is still in
this repo, dormant — see §6 — in case a lower-resource opt-in mode is ever wanted later, but it is
not instantiated by `plugin-main.cpp` and ships in no current build.)

## 3. How buffer mode actually works

Buffer mode never calls `obs_output_set_delay()` or touches the streaming output at all. Instead, it
delays what OBS is *showing on Program*, using two OBS filters plus a small state machine:

### 3.1. `VideoDelayFilter` — video

A `OBS_SOURCE_VIDEO`-only OBS filter, attached to a hidden **wrapper scene** that contains the user's
chosen live scene. Every `video_render` call:

1. Renders the live scene into one reusable RGBA capture render target.
2. Converts that RGBA frame to **NV12** (Y + half-resolution, box-averaged UV planes, BT.601) via a
   small custom shader compiled once with `gs_effect_create()` — libobs has no public compute/dispatch
   API at all (checked directly against `graphics.h`), so an ordinary two-pass pixel shader is the only
   public mechanism for this conversion.
3. Kicks off an **async GPU→CPU readback** into one of 2 rotating staging-surface pairs
   (`gs_stage_texture` + a *deferred* `gs_stagesurface_map` one frame later, so the CPU thread never
   stalls waiting on a copy that just started).
4. Copies the finished Y/UV bytes into the current write slot of a plain **system-RAM ring buffer**
   (`std::vector<Slot>`, `Slot::pixels` — NV12, 1.5 bytes/pixel, not RGBA's 4).
5. Uploads whichever slot is `delaySeconds` old into 2 small reusable playback textures and draws it
   via the same shader's reverse (NV12→RGBA) technique.

Only a **small, fixed number of GPU objects** exist regardless of how many seconds are buffered — one
capture render target, the two conversion render targets, 2 staging-surface pairs, 2 playback
textures. This replaced an earlier design (one persistent GPU texture per buffered frame) after live
feedback questioned whether that much VRAM was really required, and after a real "Device Remove/Reset"
game crash on Disable raised the cost of holding dozens of large GPU textures alive at once — not
conclusively proven to be caused by that design, but plausibly reduced by moving to a fixed, small GPU
footprint (see §7).

### 3.2. `AudioDelayFilter` — audio

Delaying audio needed a **different attachment point**, found live: `obs_source_filter_add()`
(`obs-source.c`) silently refuses to attach ANY filter requesting `OBS_SOURCE_AUDIO` to a **scene** —
`obs-scene.c`'s own registration proves scenes only ever advertise `OBS_SOURCE_VIDEO` in their own
`output_flags` (a scene's audio goes through a separate `audio_render` callback, never exposed to the
`filter_compatible()` check `obs_source_filter_add()` runs). This isn't fixable by renaming things or
keep-alive tricks — it's a hard `libobs` constraint.

So `AudioDelayFilter` (an `OBS_SOURCE_AUDIO`-only filter) is instead attached to **each individual
audio-capable leaf source** inside the live scene (mic, desktop audio, etc.), one instance per source,
each running the same delay-window ring-buffer logic as video, in sample space (`ring_[channel][frame]`).
Video and audio use the same `delaySeconds`, so both land in sync.

### 3.3. `BufferModeController` — the state machine

States: `Inactive → Filling → Active`, with `Error` reachable from any state. On **Enable()**:

1. Ensures a hidden wrapper scene exists, nesting the chosen live scene.
2. Attaches/enables `VideoDelayFilter` on the wrapper scene and `AudioDelayFilter` on the live scene's
   audio-capable leaf sources.
3. Forces the live scene to keep rendering in the background during the fill window
   (`obs_source_inc_showing`/`dec_showing`) — a source that isn't on Program/Preview normally renders
   nothing at all, so without this the ring buffer would just sit empty for the whole wait. (Audio
   doesn't need this: a leaf source's own audio pipeline runs continuously regardless of Program
   visibility — only video rendering needed forcing.)
4. Switches Program to the optional loading scene (or leaves the live scene up if none was chosen).
5. Once the dock's own fill timer reaches `delaySeconds`, switches Program to the wrapper scene, which
   is now showing the delayed buffered content — **an internal scene switch only**, the streaming
   output never sees this happen.

On **Disable()**: switches Program back to whatever scene was showing before Enable, disables both
filters, and frees the RAM ring (see §3.4) — reverting to instant, non-delayed, non-buffered behavior.

**Important side effect of switching Program:** step 5 calls `obs_frontend_set_current_scene()` — the
exact same function OBS's own scene list uses. This means buffer mode delays *everything that watches
Program*, not just the streaming output: local recording and the Replay Buffer, if either is set to
capture Program (the common/default setup), will also show the loading scene and then the delayed
content while buffer mode is Active, not the true live feed. This is a real difference from the
abandoned reconnect design (§2), which only affected the streaming output's own delay and left
Program/recording untouched. Not currently configurable — a streamer who wants an un-delayed local
recording alongside a delayed stream isn't served as-is.

**v0.3.1: restoring the dock's scene selectors on a fresh OBS start.** `LoadSettings()` (called from
`plugin-main.cpp`'s `obs_module_load()`) restores `liveSceneName`/`loadingSceneName` from disk
correctly on every OBS start, but `TrigglowDelayDock`'s constructor — which runs immediately
afterward, still inside `obs_module_load()` — populates its scene combo boxes by calling
`ListAvailableScenes()` right then. Found live, 2026-08-27: OBS hasn't finished loading the scene
collection yet at that point (module loading happens before scene collection loading), so that very
first population always came back empty, leaving both dropdowns blank even though the underlying
settings were fine — indistinguishable, from the user's side, from the plugin having forgotten the
configuration. Fixed by finally wiring up `ObsFrontendBridge`'s existing (previously orphaned — see
§2, `DelayController` used to own it) `FrontendEvent::FinishedLoading` callback:
`BufferModeController` now subscribes to it in its constructor and exposes
`SetSceneListRefreshCallback()`, which the dock uses to repopulate both combos — using the same
already-correct `status_.liveSceneName`/`loadingSceneName` — once OBS actually finishes loading.

**v0.3.2: keeping audio from drifting behind a shortened video delay.** §3.5 below explains that
`VideoDelayFilter` shortens its own ACTUAL buffered duration (never the quality floor) whenever the
RAM budget doesn't fit the full requested `delaySeconds` at the chosen quality — this has been true
since before compression even existed. `AudioDelayFilter`, however, always buffers exactly the full
requested seconds (cheap PCM, no RAM budget check at all) — so whenever video had to shorten, audio
kept delaying the untouched, longer duration, and the two drifted apart by exactly the shortfall.
Reported live, 2026-08-27, on long/high-quality requests (30s+, 1080p) as "audio comes out after the
video action." Fixed with a new cross-thread query, `get_effective_delay_seconds` (a `proc_handler`
on the video filter's `obs_source_t`, following the same `obs_queue_task(OBS_TASK_GRAPHICS, ...,
wait=true)` pattern `release_buffers` already used — `ring_`/`currentFps_` are only safe to read from
the graphics thread): `ObsFrontendBridge::GetVideoEffectiveDelaySeconds()` calls it, and
`BufferModeController::SyncAudioDelayToVideoEffective()` clamps audio's delay down to whatever that
reports whenever it's smaller than the requested value. Called from `Enable()` (a no-op there, since
the ring isn't sized yet), `OnFillTimerElapsed()` (the real correction point — by then the ring has
definitely been sized), and any live delay/quality change while `Active`. One accepted side effect:
`AudioDelayFilter::EnsureRingSized` resets that ring's history whenever its configured seconds
actually changes, so a correction at `OnFillTimerElapsed()` causes a brief (well under a second)
silence right as Program switches to the delayed wrapper scene, while audio's ring re-fills to the
now-matching duration — judged a clearly better trade than staying out of sync for the rest of the
session. (Known limit, still open in v0.5.1: the query reports the ring's span floored to whole
seconds, while `Render()` caps the video delay at the unfloored span, so when the RAM budget trims
the ring video and audio can be up to ~1s apart. With the full delay in the ring both are exactly
the requested delay. Aligning them is v0.6.0 work. Since v0.5.1 the span is computed with rational
arithmetic at the exact `fps_num/fps_den` — multiplying by a truncated frame interval could floor a
1800-frame span at 60fps to 29s.)

**v0.5.1: switching off delay filters left on by a previous session.** The filters are saved
enabled in the scene collection when OBS closes while the delay is Active, so the next start would
delay the live scene for good while the dock showed `Inactive`. v0.5.0 added
`ObsFrontendBridge::DisableAllDelayFilters()` on `FrontendEvent::FinishedLoading` (only while the
controller is `Inactive`), but it enumerated with `obs_enum_sources()`, which never returns filters
(libobs 31.1.1: non-private inputs and groups only), so it disabled nothing. It now collects every
source that can carry one of our filters — inputs and groups (`obs_enum_sources`), the main canvas's
scenes (`obs_enum_scenes`), and the global audio sources on the output channels
(`obs_get_output_source`) — walks their filters with `obs_source_enum_filters`, and only then
disables the matching ones, outside every libobs lock (`obs_source_set_enabled()` emits a signal
whose handlers may take those locks themselves). Known limitation: it runs on `FinishedLoading`
only, not when the user switches scene collection without restarting OBS.

### 3.4. Freeing the RAM ring on Disable

OBS bypasses a **disabled** filter's `video_render` entirely — `VideoDelayFilter::Render()` never
runs again once disabled, so nothing would ever shrink/free its RAM ring on its own. `VideoDelayFilter`
registers a `"release_buffers"` proc handler on its own `obs_source_t` (`obs_source_get_proc_handler`);
`ObsFrontendBridge::SetBufferFilterEnabled(false)` calls it. Since that call can happen from any thread
(in practice, the Qt dock's button-click thread) while the ring is only ever touched from the
graphics/video thread, the actual `ring_.clear()` is queued onto `OBS_TASK_GRAPHICS` via
`obs_queue_task(..., wait=true)` rather than done in place — it runs strictly after any Render() call
still in flight, with no lock needed.

### 3.5. Buffer sizing: output ceiling, then quality floor, then duration

The user picks two things, both fully free choices, never restricted:

- **Delay (seconds)** — 1 to 60 in the dock's spinner (a UI sanity cap; the real limit is the RAM
  math below).
- **Minimum quality** — 480p / 720p / 1080p. The floor for the RAM reduction below: rather than store
  the delayed video below this height, the plugin stores fewer seconds. Since v0.5.1 it never raises
  the buffer above OBS's output resolution, though (rule 1 below): with a 720p output the buffer is
  720p even with a 1080p minimum, since storing more rows than OBS emits buys nothing.

`EnsureRingSized()` computes a **RAM budget** from the machine's total system RAM (`GlobalMemoryStatusEx`
on Windows; unimplemented on other platforms, in which case a conservative fallback is used) — 50% of
total RAM, clamped between a 1GB floor and a 24GB ceiling.

Since v0.5.1 the sizing itself is a pure function, `bufferfit::Compute()` (`src/buffer-fit.hpp`/`.cpp`,
free of libobs and FFmpeg so it can be unit-tested), called with the same input by `EnsureRingSized()`
(which actually resizes the ring) and by `VideoDelayFilter::EstimateBufferFit()` (the dock's
estimate), so what the dock predicts is what happens. Its input is the target's size, the canvas
(`obs_video_info::base_*`) and output (`output_*`) geometry, the exact `fps_num/fps_den`, the delay,
the minimum quality, the RAM budget and the assumed compression ratio (§4.3). It applies three rules,
in order:

1. **Output ceiling (v0.5.1).** The buffer never holds more pixels than OBS's main mix outputs
   (Settings → Video → Output (Scaled) Resolution). Anything above it is discarded by OBS's own output
   rescale, but it cost the plugin CPU: MJPEG encode/decode (§4.2) scale roughly linearly with pixels
   and run on OBS's graphics thread. A user with a 2560x1440 canvas and a 1920x1080 output at 60fps
   got "Encoding overloaded" from OBS with v0.5.0, which buffered at the canvas size; benchmarked
   offline with the same FFmpeg build and pipeline, a 2560x1440 buffer costs 14.3/17.4ms p50/p95 of
   encode + decode per frame (single-threaded encoder). The ceiling is a uniform scale,
   `capScale = min(1, max(outW/baseW, outH/baseH))`: it keeps the canvas aspect ratio and never goes
   below the output on either axis (erring towards quality when canvas and output aspect ratios
   differ). It only applies when the target is canvas-sized (the normal live-scene case) and the
   geometry is known; any other target is left uncapped, the conservative choice.
2. **Quality floor.** If the full delay doesn't fit the budget at the ceiling, the scale is reduced,
   but never below `minResolutionHeight` rows — and that floor never exceeds the ceiling. The
   reduction is computed against the continuous cost, then walked down whole source rows until the
   rounded frame really fits, so a RAM-reduced ring still holds the full delay and the result is
   monotonic in budget and output size. Dimensions are rounded with `llround` and forced even (NV12
   chroma subsampling), with a floor of 2.
3. **Duration last.** If even the floor doesn't fit, the ring gets fewer frames: the plugin shortens
   the **actual buffered duration** instead of dropping below the floor.

The frame count uses the exact rate: `ceil(delay × fps_num / fps_den)` plus a quarter second of slack
(capture jitter must never leave the oldest slot younger than the delay) plus one slot, so 30s at
60000/1001 needs 1815 slots rather than a truncated 59fps count. The capture cell is
`1e9 × fps_den / fps_num` ns (16,683,333ns at 59.94fps); before v0.5.1 it was `1e9 / 59` there, which
missed almost one real frame per second.

The dock shows a live, non-blocking estimate of what a given (seconds, quality) combination will
actually achieve — duration and buffer resolution, cap included — *before* the user presses Enable —
"aconsejar según el hardware, pero a su elección" (advise based on hardware, but the choice is always
theirs): this estimate never blocks or clamps the user's picks, it only warns. `EnsureRingSized()`
logs `buffering at WxH (capped to OBS output WxH, canvas WxH)` whenever the ceiling applies, and a
WARN if the ring has to be rebuilt while it holds frames (OBS video settings, delay or quality
changed while the delay is on — the ring then refills from empty).

**Escape hatch: `buffer_output_cap` (v0.5.1).** A hidden plugin setting: `"buffer_output_cap":
false` in `buffer-mode-settings.json` (the plugin's config folder from `obs_module_get_config_path()`;
on Windows `%APPDATA%\obs-studio\plugin_config\obs-trigglow-dynamic-delay\`) drops rule 1 and sizes
like v0.5.0: source resolution, only reduced for RAM, never below the minimum quality. Not
byte-for-byte the same, though: the exact frame rate and the quarter second of slack rounded up give
one more slot than v0.5.0 at 25/30/50fps, and when RAM runs short the row-by-row reduction of rule 2
can land up to 2px away from v0.5.0's size. It exists for support — for example someone recording at
canvas resolution who prefers that quality at the cost of CPU — so it has no filter property and no
dock control. `plugin-main.cpp` reads it only at load, strictly: only a literal JSON `false` turns the
cap off (checked with `obs_data_item_gettype() == OBS_DATA_BOOLEAN`); absent, `true` or `null`
(libobs drops it on load) keep it on, and any other type — `"false"` as a string, `0` — keeps it on
with a WARN. A settings file that exists but isn't valid JSON is renamed (or, failing that, copied)
to `buffer-mode-settings.json.<yyyyMMdd-HHmmss>.invalid` with a WARN naming both paths before the
defaults are used, so the save at exit never replaces it silently; if neither works, the file is left
untouched at exit. `BufferModeController` pushes it to the filter on every `Enable()` through
`ObsFrontendBridge::SetBufferFilterOutputCap()`, so the settings file always wins over whatever the
filter saved with the scene collection, and the dock estimate uses it too. `SaveBufferSettings()`
rewrites the file from scratch at OBS exit and writes the key back only when it is `false` (everyone
else's file stays as it was), which is also why it must be edited with OBS closed. The
`settings loaded` and `enabled` log lines report `output_cap=on|off`, and the sizing log and the
`perf check` line (§4.5) say when the cap is off. User-facing steps: `docs/TROUBLESHOOTING.md`.

**Encoders above the buffer (v0.5.1).** With the ceiling on, an output whose video encoder runs above
the buffer — a recording rescaled to the canvas, or an Enhanced Broadcasting track above the output —
gets upscaled delayed video. `BufferModeController::WarnAboutEncodersAboveBuffer()` logs one WARN per
encoder, size and delay cycle (an encoder restarted at another size is warned about again), naming
the encoder and output, both resolutions, the uncapped size and the `buffer_output_cap` switch, and
adding that lifting the cap costs CPU and RAM and can shorten the delay if RAM runs short (with the
actual figure when the uncapped buffer wouldn't hold the full delay). It checks every video encoder
slot (`MAX_OUTPUT_VIDEO_ENCODERS`) of every active output, and only when lifting the cap would
actually buffer more. It runs on the UI thread only — at `Enable()`, when the buffer fills, and on
the streaming / recording / replay buffer "started" frontend events — never from the graphics
thread, since `obs_enum_outputs()` holds `outputs_mutex`.

Which encoders see the main canvas (`ObsFrontendBridge::ListActiveVideoEncoders()`; the decision is
the pure `src/encoder-check.*`): in libobs 31.1.1 an encoder renders the `video_t` it was given and
`obs_encoder_parent_video()` returns it, but a GPU-rescaled encoder (Settings → Output → Advanced →
Rescale output, and every Enhanced Broadcasting track, which also sets a preferred format) is moved
at start to an encoder-only mix of the same view at the scaled size (`maybe_set_up_gpu_rescale()` in
`obs-encoder.c`), a `video_t` that no public API maps back to its canvas. So:

- `obs_get_video()` → the main canvas (exact).
- the video of another named canvas (`obs_enum_canvases()` + `obs_canvas_get_video()`, e.g. a
  vertical canvas) → not the main canvas (exact).
- any other mix → the main canvas if the encoder's size has the canvas's or the output's aspect
  ratio (2% tolerance, enough for OBS's even-rounded sizes such as 852x480), otherwise not. The WARN
  says when it relied on this.

Limits of that heuristic: a GPU-rescaled encoder of another canvas, private canvas or plugin view
with the main canvas's aspect ratio (a second 16:9 canvas) is taken for the main canvas — a WARN
that doesn't apply — and a main-canvas encoder rescaled to another aspect ratio (stretched) is missed.
A vertical canvas (9:16) on a 16:9 main canvas is always told apart.

**Tests (v0.5.1).** `tests/buffer_fit_tests.cpp` exercises `bufferfit::Compute()` without OBS:
canvas = output, 1440p → 1080p, 4K → 1080p, output above canvas, mismatched aspect ratios, a floor
above the output, RAM reduction in range, an active floor, rounding, odd dimensions, degenerate
input, a non-canvas target, zero delay, 59.94fps, the cap switched off, and budget/output sweeps.
`tests/encoder_check_tests.cpp` exercises the encoder WARN's decision (`encodercheck`), with the
buffers sized by `bufferfit::Compute()` cap on and off: aspect-ratio matching, a canvas encoder not
scaled and CPU-scaled, GPU-rescaled encoders above/equal/below the buffer, Enhanced Broadcasting
tracks, vertical canvases (720x1280, 1080x1920) and a vertical main canvas, and a buffer the cap
doesn't limit (cap off, output = canvas, RAM-limited). Off by default, so plugin packaging is
unchanged. Two ways to build them (`tests/CMakeLists.txt`):

```
# From the plugin's own build (same compiler flags as the plugin):
cmake --preset <preset> -DTRIGGLOW_BUILD_TESTS=ON
cmake --build <dir> --target buffer_fit_tests encoder_check_tests --config RelWithDebInfo
ctest --test-dir <dir> -C RelWithDebInfo --output-on-failure

# Standalone, without downloading any OBS/Qt/FFmpeg dependency:
cmake -S tests -B build_tests
cmake --build build_tests --config RelWithDebInfo
ctest --test-dir build_tests -C RelWithDebInfo --output-on-failure
```

CI's `unit-tests` job (`.github/workflows/build-project.yaml`) runs the standalone path on Ubuntu,
Windows and macOS, and gates the release job.

## 4. v0.3.0: real MJPEG compression of the RAM ring

§3.5's RAM budget math above still assumes an uncompressed NV12 ring for §3's core mechanism —
that was accurate for v0.2.0, but as of v0.3.0 each ring slot (`VideoDelayFilter::Slot`,
`src/video-delay-filter.hpp`) is additionally MJPEG-compressed on capture and decompressed on
playback, on the platforms where that's available. This section describes that pipeline; §3's
description of the capture/playback loop and the fixed GPU object pool is otherwise unchanged.

**Platform scope:** only where `TRIGGLOW_HAVE_FFMPEG` is defined — Windows and Linux, via a vendored,
pinned-release BtbN LGPL-shared FFmpeg build (`avcodec`/`avutil`/`swresample`; never "latest", to keep
builds reproducible). No equivalent trusted static FFmpeg build exists for macOS yet, so macOS isn't
part of this: it keeps working exactly as it did in v0.2.0, uncompressed. Everywhere the codec isn't
available (macOS today, or a build without FFmpeg linked), every code path below falls back to the
pre-v0.3.0 raw-NV12 behavior automatically — compression is an addition on top of the existing
mechanism, never a new requirement for it to work.

### 4.1. Why MJPEG, all-intra

The ring's core invariant (§3.1, §3.4) is that any slot can be independently overwritten, evicted, or
read by plain index arithmetic at any moment — `EnsureRingSized()` can resize/clear the whole ring
abruptly on a resolution or delay change, and playback reads whichever slot is `delaySeconds` old with
no notion of "the frame before it" ever entering into that. A real predictive/GOP codec (P/B frames
referencing prior frames, keyframe-seek logic, a decode order distinct from display order) carries
exactly the kind of frame-to-frame state-machine assumptions that don't map onto that: decoding slot
N would require also holding onto whatever slot N depended on, even after the ring has moved on or
resized past it. MJPEG sidesteps this by construction — every frame is encoded and decoded
independently (all-intra, no inter-frame prediction at all), so a slot's compressed bytes are
self-contained and bitstream-independent, exactly matching the ring's "any slot, any time" contract.
The tradeoff is a worse compression ratio than a real GOP structure would achieve; that's accepted
as the cost of fitting this specific architecture rather than a general-purpose delivery codec.

### 4.2. Encode/decode flow

The capture step in `VideoDelayFilter::Render()` is unchanged through the async GPU→CPU readback
(§3.1, steps 1–3): the harvested Y/UV bytes for the current tick land in `scratchNv12_`, one frame's
worth of tightly-packed raw NV12, reused every tick as scratch space. From there:

- **Encode (capture):** `EncodeScratchNv12Into()` de-interleaves `scratchNv12_`'s NV12 UV plane into
  two separate, tightly-packed U and V scratch buffers (`encodeScratchU_`/`encodeScratchV_`) — MJPEG's
  encoder wants planar 4:2:0, and NV12 is semi-planar (interleaved UV), a different memory layout.
  Those planes are copied into a reused `AVFrame` (`encodeFrame_`, `AV_PIX_FMT_YUV420P` with
  `AVCOL_RANGE_JPEG`) and pushed through `encoderCtx_`, the MJPEG `AVCodecContext` opened by
  `EnsureCodecContextsOpen()` at the ring's current `bufferWidth_`/`bufferHeight_`, fixed quality via
  `AV_CODEC_FLAG_QSCALE` (`qmin`/`qmax` both set to `kMjpegQuality = 5` — a commonly-cited MJPEG sweet
  spot, visually close to lossless while still compressing meaningfully; not yet a dock setting). The
  resulting packet's bytes are copied into the destination `Slot`'s `pixels`, with `usedBytes` set to
  the packet size and `compressed = true`.
- **Decode (playback):** `DecodeSlotIntoScratchNv12()` reverses this — feeds the slot's MJPEG packet
  (`src.compressed` must be true) through `decoderCtx_` and writes the decoded planes back into
  `scratchNv12_` as plain NV12 bytes, ready for the existing `gs_texture_set_image()` playback upload
  (§3.1, step 5) exactly as if that slot had never been compressed at all.
- **Fallback:** either function returns `false` (destination untouched) if the relevant codec context
  isn't open — not found in the linked FFmpeg, or `avcodec_open2()` failed at this resolution — or a
  specific frame genuinely fails to encode/decode. Callers then store/read `scratchNv12_` raw, exactly
  the same code path platforms without `TRIGGLOW_HAVE_FFMPEG` always use. Compression is therefore
  never a hard requirement for the ring to work at all — only ever a RAM optimization on top of it.

**Where this runs, and what it costs (v0.5.1).** Both encode and decode run synchronously inside
`Render()`, on OBS's graphics thread — the thread that has to finish every frame within 1/fps
(16.7ms at 60fps). Their cost scales roughly with the buffer's pixel count, which is why v0.5.1 caps
the buffer at the output resolution (§3.5, rule 1) and adds the changes below. Benchmarked offline
with the same FFmpeg build (encode + decode per frame, p50/p95): 1080p from 8.6/10.4ms (v0.5.0) to
5.2/5.9ms; a 1440p canvas with a 1080p output from 14.3/17.4ms to ~5.2/5.9ms. A 1440p *output* is
still buffered at 1440p: expected to be fine at 30fps, not yet at 60fps. Moving encode/decode onto
worker threads, off the graphics thread (target 1440p60), is the next step — see
`docs/ROADMAP.md`, v0.6.0.

- **Slice-threaded encoder (v0.5.1).** `EnsureCodecContextsOpen()` opens the MJPEG encoder with
  `thread_type = FF_THREAD_SLICE` and `thread_count = clamp(hardware_concurrency() / 4, 1,
  kMaxEncodeSliceThreads = 4)` — 4 on an 8-core/16-thread CPU, 1 on a 4-thread one — so each frame's
  slices are encoded in parallel and the wall time on the graphics thread drops. `thread_type` must be
  set explicitly: with only `thread_count > 1`, FFmpeg picks frame threading for MJPEG (`QSCALE` skips
  its single-thread exception), `receive_packet()` then returns `EAGAIN` within the same tick, and
  every frame would be stored raw. If `active_thread_type` still reports frame threading the encoder
  is reopened single-threaded (WARN), and if opening with slices fails it retries with one thread.
  Measured with the vendored n9.0.2: the 1080p encode path goes from 5.1 to 1.8ms p50 with 4 slices,
  same total CPU, ~3.5% bigger packets. The decoder stays at `thread_count = 1` (n9.0's MJPEG decoder
  has no threading). The `MJPEG encoder/decoder open` log line reports the thread counts, the logical
  CPUs and `active_thread_type`.
- **Standard Huffman tables (v0.5.1).** The encoder is opened with `huffman=default`
  (`av_opt_set()` on its private options) instead of FFmpeg's default "optimal" tables, which need a
  second pass per frame. With more than one slice FFmpeg already uses the standard tables, so this
  changes the single-threaded encoder (fewer than 8 logical CPUs, or the single-threaded reopen):
  10-17% less encode time (p50-p95, 1440p, vendored n9.0.2) for ~3% bigger packets. Huffman coding
  is lossless, so the decoded frames are identical, and every packet still carries its tables (DHT),
  so the decoder is unchanged. The result is checked and shown in the open log line
  (`huffman=default`, or why it couldn't be set).
- **Decode and upload each slot at most once (v0.5.1).** `Render()` runs once per view of the scene
  (Program, preview, projectors, multiview) and used to decode and `gs_texture_set_image()` the due
  slot in every one of those calls. The filter now remembers which slot (index + capture timestamp)
  an upload was last attempted for; later calls for the same slot only draw the existing playback
  textures, and a slot that failed to decode isn't retried on every call. If the due slot never got a
  frame or fails to decode, the last frame uploaded in this cycle is drawn again (a "hold") instead of
  drawing nothing; the next slot is still picked by age, so a hold is a one-frame stutter, never a
  drift in the delay. While the ring is still filling nothing is drawn, as before. The cache is
  invalidated in `ReleaseRing()` (Disable, ring resize) and `ReleaseSizedGpuObjects()` (resolution
  change, destructor), so a hold can never show a frame from a previous cycle or a destroyed texture.

### 4.3. Budgeted allocation, grow-only

Compressing the bytes stored inside a slot only pays off in practice if the slot's own memory
allocation shrinks too — otherwise a smaller `usedBytes` inside a still-full-size `pixels` vector is a
paper win, not a real one. `EnsureRingSized()` now allocates each slot's `pixels` at a **budgeted**
size — `frameBytes / kAssumedCompressionRatio` — instead of the full raw NV12 ceiling.
`kAssumedCompressionRatio` is a conservative, deliberately-chosen-up-front constant. It was `3.0` on
`TRIGGLOW_HAVE_FFMPEG` platforms when this was introduced (chosen conservative because OBS capture
content is disproportionately full of exactly what compresses worst — sharp UI/HUD edges, on-screen
text — even though MJPEG on natural video often does much better), and has been `5.0` since v0.4.0
(see the retune note below); it is `1.0` (no assumed compression at all) everywhere else, which keeps
that platform's allocation identical to pre-v0.3.0 behavior.

`EncodeScratchNv12Into()` and the raw-storage fallback both grow a slot's `pixels` past its budget
(`Slot::EnsureCapacity()`) whenever one particular frame genuinely needs more room than assumed —
hard-to-compress content, or the codec unavailable/failed for that frame. That growth is
**permanent** for the rest of the slot's lifetime: capacity is never shrunk back down, to avoid
paying a realloc+memcpy on every single frame. This is a deliberate one-way ratchet, not a bug —
worst case, a slot that consistently compresses worse than assumed eventually grows to the full raw
size (identical to storing everything uncompressed), and the only way to release that growth is
releasing the whole ring
(`Disable()`, or a resolution change that reallocates `ring_` from scratch).

**No zero fill (v0.5.1).** Slots used to be `std::vector<uint8_t>` sized with
`assign(budgetedBytes, 0)`, which touches every page of the ring on the graphics thread the moment it
is sized: resident memory jumped to the whole ring at once (~1.9GB at 1440p60) and the first
`Render()` after `Enable()` stalled for ~381ms. `Slot::pixels` is now a default-initialized
`std::unique_ptr<uint8_t[]>` with an explicit `capacity`, so a page only becomes resident when a frame
is written to it: resident memory follows the real packet sizes (651-896MB instead of 1923MB at
1440p60 in the offline simulation) and sizing takes ~5ms. The commit charge doesn't change. This
relies on nothing ever reading past `usedBytes`: the decoder copies exactly `usedBytes` (checked
against `capacity`), and a raw slot is only uploaded if it holds a whole NV12 frame.

**v0.4.0 retune.** `kAssumedCompressionRatio` is `5.0` on `TRIGGLOW_HAVE_FFMPEG` platforms since
v0.4.0, after the first real measurement (a live 30s/1080p60 gameplay session held a steady ~11-15x
ratio, and no slot ever grew past the 3.0 budget). Still conservative on purpose: it comes from one
session and one game, and a slot that needs more simply grows. See `CHANGELOG.md`, v0.4.0, and the
constant's comment in `src/video-delay-filter.cpp`.

### 4.4. Measured result, and what's still open

Real, live measurement — 30 seconds of buffered 1080p60 video, buffer `Active` — showed total RAM
usage for the ring going from **~6.3GB** (before the budgeted-allocation change, i.e. compressing the
bytes but still reserving the full raw ceiling per slot) to **~2.9GB** (after it) on the same machine,
the same session: a real **~2.2x** reduction, measured directly rather than assumed.

Left open: the actual per-frame MJPEG compression ratio has **not** been measured against sustained
real gameplay content yet. The one ratio logged so far (`loggedFirstEncode_`, first successful encode
after `EnsureCodecContextsOpen()`) came from a mostly-static loading-scene frame and is explicitly not
representative of real content — it should not be read as a typical or expected number. The honest
statement is that the real compression ratio depends heavily on what's on screen and is still an open
measurement, not that `kAssumedCompressionRatio`'s value at the time (`3.0`, or any other specific
number) is what gameplay actually achieves.

*Update:* v0.4.0's periodic `compression check` line produced the first real gameplay measurement
(~11-15x, see §4.3's retune note), and since v0.5.1 those numbers are part of the `perf check` line
(§4.5), measured on every user's session. It's still one data point per session and content type,
not a guarantee.

### 4.5. v0.5.1: `perf check` telemetry

So a performance report can be checked from a user's log rather than guessed, `VideoDelayFilter`
writes one INFO line every 10 seconds of rendering (`kPerfLogIntervalNs`), during `Filling` and
`Active`. It replaces the 300-frame `compression check` line and includes its numbers:

```
perf check (periodic, 10.0s): buf=1920x1080 (capped to OBS output 1920x1080, canvas 2560x1440)
fps=60.00 | render p50/p95/max=…ms readback=… encode=… decode=… upload=… | calls=…/s captures=…/s
uploads=…/s holds=N rawFallback=N | obs frames+N lagged+N skipped+N | ratio=…x (cycle …x)
ring=…MB/N slots growths=N
```

(one line in the log; wrapped here.)

- **Geometry:** the buffer resolution and why — `capped to OBS output WxH, canvas WxH`, `not capped,
  source WxH, output WxH`, or `cap off (buffer_output_cap=false), …` — with `, reduced for RAM` when
  §3.5's rule 2 kicked in; plus the exact fps.
- **Timings:** per-phase wall time (`os_gettime_ns`) for the whole `Render()` call, the staging
  readback, the encode (or the raw copy), the decode and the texture upload; p50/p95 through
  `std::nth_element` over a fixed 4096-sample array per phase, max exact. No allocation per frame or
  per window.
- **Counters:** `Render()` calls, captured cells, uploads, holds (§4.2), raw fallbacks (encode
  failed), this window's and the whole cycle's compression ratio, ring memory (allocated, i.e.
  committed; since v0.5.1 the resident part is lower, see §4.3) and slot count, slot growths (§4.3),
  and the deltas of OBS's own counters over the same window — total frames
  (`obs_get_total_frames`), lagged frames (rendering lag, `obs_get_lagged_frames`) and skipped frames
  (encoding lag, `video_output_get_skipped_frames`).
- **Windows:** a pause in `Render()` calls longer than 1s (filter hidden, OBS stalled) closes the
  current window early (`resumed after a pause`), so its per-second rates aren't diluted. The last
  partial window is logged when the delay is disabled (`filter disabled`, from the `release_buffers`
  task) and when the filter is destroyed with the delay on, e.g. OBS closing (`filter destroyed`;
  safe in the destructor because OBS destroys a source only after detaching it, and the line reads
  only the filter's own members). Before v0.5.1 the filter was in practice never destroyed:
  `ObsFrontendBridge` leaked a reference to it on every `Enable()` (`obs_source_get_filter_by_name()`
  returns a new reference); every lookup now holds it in an `OBSSourceAutoRelease`, and the
  destructor, which libobs runs on its destruction thread, enters the graphics context to free the
  GPU objects. Windows shorter than 1s are dropped.

How to read each field from the user's side: `docs/TROUBLESHOOTING.md`, "Encoding overloaded".

## 5. Scope of v0.2.0, as actually shipped

Included:

- Enable / Disable / Toggle, with the streaming output **never** touched.
- Live scene (required) + optional loading scene shown while the buffer fills.
- Selectable minimum quality (480p/720p/1080p); requested duration shortens instead of ever lowering it.
- Live buffer-fit estimate in the dock, with a non-blocking warning if the full requested duration
  won't fit the detected RAM budget at the chosen quality.
- Video and audio delayed together, in sync.
- RAM buffer freed automatically when the user presses Disable.
- Visible state with a live countdown while filling: `Inactive` / `Filling (Ns restantes)` / `Active`
  / `Error`.
- 3 native OBS hotkeys (Toggle mandatory, Enable/Disable optional) — directly usable from Stream Deck
  via its own "System: Hotkey" action, no dedicated Stream Deck plugin needed.
- Per-OBS-profile settings persistence.
- Crash-free error handling (no live scene chosen, live scene resolves to nothing, etc. → `Error`
  state with a clear dock message, never a crash).

Explicitly out of scope for v0.2.0 (see `docs/ROADMAP.md`):

- Real video/audio **compression** — the ring buffer stores uncompressed NV12 today. A real
  encode/decode pipeline was investigated (vendoring FFmpeg for both encode AND decode, since
  `obs_video_encoder_create` can only feed OBS's own output pipeline and libobs has no public decoder
  API at all) and explicitly deferred as multi-session future work. A GPU-compute-shader path was also
  investigated and ruled out — libobs exposes no compute/dispatch API in its public graphics API,
  confirmed by an exhaustive `graphics.h` review. (Video compression on Windows/Linux is what v0.3.0
  actually adds — see §4; audio compression remains out of scope.)
- Saved delay presets.
- Choosing an individual source instead of a whole scene as "live".
- A dedicated Trigglow Stream Deck plugin (the native-hotkey path already works today).
- Signed installers (the current Windows installer is unsigned — Early Access).
- macOS/Linux **live-tested** (both already build green in CI; neither has been run live yet).

## 6. Legacy reconnect-mode code

`delay-controller.*` and the reconnect-specific hotkey wiring from §2's abandoned first design are
still present in this repo, unused — `plugin-main.cpp` no longer instantiates `DelayController` or
wires its hotkeys/dock. Kept rather than deleted in case a lower-resource, opt-in reconnect mode is
wanted later (buffer mode trades RAM for zero reconnection; some users on very constrained hardware
might prefer the old trade-off) — not a promise that it will return, just not thrown away.

## 7. Known risk, not yet confirmed resolved

A real "Device Remove/Reset" GPU crash was observed live when pressing Disable, under the earlier
one-GPU-texture-per-buffered-frame design. It was never conclusively root-caused to this plugin before
the RAM/NV12 rewrite landed (§3.1) — the rewrite plausibly reduces the risk (a handful of small, fixed
GPU objects instead of dozens of large ones scaling with delay length), but this is not the same as a
confirmed fix. Treat it as a real, live-testable risk area rather than a resolved issue until it's been
specifically re-tested for recurrence.

## 8. Architecture

```
plugin-main.cpp              → obs_module_load/unload, wiring for all components
buffer-mode-controller.*      → the state machine described in §3.3 (Inactive/Filling/Active/Error)
obs-frontend-bridge.*         → the only layer that touches obs-frontend-api.h + filter/scene wiring
video-delay-filter.*          → §3.1 -- the RAM/NV12 ring buffer and its small fixed GPU object pool
                                (§4 -- v0.3.0 MJPEG compression, Windows/Linux only;
                                §4.5 -- v0.5.1 perf check telemetry)
buffer-fit.*                  → §3.5 -- pure ring sizing math (output ceiling, quality floor, RAM
                                budget, exact fps), no libobs/FFmpeg dependency (v0.5.1)
encoder-check.*               → §3.5 -- pure decision behind the "encoder above the buffer" WARN
                                (which encoders see the main canvas), no libobs dependency (v0.5.1)
audio-delay-filter.*          → §3.2 -- per-leaf-source audio delay ring
hardware-info.*               → total system RAM query (replaces the earlier VRAM/DXGI query)
scene-combo-box.*             → dock combo box populated from obs_frontend_get_scene_names()
settings-ui.*                 → the Qt dock (live/loading scene, seconds, quality, fit estimate,
                                Enable/Disable, live fill countdown)
hotkeys.*                     → registration of the 3 native OBS hotkeys, wired to
                                BufferModeController's Enable()/Disable()/Toggle()
logging.*                     → logging wrapper with a component prefix
delay-controller.*            → §6 -- legacy reconnect-mode logic, present but unused
tests/buffer_fit_tests.cpp    → §3.5 -- unit tests for buffer-fit.*, built only with
                                -DTRIGGLOW_BUILD_TESTS=ON (v0.5.1)
tests/encoder_check_tests.cpp → §3.5 -- unit tests for encoder-check.*, same (v0.5.1)
```

Design principle: **`BufferModeController` is the only owner of state**. The dock and the hotkeys both
call the same public methods (`Enable()`, `Disable()`, `Toggle()`) — there is no separate code path for
"button" vs. "hotkey", which halves the state-sync bug surface.

## 9. License and repository

This project is built on the official `obsproject/obs-plugintemplate`, distributed under
**GPL-2.0-or-later** (see `LICENSE`). This isn't a branding choice: **`libobs`, the library any OBS
plugin links against, is itself GPL-2.0**, and the extended consensus in the OBS community is that any
plugin linking against it is bound by that license. In practice, this means that if you distribute a
compiled binary of this plugin, the GPL requires making the corresponding source code available to
whoever receives that binary — "free but closed-source" isn't a combination this license allows. (This
isn't legal advice — for certainty about your specific situation, a real legal review is worth it
before large-scale distribution.)

**How this project handles it:** Trigglow Dynamic Delay's code is published in its **own, separate
repository**, dedicated only to this plugin — **not** in Trigglow's main repository (where the rest of
the product lives: backend, database, web frontend), which stays private. Publishing only the plugin
in its own repo satisfies the GPL without exposing the rest of the product. The "Source code / GitHub"
button on `/dynamic-delay` on the website points at this dedicated repository, not at Trigglow's main
one.
