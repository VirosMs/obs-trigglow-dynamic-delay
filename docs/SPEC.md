*[Versión en español](SPEC-es.md)*

# Technical specification — Trigglow Dynamic Delay for OBS

Status: **MVP / v0.7.0 — Early Access**
Last updated: 2026-10-20 (v0.7.0: §3.5, §4–§4.8, §8; v0.6.0: §3.1–§3.8, §4–§4.7, §8; rewritten from
scratch on 2026-08-26 — see §0). A v0.5.1 hotfix was prepared but never published; everything in it
shipped in v0.6.0, so this document dates those changes v0.6.0.
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
chosen live scene. Every `video_render` call (as of v0.6.0):

1. At most once per capture cell (a wall-clock interval of `1e9 × fps_den / fps_num` ns, two of
   them on the half-rate degradation rung, §4.7), renders the live scene into one reusable RGBA
   capture render target at the buffer's current size — its ortho projection still spans the
   scene's real size, so the GPU rasterizes the scene down to fit.
2. Converts that RGBA frame to **I420** — Y, plus U and V at half width and height (box-averaged,
   BT.601, studio range) — in three small full-screen passes into three R8 render targets, with a
   custom shader compiled once with `gs_effect_create()`. libobs has no public compute/dispatch API
   at all (checked directly against `graphics.h`), so ordinary pixel-shader passes are the only public
   mechanism for this conversion. (Up to v0.5.0 it was NV12, two planes, de-interleaved on the CPU
   before every encode and re-interleaved after every decode.)
3. Kicks off an **async GPU→CPU readback** into one of 3 rotating staging slots (three R8 surfaces
   each): `gs_stage_texture` now, `gs_stagesurface_map` two captures later, so the graphics thread
   never stalls on a copy that just started. The mapped planes are copied into a raw frame from a
   small pool and handed to the encode workers (§4.6).
4. Reserves the next slot of a **system-RAM ring** (`FrameRing`, §4.6) for that capture, stamped
   with its capture time; an encode worker compresses the frame (§4) and stores the packet in the
   ring's arena.
5. Picks the newest frame captured at least `delay` seconds ago, already decoded ahead by the decode
   worker, uploads it into 3 small playback textures **only when it changes** (every other view of
   the scene just redraws it), and draws it with the same shader's reverse technique (`DrawI420`).

A 0 s delay skips all of this since v0.6.0: the video passes through undelayed, with no ring and no
codec. The dock's minimum is 1 and a filter the dock manages ignores any other value (§3.3), so this
is only reachable for a filter the dock doesn't manage yet (one added by hand).

Only a **small, fixed number of GPU objects** exist regardless of how many seconds are buffered — one
capture render target, three plane render targets, 3 staging slots of three surfaces, 3 playback
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
Video and audio use the same `delaySeconds`, so both land in sync; when the RAM budget trims the
video ring, both use the same whole-second effective delay instead (§3.3).

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
session. (Known limit until v0.6.0: the query reported the ring's span floored to whole seconds,
while `Render()` capped the video delay at the unfloored span, so when the RAM budget trimmed the
ring video and audio could be up to ~1s apart. v0.6.0 closes it, see below. The query also no
longer hops to the graphics thread.)

**v0.6.0: switching off delay filters left on by a previous session.** The filters are saved
enabled in the scene collection when OBS closes while the delay is Active, so the next start would
delay the live scene for good while the dock showed `Inactive`. v0.5.0 added
`ObsFrontendBridge::DisableAllDelayFilters()` on `FrontendEvent::FinishedLoading` (only while the
controller is `Inactive`), but it enumerated with `obs_enum_sources()`, which never returns filters
(libobs 31.1.1: non-private inputs and groups only), so it disabled nothing. It now collects every
source that can carry one of our filters — inputs and groups (`obs_enum_sources`), the main canvas's
scenes (`obs_enum_scenes`), and the global audio sources on the output channels
(`obs_get_output_source`) — walks their filters with `obs_source_enum_filters`, and only then
disables the matching ones, outside every libobs lock (`obs_source_set_enabled()` emits a signal
whose handlers may take those locks themselves). It runs on `FrontendEvent::FinishedLoading` and on
`FrontendEvent::SceneCollectionChanged` (a collection switched to can carry them as much as the one
loaded at startup), both only while the controller is `Inactive`, right after the name migration of
§3.7.

**v0.6.0: switching scene collection turns the delay off.** The wrapper scene and the filters belong
to the collection they were created in, so with the delay on a switch left the dock saying "Delay
on" while the new collection ran without any delay. On `OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING`
(`FrontendEvent::SceneCollectionChanging`: switch, new, remove), while the old collection is still
loaded, `BufferModeController::StopForSceneCollectionChange()` stops the delay like `Disable()` —
filters off, Program back on the scene shown before Enable — so the old collection is saved that
way, and goes `Inactive` with `Status.CollectionChanged` ("The delay turned off because you switched
scene collection…") and a WARN. OBS 31 sends no `COLLECTION_CHANGING` for a duplicate or a rename,
only `COLLECTION_CHANGED` with the copy already loaded: the same stop runs then (first, before the
migration and clean-up above), switching off the copy's filters, and it changes Program only if the
copy opened on the wrapper scene, which would otherwise show the live scene undelayed.

**v0.6.0: one whole-second delay for video and audio (design A.4).** `bufferfit::Compute()`
returns `effectiveDelaySeconds`: the requested delay when the ring holds it, otherwise
`floor((frames − 1 − slack) / fps)`, whole seconds that keep the quarter second of slack against
capture jitter. `Render()` delays video by exactly that (`effectiveDelaySeconds × 1e9`; only when less
than a second fits does it fall back to the ring's own span), and audio gets the same value.
`get_effective_delay_seconds` no longer hops to the graphics thread:
`VideoDelayFilter::PlannedEffectiveDelaySeconds()` runs the same `bufferfit::Compute()` on the
caller, from the delay and quality floor the dock pushed (or, for a filter the dock doesn't manage
yet, its settings), OBS's video info and the target's size — the inputs `EnsureRingSized()` uses.
So `SyncAudioDelayToVideoEffective()` gives audio the right value from `Enable()` on and right after
a setting the controller changes, not a `Render()` later; a 30 → 40 s change no longer leaves audio
at 30 s. The result is stored in `BufferModeStatus::effectiveDelaySeconds`, the only copy, which the
dock and the overlay show from `Filling` on: with 60 s requested and 41 s fitting, the chip reads
"41 / 60 s" in amber, the message is `Status.FillingTrimmed` and the fill window lasts 41 s, all the
trimmed ring has to hold. The span itself uses rational arithmetic at
the exact `fps_num/fps_den`: multiplying by a truncated frame interval could floor a 1800-frame span
at 60fps to 29s.

**v0.6.0: refilling behind the loading scene when the buffer restarts.** A hard resize (§4.6) — the
delay, the minimum quality or OBS's video settings changed while the ring holds frames, or the kill
switch flipped — empties the ring, so the wrapper would show nothing for `delay` seconds. The filter
emits `buffer_restarted` on its source; `ObsFrontendBridge` connects to it when it enables the filter
and hops it to the UI thread as `FrontendEvent::BufferRestarted`; `BufferModeController` then puts
the loading scene back on Program (keeping the live scene rendering), re-syncs audio and goes back
to `Filling`. `Enable()` and that refill bump `BufferModeStatus::fillGeneration`, and the dock
re-arms its fill timer only when it changes, so other status updates while filling (the overlay
toggled, its corner changed) no longer restart the countdown and the switch to the wrapper. For a
change it makes itself (`SetDelaySeconds()` / `SetMinResolutionHeight()` while `Active`), the
controller predicts the restart with `EstimateBufferFit()` and switches Program before pushing the
setting. Without a loading scene, Program stays on the blank wrapper, never on the undelayed live
scene. Since v0.6.0 the dock locks the live and loading scenes, the seconds and the minimum quality
while `Filling`/`Active` (a disabled control is also out of the tab order), and the filters only
apply the dock's values (below), so in practice restarts come from OBS video resets.

**v0.6.0: the dock is the only place to set the delay.** The "(auto)" filters' own properties used
to let anyone bypass the plugin (90 s in the video filter's dialog put video at 90 s and audio at
60; 0 s passed the live scene through while the dock said "Delay on"). Now:
- Their properties hold no delay or quality field, only a note (`Filter.VideoDelay.Managed`,
  `Filter.AudioDelay.Managed`) that the dock manages them.
- `ObsFrontendBridge` pushes the dock's delay and quality floor through `set_managed_delay_seconds`
  and `set_managed_min_resolution_height` procs on each filter. From then on the filter is
  *managed*: `Update()` ignores a different value in its settings (OBS's Defaults button, an older
  dialog, obs-websocket) with one WARN per push, and `PlannedEffectiveDelaySeconds()` reads the
  managed values. A filter not managed yet (added by hand, or loaded with a collection before the
  first Enable) still applies its settings.
- While the delay is on, the bridge keeps weak references to the filters it switched on and listens
  to their `enable` signal: one switched off from outside the dock (its eye icon in the Filters
  dialog, obs-websocket) is switched straight back on (at most the frame being rendered skips it),
  with one WARN per activation. The same weak references serve the dock's once-a-second health poll
  (no scene walk, no WARN flood when the live scene is renamed), the setters, the effective-delay
  query and switching the filters off, so Disable still works after a live scene rename.

Known limitation: deleting an "(auto)" delay filter by hand while the delay is on is not caught —
the wrapper then shows the live scene with no filter, undelayed, until the user disables and
re-enables the delay from the dock (`Enable()` recreates the filter).

Known limitation, already present before v0.6.0: the plugin doesn't watch Program while the delay is
`Active`. If the user (or a Stream Deck/obs-websocket action) switches Program to the live scene
itself instead of the wrapper, the live scene is shown and streamed directly, without delay, since
the video filter sits on the wrapper scene. Program must stay on the scene the plugin chose while
the delay runs; changing scenes means turning the delay off first.

### 3.4. Freeing the RAM ring on Disable

OBS bypasses a **disabled** filter's `video_render` entirely — `VideoDelayFilter::Render()` never
runs again once disabled, so nothing would ever shrink/free its RAM ring on its own. `VideoDelayFilter`
registers a `"release_buffers"` proc handler on its own `obs_source_t` (`obs_source_get_proc_handler`);
`ObsFrontendBridge::SetBufferFilterEnabled(false)` calls it. Since that call can happen from any thread
(in practice, the Qt dock's button-click thread) while the ring is only ever touched from the
graphics/video thread, the release is queued onto `OBS_TASK_GRAPHICS` via
`obs_queue_task(..., wait=true)` rather than done in place — it runs strictly after any Render() call
still in flight, with no lock needed.

Since v0.6.0 that task (`ReleaseBuffersTask`) logs the cycle's last telemetry window (§4.5), then
`StopCycle(true)`: the ring, every queued job's reference to it and the raw frame pool's storage are
handed to the codec pipeline's reaper thread (§4.6), every codec worker closes its own codec
contexts, and `ReleaseSizedGpuObjects()` frees the capture and plane render targets, the staging
surfaces and the playback textures — ~100 MB of RAM (measured) and ~37 MB of VRAM (estimated) at
1440p that used to stay allocated while the delay was off. Reopening them on the next Enable costs
about a millisecond. Contract change: `wait=true` now guarantees that the ring is detached from the
filter, not that its memory is already back with the OS — the reaper frees it milliseconds later.
The graphics thread never frees the arena (hundreds of MB, tens of ms) nor waits for a worker.

### 3.5. Buffer sizing: output ceiling, then quality floor, then duration

The user picks two things, both fully free choices, never restricted:

- **Delay (seconds)** — 1 to 60 in the dock's spinner (a UI sanity cap; the real limit is the RAM
  math below).
- **Minimum quality** — 480p / 720p / 1080p / 1440p (2K) / 2160p (4K). The floor for the RAM
  reduction below: rather than store the delayed video below this height, the plugin stores fewer
  seconds. Since v0.6.0 it never raises the buffer above OBS's output resolution, though (rule 1
  below): with a 720p output the buffer is 720p even with a 1080p minimum, since storing more rows
  than OBS emits buys nothing. It is also the floor of the adaptive degradation (§4.7). With the
  floor at the output's height (e.g. 1440p with a 1440p output) the RAM reduction trims seconds
  instead of rows, and the degradation has only its half-rate rung left.

`EnsureRingSized()` computes a **RAM budget** from the machine's total system RAM (`GlobalMemoryStatusEx`
on Windows; unimplemented on other platforms, in which case a conservative fallback is used) — 50% of
total RAM, clamped between a 1GB floor and a 24GB ceiling.

