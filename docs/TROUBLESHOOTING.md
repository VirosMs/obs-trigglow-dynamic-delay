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
  `[trigglow-dynamic-delay]` around the time `Error` appeared — they carry more detail than the
  short message the dock can show.

## The delayed video looks lower quality than expected
- This is buffer sizing behavior, not a bug: `EnsureRingSized()` computes a RAM budget from your
  machine's total system RAM (50% of it, clamped between 1GB and 24GB) and never lets the buffered
  video drop below the **minimum quality** you selected (480p/720p/1080p) — instead, if your requested
  seconds × that quality don't fit the budget, it silently shortens the **actual buffered duration**.
  So if the picture itself looks soft, it's very unlikely to be the plugin lowering quality; check
  your OBS canvas/output resolution and encoder settings instead.
- To see whether your requested combination of seconds + quality actually fits your hardware, watch
  the dock's live fit estimate line *before* pressing Enable — it warns (non-blocking) when the full
  requested duration won't fit at the chosen quality, and tells you what will actually be achieved.
  See `docs/SPEC.md` §3.5 for the full sizing logic.
- If you want the full requested duration guaranteed, lower the quality floor, lower the requested
  seconds, or free up system RAM (close other RAM-heavy applications) before pressing Enable.

## The buffer never seems to fill / `Filling` never reaches `Active`
- Check the OBS log for `[trigglow-dynamic-delay]`-prefixed warnings around the time you pressed
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
  Device-Remove/Reset risk described in `docs/SPEC.md` §7, and any dedicated automated test suite).
- A compile error on your own machine is valuable information — it's likely a difference between
  your exact OBS/Qt version and the ones pinned in `buildspec.json`. Open an issue in the repository
  with the full error log.

## I don't know if the problem is the plugin or my streaming configuration
- Disable the plugin (temporarily remove it from the plugins folder) and confirm that streaming
  works fine without it. If the problem persists without the plugin, it's not the plugin's fault —
  and this is a particularly clean test for this plugin specifically, since it never touches the
  streaming output at all; if you have streaming trouble, it did not cause it.
