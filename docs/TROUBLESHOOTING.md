*[Versión en español](TROUBLESHOOTING-es.md)*

# Basic troubleshooting — Trigglow Dynamic Delay

## The dock doesn't show up in OBS
- Go to `View → Docks` and check that "Trigglow Dynamic Delay" is checked.
- Check the OBS log (`Help → Logs and Profiles → View Current Log`) for the line
  `[obs-trigglow-dynamic-delay] loaded successfully`. If it's not there, the plugin didn't load —
  check that the binary is in the correct plugins folder for your OS (see README) and that the
  architecture (64-bit) matches your OBS installation.

## The "Enable" button is greyed out
- `Enable` stays disabled until you've picked a **live scene** in the dock's scene combo box — the
  plugin refuses to arm itself with nothing to buffer, rather than letting you press it and land in
  `Error`. Pick your live scene from the dropdown; `Enable` becomes clickable as soon as one is
  selected.
- `Enable`/`Disable` are also both disabled while a stream-safe operation is already in progress
  (the dock shows this via the other controls — scene combos, the seconds spinner, and the quality
  combo — being greyed out too during `Filling`/`Active`). This is expected: change scenes/seconds/
  quality only while `Inactive`.

## The dock shows an "Error" state
- The dock's status line reads `● Error` with a human-readable detail line underneath it explaining
  what went wrong (e.g. no live scene resolves to an actual OBS scene anymore, or a source that used
  to exist was removed). This is a controlled, crash-free failure state, not a hang.
- Press `Disable` to reset back to `Inactive`, fix whatever the detail line describes (usually:
  re-pick the live scene, since a saved scene name may no longer exist), and press `Enable` again.
- If the detail line is unclear or seems wrong, check the OBS log for lines prefixed
  `[obs-trigglow-dynamic-delay]` around the time `Error` appeared — they carry more detail than the
  short message the dock can show.

## "Encoding overloaded" (or lagged frames) as soon as the delay is on

**What causes it.** While the delay is on, the plugin compresses every captured frame (MJPEG) and
decompresses the delayed one inside OBS's graphics thread — the same thread that has to finish each
frame of your stream in time (16.7 ms per frame at 60 fps, 33.3 ms at 30 fps). When that work takes
too long, OBS falls behind and shows "Encoding overloaded" (frames skipped due to encoding lag) or
rendering lag (frames missed). Up to v0.5.0 the buffer was stored at your **canvas** resolution, so
with a 2560x1440 canvas and a 1920x1080 output each frame cost 14.3/17.4 ms p50/p95 there — most of
a 60 fps frame — even though OBS throws those extra pixels away when it scales to the output.

**v0.5.1 fixes the canvas-larger-than-output case.** The buffer is never stored above OBS's output
resolution (`Settings → Video → Output (Scaled) Resolution`), and compression uses several threads:
the same 1440p-canvas / 1080p-output setup now costs ~5.2/5.9 ms per frame. Update to v0.5.1 first
(the dock shows a notice when a newer version exists). If you lowered your canvas to work around
this, you can set it back.

**If it still happens on v0.5.1:**
- **Make the canvas equal to the output if you can** (`Settings → Video`: Base (Canvas) Resolution =
  Output (Scaled) Resolution). A larger canvas no longer costs the plugin anything extra, but OBS
  itself still renders every scene at canvas resolution.
- **Close the projectors, multiview and extra previews you don't need.** Since v0.5.1 each delayed
  frame is decompressed only once however many views are open, but every open view is still extra
  rendering work for OBS.
- **1440p output (not just canvas):** at 30 fps it should work; at 60 fps it may still overload OBS,
  because compression still runs inside the graphics thread. Moving it off that thread, with 1440p60
  as the target, is the plan for the next version (v0.6.0, see `docs/ROADMAP.md`); there's no date
  yet. Until then use a 1080p output for 60 fps, or 30 fps with a 1440p output.
- **Rule out the encoder itself:** if "Encoding overloaded" also shows up with the delay off, the
  plugin isn't the cause — check your encoder preset and settings.
- Still stuck: use the dock's **Report a problem** button after having the delay Active for at least
  30 seconds in the same OBS session. It attaches the current OBS log automatically, including the
  `perf check` lines below.

**Reading the `perf check` line (for support).** While the delay is on (Filling or Active), the
plugin writes one line to the OBS log every 10 seconds, plus a last one when you disable the delay
or close OBS. Example (illustrative values — yours will differ):