Since v0.6.0 the sizing itself is a pure function, `bufferfit::Compute()` (`src/buffer-fit.hpp`/`.cpp`,
free of libobs and FFmpeg so it can be unit-tested), called with the same input by `EnsureRingSized()`
(which actually resizes the ring) and by `VideoDelayFilter::EstimateBufferFit()` (the dock's
estimate), so what the dock predicts is what happens. Its input is the target's size, the canvas
(`obs_video_info::base_*`) and output (`output_*`) geometry, the exact `fps_num/fps_den`, the delay,
the minimum quality, the RAM budget and the assumed compression ratio (§4.3). It applies three rules,
in order:

1. **Output ceiling (v0.6.0).** The buffer never holds more pixels than OBS's main mix outputs
   (Settings → Video → Output (Scaled) Resolution). Anything above it is discarded by OBS's own output
   rescale, but it cost the plugin CPU: MJPEG encode/decode (§4.2) scale roughly linearly with pixels
   and ran on OBS's graphics thread until v0.6.0. A user with a 2560x1440 canvas and a 1920x1080
   output at 60fps got "Encoding overloaded" from OBS with v0.5.0, which buffered at the canvas
   size; benchmarked
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
   monotonic in budget and output size. Heights are rounded with `llround` and forced even (4:2:0
   chroma), with a floor of 2. Widths are the nearest multiple of 8 not above the source
   (`AlignedWidth()`, floor 8), for every rule and every degradation rung, so each I420 plane row is
   a whole number of 4-byte words: libobs-opengl maps a staging surface with `linesize = width`
   while `glReadPixels()` pads every row to 4 bytes, which sheared any other width on Linux and macOS.
3. **Duration last.** If even the floor doesn't fit, the ring gets fewer frames: the plugin shortens
   the **actual buffered duration** instead of dropping below the floor.

The frame count uses the exact rate: `ceil(delay × fps_num / fps_den)` plus a quarter second of
slack (capture jitter must never leave the oldest slot younger than the delay) plus one slot, so 30s
at 60000/1001 needs 1815 slots rather than a truncated 59fps count. Since v0.7.0 the slack is at
least one GOP (half a second) when the cycle may store an inter-frame codec, and the assumed
compression ratio comes from the cycle's storage class: 5x for MJPEG, 12x once a hardware codec has
worked in the session (§4.8). The capture cell is `1e9 × fps_den / fps_num` ns (16,683,333ns at
59.94fps); before v0.6.0 it was `1e9 / 59` there, which missed almost one real frame per second.

The dock shows a live, non-blocking estimate of what a given (seconds, quality) combination will
actually achieve — duration and buffer resolution, cap included — *before* the user presses Enable —
"aconsejar según el hardware, pero a su elección" (advise based on hardware, but the choice is always
theirs): this estimate never blocks or clamps the user's picks, it only warns. `EnsureRingSized()`
logs `buffering at WxH (capped to OBS output WxH, canvas WxH)` whenever the ceiling applies, and a
WARN if the ring has to be rebuilt while it holds frames (OBS video settings, delay or quality
changed while the delay is on — the ring then refills from empty).

**Escape hatch: `buffer_output_cap` (v0.6.0).** A hidden plugin setting: `"buffer_output_cap":
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
`perf check` line (§4.5) say when the cap is off. The `codec_workers` kill switch (§4.6) is read and
pushed the same way. User-facing steps: `docs/TROUBLESHOOTING.md`.

**Encoders above the buffer (v0.6.0).** With the ceiling on, an output whose video encoder runs above
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

**Encoders' ceiling (v0.7.0).** Storing more lines than any encoder sends buys nothing, so when
every active encoder of the main canvas encodes below OBS's output the buffer is capped at the
largest of them (`encodercheck::EncoderCeiling()`, pure, `tests/encoder_check_tests.cpp`):
- E = the active video encoders whose scope is the main canvas, exactly or by aspect ratio (stream,
  recording, Replay Buffer, other plugins' outputs, every Enhanced Broadcasting track:
  `obs_output_get_video_encoder2()` for each index); other canvases never count.
- **No ceiling** (the output cap of rule 1 stands) when E is empty, when a video output without an
  encoder runs on the main mix (virtual camera, NDI, DeckLink: they take the output as is), when a
  GPU-rescaled encoder has an aspect ratio of neither the canvas nor the output (it could be the
  main canvas stretched), or with `buffer_output_cap` off.
- Otherwise `s = max over E of max(w / baseW, h / baseH)`, and the ceiling is the canvas scaled by
  `s` (width a multiple of 8, height even) when `s` is below the output cap's scale: only ever lower
  than the output. An encoder at or above the output keeps the output cap (and the WARN above).
- It is a **soft ceiling**: `bufferfit` and the ring are still sized for the output (the RAM limit,
  the slot count and the whole-second A/V delay don't change), and the filter rebuilds its
  degradation ladder from the ceiling down to `min(floor, ceiling)`. So it can lift at any time
  without restarting the ring; a recording started later above the ceiling gets frames at the new
  size after `delay` seconds.
- `BufferModeController` recomputes it at `Enable()`, on every stream, recording, Replay Buffer and
  virtual camera start or stop, and once a second while the delay runs (outputs other plugins start
  raise no frontend event), and pushes it through the filter's `set_encoder_ceiling` proc. The log
  says `encoder ceiling WxH: … (output WxH, canvas WxH)` or `no encoder ceiling: <why>`; the dock
  shows nothing new (its rung 0 is the ceiling). With a 1440p output streamed at 1080p it stores
  1080p: −34% of the buffer with MJPEG, −42% with HEVC (offline estimate).

**Tests (v0.6.0).** `tests/buffer_fit_tests.cpp` exercises `bufferfit::Compute()` without OBS:
canvas = output, 1440p → 1080p, 4K → 1080p, output above canvas, mismatched aspect ratios, a floor
above the output, RAM reduction in range, an active floor, rounding, widths multiple of 8, odd
dimensions, degenerate input, a non-canvas target, zero delay, 59.94fps, the whole-second effective
delay, the cap switched off, and budget/output sweeps. `tests/encoder_check_tests.cpp` exercises the
encoder WARN's decision (`encodercheck`), with the buffers sized by `bufferfit::Compute()` cap on
and off: aspect-ratio matching, a canvas encoder not scaled and CPU-scaled, GPU-rescaled encoders
above/equal/below the buffer, Enhanced Broadcasting tracks, vertical canvases (720x1280, 1080x1920)
and a vertical main canvas, and a buffer the cap doesn't limit (cap off, output = canvas,
RAM-limited). `buffer_fit_tests` also covers the 1440p and 2160p floors (a 1440p output with a 1440p
floor stays 2560x1440 and trims the delay to the whole seconds that fit). Seven more suites cover
the rest of the OBS-free code: `ui_tokens_tests` (WCAG contrast on OBS 31's palettes),
`auto_names_tests` (§3.7), `overlay_layout_tests` (§3.6), `frame_arena_tests`, `frame_ring_tests`
and `degradation_policy_tests` (§4.6, §4.7), and `pipeline_stress_tests` (§4.6). Off by default, so
plugin packaging is unchanged. Two ways to build them (`tests/CMakeLists.txt`):

```
# From the plugin's own build (same compiler flags as the plugin):
cmake --preset <preset> -DTRIGGLOW_BUILD_TESTS=ON
cmake --build <dir> --config RelWithDebInfo
ctest --test-dir <dir> -C RelWithDebInfo --output-on-failure

