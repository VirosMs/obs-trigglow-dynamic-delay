*[Versión en español](README-es.md)*

# Trigglow Dynamic Delay for OBS

Native OBS Studio plugin that delays your stream's **video and audio together**, by a configurable
number of seconds, from a button, a native OBS hotkey, or a Stream Deck — **without the streaming
output ever being touched.** No reconnection, no cut, at any point, for any reason. No external
app, no web panel, no separate process: everything lives inside the OBS process.
**Status: MVP / v0.1.0 — Early Access.**

Before anything else, read `docs/SPEC.md` (full technical specification of how buffer mode
actually works, and why the obvious "just change OBS's own stream delay live" approach was tried
and rejected) and `docs/BUILD_VALIDATION.md` (what was actually verified, and what wasn't, in this
first release).

## Usage requirements

**OBS Studio 31.1.0 or newer.** The v0.1.0 binary is built against the OBS 31.1.1 sources (see
`buildspec.json`), and it has been verified to load and work correctly on OBS **32.2.2** (OBS's
plugin compatibility mechanism only rejects plugins built against a version *newer* than the one
currently running; it has never been tested on versions older than 31.1.1).

## Build requirements

Based on the official [`obsproject/obs-plugintemplate`](https://github.com/obsproject/obs-plugintemplate)
template, without modifying its build system (`cmake/`, `CMakePresets.json`, `.github/`) except to
enable `ENABLE_FRONTEND_API` and `ENABLE_QT` and register the new source files.

| Platform | Tools |
|---|---|
| Windows | Visual Studio 17 2022, CMake ≥ 3.30.5 |
| macOS | Xcode 16.0, CMake ≥ 3.30.5 |
| Ubuntu 24.04 | CMake ≥ 3.28.3, `ninja-build`, `pkg-config`, `build-essential` |

**Windows is the priority platform for v0.1.0** (as requested by product); macOS/Linux build green
in CI on the template's own build system, but have not been run live in this release.

## Build (Windows)

```powershell
git clone <your-repo>/obs-trigglow-dynamic-delay.git
cd obs-trigglow-dynamic-delay
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

The first configuration step will automatically download the OBS sources and the pre-built
dependencies (`obs-deps`, Qt6) declared in `buildspec.json` — you don't need to install OBS or Qt
yourself separately. Requires an internet connection the first time.

## Build (macOS / Linux)

```bash
cmake --preset macos      # or: ubuntu-x86_64
cmake --build --preset macos --config RelWithDebInfo
```

## Installing the compiled plugin in OBS

1. Locate the generated binary (`.dll` on Windows inside `build_x64/RelWithDebInfo/`, `.plugin`
   on macOS, `.so` on Linux).
2. Copy it into the OBS plugins folder:
   - Windows: `C:\Program Files\obs-studio\obs-plugins\64bit\`
   - macOS: `~/Library/Application Support/obs-studio/plugins/`
   - Linux: `~/.config/obs-studio/plugins/`
3. Restart OBS.

(A one-click installer is out of scope for the MVP — see `docs/ROADMAP.md`.)

## Using the plugin

1. Open OBS. If the **"Trigglow Dynamic Delay"** dock doesn't show up, go to `View → Docks` and
   enable it.
2. In the dock, pick your **live scene** (required — the scene with your real content) and,
   optionally, a **loading scene** to show while the buffer fills instead of staying on the
   undelayed live scene. Set **Delay (seconds)** (1–60) and the **minimum quality** floor
   (480p/720p/1080p) the delayed video must never drop below. The dock shows a live estimate of
   whether your chosen seconds actually fit in RAM at that quality *before* you press Enable — if
   they don't, it warns that the buffered duration will be shortened instead of ever lowering the
   quality, but never blocks your choice.
3. Go to `Settings → Hotkeys`, search for "Trigglow", and assign a key to **Toggle Dynamic
   Delay** (and optionally to Enable/Disable separately).
4. Press Enable (from the dock button, the hotkey, or Stream Deck). The dock shows `Filling` with
   a live countdown while the buffer fills up to the requested delay; once full, it switches
   automatically to `Active` and Program starts showing the delayed video and audio, in sync.
   **The streaming output itself is never touched at any point** — viewers on Twitch/YouTube/Kick/
   wherever never see a cut or a reconnect, whether you press the button before or during a live
   stream.

Pressing Disable frees the RAM buffer immediately and switches back to whatever scene was showing
before you enabled it.

## Mapping a Stream Deck button

No dedicated Stream Deck plugin is needed in v0.1.0:

1. In the Stream Deck software, add the **"System" → "Hotkey"** action (the exact name may vary
   slightly depending on your Stream Deck version).
2. Configure it to send the key combination you assigned to "Toggle Dynamic Delay" in the previous
   step.
3. Keep the OBS window focused when you press the button (Stream Deck sends the keystroke to the
   system; OBS needs to have focus, or at least be running and listening for the global shortcut,
   depending on how you have OBS/your OS configured).
4. Pressing it while already live is exactly as safe as pressing it before going live: OBS moves
   through `Filling` (with the dock's live countdown) to `Active` once the buffer is full — there
   is no reconnection and no visible cut on stream at any point in that transition.

More detailed guide, with example screenshots and troubleshooting: `docs/STREAM_DECK.md`.

## Project structure

```
src/
  plugin-main.cpp                 → obs_module_load/unload, wiring for all components
  buffer-mode-controller.{hpp,cpp} → the state machine (Inactive/Filling/Active/Error) that
                                     orchestrates buffer mode — the only owner of state
  obs-frontend-bridge.{hpp,cpp}    → the only layer that touches obs-frontend-api.h + filter/
                                     scene wiring
  video-delay-filter.{hpp,cpp}     → RAM/NV12 ring buffer for delayed video + its small, fixed
                                     GPU object pool
  audio-delay-filter.{hpp,cpp}     → per-leaf-source audio delay ring, kept in sync with video
  hardware-info.{hpp,cpp}          → total system RAM query, used to size the buffer budget
  scene-combo-box.{hpp,cpp}        → dock combo box populated from OBS's own scene list
  settings-ui.{hpp,cpp}            → the Qt dock (live/loading scene, seconds, quality, fit
                                     estimate, Enable/Disable, live fill countdown)
  hotkeys.{hpp,cpp}                → registration of the 3 native OBS hotkeys, wired to
                                     BufferModeController
  logging.{hpp,cpp}                → logging wrapper with a component prefix
  delay-controller.{hpp,cpp}       → legacy reconnect-mode logic from the abandoned first design
                                     (see `docs/SPEC.md` §5) — present in the repo but never
                                     instantiated by plugin-main.cpp; ships in no current build
docs/
  SPEC.md                  → full technical specification (start here)
  BUILD_VALIDATION.md       → what was actually verified in this release, and what wasn't
  ROADMAP.md                → v0.2, v0.3, and what's out of scope
  STREAM_DECK.md             → detailed Stream Deck guide
  FAQ.md · TROUBLESHOOTING.md
CMakeLists.txt, CMakePresets.json, buildspec.json, cmake/  → the official template's build system
```

## License and repository

GPL-2.0-or-later, inherited from `obsproject/obs-plugintemplate` (see `LICENSE`) — this isn't just
a formality: `libobs` is GPL-2.0, so linking against it carries the same license over to this
plugin, which requires publishing the source code of any distributed binary.

That's why this project lives in **its own repository, separate from Trigglow's main monorepo**
(which stays private) — publishing only the plugin here satisfies the GPL without exposing the
rest of the product. Full detail on this decision in `docs/SPEC.md` §8.