```
[obs-trigglow-dynamic-delay] [video-delay-filter] perf check (periodic, 10.0s): buf=1920x1080 (capped to OBS output 1920x1080, canvas 2560x1440) fps=60.00 | render p50/p95/max=5.20/5.90/8.40ms readback=0.35/0.52/1.10 encode=2.10/2.60/4.30 decode=1.70/2.00/3.10 upload=0.40/0.55/1.20 | calls=60.0/s captures=60.0/s uploads=60.0/s holds=0 rawFallback=0 | obs frames+600 lagged+0 skipped+0 | ratio=12.4x (cycle 12.1x) ring=1077MB/1816 slots growths=0
```

| Field | What it tells you |
|---|---|
| `perf check (reason, Ns)` | Why the line was written (`periodic`, `resumed after a pause`, `filter disabled`, `filter destroyed`) and how many seconds it covers. |
| `buf=WxH (…)` | Resolution the buffer is stored at, and why: `capped to OBS output …` (the v0.5.1 cap is active), `not capped …` (usually: the canvas is already equal to or smaller than the output), `cap off (buffer_output_cap=false) …` (cap disabled by hand, see below). `, reduced for RAM` means it also had to shrink to fit the delay in the RAM budget. |
| `render p50/p95/max` | Total time the plugin spends per call on OBS's graphics thread, in ms. Compare it with the frame budget: 16.7 ms at 60 fps, 33.3 ms at 30 fps. A p95 well below the budget means the plugin isn't what's overloading OBS. |
| `readback` / `encode` / `decode` / `upload` | The same, per phase: GPU→CPU copy, compression, decompression, upload of the delayed frame to the GPU. |
| `calls`, `captures`, `uploads` (per second) | Plugin render calls (one per open view: Program, preview, projectors…), frames captured (should be close to your fps) and delayed frames uploaded. |
| `holds` | Times the last frame was repeated because the one due couldn't be shown. An occasional one is harmless; a steadily growing count means capture is falling behind. |
| `rawFallback` | Frames stored uncompressed because compression failed. Should be 0. |
| `obs frames+ lagged+ skipped+` | OBS's own counters over the same window: frames rendered, frames missed due to rendering lag, frames skipped due to encoding lag. Non-zero `lagged`/`skipped` with `render` p95 close to the frame budget points at the plugin; with a low `render`, look elsewhere (game, encoder, GPU load). |
| `ratio`, `ring`, `growths` | Compression ratio (this window, and the whole delay cycle), RAM used by the buffer and its slot count, and how many slots had to grow beyond their budgeted size. |

The `settings loaded` line (when OBS starts) and the `enabled` line (each time you press Enable)
end in `output_cap=on` or `output_cap=off`, so support can tell which sizing was in use.

**Turning the output cap off (`buffer_output_cap`).** Only for someone who records (or sends an
Enhanced Broadcasting track) above the output resolution and prefers that quality for the delayed
part at the cost of CPU. It brings back v0.5.0's sizing, including the risk of "Encoding
overloaded" described above. When it would make a difference, the log warns with a line like
`video encoder "…" (output "…") is 2560x1440, above the delay buffer's 1920x1080 …: its delayed
video is upscaled`.

1. **Close OBS completely.** The setting is only read when OBS starts, and OBS rewrites this file
   when it exits, so a change made with OBS open is lost.
