*[Versión en español](README-es.md)*

# Trigglow Dynamic Delay for OBS

Native OBS Studio plugin that delays your stream's **video and audio together**, by a configurable
number of seconds, from a button, a native OBS hotkey, or a Stream Deck — **without the streaming
output ever being touched.** No reconnection, no cut, at any point, for any reason. No external
app, no web panel, no separate process: everything lives inside the OBS process.
**Status: MVP / v0.7.0 — Early Access.**

As of v0.3.0, the RAM ring buffer is real MJPEG-compressed on Windows and Linux (vendored FFmpeg),
with an automatic, safe fallback to uncompressed storage if the codec isn't available — no quality
floor is ever lowered because of this. Ring slots also now reserve RAM at a budgeted size instead of
the full raw ceiling, growing only when a frame actually needs more room. Measured on a real 30s@1080p60
session: RAM usage for the buffer went from ~6.3GB to ~2.9GB. The exact compression ratio depends
heavily on content and hasn't been measured against real gameplay yet (macOS isn't part of this yet —
it keeps working exactly as before, uncompressed). See `docs/SPEC.md` for the full technical detail.

v0.3.1 fixes the dock's scene selectors coming up blank on a fresh OBS start or plugin reinstall
(saved settings were fine, the UI just wasn't restoring them yet — see `docs/SPEC.md` §3.3), and
gives the Windows installer real Trigglow branding instead of Inno Setup's generic defaults.

v0.3.2 fixes audio noticeably lagging behind video on long/high-quality delays (30s+, 1080p): video
silently shortens its own actual buffered duration when the RAM budget doesn't fit the full request,
but audio always buffered the full seconds regardless — the two drifted apart by exactly that
shortfall. Audio now matches whatever video is actually able to deliver (see `docs/SPEC.md` §3.3).

v0.3.3 adds a free-account gate before Enable (see `docs/ACCOUNT_GATE.md`) — still 100% free, just
a one-time sign-in.

v0.4.0 adds a **"Report a problem"** dock button (creates a real support ticket with the current
OBS log attached automatically), an **in-app update check** against this repo's GitHub releases,
and **real localization** (the dock, dialogs, filter/hotkey names, and status messages now follow
OBS's own language setting instead of being hardcoded Spanish). It also retunes the RAM ring's
compression assumption using the first real measurement of live gameplay compression ratios
(~11-15x, not the 3x conservatively assumed since v0.3.0) — measured total OBS process memory at
30s/1080p60 went from ~2.8GB to ~2.1GB — and redesigns the dock's layout. See `CHANGELOG.md` for
all of the above.

v0.5.0 adds an optional **"Delay Ns" overlay** (with a four-corner selector), a single
Enable/Disable button and a responsive dock, and fixes the video delay drifting from real time
(it now follows the wall clock like the audio does), the chosen live scene being ignored when
the wrapper scene was reused, and the Windows installer missing the Spanish locale. See
`CHANGELOG.md`.

v0.6.0 fixes OBS showing **"Encoding overloaded"** as soon as the delay was enabled with a canvas
larger than the output (e.g. 2560x1440 canvas, 1920x1080 output): the buffer is never stored above
OBS's output resolution, and compression and decompression run on the plugin's own threads instead
of OBS's graphics thread — at 1440p60 the plugin's work there drops from ~14/17 ms to
~1.2–1.7/1.6–2.1 ms per frame (p50/p95, offline benchmark), so **a 1440p output at 60 fps is
supported**. If your computer can't keep up, the plugin lowers the buffer's quality step by step
(never below your minimum quality, never cutting the delay) and says so in the dock. It also uses
less RAM (~0.6–0.8 GB instead of ~1.9 GB for 30 s at 1440p60), keeps audio and video on exactly the
same delay, really switches off delay filters left on when OBS was closed while Active, and brings a
redesigned dock, report dialog and live overlay aligned with trigglow.com. The objects the plugin
manages are renamed from "(no tocar)" to "(auto)": update any Stream Deck or obs-websocket action
that uses the old names; the live overlay is signed "By Trigglow". The minimum quality now goes up
to 1440p (2K) and 2160p (4K). (A v0.5.1 hotfix was prepared but never published; it's part of
v0.6.0.)
See `CHANGELOG.md` and `docs/TROUBLESHOOTING.md`.

v0.6.0 also brings **Trigi**, Trigglow's AI support assistant, to "Report a problem" (Windows,
signed in): ask about Dynamic Delay in the dialog and turn the conversation into a ticket if it
doesn't sort it out. Your OBS log is never sent to the AI, and the OBS technical data is optional,
with a preview. **Sign-in is new and safer** (a code to compare on trigglow.com, the plugin's own
sign-in encrypted for your Windows user, revoked by Sign out), reports go to your account with the
OBS log redacted and only if you agree, and the plugin's network calls are hardened. **Updating
is required: once v0.6.0 is published, v0.5.x and earlier can no longer sign in, and the sign-ins
they already have stop working after a transition period.** Sign-in, Trigi and reports from OBS
work only on Windows (still the case in v0.7.0); macOS and Linux get them with the HTTP client
planned next (`docs/ROADMAP.md`). See `docs/FAQ.md`.

v0.7.0 is an optimization release: **on Windows the delay buffer takes about half the RAM.** The
frames waiting out the delay are stored with your GPU's hardware video encoder (HEVC through NVENC,
AMF or Quick Sync, or H.264 where the GPU has no HEVC) and decoded on the GPU too, instead of being
compressed one by one as MJPEG pictures. At 1440p60 the buffer goes from ~675 MB to ~325 MB for
30 s, and from ~1.2 GB to ~520 MB for 60 s (measured outside OBS with an AMD GPU; NVIDIA's NVENC
isn't measured yet). The delayed picture is closer to the original and the plugin uses less CPU. To
keep "Encoding overloaded" away when the buffer shares the video engine with OBS's own encoder, it
lowers its resolution when that engine gets busy and goes back to MJPEG at once if OBS starts
skipping frames, without touching the delay. Without a hardware encoder it stores MJPEG as before.
No new controls or warnings; a hidden `buffer_codec` setting forces MJPEG for support. Linux and
macOS are unchanged. See `CHANGELOG.md`, and the RAM table in `docs/FAQ.md`.

Before anything else, read `docs/SPEC.md` (full technical specification of how buffer mode
actually works, and why the obvious "just change OBS's own stream delay live" approach was tried
and rejected) and `docs/BUILD_VALIDATION.md` (what was actually verified, and what wasn't, in this
first release).

## Usage requirements

**OBS Studio 31.1.0 or newer**, on every platform. The plugin is built against the OBS 31.1.1
sources (see `buildspec.json`; since OBS 32.0, OBS refuses a plugin built against a *newer* OBS
than the one running), and from v0.7.1 it refuses to load in an older OBS, with
`requires OBS Studio 31.1.0 or newer, running X` in the OBS log; the Windows installer checks it
too. Tested live up to OBS **32.2.2** on Windows. OBS 33 (in release candidate) is supported by the
v0.7.1 install locations but not tested live yet. The minimum is set in one place,
`buildspec.json`'s `obsCompat`.

| Plugin | Minimum OBS | Tested up to | Win | macOS | Linux |
|---|---|---|---|---|---|
| 0.7.1 | 31.1.0 | 32.2.2 | Installer, `.zip` (OBS 33 location) | `.pkg` | `.deb`, `.rpm`, `.pkg.tar.zst`, `.tar.xz` |
| 0.7.0 | 31.1.0 | 32.2.2 | Installer, `.zip` ("Legacy" in OBS 33) | `.pkg` | `.deb`, Ubuntu 24.04 with OBS 32.2 only |

Linux: one x86_64 build for Ubuntu 24.04 LTS (OBS from the OBS PPA), Ubuntu 26.04 LTS and 26.10,
Fedora, Arch and their derivatives (`.deb`, `.rpm`, `.pkg.tar.zst`), and a per-user `.tar.xz` for
the rest, with Qt 6.4 or newer. Debian 13 ships OBS 30.2, below the minimum: the `.deb` refuses to
install until OBS 31.1 or newer is there. The Flatpak OBS isn't supported. Details in
`docs/INSTALL_GUIDE.md`.

**Resolution and frame rate.** Any canvas: the delay buffer follows OBS's **output** resolution
(`Settings → Video → Output (Scaled) Resolution`) and never goes above it, so a canvas larger than
the output costs the plugin nothing extra. 1080p and **1440p outputs at up to 60 fps** are
supported since v0.6.0 (figures in `CHANGELOG.md`, measured offline on an 8-core/16-thread CPU). On
a machine that can't keep up, the buffer drops to a lower resolution (never below the minimum
quality you choose) and, last, to half the capture frame rate, instead of overloading OBS or cutting
the delay; the dock shows an amber notice while that happens.