# Standalone, without downloading any OBS/Qt/FFmpeg dependency:
cmake -S tests -B build_tests
cmake --build build_tests --config RelWithDebInfo
ctest --test-dir build_tests -C RelWithDebInfo --output-on-failure
```

CI's `unit-tests` job (`.github/workflows/build-project.yaml`) runs the standalone path on Ubuntu,
Windows and macOS, and gates the release job.

### 3.6. The live overlay (v0.6.0)

While the delay is `Active` and "Show delay overlay on stream" is checked, the wrapper scene shows a
"DELAY 30 s" badge with the delay really in effect (`BufferModeStatus::effectiveDelaySeconds`). Since
v0.6.0 it is **one image the plugin paints itself** (`src/overlay-badge.*`, QPainter on the UI
thread only: ink plate, brand rule, Dynamic Delay mark, the bundled Archivo, or the system UI font when
it isn't bundled; under the plate, outside it and below the mark, the small `Overlay.Signature`
text, "By Trigglow" in every language, in the bundled Geist Mono at 11 px, mist at 75 % with a faint
dark halo and no box: 7.3 px on a 720p stream), written as one PNG per seconds value to the plugin's
config folder (`overlay/delay-badge-r3-<N>s.png`, 500x180, 2.5x the 200x72 design -- the 200x58
plate, then the signature line on transparent pixels -- so it stays sharp) and shown by a single
image source, **Trigglow Delay Badge (auto)**. The `r3` is `overlaylayout::kRenderVersion`, bumped
whenever the drawing changes: a PNG an older build left in that folder is never shown, and the first
render of a session deletes it. It replaces v0.5.x's badge PNG plus a separate text source in Segoe
UI (Windows only, magic offsets to line the two up); its text can no longer be edited, and the old
text source is removed on sight.

Placement (`src/overlay-layout.*`, pure, `tests/overlay_layout_tests.cpp`): 2.2% of the canvas's
short side away from the chosen corner (24 px at 1080p), keeping the share of the short side it has
at 1080p, so it reads the same at 720p, 1440p and 4K and doesn't spread across a 1080x1920 vertical
canvas. The whole image keeps that margin: in a top corner the plate sits at it, in a bottom corner
the signature does. What the plugin last applied (corner, canvas size, position, scale, badge
height) is kept in the scene item's private settings: the plugin places it again only when the
corner changes in the dock, or when the canvas size or the badge's height (`kDesignHeight`, 58
before the signature) changes and the item still sits where the plugin put it. A position or size
the user set in OBS is kept. An existing image source with that name is reused instead of adding
"... 2" copies.

### 3.7. Names of the objects the plugin manages (v0.6.0)

OBS looks these up by name, so they are fixed English ASCII constants, never translated
(`src/auto-names.*`): the wrapper scene **Trigglow Delay Buffer (auto)**, the video filter
**Trigglow Buffer Mode Delay (auto)**, one audio filter per source
`Trigglow Audio Delay (auto) - <source>`, and the overlay **Trigglow Delay Badge (auto)**. Up to
v0.5.x they ended in "(no tocar)" / "(auto, no tocar)" in every language.

`ObsFrontendBridge::MigrateLegacyAutoNames()` renames old collections once, in place (position,
filters and settings kept), on `FinishedLoading` and `SceneCollectionChanged` while `Inactive`.
Every lookup also accepts the old names (`LookupNames()`), so a collection whose new name was
already taken keeps working under the old one. The wrapper scene, under either name, is never
offered as a live or loading scene. Consequences documented for users (`CHANGELOG.md`,
`docs/TROUBLESHOOTING.md`): actions that refer to these objects by name (Stream Deck,
obs-websocket) must be updated, and going back to v0.5.0 or earlier after the rename creates a
"(no tocar)" set next to the "(auto)" one (the older version switches the "(auto)" filters off on
load, so they stay inert).

### 3.8. Account, Trigi and support client (v0.6.0)

This section covers only the plugin's side. How trigglow.com answers (its limits, the assistant
itself, how long it keeps data) lives outside this repository; the plugin only reacts to the
documented answers and error codes, and never shows the server's own error text.

**One network path.** `win-http.*` is the only code that opens connections: WinHTTP on Windows; on
macOS and Linux `Available()` is false and every call returns "unavailable" at once (no sign-in,
Trigi or reports there until they get an HTTP client, planned next). Every request is HTTPS with TLS
1.2 or newer, never follows a redirect (a 3xx is an error, so a Bearer token can't be bounced to
another host), sends no cookies and no Windows auto-logon, has per-step timeouts plus an overall
deadline (a longer receive window for Trigi's answers and summaries), caps the response size,
rejects header values that could inject lines, and logs nothing by itself. `api-client.*` sits on
top: requests only to `api.trigglow.com`, a `User-Agent` of `TrigglowDynamicDelay/<X.Y.Z> (OBS
plugin; <windows|macos|linux>)` with the real version, JSON headers, the `{ "error": "<code>" }`
body mapped to plugin states, `Retry-After`, and the fixed list of pages the plugin may open
(sign-in, support, privacy and plugin pages on www.trigglow.com, this repository on GitHub), checked
by prefix. Requests that create something (a conversation, a question, a summary, a ticket, an
attachment) are never retried automatically. A local test build can point at another host only with
the CMake option `TRIGGLOW_ALLOW_API_HOST_OVERRIDE`, which CI refuses.

**Sign-in (device link v2; `auth-client.*`, `auth-manager.*`, `token-store.*`).**
1. Start returns a link code (it goes in the page's URL), a short user code shown in the dock, a poll
   secret that stays in memory, the page to open, an expiry and a poll interval. Every value is
   checked (format, the page's prefix, expiry and interval clamped) before anything is opened; if
   one doesn't fit, sign-in fails cleanly and no older flow is tried.
2. The page opens in the system browser and the dock shows the user code. The plugin polls with the
   secret in the request body at the given interval until the expiry: pending, approved (the token
   is delivered once), denied ("It wasn't me"), expired; "slow down" lengthens the interval and
   network errors keep polling until the expiry. Cancel forgets the link and the secret locally.
3. The token is opaque (the plugin never interprets it) and only works for the plugin's own calls.
   On Windows it's stored with `CryptProtectData` for the current user, without UI, apart from the
   plugin's settings, written to a temporary file and moved into place. The display name is kept; the
   email isn't. If it can't be decrypted it's deleted and the plugin is signed out.
4. On every load: one account check, and a renewal before any other signed-in call when it's due
   (the new token is stored before it's used). `401 token_rotated` re-reads the stored token or
   renews, then retries once; any other 401 ends the session (storage deleted; Trigi keeps the text
   being typed) unless a newer token is stored by then; `403 suspended` or `account_inactive` keeps
   the token but blocks Enable; network errors, 5xx or an older server keep the plugin signed in, and
   Enable works offline. The local expiry never blocks Enable: the server decides when it answers.
   A renewal that a "Report a problem" call needs runs on a job of `AuthManager` that only unloading
   the plugin cancels, and its token is stored whenever the server answers: closing the dialog
   midway can't lose the new token (the old one, used again, would look stolen to the server, which
   then ends the sign-in).
5. Sign out sends the revocation in the background with a short timeout and deletes the local
   storage whatever the result. It wins over anything still waiting for the server (a renewal, the
   v0.5.x exchange, a sign-in wait): the token store counts sign-outs, a token obtained across one is
   never stored, and it's revoked too. So a sign-out never comes undone at the next start.
6. v0.5.x migration: when only the old plain-text sign-in exists, it's exchanged once for a plugin
   token; on success the old file is overwritten with zeros and deleted; a 401 deletes it and ends
   the session; network errors, 5xx, an older server or a rate limit keep it for the next start (Enable
   keeps working meanwhile, but Trigi and signed-in tickets don't use the old sign-in). If an
   encrypted sign-in (or an unreadable one) is already there, a leftover old file is overwritten and
   deleted at load.

The dock and the log never show the link, the codes, the secret or the token.

**Trigi (`support-client.*`, `support-service.*`, `trigi-page.*`, `trigi-states.*`).**
- Shown only on Windows, signed in, when the account check says Trigi is available and the plugin's
  version is at least the minimum it reports (numeric `X.Y.Z` comparison). Otherwise the dialog opens
  on the ticket form; signed out it offers "Sign in to ask Trigi".
- The first-use notice (version 1) has to be accepted before the first question. The accepted
  version is stored locally, and a higher version shows the notice again.
- The conversation is created lazily, with the first question; opening the dialog makes no Trigi
  call. Its creation body carries exactly the locale, the client (`obs_dynamic_delay`, version, OS),
  the notice version and, only if the box is ticked, the technical data. The conversation ID lives in
  memory only.
- At most one Trigi request is in flight; a generation counter drops answers that belong to an
  earlier conversation or a closed dialog. Calls run as background jobs (below) and come back to the
  UI thread; nothing runs on OBS's graphics thread.
- Three answer shapes: an answer (text plus an optional action), a refusal, or "unavailable". An
  action to a Dynamic Delay ticket or to a person becomes "Create a ticket with this conversation";
  a ticket in any other category becomes "Open support on trigglow.com". Feedback, "resolved",
  "abandoned" (sent in the background when a dialog with questions and no ticket closes) and
  hand-off events are fire-and-forget.
- Errors map to page states (`trigi-states.*`, tested without Qt): no connection and "took too long"
  with a manual retry, server error, rate limit (Send off for the `Retry-After` countdown), session
  ended, conversation closed (a new one on the next question), Trigi turned off (the form opens with
  the text), too old a version (the form plus "Get the update"), inactive account. A question that
  didn't go through is marked "Not sent".
- **Technical data v1** (`support-diagnostics.*`, `diagnostics-collector.*`): opt-in, unticked by
  default and remembered locally, previewed as the exact JSON. Only numbers, booleans and fixed
  values (versions, architecture, renderer, GPU vendor from the adapter's numeric vendor ID, total
  RAM, encoder family, the delay's state and settings, canvas/output, fps, OBS's lag counters,
  pipeline health, up to 5 recent problem codes), every number clamped to its range with at most 2
  decimals, at most 1 KB. The type has no field that could hold a name, path, URL, key, user, GPU
  name, hardware ID or IP. The snapshot is taken on the UI thread without waiting on the graphics
  thread and sent once, when the conversation is created. A new field or code means version 2, sent
  only if the server says it accepts it.
- **Rendering** (`chat-text.*`, no Qt, tested): strips control characters except the newline, bidi
  controls, emoji and pictographs and invalid UTF-8; trims to 4,000 characters; splits the text into
  lines (paragraph, `- ` bullet, `N. ` step) and segments (text, `**bold**`, `` `code` ``); headings
  and code fences stay text. The page builds rich text only from escaped segments with `<b>` and
  `<span class="code">` (`trigi-widgets.*`), never with automatic or Markdown text formats, never
  with external links, and its document never loads a resource. URLs in an answer stay plain text.

**Tickets and the OBS log (`report-bug-dialog.*`, `bug-report.*`, `log-redact.*`).**
- Signed in: a ticket in category `dynamic_delay` with subject, message (ending in a visible,
  editable footer with the plugin, OBS and OS versions) and, after a conversation, a link to it. Only
  the ticket's ID is read, and it's neither shown nor logged. Then, if the box is ticked, the log goes
  as one `text/plain` part named `obs-log.txt`; if that fails, the ticket already exists and the
  dialog offers "Save log".
- Guest: the dialog first asks whether trigglow.com requires a human check for guests; if it does
  (or a guest ticket is refused for that reason), it doesn't show the guest form but "sign in or use
  the web form", without retrying. The guest attachment sends its access key in a header, never in
  the URL, and the ticket then opens in the browser.
- The log: only the current log of the OBS the plugin runs in (located on Windows only). No libobs
  or frontend call names that file, and the newest file in a logs folder may be another OBS's (a
  portable one, a second instance), so at load the plugin writes a line with a random marker and
  later reads only the file, among this OBS's log folders, that holds it; without it, nothing is
  attached. The log is read and redacted in memory, line by line, with linear scanners instead of
  `std::regex` (`src/log-redact.hpp` lists the rules): stream URLs, any other URL (host kept), user
  names in Windows/macOS/Linux paths (moved profiles, network shares and work OneDrive folders
  too), overlay links, emails (accents and a URL-encoded '@' too), values after key-like names
  (`client_secret`, `x_api_key`, JSON keys), credential headers, JWTs, IPv4/IPv6 addresses, long
  tokens, stream-key-shaped groups and base64, plus, best effort, quoted device names and window
  titles; timestamps, versions and hotkeys survive. Above 4.5 MB after redaction it keeps the first
  256 KB, a marker and the end. The "Attach OBS's log" box starts unticked and can only be ticked
  once the preview (the first and last 200 lines) has been seen; it says that names of scenes,
  sources, audio devices and windows can remain. No copy is written any more, and copies older
  versions left in `%TEMP%\Trigglow` are deleted at load.

**Background jobs and unloading (`background-tasks.*`, `cancel-token.*`, `ui-tasks.*`).** Every
network job (the sign-in check, renewal and v0.5.x exchange, the sign-in wait, Trigi, tickets and
their attachments, reading the log for a ticket or a preview, the update check) runs on its own
thread with a cancellation token, in a group owned by `AuthManager`, a support dialog or the dock.
WinHTTP runs in its asynchronous mode, so cancelling a job aborts its call at once (a Trigi answer
can otherwise take up to 90 s), and a timeout is its own status ("Trigi took too long") instead of
"no connection". Results reach the UI thread through one queue that the plugin drains; destroying
an owner cancels its jobs and drops their results, queued ones included. `obs_module_unload()`
cancels everything, refuses new jobs and waits at most 2 s; on Windows each job thread holds a
reference to the DLL and exits with `FreeLibraryAndExitThread()`, so no plugin code runs after OBS
unloads it. Measured against a local stand-in that kept a Trigi answer waiting for 90 s: unloading
took at most 7 ms. Revoking the token on Sign out and telling trigglow.com that a Trigi conversation
was abandoned belong to no owner: only unloading cancels them. Tested by `background_tasks_tests`.

**What the OBS log gets.** `[auth-manager]` signed in, signed out, session ended, token renewed,
migrated legacy login; `[trigi]` session started, message sent (length, technical data on or off),
reply (HTTP code, length, action), refused, unavailable, rate limited, HTTP errors by code;
`[support]` ticket created (with or without the log). Never the token, the link, codes or secret,
conversation or ticket IDs, a ticket's access key, the text of questions or answers, the technical
data, an email or a name, or absolute paths.

## 4. v0.3.0: real MJPEG compression of the RAM ring

§3.5's RAM budget math above still assumes an uncompressed ring for §3's core mechanism — that was
accurate for v0.2.0, but as of v0.3.0 each buffered frame is additionally MJPEG-compressed on
capture and decompressed on playback, on the platforms where that's available. This section
describes that pipeline; since v0.6.0 it runs on worker threads (§4.6) over a ring of slot headers
and a block arena, instead of synchronously in `Render()` over one buffer per slot.

**Platform scope:** only where `TRIGGLOW_HAVE_FFMPEG` is defined — Windows and Linux, via a vendored,
pinned-release BtbN LGPL-shared FFmpeg build (`avcodec`/`avutil`/`swresample`; never "latest", to keep
builds reproducible). No equivalent trusted static FFmpeg build exists for macOS yet, so macOS isn't
part of this: it stores frames uncompressed (raw I420 since v0.6.0, `MakeRawCodec()`). Everywhere
the codec isn't available (macOS today, or a build without FFmpeg linked), every code path below
falls back to uncompressed storage automatically — compression is an addition on top of the existing
mechanism, never a new requirement for it to work.

**Since v0.7.0** the Windows build stores the buffer with a hardware inter-frame codec (HEVC, or
H.264 on a GPU without HEVC, through NVENC, AMF or Quick Sync, decoded with D3D11VA) wherever one
works, and MJPEG otherwise: §4.8. MJPEG stays the codec of Linux, of Windows without a usable
hardware encoder, of the moments while one is being set up, and of every return to MJPEG; §4.1–§4.7
describe it and the pipeline both codecs share.

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

*v0.7.0:* MJPEG made 80–85% of the plugin's RAM at 1440p60, and a hardware HEVC encoder stores the
same frames in 67–74% fewer bytes with a better picture, so the ring learned GOPs instead (§4.8):
every packet carries its codec, whether it is a key, and its chain and seq; a P-frame is only
decoded right after its reference; the ring keeps a GOP of slack so the key of the frame due is
always still there; and a decoder that starts, resumes or jumps catches up from that key unshown.
"Any slot, any time" became "any key, any time", and MJPEG, every packet a key, still behaves
exactly as described here.

### 4.2. Encode/decode flow

The codec sits behind a small interface: up to v0.6.0 `VideoCodec` (`src/codec-pipeline.hpp`), since
v0.7.0 a separate `storage::Encoder` and `storage::Decoder` (`src/storage-codec.hpp`, §4.8). Each
MJPEG instance is opened, used and closed by one thread only. On `TRIGGLOW_HAVE_FFMPEG` platforms it
is MJPEG (`src/mjpeg-codec.*`, `MjpegEncoder`/`MjpegDecoder` since v0.7.0); elsewhere the raw codec
stores frames uncompressed.

- **Encode:** the I420 planes read back from the GPU (§3.1, step 3) are copied into a reused
  `AVFrame` (`AV_PIX_FMT_YUV420P`, labelled `AVCOL_RANGE_MPEG` — the studio range the shader writes;
  FFmpeg's MJPEG encoder accepts that label only with `strict_std_compliance =
  FF_COMPLIANCE_UNOFFICIAL`, and it is metadata only: a 14-byte comment, identical decoded planes)
  and pushed through an MJPEG `AVCodecContext` with fixed quality via `AV_CODEC_FLAG_QSCALE` (`qmin` =
  `qmax` = `kMjpegQuality = 5` — a commonly-cited MJPEG sweet spot, visually close to lossless while
  still compressing meaningfully; not a dock setting). The encoder is reopened lazily when the frame
  size or the slice-thread count changes (about 1 ms, on the encoding thread). The encode worker
  copies the packet into the ring's arena and publishes the slot (§4.6).
- **Decode:** the decode worker pins the slot, copies its packet out, unpins it and decodes it
  outside every lock. The decoded frame is the decoder's own `AVFrame`, uploaded plane by plane with
  its own stride (no copy, no re-interleave), and returned to FFmpeg's buffer pool when released.
- **Fallback:** if the encoder can't open at this size or a frame fails to encode, the frame is
  stored raw (tightly packed I420) in the arena; raw frames are counted (`rawFallback`) and shown
  without decoding. Compression is never a hard requirement for the ring to work at all — only ever a
  RAM optimization on top of it.

**Where this runs, and what it costs (v0.6.0).** Up to v0.5.0 both encode and decode ran
synchronously inside `Render()`, on OBS's graphics thread — the thread that has to finish every
frame within 1/fps (16.7ms at 60fps) — at canvas resolution: 14.3/17.4ms p50/p95 per frame at
1440p, 8.6/10.4ms at 1080p (offline benchmark, same FFmpeg build). Since v0.6.0 they run on the
codec worker threads (§4.6), and the graphics thread keeps only the conversion passes, the readback
copy and the upload: ~1.2–1.7/1.6–2.1ms p50/p95 per frame at 1440p60 in the offline worker
benchmark (Ryzen 7 7700X), with encode ~8.8/10.4ms and decode ~6.2/7.5ms on their own threads.

- **Slice threads, explicitly.** An encode context can use 1 or 2 slice threads since v0.7.0 (1, 2
  or 4 before; every slice thread holds its own encoder buffers, ~15MB at 1440p and ~33MB at 2160p)
  (`thread_type = FF_THREAD_SLICE`, `thread_count = k`); the degradation policy picks how many
  (§4.7), and in sync mode (§4.6) it starts at `hardware_concurrency() / 4` clamped to [1, 2]
  (`CodecPipeline::kMaxSlicesPerContext = 2`), because there they cut the encode's wall time on the
  graphics thread (1080p encode path 5.1 → 1.8ms p50 with 4 slices in v0.6.0, about 3ms with 2, same
  total CPU, ~3.5% bigger packets). `thread_type` must be set explicitly: with only `thread_count >
  1`, FFmpeg picks frame threading for MJPEG (`QSCALE` skips its single-thread exception),
  `receive_packet()` then returns `EAGAIN` and every frame would be stored raw. If
  `active_thread_type` still reports frame threading the encoder is reopened single-threaded (WARN).
  The decoder stays at `thread_count = 1` (n9.0's MJPEG decoder has no threading). The `MJPEG
  encoder open` log line reports size, qscale, Huffman tables, slice threads requested and granted,
  `active_thread_type` and the logical CPUs.
- **Standard Huffman tables.** The encoder is opened with `huffman=default` (`av_opt_set()` on its
  private options) instead of FFmpeg's default "optimal" tables, which need a second pass per frame.
  With more than one slice FFmpeg already uses the standard tables, so this matters for the
  single-slice contexts the worker pipeline starts with: 10-17% less encode time (p50-p95, 1440p,
  vendored n9.0.2) for 3-3.5% bigger packets. Huffman coding is lossless, so the decoded frames are
  identical, and every packet still carries its tables (DHT). The open log line says
  `huffman=default`, or why it couldn't be set.
- **Each frame decoded and uploaded at most once.** `Render()` runs once per view of the scene
  (Program, preview, projectors, multiview). The decode worker decodes each frame once, and
  `Render()` uploads a decoded frame only when it isn't already in the playback textures; other views
  just redraw them. If the frame due isn't ready (dropped, still encoding, or not decoded in time),
  the last frame uploaded in this cycle is drawn again (a "hold") instead of nothing; the next frame
  is still picked by age, so a hold is a one-frame stutter, never a drift in the delay. While the
  ring is still filling nothing is drawn. The playback cache is invalidated on every cycle end and
  every resize, so a hold can never show a frame from a previous cycle or a destroyed texture.

### 4.3. Memory: budgeted slots (v0.3.0–v0.5.0), then a block arena (v0.6.0)

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

**Block arena (v0.6.0).** Per-slot buffers had two more costs. They were zero-filled when sized
(`assign(budgetedBytes, 0)`), which touched every page of the ring on the graphics thread at once:
~1.9GB resident at 1440p60 and a ~381ms stall (about 23 dropped frames) on the first `Render()`
after `Enable()`. And freeing the ring meant one `VirtualFree` per slot (1816 at 1440p60, 87ms
measured, 141ms in a user's log), also on the graphics thread. Since v0.6.0 the payloads live in a
`FrameArena` (`src/frame-arena.*`): packets are bump-allocated back to back in blocks, never
zero-filled, and a packet never straddles two blocks. Every block counts its live allocations;
when the ring has overwritten every frame a block held, the block is recycled whole. Since the ring
frees frames in roughly the order it wrote them, committed and resident memory both stay close to
the real window of packets plus a block or two: ~576-768MB for 30s at 1440p60 in the offline
simulation (main to heavy content), against 1923MB resident before. Allocations and frees can come
from any thread in any order (encoders allocate, the ring frees when it reuses a slot). `Free()`
never calls the OS: emptied blocks go to a spare list, and `Trim()`, the only call that returns
memory (spares beyond two), runs on the reaper thread (§4.6). Sizing (`ArenaConfigFor()`): blocks of
about 1/16 of the expected ring bytes, between 4 and 32MiB and never less than two raw frames
(room for the raw fallback); a hard limit at the RAM budget (or what the ring was sized for, if
more), past which an allocation fails and the frame is dropped (`drops arena`) instead of memory
growing without bound. `kAssumedCompressionRatio` now only decides how many seconds fit the budget
and the arena's expected size; nothing is reserved per slot any more.

**Smaller blocks (v0.7.0).** `FrameArena::SizedFor()` now takes blocks of 1/64 of the expected ring
bytes in whole MiB, between 4 and 8MiB, and keeps at most one spare (`maxSpareBlocks = 1`); the
"two raw frames per block" minimum is gone, since a raw frame bigger than a block (2160p, the raw
fallback) already takes the oversized path. The memory committed past the live packets drops from
35–71MB to about two blocks (the current block's free tail, the oldest block's freed head and the
spare). Without FFmpeg every payload is a raw frame, so blocks there hold a whole number of them.
The expected bytes follow the cycle's storage class (§4.8): `slots × raw / 5` for MJPEG, `/ 12` for
the hardware codec.

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
(~11-15x, see §4.3's retune note), and since v0.6.0 the ratio is part of the `pipeline check` line
(§4.5), measured on every user's session. It's still one data point per session and content type,
not a guarantee.

### 4.5. v0.6.0: `perf check` and `pipeline check` telemetry

So a performance report can be checked from a user's log rather than guessed, `VideoDelayFilter`
writes two INFO lines every 10 seconds of rendering (`kPerfLogIntervalNs`), during `Filling` and
`Active`. They replace the 300-frame `compression check` line:

```
perf check (periodic, 10.0s): buf=1920x1080 (capped to OBS output 1920x1080, canvas 2560x1440)
fps=60.00 | render p50/p95/max=…ms own=… readback=… upload=… | calls=…/s captures=…/s uploads=…/s
holds=N (late decode N, dropped N, encoding N) | obs frames+N lagged+N skipped+N