2. Open `%APPDATA%\obs-studio\plugin_config\obs-trigglow-dynamic-delay\buffer-mode-settings.json`
   in a text editor (portable OBS: `config\obs-studio\plugin_config\obs-trigglow-dynamic-delay\`
   inside the OBS folder; macOS/Linux: the same `plugin_config/obs-trigglow-dynamic-delay/` folder
   inside OBS's config directory). If the file doesn't exist yet, open and close OBS once with the
   plugin installed.
3. Add `"buffer_output_cap": false` as one more key, keeping everything else as it is (mind the
   comma on the line before):

   ```json
   {
       "delay_seconds": 30,
       "overlay_corner": 0,
       "buffer_output_cap": false
   }
   ```

   (Your file has more keys and your own values; only the last line is new.)
4. Start OBS. The log shows `output_cap=off` in the `settings loaded` line, and again when you press
   Enable; the `perf check` line says `cap off (buffer_output_cap=false)`.

To undo it: with OBS closed, delete the key (or set it to `true`) and start OBS again.

## The delayed video looks lower quality than expected
- This is buffer sizing behavior, not a bug: `EnsureRingSized()` computes a RAM budget from your
  machine's total system RAM (50% of it, clamped between 1GB and 24GB) and never lets the buffered
  video drop below the **minimum quality** you selected (480p/720p/1080p) — instead, if your requested
  seconds × that quality don't fit the budget, it silently shortens the **actual buffered duration**.
  The one exception, since v0.5.1: the buffer is never stored above OBS's output resolution, so with a
  720p output it's 720p even with a 1080p minimum — OBS doesn't send anything above its output
  anyway. So if the picture itself looks soft, it's very unlikely to be the plugin lowering quality;
  check your OBS canvas/output resolution and encoder settings instead.
- If you **record** at a resolution above your output (e.g. recording rescaled to the canvas), the
  delayed part of that recording is upscaled from the output-sized buffer since v0.5.1; the log
  warns about it. See `buffer_output_cap` in the section above if you prefer the old behavior.
- To see whether your requested combination of seconds + quality actually fits your hardware, watch
  the dock's live fit estimate line *before* pressing Enable — it warns (non-blocking) when the full
  requested duration won't fit at the chosen quality, and tells you what will actually be achieved.
  See `docs/SPEC.md` §3.5 for the full sizing logic.
- If you want the full requested duration guaranteed, lower the quality floor, lower the requested
  seconds, or free up system RAM (close other RAM-heavy applications) before pressing Enable.

## The buffer never seems to fill / `Filling` never reaches `Active`
- Check the OBS log for `[obs-trigglow-dynamic-delay]`-prefixed warnings around the time you pressed
  Enable. Two known causes that log a specific warning instead of silently doing nothing:
  - `Render(): obs_filter_get_target() returned null` — the wrapper scene's filter target
    disappeared (e.g. the live scene was deleted or renamed out from under the plugin after Enable).
  - `Render(): target's base size is 0x0, skipping until it's real` — the live scene hasn't produced
    a real frame size yet (this is normal for the first render or two right after Enable and should
    resolve itself; if it persists, check that the live scene actually has visible, active sources in
    it).
- If neither warning appears and the countdown just seems slow, that's expected: the fill window is
  exactly as long as the `delaySeconds` you requested — a 60-second delay takes 60 real seconds to
  fill before switching to `Active`.

## The status stays stuck in "Filling" far longer than the requested seconds
- This is a symptom, not a documented mode — the plugin has no reconnection or "Applying" state to
  wait on. If `Filling` genuinely never reaches `Active`, follow the "buffer never seems to fill"
  section above and check the OBS log for warnings.

## The hotkey doesn't do anything
- Confirm you assigned it in `Settings → Hotkeys` (searching for "Trigglow") — the plugin registers
  3 hotkeys (`Trigglow: Toggle Dynamic Delay`, `Trigglow: Enable Dynamic Delay`,
  `Trigglow: Disable Dynamic Delay`) with no key assigned by default.
- Try the dock button first to rule out an issue with the plugin's own configuration (e.g. no live
  scene chosen, which also blocks the hotkey's Enable/Toggle from doing anything), as opposed to the
  hotkey/Stream Deck binding itself.

## The Stream Deck button doesn't do anything
- Verify that the combination configured in Stream Deck exactly matches the one assigned in
  OBS.
- See `docs/STREAM_DECK.md` for the full step-by-step flow.

## The plugin doesn't compile
- Check `docs/BUILD_VALIDATION.md` — it documents what's been verified (real CI builds against the
  real OBS SDK and Qt6 on Windows/macOS/Ubuntu, plus live testing inside a real OBS instance on
  Windows) and what hasn't (live testing on macOS/Linux, a confirmed fix for the GPU
  Device-Remove/Reset risk described in `docs/SPEC.md` §7, and any broad automated test suite —
  v0.5.1 only adds unit tests for the buffer sizing math, built with `-DTRIGGLOW_BUILD_TESTS=ON`).
- A compile error on your own machine is valuable information — it's likely a difference between
  your exact OBS/Qt version and the ones pinned in `buildspec.json`. Open an issue in the repository
  with the full error log.

## I don't know if the problem is the plugin or my streaming configuration
- Disable the plugin (temporarily remove it from the plugins folder) and confirm that streaming
  works fine without it. If the problem persists without the plugin, it's not the plugin's fault —
  and this is a particularly clean test for this plugin specifically, since it never touches the
  streaming output at all; if you have streaming trouble, it did not cause it.