**Memory.** The buffer's RAM budget is half of your system RAM, between 1 GB and 24 GB (on macOS and
Linux, where RAM isn't detected yet, a fixed 2 GB). What the buffer really takes depends on what's
on screen and on how it's stored. Measured outside OBS with gameplay-like content at 60 fps (the
buffer only; audio adds about 11 MB per source with sound for 30 s):

| Output | 30 s, hardware HEVC (Windows, v0.7.0) | 30 s, MJPEG | 60 s, hardware HEVC | 60 s, MJPEG |
|---|---|---|---|---|
| 1080p60 | ~190 MB | ~400 MB | ~290 MB | ~750 MB |
| 1440p60 | ~325 MB | ~640 MB | ~520 MB | ~1,200 MB |
| 2160p60 | ~590 MB | ~1,250 MB | ~920 MB | ~2,300 MB |

The hardware figures are from an AMD GPU (AMF); NVIDIA's NVENC isn't measured yet. MJPEG is what
Linux, Windows without a hardware encoder, and the `buffer_codec` support setting use. If the
seconds you ask for don't fit at your minimum quality, fewer seconds are buffered (the dock tells
you before you enable it). Compression needs FFmpeg, bundled on Windows and Linux; on macOS the
buffer is stored uncompressed. Details in `docs/FAQ.md`.