pipeline check (periodic): mode=workers codec=mjpeg rung=0/4 capture=1920x1080@60.00fps enc=1x1 L=3
S=3 | enc0 p50/p95/max=…ms busy=…% enc1=… busy=…% dec=… busy=…% | drops reserve=N backpressure=N
arena=N capture=N rawFallback=N encFail=N decFail=N staleEnc=N decJumps=N staleDec=N | queue enc
max=N avg=… dec ahead avg=… min=N | ratio=…x arena=…MB committed (live …MB, peak …MB, N blocks of
…MB, limit …MB) ring=N slots (ready N, dropped N, in flight N, pinned N)
```

(two lines in the log; wrapped here. `codec=` since v0.7.0, followed by a third line, `storage codec
check`, §4.8.)

- **`perf check` — the graphics thread.** The full-quality buffer resolution and why —
  `capped to OBS output WxH, canvas WxH`, `not capped, source WxH, output WxH`, or
  `cap off (buffer_output_cap=false), …` — with `, reduced for RAM` when §3.5's rule 2 kicked in;
  the exact fps; wall time (`os_gettime_ns`) of the whole `Render()` call (`render`), of the
  plugin's own part of it (`own`: everything but rendering the live scene into the capture texture,
  which OBS would render anyway — what the degradation policy judges, §4.7), of the staging readback
  and copy, and of the texture upload; `Render()` calls, captures and uploads per second; holds
  (§4.2) split by the state of the slot that was due (Ready but not decoded in time, Dropped, still
  Encoding); and the deltas of OBS's own counters over the same window — total frames
  (`obs_get_total_frames`), lagged frames (rendering lag, `obs_get_lagged_frames`) and skipped frames
  (encoding lag, `video_output_get_skipped_frames`).
- **`pipeline check` — the workers and the memory.** Mode (`workers`, or `sync` with the kill
  switch, §4.6), the codec storing the frames now (v0.7.0: `mjpeg`, the hardware encoder's FFmpeg
  name such as `hevc_nvenc`, or `raw` without FFmpeg), degradation rung and the last one, the size
  and rate captured right now, encode contexts × slice threads, decode-ahead (`L`) and staging slots
  (`S`); p50/p95/max per frame and busy share of each encode context and of the decoder; drops (no
  free slot, encoders behind, arena at its limit, failed readback), raw fallbacks, codec failures,
  encodes made stale by a reused slot, decoder jumps, decoded frames dropped as stale after a render
  pause; encode queue depth and frames decoded ahead; compression ratio; arena committed, live and
  peak memory, blocks, block size and limit; and the ring's slot states.
- **Samples:** p50/p95 through `std::nth_element` over a fixed 4096-sample array per series, max
  exact. No allocation per frame or per window. The dropped and repeated rates of the last window
  also feed the dock's degradation notice (§4.7).
- **Windows:** a pause in `Render()` calls longer than 1s (filter hidden, OBS stalled) closes the
  current window early (`resumed after a pause`), so its per-second rates aren't diluted. The last
  partial window is logged when the delay is disabled (`filter disabled`, from the `release_buffers`
  task), set to 0s (`delay set to 0`), and when the filter is destroyed with the delay on, e.g. OBS
  closing (`filter destroyed`; safe in the destructor because OBS destroys a source only after
  detaching it, and the lines read only the filter's own members). Before v0.6.0 the filter was in
  practice never destroyed: `ObsFrontendBridge` leaked a reference to it on every `Enable()`
  (`obs_source_get_filter_by_name()` returns a new reference); every lookup now holds it in an
  `OBSSourceAutoRelease`, and the destructor, which libobs runs on its destruction thread, joins the
  codec threads and then enters the graphics context to free the GPU objects. Windows shorter than
  1s are dropped.
- **Other lines:** `delay pipeline: …` at every cycle start (mode, buffer size, slots, delay, the
  storage class with its assumed ratio and GOP margin since v0.7.0, encode contexts × slices,
  decode-ahead, staging slots, the degradation ladder down to its last rung, arena block size and
  limit); a WARN per rung change (§4.7); `settings loaded` and `enabled` with
  `output_cap=on|off codec_workers=on|off` and, since v0.7.0, `buffer_codec=auto|mjpeg` (`enabled`
  also gives the cycle's `storage_class`). The storage codec's own lines are in §4.8.

How to read each field from the user's side: `docs/TROUBLESHOOTING.md`, "Reading the log lines".

### 4.6. v0.6.0: codec worker threads, ring and arena

Design invariants: the video delay is exact by wall clock, frames are picked by capture age, and
video and audio share one whole-second delay (§3.3); "never cut": while `Active` the ring is only
emptied by a real geometry change; everything `gs_*` stays on the graphics thread (workers never
touch the GPU); and each `AVCodecContext` is used by one thread only, which also opens and closes it
(since v0.7.0 a hardware one is opened on the setup thread and handed over, still to one thread at a
time: invariant I8, §4.8). `src/codec-pipeline.*`, `src/frame-ring.*` and `src/frame-arena.*` are
pure C++ (std::thread, no OBS, the codec injected), so tests drive them without OBS or FFmpeg.

**Threads.**

| Thread | Does | Never does |
|---|---|---|
| OBS graphics thread (`Render()`) | Renders and converts the capture, harvests the staging slot staged two captures ago into a raw frame from a pool (`kMaxEncodeContexts + 2` frames), reserves the ring slot, submits the encode job; picks the decoded frame due, uploads it if new, draws it; runs the degradation policy every 250ms | Encode, decode, (de)interleave, free the ring, wait for a worker |
| Encode workers (1-2, `trigglow-enc-N`) | Pop jobs from a bounded FIFO, encode outside every lock with their own codec context, copy the packet into the arena, publish the slot | Touch the GPU, share a codec context |
| Decode worker (1, `trigglow-dec`) | Decodes in capture order up to `L = 3` frames ahead of what's on screen, from the playback target the graphics thread publishes (`now − delay`, predictable from the clock); jumps straight to the frame due when it falls behind; is pointed at the oldest frames during the last 250ms of the fill window (`PrimeDecode()`), so the first delayed frame is ready when due; keeps `L + 2` decoded frames | Decode frames that will never be shown |
| Reaper (1, `trigglow-reap`) | Frees what must never be freed on the graphics thread nor hold up an encode: detached rings and their arenas (600-770MB at 1440p, tens of ms), the raw frame pool after Disable, decoded frames dropped by a detach; runs the arena's `Trim()`; since v0.7.0 also closes retired hardware encoders and decoders | Anything else |
| Codec setup (1, `trigglow-codec-setup`, v0.7.0, created on first use) | Opens and probes a hardware encoder for the frame size worker 0 asks for, and hands it over with a warmed-up decoder (§4.8) | Encode or decode a captured frame; delay a capture |

**Ring and slot states.** `FrameRing` is a fixed number of slot headers plus the arena. Each slot is
`Free`, `Reserved`, `Encoding`, `Ready` or `Dropped`, and whoever owns a state is the only one who
touches the payload in it: the graphics thread reserves (`Free/Ready/Dropped → Reserved`, in capture
order, with a new generation unique across rings), an encoder takes and publishes
(`Reserved → Encoding → Ready`) only if the generation still matches, the decoder pins a `Ready` slot
to copy its packet without the lock. `captureNs` is assigned at reservation, so timestamps never
decrease from the oldest slot to the newest whatever order the encoders finish in, and playback's
binary search stays valid with `Reserved/Encoding/Dropped` slots in between; display picks the
newest `Ready` slot at or before the target, looking back past a few non-ready ones (a repeat of an
older frame, never one from the future). One mutex guards headers and indices only, held for
O(1)/O(log n) work; copies, encodes, decodes and frees happen outside it, and it is never nested
with the pipeline's or the arena's locks.

**Backpressure.** No free raw frame (encoders behind): the graphics thread drops the **newest**
capture — nothing already done is wasted, the queue never grows, and playback by age just repeats
the previous frame. No free slot (an encoder or the decoder a whole ring behind), or the arena at
its limit: same, the frame is dropped. All of it is counted (`drops …`) and, sustained, it is the
degradation policy's job (§4.7). After a pause in rendering (more than 4 capture intervals since the
previous playback call), decoded frames left far behind the frame due are dropped unshown
(`staleDec`) and the frame on screen is held until the decoder, which jumps to the due frame, has
it — instead of briefly showing frames hundreds of ms older than the delay.

**Cycle lifecycle.** The first `Render()` after Enable sizes the ring (`EnsureRingSized()`), builds
the degradation ladder and attaches a new `FrameRing` to the pipeline; the worker threads are created
lazily the first time and live as long as the filter, waiting on condition variables (no cost while
idle). Disable: §3.4. A **hard resize** — new buffer size, slot count, frame interval, effective
delay or kill-switch value with frames in the ring — ends the cycle (the old ring to the reaper) and
starts a new one, with a WARN and the `buffer_restarted` signal (§3.3). A **soft change** — a
degradation rung or the encode parallelism — keeps the same ring and slot count, so the span and the
audio are untouched: staging slots and raw frames carry their own size, each encoder reopens its
context lazily, the MJPEG decoder adapts to the size in each frame, and the playback textures are
recreated when the uploaded frame's size changes. Destruction (OBS's destruction thread, never the
graphics thread): stop and join the workers (each closes its codec; a join waits for at most one
frame's encode or decode, or a ring being freed), then free the GPU objects inside a graphics
context.

**Kill switch: `codec_workers` (hidden).** `"codec_workers": false` in `buffer-mode-settings.json`
puts the pipeline in sync mode: `SubmitEncode()` and `AcquireForDisplay()` run the codec inline on
the graphics thread, as up to v0.5.0 but slice-threaded, on the same ring and arena; the reaper
still frees memory off the graphics thread (and if it can't start, memory is freed inline). The
degradation still runs, adding slice threads before lowering the rung. Same rules as
`buffer_output_cap` (§3.5): read only at OBS load by `ReadHiddenSwitch()`, only a literal JSON
`false` turns it off (any other type keeps it on with a WARN and is dropped from the file at exit),
written back only when `false`, pushed to the filter on every `Enable()`
(`ObsFrontendBridge::SetBufferFilterCodecWorkers()`), no filter property and no dock control. A
value that differs from the running pipeline's builds a new pipeline at the next cycle (the old
one's threads are joined in the destructor, never on the graphics thread). If a worker thread can't
be started, the pipeline falls back to sync mode by itself. Reported as `codec_workers=on|off` in
the `settings loaded` and `enabled` lines and as `mode=` in `pipeline check`. Since v0.7.0 sync mode
always stores MJPEG: the hardware codec needs the asynchronous workers, so `codec_workers=false`
wins over `buffer_codec` (the `enabled` line says so).

**Measured** (offline worker benchmark `bench-workers`, Ryzen 7 7700X, vendored FFmpeg n9.0.2,
synthetic gameplay-like content, 20s per run, one encode context and one slice, p50/p95 ms):

| Case | Graphics thread total | Encode | Decode | Encode / decode busy | CPU cores in total |
|---|---|---|---|---|---|
| 1440p60 main | 1.2–1.7 / 1.6–2.1 | 8.8–9.6 / 10.4–11.2 | 6.2–6.5 / 7.5–7.6 | 53–57% / 37–39% | ~1.0 |
| 1440p60 heavy | 1.3 / 1.6 | 10.0–10.3 / 12.5–12.7 | 6.7–6.8 / 8.1–8.3 | 61–62% / 41–42% | ~1.1 |
| 1080p60 main | 0.6 / 0.9 | 5.1 / 6.2 | 3.6 / 4.3 | 30% / 22% | ~0.5 |
| 1440p60, sync mode, 4 slices | 10.6 / 12.2 | — | — | — | ~1.0 |

The graphics-thread figures are a proxy (copies into plain memory); real D3D11 readback plus upload
measured 0.47/0.58ms at 1440p on an RX 7800 XT. Not measured live inside OBS yet, nor on NVIDIA
GPUs.

**Tests.** `frame_arena_tests` (bump allocation, out-of-order frees, block recycling, the limit,
oversized payloads, `Trim()`, a FIFO steady state, a multi-threaded run with content checks),
`frame_ring_tests` (state machine, generations, reservation failures, age-based selection with
dropped slots in the way, decode-ahead, payload freeing) and `pipeline_stress_tests`: a simulated
graphics thread at 250fps (with the 2-frame staging delay) that at random moments detaches and
reattaches (Disable, hard resize), changes the parallelism, the frame size (soft rung change) and
the capture rate, with a fake codec that stamps every frame with its capture time. Hard checks: a
shown frame is never younger than the playback target, shown capture times only increase, every
frame's content matches its timestamp, no deadlock (watchdog), no arena leak, every ring is freed
and none on the simulated graphics thread. It also covers a 300ms render pause and a decoder 2.5
captures slow.

### 4.7. v0.6.0: adaptive degradation

`src/degradation-policy.*` (pure, `tests/degradation_policy_tests.cpp`) decides, from numbers only,
how much parallelism the encoders get and which **quality rung** the buffer is captured at, so that
a machine that can't keep up lowers the delayed picture's quality instead of lagging OBS or cutting
the delay.

**The two knobs.**
- *Parallelism* (encode contexts × slice threads): with workers `1×1 → 2×1 → 2×2` since v0.7.0 (`→
  2×4` before); in sync mode `1 → 2` slices on the one context (`→ 4` before). The starting step is
  predicted from the resolution: the first whose predicted encode p95 (`1.1 × (0.3 + 2.3 ms per
  megapixel)`, divided by 1.6 per doubling of slices) is at most 70% of what the contexts have per
  frame — 1440p60 starts at 1×1, 4K60 at 2×1; sync mode starts at `hardware_concurrency() / 4`
  slices clamped to [1, 2]. The hardware codec (§4.8) has a single step, `1×1`: one context keeps
  the bitstream in capture order, and slices mean nothing to it. `SetParallelism()` switches the
  steps when the codec in use changes and keeps the rung.
- *Quality rung*: rung 0 is the fitted buffer (§3.5); then steps of ×0.83 in scale down to the
  user's minimum quality in buffer pixels (the same floor `bufferfit` used; the floor itself is the
  last resolution rung, never below it); then that floor captured at half the frame rate (every
  other capture cell), only when the output runs at 48fps or more. Widths are multiples of 8,
  heights even. Every rung is a soft change (§4.6): same ring, same slot count, so the delay never
  moves; on the half-rate rung the span in time is unchanged and playback by age shows each frame
  twice.

**Evaluation.** Every 250ms, over trailing windows of 2s (0.5s for the immediate rule), and only 2s
after the previous change (cooldown). Every metric needs at least 20 samples (10 for the immediate
rule) or it is ignored.

| Trigger (down, one step) | Condition | Action |
|---|---|---|
| Graphics thread | Plugin's own work (`own`, §4.5): p95 > 40% of the frame interval, or p95 > 25% while OBS lagged frames in the window, or p50 over the last 0.5s > 50% (checked 0.5s after a change) | Sync: more slices first, then a lower rung. Workers: a lower rung (what's left on the graphics thread scales with pixels) |
| Encode (workers) | p95 > 80% of what the contexts have per frame (`p95 / (capture interval × contexts)`), or > 2% of captures dropped | More parallelism first, a lower rung once it's maxed out |
| Decode (workers) | p95 > 80% of the capture interval, or > 5% of due frames not decoded in time | A lower rung (the MJPEG decoder has no threading) |
| Up (one step) | The load predicted one step up (measured p95 × pixel ratio, or × 1.6 per halving of slices, × 1.1 margin) stays ≤ 25% of the frame interval on the graphics thread and ≤ 50% for the workers, for 30s in a row | Quality first (`higher rung`), then parallelism given back down to the starting step |

The decoder works `delay` seconds behind the capture, so right after a rung change it still decodes
the previous rung's frames: every frame is tagged with the rung it was captured at, and decode
metrics only count once they come from the current rung — no second step for a load the first one
hasn't reached the decoder yet, and no going up on the old rung's numbers. At the last rung with the
load still too high, nothing more is done (no oscillation, no cut); captures keep being dropped and
counted.

**With the storage codec (v0.7.0).** The decode tag is `(rung, codec class)`, so the hardware
decoder's numbers and MJPEG's never mix after a change of codec. Holds the decoder causes on purpose
(catching up from a key, waiting for the next one, §4.8) and packets the encoder never stored are
not decode misses. The encoder's latency never counts as encode load (invariant I6, §4.8): only the
worker's own time in its calls does. The storage codec policy can force one rung down at once
(`ForceLower()`, cause `Encode`, so the dock's existing notice) and veto going back up for 5 minutes
(`Sample::blockUp`). `Rebase()` moves the ladder under the encoders' ceiling (§3.5) on a given rung,
keeping the parallelism.

**Reporting.** Every rung change is a WARN (`delay buffer lower rung (encode, load 85%): now WxH at
F fps capture, rung N of M (…) -- last 2s: … The delay itself is unchanged.`); parallelism changes
are INFO. The filter keeps a `PipelineHealth` (`src/pipeline-health.hpp`: rung, maximum rung, size
now and full, capture and output fps, encode contexts and slices, cause of the last step down,
dropped and repeated frames per minute over the last telemetry window, a change counter) under a
small mutex; its `get_pipeline_state` proc only copies it, never hopping to the graphics thread, so
a lagging render can't block the UI. The dock polls it once a second while `Filling`/`Active`
(through the bridge's weak reference to the filter, §3.3) and, while the rung is above 0, shows
"Buffer quality lowered" in the status card: what was lowered (resolution against the full size,
capture fps, or both), the cause (OBS's graphics thread, compressing or decompressing the frames)
and that the delay is unchanged, plus repeated/dropped frames per minute when above zero. The RAM
estimate line under the fields (rung 0, "RAM only fits 41 s of 60 s…") hides meanwhile, since the
notice then says what the buffer holds. Screen readers get it once: the card's accessible name
changes only with the state and when the notice appears or goes, what was lowered goes in its
description, and on Qt 6.8+ a `QAccessibleAnnouncementEvent` reads the notice when it appears or its
cause changes, and `Dock.Health.Restored` when it goes with the delay still on (older Qt keeps the
card's name change).

### 4.8. v0.7.0: the storage codec — hardware inter-frame storage on Windows

**Why.** Measured offline at 1440p60 with 30 s, 80–85% of the plugin's RAM was the arena, that is,
the MJPEG packets themselves; the rest (codec contexts, frame pools, block slack) was small. Only
fewer bytes per frame could halve it. MJPEG can't give them without hurting the picture (each qscale
step up costs about one VMAF point after the stream's encode), while a hardware HEVC encoder at
constant QP 20 with a key per second stored the same frames in 67–74% fewer bytes, with a better
picture, at a fraction of the CPU. No non-GPL software encoder is fast enough at 1440p60 next to a
game, so without a hardware encoder the buffer stays on MJPEG.

**Pieces.**

```
G (graphics thread)       capture -> I420 readback -> raw frame, ring slot reserved
                              |