**A free trigglow.com account, on Windows.** `Enable delay` needs you to sign in once from the
dock (in your browser; the plugin stays 100% free). Sign-in works only on Windows for now (v0.6.0
and v0.7.0): macOS and Linux don't have the plugin's HTTP client yet, planned next (see
`docs/ROADMAP.md`). See `docs/ACCOUNT_GATE.md`.

## Network use and privacy

- **On every OBS start**: one check of your sign-in with `api.trigglow.com` (plus a renewal when
  it's due) and one check of this repository's latest release on `api.github.com`. Nothing else
  runs in the background.
- **Only when you use them**: signing in and out, asking Trigi and sending a report, all to
  `api.trigglow.com`. The plugin opens only its own fixed pages on www.trigglow.com and this
  repository on GitHub, in your browser.
- **How**: HTTPS only, redirects never followed, the sign-in sent only to `api.trigglow.com`,
  stored encrypted for your Windows user, and never written to the OBS log, like the sign-in
  codes, IDs and what you write to Trigi.
- **Trigi** receives what you type and, only if you tick it, a small set of OBS technical numbers
  and states (never names, paths, URLs or keys), shown before you send it. **Your OBS log is never
  sent to the AI**; it's attached to a ticket only if you tick its box, which you can do once
  you've seen it in **See what is sent**. Your Windows user name, URLs, keys, emails and IPs are
  hidden in it, but names of scenes, sources, audio devices and windows can still show up.
  Conversations are kept for up to 12 months and can be deleted earlier on trigglow.com. Details
  in `docs/FAQ.md` and the
  [Privacy Policy](https://www.trigglow.com/legal/privacy).

## Build requirements

Based on the official [`obsproject/obs-plugintemplate`](https://github.com/obsproject/obs-plugintemplate)
template, without modifying its build system (`cmake/`, `CMakePresets.json`, `.github/`) except to
enable `ENABLE_FRONTEND_API` and `ENABLE_QT` and register the new source files.

| Platform | Tools |
|---|---|
| Windows | Visual Studio 17 2022, CMake ≥ 3.30.5 |
| macOS | Xcode 16.0, CMake ≥ 3.30.5 |
| Linux (Ubuntu 24.04) | CMake ≥ 3.28.3, Ninja, GCC 13, `nasm`, `jq`, Qt 6.4: `.github/scripts/linux/install-build-deps.sh` |

**Windows is the priority platform for v0.7.0** (as requested by product); macOS/Linux build green
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
cmake --preset macos
cmake --build --preset macos --config RelWithDebInfo
```

On Linux the plugin links its own static FFmpeg and builds libobs from the pinned OBS sources on
the first configure, as CI does in an `ubuntu:24.04` container:

```bash
sudo .github/scripts/linux/install-build-deps.sh   # Ubuntu 24.04 packages
.github/scripts/linux/build-ffmpeg.sh              # FFmpeg from buildspec.json, LGPL, MJPEG only
.github/scripts/linux/build.sh RelWithDebInfo      # stages release/stage/usr
.github/scripts/linux/package.sh                   # .deb, .rpm, .pkg.tar.zst, .tar.xz
```

## Installing the plugin

**Windows: use the installer.** Download `obs-trigglow-dynamic-delay-0.7.1-windows-x64-setup.exe`
from the [latest release](https://github.com/VirosMs/obs-trigglow-dynamic-delay/releases/latest)
and run it — it installs into OBS's plugin folder for all users and moves an install of v0.7.0 or
earlier out of OBS's own folder. It isn't code-
signed yet (Early Access), so Windows SmartScreen will warn you before letting it run. **Full guide
with screenshots for every step, including the SmartScreen warning: `docs/INSTALL_GUIDE.md`.**
Install it before you go live, not mid-stream — it needs OBS closed to overwrite its files.

**Linux: use your distro's package.** `obs-trigglow-dynamic-delay-0.7.1-x86_64-linux-gnu.deb`
(Debian, Ubuntu, Mint, Pop!_OS: `sudo apt install ./<file>.deb`), `.rpm` (Fedora, openSUSE:
`sudo dnf install ./<file>.rpm`) or `.pkg.tar.zst` (Arch, Manjaro, EndeavourOS:
`sudo pacman -U <file>.pkg.tar.zst`), which pull in what they need. Any other distro: the `.tar.xz`
and its `./install.sh`, for your user only.

**macOS:** the `.pkg` from the same release.

**Where the files go (v0.7.1):**

| Platform | OBS 33 and later | OBS 31 and 32 (legacy location) |
|---|---|---|
| Windows | `C:\ProgramData\obs-studio\plugins\obs-trigglow-dynamic-delay\obs-trigglow-dynamic-delay.dll`, `data\` next to it | `...\obs-trigglow-dynamic-delay\bin\64bit\obs-trigglow-dynamic-delay.dll`, same `data\` |
| Linux, package | `<libdir>/obs-modules/plugins/obs-trigglow-dynamic-delay.so`, `/usr/share/obs/obs-modules/plugins/obs-trigglow-dynamic-delay/` | symlinks in `<libdir>/obs-plugins/` and `/usr/share/obs/obs-plugins/` |
| Linux, `.tar.xz` | `~/.local/share/obs-studio/plugins/obs-trigglow-dynamic-delay/` | symlinks in `~/.config/obs-studio/plugins/obs-trigglow-dynamic-delay/` |
| macOS | `~/Library/Application Support/obs-studio/plugins/obs-trigglow-dynamic-delay.plugin` | the same |

`<libdir>` is `/usr/lib/x86_64-linux-gnu` (`.deb`), `/usr/lib64` (`.rpm`) or `/usr/lib`
(`.pkg.tar.zst`). OBS 33 loads the first column and skips the legacy copy; OBS 31 and 32 only know
the second. On Windows each copy has its own FFmpeg DLLs next to it.

**Manual install on Windows:** extract the `.zip` into `C:\ProgramData\obs-studio\plugins\` (it
contains the `obs-trigglow-dynamic-delay` folder) and restart OBS. Don't copy anything into
`C:\Program Files\obs-studio`: OBS 33 marks plugins there as "Legacy" and OBS 34 won't load them.

## Using the plugin

1. Open OBS. If the **"Trigglow Dynamic Delay"** dock doesn't show up, go to `View → Docks` and
   enable it.
2. In the dock, pick your **live scene** (required — the scene with your real content) and,
   optionally, a **loading scene** to show while the buffer fills instead of staying on the
   undelayed live scene. Set **Delay (seconds)** (1–60) and the **minimum quality** floor (480p,
   720p, 1080p, 1440p (2K) or 2160p (4K)) the delayed video won't drop below to save RAM (it's never
   stored above OBS's output resolution either: with a 720p output the buffer is 720p even with a
   1080p minimum). The dock shows a live estimate of whether your chosen seconds actually fit in RAM
   at that quality *before* you press Enable — if they don't, it warns that the buffered duration
   will be shortened instead of lowering the quality further, but never blocks your choice. These
   fields are locked while the delay is preparing or on ("Turn the delay off to change it.").
3. Go to `Settings → Hotkeys`, search for "Trigglow", and assign a key to **Trigglow: Toggle
   Dynamic Delay** (and optionally to Enable/Disable separately; in Spanish they read "Trigglow:
   Activar/desactivar Dynamic Delay", "Activar" and "Desactivar").
4. Press Enable (from the dock button, the hotkey, or Stream Deck). The dock shows **Preparing
   delay** with a progress bar and a live countdown while the buffer fills up to the requested
   delay; once full, it switches automatically to **Delay on** and Program starts showing the
   delayed video and audio, in sync.
   **The streaming output itself is never touched at any point** — viewers on Twitch/YouTube/Kick/
   wherever never see a cut or a reconnect, whether you press the button before or during a live
   stream.

Pressing Disable frees the RAM buffer immediately and switches back to whatever scene was showing
before you enabled it.

The plugin manages a few objects in your scene collection by itself: the scene **Trigglow Delay
Buffer (auto)**, the filters **Trigglow Buffer Mode Delay (auto)** and `Trigglow Audio Delay (auto) -
<source>`, and the overlay image **Trigglow Delay Badge (auto)**. You can move or resize the overlay
in OBS and it stays where you put it; don't rename or delete the others.

## Mapping a Stream Deck button

No dedicated Stream Deck plugin is needed:

1. In the Stream Deck software, add the **"System" → "Hotkey"** action (the exact name may vary
   slightly depending on your Stream Deck version).
2. Configure it to send the key combination you assigned to "Toggle Dynamic Delay" in the previous
   step.
3. Keep the OBS window focused when you press the button (Stream Deck sends the keystroke to the
   system; OBS needs to have focus, or at least be running and listening for the global shortcut,
   depending on how you have OBS/your OS configured).
4. Pressing it while already live is exactly as safe as pressing it before going live: OBS moves
   through **Preparing delay** (with the dock's live countdown) to **Delay on** once the buffer is
   full — there is no reconnection and no visible cut on stream at any point in that transition.

More detailed guide, with example screenshots and troubleshooting: `docs/STREAM_DECK.md`.

## Project structure

```
src/
  plugin-main.cpp                 → obs_module_load/unload, wiring for all components
  buffer-mode-controller.{hpp,cpp} → the state machine (Inactive/Filling/Active/Error) that
                                     orchestrates buffer mode — the only owner of state
  obs-frontend-bridge.{hpp,cpp}    → the only layer that touches obs-frontend-api.h + filter/
                                     scene wiring
  video-delay-filter.{hpp,cpp}     → the video delay filter: capture, I420 conversion on the GPU,
                                     readback, playback, its small fixed GPU object pool, and the
                                     perf check / pipeline check telemetry
  codec-pipeline.{hpp,cpp}         → encode/decode worker threads, the storage codec's setup thread
                                     and the reaper thread, off OBS's graphics thread,
                                     stress-tested in tests/
  storage-codec.hpp                → the storage codec interface (asynchronous encoder, decoder),
                                     the raw codec, NV12 helpers and the chain bookkeeping of
                                     inter-frame codecs
  hw-storage-codec.{hpp,cpp}       → v0.7.0, Windows only: hardware HEVC/H.264 storage codec
                                     (NVENC, AMF, Quick Sync) with D3D11VA decode, and its probe
  storage-codec-select.{hpp,cpp}   → pure: the hidden buffer_codec setting, OBS encoder families,
                                     the ranking of hardware encoders, unit-tested
  storage-codec-policy.{hpp,cpp}   → pure: when the hardware codec runs and when the buffer goes
                                     back to MJPEG ("Encoding overloaded" guard), unit-tested
  annexb.{hpp,cpp}                 → pure Annex B walk: parameter sets in band, unit-tested
  mjpeg-codec.{hpp,cpp}            → MJPEG over the vendored FFmpeg, one instance per thread
  frame-ring.{hpp,cpp}             → ring of slot headers with a state machine and per-packet
                                     codec, key, chain and seq, unit-tested
  frame-arena.{hpp,cpp}            → block arena for the compressed frames, unit-tested
  degradation-policy.{hpp,cpp}     → pure adaptive degradation (threads, resolution rungs, half
                                     rate), unit-tested
  pipeline-health.hpp              → what the dock reads about the buffer's degradation
  perf-stats.{hpp,cpp}             → fixed-size percentile series for the telemetry
  buffer-fit.{hpp,cpp}             → pure buffer sizing math (output-resolution cap, quality
                                     floor, RAM budget), unit-tested in tests/
  encoder-check.{hpp,cpp}          → pure decisions about OBS's encoders: which see the main
                                     canvas, the "encoder above the buffer" log warning and the
                                     encoders' ceiling on the buffer (v0.7.0), unit-tested
  audio-delay-filter.{hpp,cpp}     → per-leaf-source audio delay ring, kept in sync with video
  hardware-info.{hpp,cpp}          → total system RAM query, used to size the buffer budget, and
                                     the GPUs present (vendor, for the hardware storage codec)
  scene-combo-box.{hpp,cpp}        → dock combo box populated from OBS's own scene list
  settings-ui.{hpp,cpp}            → the Qt dock (live/loading scene, seconds, quality, fit
                                     estimate, Enable/Disable, live fill countdown, degradation
                                     notice, update notice)
  ui-tokens.{hpp,cpp}              → pure color tokens derived from OBS's palette, with the WCAG
                                     AA contract unit-tested in tests/
  ui-theme.{hpp,cpp}               → the UI's single style sheet, bundled brand fonts and
                                     recolored SVG icons (src/resources/), shared by dock and dialog
  overlay-badge.{hpp,cpp}          → paints the live "DELAY 30 s" overlay as one PNG (QPainter)
  overlay-layout.{hpp,cpp}         → pure placement of that overlay on the canvas, unit-tested
  auto-names.{hpp,cpp}             → names of the scene/filters/overlay the plugin manages, and
                                     their migration from the v0.5.x "(no tocar)" names, unit-tested
  report-bug-dialog.{hpp,cpp}      → "Report a problem" dialog: Trigi first when available, then
                                     the ticket form (signed-in or guest), with the redacted OBS
                                     log only if the user ticks it after seeing it
  trigi-page.{hpp,cpp}             → Trigi's page in that dialog: notice, technical data,
                                     conversation, states and the hand-off to a ticket
  trigi-widgets.{hpp,cpp}          → the conversation view (never loads anything) and its bubbles
  trigi-states.{hpp,cpp}           → pure state, button and countdown decisions, unit-tested
  chat-text.{hpp,cpp}              → pure sanitizer and tiny markup parser for Trigi's answers
                                     (escaped text, bold, code, lists only), unit-tested
  support-client.{hpp,cpp}         → Trigi and ticket calls, answers and errors, unit-tested
  support-service.{hpp,cpp}        → the Qt side of support: background threads, one Trigi
                                     request at a time, local choices, log lines
  support-diagnostics.{hpp,cpp}    → the optional OBS technical data (numbers, booleans and
                                     fixed values only, within 1 KB), unit-tested
  diagnostics-collector.{hpp,cpp}  → fills that snapshot from OBS and the plugin
  bug-report.{hpp,cpp}             → finds this OBS's current log (Windows only for now: the one
                                     holding a marker the plugin writes at load), reads and
                                     redacts it in memory, and deletes old copies in %TEMP%
  log-redact.{hpp,cpp}             → pure, line-by-line redaction of the OBS log, unit-tested
  update-checker.{hpp,cpp}         → checks this repo's latest GitHub release against the running
                                     version
  background-tasks.{hpp,cpp}       → cancellable background jobs (sign-in, Trigi, tickets, update
  cancel-token.{hpp,cpp}             check) with a bounded shutdown when OBS unloads the plugin,
  ui-tasks.{hpp,cpp}                 unit-tested; ui-tasks ties them to Qt's event loop
  auth-manager.{hpp,cpp}           → the free-account login gate (see `docs/ACCOUNT_GATE.md`),
                                     on background threads
  auth-client.{hpp,cpp}            → sign-in protocol: device link v2, renewal, sign-out, the
                                     one-time v0.5.x exchange, 401/403 rules, unit-tested
  token-store.{hpp,cpp}            → the plugin's sign-in, encrypted with the Windows data
                                     protection API, unit-tested
  api-client.{hpp,cpp}             → requests to api.trigglow.com only, errors, Retry-After and
                                     the fixed pages the plugin may open, unit-tested
  json.{hpp,cpp}                   → strict, bounded JSON reader/writer, unit-tested
  win-http.{hpp,cpp}               → the one hardened WinHTTP path (no redirects, TLS 1.2+,
                                     timeouts, size cap) for every network call; macOS/Linux
                                     report "unavailable" until they get an HTTP client
  i18n.hpp                         → Str() helper (obs_module_text()) — every user-facing string
                                     in this plugin goes through it; see data/locale/*.ini
  hotkeys.{hpp,cpp}                → registration of the 3 native OBS hotkeys, wired to
                                     BufferModeController
  logging.{hpp,cpp}                → logging wrapper with a component prefix
  delay-controller.{hpp,cpp}       → legacy reconnect-mode logic from the abandoned first design
                                     (see `docs/SPEC.md` §6) — present in the repo but never
                                     instantiated by plugin-main.cpp; ships in no current build
data/
  locale/en-US.ini, es-ES.ini      → the plugin's actual translated strings (see src/i18n.hpp)
  fonts/                           → bundled Archivo and Geist Mono + their OFL licenses
  licenses/                        → notices of third-party components (Lucide icons, FFmpeg DLLs)
tests/
  buffer_fit_tests.cpp             → unit tests for buffer-fit (off by default:
                                     -DTRIGGLOW_BUILD_TESTS=ON, then ctest)
  encoder_check_tests.cpp          → unit tests for encoder-check (same)
  ui_tokens_tests.cpp · auto_names_tests.cpp · overlay_layout_tests.cpp ·
  frame_arena_tests.cpp · frame_ring_tests.cpp · degradation_policy_tests.cpp ·
  storage_codec_tests.cpp · storage_policy_tests.cpp → same, for those modules
  pipeline_stress_tests.cpp        → concurrency stress test of ring + arena + codec pipeline
  pipeline_gop_stress_tests.cpp · adversarial_gop_tests.cpp → the same with a fake inter-frame
                                     codec (fake-gop-codec.hpp): chains, losses, evacuations,
                                     yields, render pauses
  hw_storage_codec_tests.cpp       → the hardware storage codec over the real FFmpeg, failures
                                     injected (Windows, local builds with the plugin's FFmpeg)
  json_tests.cpp · log_redact_tests.cpp · chat_text_tests.cpp · support_diagnostics_tests.cpp ·
  api_client_tests.cpp · token_store_tests.cpp · auth_client_tests.cpp ·
  support_client_tests.cpp · trigi_states_tests.cpp → account, Trigi and ticket client, with
                                     a scripted network stand-in (fake-transport.hpp)
  background_tasks_tests.cpp       → cancellation and bounded shutdown of the background jobs
                                     (23 suites in all, 24 with hw_storage_codec_tests)
tools/fonts/                       → how the bundled font subsets are generated
docs/
  SPEC.md                  → full technical specification (start here)
  INSTALL_GUIDE.md          → step-by-step install walkthrough with screenshots (SmartScreen,
                             installer wizard, enabling the OBS dock)
  BUILD_VALIDATION.md       → what was actually verified in this release, and what wasn't
  ROADMAP.md                → what each version shipped, what's next, and what's out of scope
  STREAM_DECK.md             → detailed Stream Deck guide
  FAQ.md · TROUBLESHOOTING.md
CMakeLists.txt, CMakePresets.json, buildspec.json, cmake/  → the official template's build system
```

## License and repository

GPL-2.0-or-later, inherited from `obsproject/obs-plugintemplate` (see `LICENSE`) — this isn't just
a formality: `libobs` is GPL-2.0, so linking against it carries the same license over to this
plugin, which requires publishing the source code of any distributed binary.

That's why this project lives in **its own repository, separate from Trigglow's main repository**
(which stays private) — publishing only the plugin here satisfies the GPL without exposing the
rest of the product. Full detail on this decision in `docs/SPEC.md` §9.

`main` is a protected branch on GitHub: merges require a pull request, even for the repo owner (no
direct pushes, no force-push, no branch deletion). Day-to-day work happens on `develop`; PRs go
`develop` → `main`.

### Third-party assets

The plugin ships a few third-party components, each under its own license (all compatible with
redistribution next to a GPL-2.0 binary):

| Asset | Where | License |
|---|---|---|
| **Archivo** (SemiBold, Bold), subset | `data/fonts/Archivo-*.ttf` | SIL Open Font License 1.1, `data/fonts/OFL-Archivo.txt` |
| **Geist Mono** (Medium), subset | `data/fonts/GeistMono-Medium.ttf` | SIL Open Font License 1.1, `data/fonts/OFL-GeistMono.txt` |
| Interface icons (Lucide paths, as in the Trigglow web app) | compiled in from `src/resources/icons/` | ISC (portions MIT, Feather), `data/licenses/LICENSE-lucide.txt` |
| **FFmpeg** libraries (avcodec, avutil, swresample), BtbN LGPL build pinned in `buildspec.json`, Windows only | DLLs next to the plugin's own (`bin/64bit` in the package), unmodified | LGPL 3.0 or later, with MIT/BSD/Apache components (libvpl, AMF, ffnvcodec); `data/licenses/LICENSE-ffmpeg.txt`, with the build's tag and its source |

The fonts are static instances subset from the upstream variable fonts in
[google/fonts](https://github.com/google/fonts) (`ofl/archivo`, `ofl/geistmono`); `tools/fonts/`
documents how. The OFL requires its text to travel with the fonts, so each `OFL-*.txt` sits next to
its font in the package and in the installer. The fonts are only used by the plugin's own dock,
dialog and overlay (OBS's UI font is never changed); without them every text falls back to OBS's
font.