trigglow-enc-0                v
  hardware encoder (async, one context)  <-- adopted at a frame boundary --  trigglow-codec-setup
  or MJPEG (bridge while one is set up, fallback)                            (rank, open, probe)
                              |
ring + arena                  v
  HEVC/H.264 and MJPEG packets, each with its codec, key, chain and seq
                              |
trigglow-dec                  v
  D3D11VA -> NV12 -> I420 (a software decoder only in an emergency)
                              |
G                             v
  playback by age; every 250 ms the storage codec policy may lower a rung, evacuate or yield
```

- **Interface** (`src/storage-codec.hpp`, pure C++17, no OBS or FFmpeg). `storage::Encoder` is
  asynchronous: `Send(frame, seq, forceKey)` copies the frame in (`Ok`, `Busy` or `Failed`), and
  `Receive(packet)` hands out ready packets in `Send()` order, each with its `seq` and whether it is
  a key; `Drain()` empties it before it is closed, `InFlight()` counts frames not out yet.
  `storage::Decoder::Decode(packet, wantImage, …)` decodes one stored packet (`wantImage = false`:
  catch-up, references only) and `Flush()` handles a discontinuity. Decoded frames are always I420,
  so playback never knows which codec was behind them. Implementations: raw, MJPEG
  (`src/mjpeg-codec.*`, every packet a key) and the hardware one (`src/hw-storage-codec.*`,
  `TRIGGLOW_HAVE_HW_STORAGE`, Windows only). `ChainTracker` and `InFlightQueue` keep a bitstream's
  books for any codec.
- **Ring metadata** (`src/frame-ring.*`). `FrameRing::Payload` carries the codec, `key`, `chain`
  (one bitstream session: an encoder from its first key on, for one ring) and `seq` (consecutive in
  the bitstream). `ringselect::Decodable()`: a key always, a P-frame only right after its reference
  (same chain, next seq). `PlanDecodeStart()` walks back from the frame due to the key it can be
  decoded from (over its own codec's packets only, so an MJPEG frame in between doesn't cut the
  chain); `ChooseDecodeStart()` catches up from it or waits for the next key, whichever shows a
  fresh frame sooner. The slot state machine, generations and `captureNs` at reservation are
  unchanged.
- **Invariants**, cited by number in the code:

| # | Invariant | How |
|---|---|---|
| I1 | Inside a chain the bitstream follows capture order | One inter encode context, packets matched in `Send()` order; a packet out of order is a hard error |
| I2 | A P-frame is never decoded without its reference | `Decodable()`; the decoder skips to the next key otherwise |
| I3 | Captures dropped before their encode consume no seq; after a packet lost post-encode, the next frame is a forced key and no orphan P-frame is stored | `ChainTracker` |
| I4 | With an inter-frame codec the key of the frame due is always in the ring | A GOP of slack in `bufferfit::SlackFrames()` |
| I5 | Playback (re)starts on a key, holding the frame on screen meanwhile | `FrameRing::PlanDecodeStart()` |
| I6 | An encoder's latency never counts as load | The degradation sees only the worker's own time; the latency goes to the codec policy |
| I7 | No change of codec, chain or resolution empties the ring or moves the delay | Rings hold several codecs; decoded frames are always I420; MJPEG bridges |
| I8 | Every encoder and decoder is used by one thread at a time, never the graphics thread | Opened on the setup thread, handed over under the pipeline's mutex, closed by its worker or the reaper |
| I9 | The storage class that sizes a cycle's ring is fixed for the cycle | Written into the filter at `Enable()`; the ring and the planned delay read the same value |

**Hardware encoders.** `RankCandidates()` (`src/storage-codec-select.*`, pure) orders `hevc_nvenc`,
`hevc_amf`, `hevc_qsv`, `h264_nvenc`, `h264_amf`, `h264_qsv`, keeping only the GPU vendors DXGI
reports (`QueryGpuAdapters()`), so no other vendor's runtime is ever loaded; engines OBS doesn't
encode on come first (an iGPU next to a stream on the dedicated GPU). NVENC is only a candidate
while OBS runs at most 3 NVENC encoders (`kMaxObsNvencForBuffer`), leaving a session free even with
the old 5-session limit. Quick Sync is never a candidate in an elevated OBS (`TokenElevation`): its
runtime is loaded by a dispatcher that searches DLL paths an unprivileged user controls. Encoders
that failed hard this session are blacklisted. Settings:
- HEVC at constant QP 20, H.264 at 18; a key every half second (`KeyframeIntervalFrames()`:
  `ceil(fps / 2)`, 30 frames at 60 fps); no B-frames; forced IDRs (`pict_type = I` plus the
  backend's option); parameter sets in band, and copied in front of a key that lacks them
  (`src/annexb.*`); `pts = seq`, matched back to the pipeline's frames.
- NVENC: `p1`, `ll`, `constqp`, zero latency, no lookahead, no multipass, no spatial AQ, 4 surfaces.
  AMF: `lowlatency_high_quality`, `speed`, `cqp`, no pre-analysis, headers on every IDR. Quick Sync:
  `veryfast`, NV12 input (the chroma interleaved on the worker), `async_depth` 1.
- Input frames in system memory from a buffer pool (I420; NV12 for Quick Sync). NVENC and AMF open
  on a D3D11 device of their vendor's adapter, OBS's own when it is that vendor's.

**Decoders.** D3D11VA on OBS's adapter (`obs_video_info.adapter`), low delay, one thread; the
picture is downloaded as NV12 into a reused frame and its chroma split into I420 on the decode
thread (no download at all for catch-up). It never falls back to software silently: a stream it
can't decode is a failure. libavcodec's software HEVC/H.264 decoder (slice threads) is only the
emergency path, for the hardware frames left in the ring when the hardware decoder fails or when the
buffer leaves an engine it shares with OBS for decoding (AMD VCN and Intel decode on the engine that
encodes; NVIDIA's NVDEC is a separate engine). It costs about 15ms of CPU per 1440p frame and can't
keep up at 2160p60; the decoder then skips to the next key and the frame on screen is held.

**Setup and adoption.** Worker 0 asks the setup thread (`trigglow-codec-setup`, created on first
use) for an encoder at the frame size it needs and keeps storing MJPEG meanwhile (the bridge; never
a dropped or delayed capture). For each candidate, within about a second: open it, send 8 synthetic
frames (moving gradient and soft noise) with keys forced at 0 and 5, and check 8 packets in order,
keys at 0 and 5 with their parameter sets, a D3D11VA decode of 0–7 and, after a flush, of 5–7, the
sizes, and a PSNR-Y above 30dB (a garbage detector, not a quality bar). What passes is handed over,
open, with its warmed-up decoder, and adopted at a frame boundary on a new chain starting with a
key. A session registry remembers what passed (a new size then only needs a 2-frame reopen), what
failed at which size, and what failed hard. The setup asks a cancellation predicate between steps
(Disable, shutdown, an MJPEG target, a yield) and closes what it opened on its own thread. Measured
on an RX 7800 XT (AMF): probe 330–380ms at 1440p, reopen about 100ms, every rung of the 2160p, 1440p
and 1080p ladders opens and round-trips, and a D3D11VA change of stream parameters costs 11ms
(15ms at 2160p).

**Encode worker with the hardware codec.** One context (I1). Every frame sent is queued in flight
`{reservation, seq, submit time}` and stored when its packet comes out; with frames in flight and no
job the worker polls every 2ms. A packet out of order, an encoder error, or 500ms with frames in
flight and no packet while frames keep coming (a `Send()` attempt in the last 250ms, so a render
pause isn't a stall) is a hard error: what was in flight is lost and the encoder goes. A packet that
doesn't fit the arena, or whose slot was reused, breaks the chain: the slot is `Dropped`, the next
`Send()` forces a key, and P-frames aren't stored until then (I3). A new frame size (a rung, an OBS
video reset) drains the old encoder (at most 50ms), bridges with MJPEG and asks the setup for the
new size. The worker's own time in `Send()`/`Receive()`/copy/publish feeds the degradation; the
latency from `Send()` to packet and the frames in flight feed the codec policy (I6).

**Decode worker.** One decoder per codec found in the ring, the hardware one handed over warmed up.
Starting, after a pause or when it falls behind onto a P-frame, it plans from the key (I5): decodes
the catch-up unshown and then the frame due, or waits for the next key; the wait is published so the
graphics thread counts those holds apart (`holdsCatchUp`), never as decode misses. A frame-size or
codec change at a chain boundary only needs a flush, or the other codec's decoder. Idle decoders
close after 3 s.

**Storage codec policy** (`src/storage-codec-policy.*`, pure, `tests/storage_policy_tests.cpp`),
evaluated on the graphics thread every 250ms, before the degradation policy:

| State | Meaning |
|---|---|
| `Mjpeg` | MJPEG only: `buffer_codec` `mjpeg`, sync mode, no GPU, or no encoder works |
| `Probing` | The setup looks for an encoder; MJPEG bridges |
| `Hw` | The hardware encoder stores the frames |
| `HwGuarded` | Its engine warned: one rung down, no going up for 5 minutes |
| `Yielded` | An OBS output starting took the NVENC session |
| `Evacuated` | Back on MJPEG until the next `Enable()` (no second try in the same cycle) |

- **Inputs.** `engineShared` (`ComputeEngineShare()`): the buffer's encoder is of the same family as
  an active OBS video encoder (`FamilyFromObsEncoderId()`: `*nvenc*`, `*amf*`, `*qsv*`, CPU ones),
  or its decode runs on an AMD or Intel GPU whose engine encodes for OBS; unknown counts as shared.
  OBS's skipped frames for encoding lag: the main mix (`video_output_get_skipped_frames()`, read on
  the graphics thread) plus the mixes of OBS's encoders (GPU rescaling, Enhanced Broadcasting
  tracks), read once a second on the UI thread and sent with `report_encoder_lag`. An OBS output
  stopping with `OBS_OUTPUT_ENCODE_ERROR` (every active output with a video encoder has its stop
  signal watched) arrives with `report_obs_encode_error`. Hard errors of the encoder or the hardware
  decoder come from the pipeline.
- **Rules**, highest first: a hard error evacuates at once (an encoder's own failure blacklists it
  for the session). On a shared engine, an OBS encode error, or at least one frame skipped by OBS in
  the last 2s, evacuates at once, never waiting for the cooldown (an OBS encode error reported while
  probing or yielded is kept until the hardware stores again). The early warning, on a shared engine
  only, held for 1s over at least 20 packets and 2s after the previous transition: latency p95 above
  2.5 capture intervals (42ms at 60fps; AMF measured 20–21ms) or 3 frames in flight. It lowers one
  rung (fewer pixels is what frees the engine: −31% per ×0.83 rung) and vetoes going up for 5
  minutes; with no rung left above the floor it evacuates. On an engine OBS doesn't share, the
  buffer can't cause OBS's encoding lag, so none of this applies (the degradation still handles CPU
  and graphics-thread load).
- **Evacuation** retargets the pipeline at MJPEG in place: the hardware encoder is drained (50ms)
  and closed on the reaper, the next frames are MJPEG keys on a new chain, the ring holds both, and
  playback goes on without a gap. With the decode on the shared engine (AMD, Intel) and OBS's lag as
  the cause, the hardware frames left in the ring are decoded in software. RAM grows to MJPEG's
  level over the next `delay` seconds, within the budget. The WARN: `delay buffer codec <encoder> ->
  mjpeg: <why> …; buffer encoder latency p95 N ms -- the delay itself is unchanged, …`.
- **NVENC sessions.** On `OBS_FRONTEND_EVENT_{STREAMING,RECORDING,REPLAY_BUFFER}_STARTING`,
  `BufferModeController` calls the filter's `yield_hw_encoder` proc, which acts only when an NVENC
  session may be open or being opened; worker 0 (or the setup) closes it and stores MJPEG. The UI
  waits for that at most 100ms, and only when OBS's NVENC encoders, the starting one and the
  buffer's add up to 4 or more. 3s after the output started (10s after the yield at most) it calls
  `resume_hw_encoder`, OBS's encoders are recounted, and the policy goes back to `Probing`.

**Storage class and sizing.** `BufferModeController` fixes the cycle's class at `Enable()` and
writes it into the filter (`set_storage_class` proc and the `storage_class` setting, I9):

| Class | When | Assumed ratio | Slack |
|---|---|---|---|
| `intra` | `buffer_codec` `mjpeg`, `codec_workers` off, no GPU, Linux (macOS: ratio 1) | 5x | `ceil(fps / 4)` |
| `intra-gop` | Hardware may take over, but no encoder passed its probe yet this session (the first cycle) | 5x | `max(ceil(fps / 4), GOP)` |
| `inter` | A hardware encoder passed its probe this session | 12x | `max(ceil(fps / 4), GOP)` |

`ApplyStorageClass()` sets both knobs for `EnsureRingSized()` and for `EstimateBufferFit()`, so the
ring, the dock's estimate and the audio delay keep one whole-second delay. The ratio only matters
when RAM is short (4K, long delays, 8–16GB machines): 12x fits 2.4 times the frames of 5x, and holds
even if the cycle ends on MJPEG (13–17x measured); with worse content the arena drops frames (holds)
instead of growing. The arena's block size follows the same expected bytes (§4.3).

**`buffer_codec`** (hidden, `buffer-mode-settings.json`). `plugin-main.cpp` reads it at load with
`storage::ParseBufferCodec()`: absent (or JSON null) is `"auto"`; only the exact lowercase strings
`"auto"` and `"mjpeg"` count; anything else (`"MJPEG"`, `"hevc"`, `false`, `0`) stays `"auto"` with
a WARN and is dropped from the file at exit; it is written back only when `"mjpeg"`. The controller
pushes it to the filter at every `Enable()` (`SetBufferFilterCodec()`), and `codec_workers=false`
wins over it. No filter property, no dock control.

**Telemetry.** At setup: `storage codec: auto -> hevc_amf (AMF, QP 20, GOP 30 = 0.50 s, no B-frames,
decode d3d11va on "<adapter>") at WxH; probe N ms (open N ms on its adapter, 8/8 packets, IDR
in-band ok, fresh-decoder start ok, psnr N dB); tried: …; engine shared with OBS: yes|no|unknown;
NVENC sessions by OBS: N`, or `storage codec: auto -> mjpeg at WxH (…)` with the reason. Every
telemetry window, right after `pipeline check`:

```
storage codec check (periodic): codec=hevc_nvenc(qp20,gop30) dec=d3d11va | hwenc lat
p50/p95/max=…ms inflight max=N send p95=…ms | keys=N forcedIdr=N chainBreaks=N orphanP=N catchUps=N
(frames N, max Nms) noKeyWaits=N noKeySkips=N | bridge frames=N codecChanges=N hardErrors=N |
policy=Hw guard=off | obs encode-lag skipped main+N mixes+N
```

(one line; a short `codec=mjpeg(q5,huff=default) dec=mjpeg policy=Mjpeg | obs encode-lag …` when the
whole cycle is MJPEG). `PipelineHealth` v2 adds the codec, the encoder's name, hardware decode, the
policy state and its change count, for support diagnostics only; `get_pipeline_state` takes the
caller's struct size and never copies past it. The dock is unchanged.

**Platforms and licensing.** Windows only: the vendored BtbN FFmpeg (LGPL 3.0, no `--enable-gpl` or
`--enable-nonfree`, checked in CI) already has the NVENC, AMF and Quick Sync encoders and D3D11VA in
`avcodec-63.dll`/`avutil-61.dll`, so no new DLL ships; the encoders' runtimes come with the GPU
driver; `data/licenses/LICENSE-ffmpeg.txt` travels with the DLLs. Linux keeps MJPEG over the
system's libavcodec (VAAPI encoders need hardware frames, and Linux hasn't been run live); macOS
stores raw I420 as before. Hardware codecs are licensed by the GPU's maker; the software HEVC/H.264
decoder was already in the shipped avcodec (this is a note, not legal advice).

**Measured** (storage benchmark built from the plugin's sources, outside OBS: Ryzen 7 7700X, RX 7800
XT with AMF, synthetic gameplay-like content at 60fps, private working set of the buffer pipeline
without audio; v0.7.0 build, `auto` against `mjpeg`):

| Output | Delay | MJPEG | Hardware HEVC | Less | PSNR-Y MJPEG / HEVC | Codec CPU MJPEG / HEVC |
|---|---|---|---|---|---|---|
| 1080p60 | 30s | ~395MB | ~190MB | −52% | 40.4–40.5 / 45.3–45.4 dB | 0.45 / 0.18–0.24 cores |
| 1080p60 | 60s | ~750MB | ~285MB | −62% | 40.4 / 45.1–45.3 dB | 0.48 / 0.19–0.21 cores |
| 1440p60 | 30s | ~635MB | ~325MB | −49% | 40.1–40.3 / 44.7–45.1 dB | 0.78–0.88 / 0.20–0.32 cores |
| 1440p60 | 60s | ~1200MB | ~520MB | −57% | 40.1–40.3 / 44.8–45.0 dB | 0.58–0.82 / 0.22 cores |
| 2160p60 | 30s | ~1250–1275MB | ~590MB | −53% | 40.9 / 45.2–45.3 dB | 1.6–2.0 / 0.60–0.66 cores |
| 2160p60 | 60s | ~2315MB | ~880–920MB | −60 to −62% | 40.8 / 45.2 dB | 2.0 / 0.39–0.63 cores |

- v0.6.0 took ~661MB (1440p60, 30s; ~685MB with its GPU-side objects) and ~1232MB (60s) with MJPEG.
- The hardware encoder's latency is about one frame (p50 19–24ms, p95 20–26ms in most runs, 39ms in
  one 2160p60 run), with 1 frame in flight. Its probe and adoption cost no capture.
- Next to a 1440p60 stream proxy encoded in real time with `h264_amf` on the same GPU (14 Mbps CBR,
  quality preset, 2 B-frames), both kept 60fps: the stream at 1.01x real time, and the buffer's
  encode and decode at ~30% of the video engine.
- A 500ms render pause: 1–2 holds in the 2s after it, the first fresh frame 33–42ms after it, one
  catch-up of 11 frames (22–25ms). Switching MJPEG ↔ hardware every 10s for a minute: 5 codec
  changes, 14 bridge frames, 0 hard errors.
- After Disable, the process went back to within 13–25MB of its baseline, with no hardware encoder
  left open.
- **Not measured**: NVIDIA's NVENC and NVDEC (bytes per frame, and so RAM, can differ), Intel's
  Quick Sync, the VRAM the hardware codec takes, and anything inside a real OBS.

**Tests.** `storage_codec_tests` (frames in flight from 0 to 16, `ChainTracker`, Annex B parameter
sets with 3- and 4-byte start codes and truncated packets, NV12 split, `ParseBufferCodec()`,
`RankCandidates()` with and without Quick Sync, `FamilyFromObsEncoderId()`, engine sharing, a
single-threaded simulation with random losses), `storage_policy_tests` (priorities, the
shared-engine rule, cooldown, veto, yields, minimum samples, a size no encoder opens at, an hour at
the edge of the thresholds with at most one transition), `pipeline_gop_stress_tests` and
`adversarial_gop_tests` (the real pipeline with a fake inter-frame codec,
`tests/fake-gop-codec.hpp`, whose decoder flags any P-frame decoded without its reference: latency
0–3 frames, GOPs of 2–8, losses, codec and size changes, render pauses, setups of 0–200ms that
sometimes fail, evacuations, yields, broken encoders and decoders, a slow software decode; never a
corrupt picture, chains from a key, one thread per codec object and never the graphics thread,
latency never counted as load), and `hw_storage_codec_tests` (Windows, local builds with the
plugin's FFmpeg in `.deps`: the real FFmpeg with failures injected, failed opens, a cancelled setup,
a decoder that fails to open). The ThreadSanitizer job runs `pipeline_gop_stress_tests` too.

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
plugin-main.cpp              → obs_module_load/unload, wiring for all components, settings file
                                (hidden buffer_output_cap / codec_workers / buffer_codec, .invalid
                                set-aside)
buffer-mode-controller.*      → the state machine described in §3.3 (Inactive/Filling/Active/Error)
obs-frontend-bridge.*         → the only layer that touches obs-frontend-api.h + filter/scene wiring
video-delay-filter.*          → §3.1 -- capture, I420 conversion, readback, playback and the small
                                fixed GPU object pool; owns the ring, the codec pipeline and the
                                degradation and storage codec policies (§4.5 -- perf check /
                                pipeline check telemetry; §4.8 -- storage codec check)
codec-pipeline.*              → §4.6, §4.8 -- encode/decode worker threads, the reaper (v0.6.0)
                                and the storage codec's setup thread (v0.7.0)
storage-codec.hpp             → §4.8 -- storage codec interface, raw codec, chain bookkeeping,
                                invariants I1-I9 (v0.7.0)
hw-storage-codec.*            → §4.8 -- hardware HEVC/H.264 encoders (NVENC, AMF, Quick Sync),
                                D3D11VA and emergency software decoders, the probe (Windows, v0.7.0)
storage-codec-select.*        → §4.8 -- buffer_codec parse, OBS encoder families, candidate
                                ranking, engine sharing; pure (v0.7.0)
storage-codec-policy.*        → §4.8 -- the storage codec policy; pure (v0.7.0)
annexb.*                      → §4.8 -- Annex B parameter sets in band; pure (v0.7.0)
mjpeg-codec.*                 → §4.2 -- MJPEG over the vendored FFmpeg, one instance per thread
                                (Windows/Linux only)
frame-ring.*                  → §4.6, §4.8 -- ring of slot headers with a state machine (v0.6.0),
                                per-packet codec/key/chain/seq and decode start plans (v0.7.0)
frame-arena.*                 → §4.3 -- block arena for the frame payloads (v0.6.0)
degradation-policy.*          → §4.7 -- adaptive degradation, pure (v0.6.0)
pipeline-health.hpp           → §4.7 -- what the dock reads about the pipeline (v0.6.0)
perf-stats.*                  → §4.5 -- fixed-size percentile series and rolling windows
buffer-fit.*                  → §3.5 -- pure ring sizing math (output ceiling, quality floor, RAM
                                budget, exact fps, whole-second effective delay), no libobs/FFmpeg
encoder-check.*               → §3.5 -- pure decision behind the "encoder above the buffer" WARN
                                (which encoders see the main canvas) and the encoders' ceiling
                                (v0.7.0), no libobs dependency
audio-delay-filter.*          → §3.2 -- per-leaf-source audio delay ring
overlay-badge.*               → §3.6 -- paints the live overlay as one PNG (QPainter, UI thread)
overlay-layout.*              → §3.6 -- pure placement of the overlay on the canvas
auto-names.*                  → §3.7 -- names of the managed objects and their migration
hardware-info.*               → total system RAM query; the GPU adapters (DXGI vendor, name, VRAM)
                                for the storage codec (v0.7.0)
win-http.* / api-client.*     → §3.8 -- the one hardened HTTPS path and the api.trigglow.com client
                                (Windows; "unavailable" on macOS/Linux until they get an HTTP
                                client)
json.*                        → §3.8 -- strict, bounded JSON reader/writer
auth-manager.* / auth-client.* → §3.8 -- sign-in (device link v2), renewal, sign-out, v0.5.x
                                exchange, 401/403 rules; the account gate (docs/ACCOUNT_GATE.md)
token-store.*                 → §3.8 -- the plugin's sign-in, encrypted with CryptProtectData
support-client.* / support-service.* → §3.8 -- Trigi and ticket calls; threads, one Trigi
                                request at a time, local choices, log lines
support-diagnostics.* / diagnostics-collector.* → §3.8 -- the opt-in technical data v1
chat-text.*                   → §3.8 -- pure sanitizer and parser of Trigi's answers
trigi-page.* / trigi-widgets.* / trigi-states.* → §3.8 -- Trigi's page, its safe view and its
                                state decisions
report-bug-dialog.*           → §3.8 -- "Report a problem": Trigi, signed-in and guest tickets
bug-report.* / log-redact.*   → §3.8 -- OBS's current log, read and redacted in memory
update-checker.*              → latest GitHub release check; opens only this repo's releases
background-tasks.* / cancel-token.* / ui-tasks.* → §3.8 -- cancellable background jobs, owners
                                and the bounded shutdown on unload
scene-combo-box.*             → dock combo box populated from obs_frontend_get_scene_names()
settings-ui.*                 → the Qt dock (live/loading scene, seconds, quality, fit estimate,
                                Enable/Disable, live fill countdown, locked fields, degradation
                                notice)
ui-tokens.* / ui-theme.*      → colors derived from OBS's palette (WCAG AA), style sheet, bundled
                                fonts and recolored SVG icons
hotkeys.*                     → registration of the 3 native OBS hotkeys, wired to
                                BufferModeController's Enable()/Disable()/Toggle()
logging.*                     → logging wrapper with a component prefix
delay-controller.*            → §6 -- legacy reconnect-mode logic, present but unused
tests/                        → 23 suites, built only with -DTRIGGLOW_BUILD_TESTS=ON or on their own
                                (cmake -S tests): buffer_fit, encoder_check, ui_tokens, auto_names,
                                overlay_layout, frame_arena, frame_ring, degradation_policy,
                                pipeline_stress, storage_codec, storage_policy, pipeline_gop_stress,
                                adversarial_gop, background_tasks, json, log_redact, chat_text,
                                support_diagnostics, api_client, token_store, auth_client,
                                support_client, trigi_states (the client ones with a scripted
                                network stand-in); plus hw_storage_codec on Windows when the
                                plugin's FFmpeg is in .deps
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
