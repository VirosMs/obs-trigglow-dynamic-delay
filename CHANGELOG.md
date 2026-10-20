*[Versión en español](CHANGELOG-es.md)*

# Changelog — Trigglow Dynamic Delay for OBS

## v0.7.1 — 2026-10-20 (Early Access)

A fix release for installing and loading the plugin: Linux packages that install and load on the
distros the Trigglow desktop app supports, an install that keeps working in OBS 33 on Windows and
Linux, and a check of the minimum OBS version. The delay itself doesn't change.

**Before you update:**
- **Windows:** close OBS and run the new installer. There's no folder to choose any more: the
  plugin now lives in `C:\ProgramData\obs-studio\plugins\obs-trigglow-dynamic-delay\`, and the
  installer removes the v0.7.0 (or earlier) copy from OBS's own folder. Your scenes, settings,
  hotkeys and sign-in are kept. If you installed by hand from a `.zip` into
  `C:\Program Files\obs-studio`, the installer cleans that up too; installing by hand from now on
  means extracting the `.zip` into `C:\ProgramData\obs-studio\plugins\`.
- **Linux:** install the `.deb`, `.rpm` or `.pkg.tar.zst` of your distro, or the per-user `.tar.xz`.
  OBS Studio 31.1.0 or newer is required: on Ubuntu 24.04 that means the OBS PPA (Ubuntu's own OBS
  is 30.0), and Debian 13's OBS 30.2 is too old.
- **macOS:** nothing changes; install the `.pkg` as before.
- Coming from v0.5.x or earlier, read v0.6.0's "Before you update" below.

**What it fixes:**
- **The Linux `.deb` only installed on Ubuntu 24.04**, and only loaded in OBS 32.2 or newer. It
  depended on that Ubuntu's exact FFmpeg (`libavcodec60`, `libavutil58`) and Qt (`t64`) package
  names, so apt refused it on Ubuntu 25.04 and later and on Debian 13, and it was built against
  the PPA's libobs 32.2, which OBS 30, 31 and 32.1 refuse. Now one `.so` for x86_64 Linux:
  - built against OBS 31.1.1's libobs, the oldest supported;
  - built on Ubuntu 24.04 (glibc 2.39, Qt 6.4), so it runs on Ubuntu 24.04, 26.04 and 26.10,
    Debian 13, Fedora and Arch;
  - with its own FFmpeg linked in (LGPL, MJPEG only, none of its symbols exported), so it needs
    no FFmpeg package and can't clash with the one OBS loads;
  - with dependencies written by hand: libc, libstdc++, Qt 6.4 or newer Core, Gui, Widgets and Svg
    under each family's names (Debian's with or without `t64`), and `obs-studio` 31.1.0 or newer.
- **New Linux packages**, the same formats as the desktop app:
  `obs-trigglow-dynamic-delay-0.7.1-x86_64-linux-gnu.deb`, `.rpm`, `.pkg.tar.zst`, and a `.tar.xz`
  with an `install.sh` for a per-user install without root.
- **OBS 33 install locations.** OBS 33 marks plugins in the old locations as "Legacy" and OBS 34
  won't load them:
  - Windows: `C:\ProgramData\obs-studio\plugins\obs-trigglow-dynamic-delay\` with the DLL at its
    root for OBS 33 and a second copy in `bin\64bit\` for OBS 31 and 32, each with its FFmpeg DLLs,
    and one `data\` folder. The `.zip` has the same layout;
  - Linux packages: `<libdir>/obs-modules/plugins/` and `/usr/share/obs/obs-modules/plugins/`,
    with symlinks in the legacy `obs-plugins` folders for OBS 31 and 32;
  - Linux `.tar.xz`: `~/.local/share/obs-studio/plugins/`, linked from
    `~/.config/obs-studio/plugins/.../bin/64bit` for OBS 31 and 32.
- **Minimum OBS version checked.** In an OBS older than 31.1.0 the plugin no longer fails in an
  unclear way: it doesn't load, and the OBS log says
  `requires OBS Studio 31.1.0 or newer, running X`. Its first log line now also gives the OBS
  version and the file it was loaded from. The Windows installer checks `obs64.exe`'s version
  before installing.
- **Windows installer:**
  - asks you to close OBS when it's open;
  - its "Launch OBS Studio" box starts OBS from OBS's own folder, now that the plugin isn't
    installed there, and as your user, not as administrator;
  - the plugin folder is writable only by administrators, so no other user of the computer can
    place a DLL next to the plugin;
  - it never touches your settings (`plugin_config`).

**Technical detail:**
- The minimum OBS version is set once, in `buildspec.json`'s new `obsCompat` (`minimum` 31.1.0,
  `testedUpTo` 32.2.2), and read from there by CMake (compile definitions for the plugin's check),
  the Windows installer script and the Linux packaging.
- Linux libobs: built from the pinned OBS 31.1.1 sources into `.deps` like on Windows and macOS
  (`cmake/linux/buildspec.cmake`).
- Linux FFmpeg 9.0.2: built from the official tarball pinned by SHA-256 in `buildspec.json`
  (`.github/scripts/linux/build-ffmpeg.sh`); the build stops on a GPL, version 3 or nonfree
  configuration. Exports limited by `cmake/linux/plugin-exports.map`.
- Packages by nfpm 2.47.0, also pinned with its SHA-256 (`.github/scripts/linux/package.sh`).
  The script fails on a shared FFmpeg, an exported FFmpeg symbol, an RPATH or a glibc newer than
  2.39. The `-dbgsym` `.ddeb` is replaced by a debug-symbols archive kept as a CI artifact.
- CI builds Linux in an `ubuntu:24.04` container and installs every package in clean containers
  (Ubuntu 24.04 with the OBS PPA, Ubuntu 26.04, Debian 13, Fedora, Arch, and the `.tar.xz` as an
  ordinary user), checking `ldd`, unresolved symbols, exported symbols and the legacy symlinks.
  On Debian 13 it checks that the `.deb` is refused.
- `data/licenses/LICENSE-ffmpeg.txt` describes the Linux static build; the Linux packages also
  carry the LGPL 2.1 text and the exact FFmpeg configuration.

**Known limitations:**
- OBS 33 is still a release candidate: the new locations follow its plugin loader, but the plugin
  hasn't been run in it live yet.
- The Linux and macOS packages are built and installed in CI, not tested live in OBS in this
  release. Sign-in is still Windows-only.
- Not supported: the Flatpak OBS, ARM and 32-bit Linux, Ubuntu 22.04 and Debian 12 or older.

## v0.7.0 — 2026-10-20 (Early Access)

An optimization release: on Windows the delay buffer takes about half the RAM. The frames waiting
out the delay are stored with your GPU's hardware video encoder and decoded on the GPU, instead of
being compressed one by one as MJPEG pictures, and the delayed picture comes out closer to the
original. There is nothing to set up: the dock, its texts and its warnings are the same. This entry
lists what you'll notice first, then the technical detail, and the known limitations last.

**Before you update:**
- Nothing to change. Install over v0.6.0 with OBS closed; your scenes, settings, hotkeys and sign-in
  are kept.
- Coming from v0.5.x or earlier, read v0.6.0's "Before you update" below: updating is required, and
  the objects the plugin manages are renamed to "(auto)".

**What changes for you:**
- **About half the RAM for the delay buffer on Windows.** With a hardware video encoder (NVIDIA
  NVENC, AMD AMF or Intel Quick Sync) the buffer is stored as HEVC video, or H.264 on a GPU without
  HEVC, and decoded on the GPU. At 1440p60 it goes from ~675 MB to ~325 MB with 30 s of delay, and
  from ~1.2 GB to ~520 MB with 60 s: between −49% and −62% depending on resolution and delay. These
  are reference figures measured outside OBS on an AMD GPU (AMF) with gameplay-like content; NVENC
  isn't measured yet. Per-resolution table in `docs/FAQ.md`.
- **The delayed picture is closer to the original.** In the same measurements the stored frames
  come back at a PSNR-Y of ~45 dB instead of ~40 dB with MJPEG, so your stream's encoder starts from
  a cleaner picture.
- **Less CPU.** The GPU's video engine does the compression: the plugin's codec work goes from ~0.8
  to ~0.2–0.3 CPU cores at 1440p60, and from ~2 to ~0.4–0.65 at 2160p60.
- **Your stream's encoder comes first ("Encoding overloaded" guard).** When the buffer encodes on
  the same GPU engine as one of OBS's encoders:
  - if the buffer encoder's latency rises, or frames queue up in it, the buffer first drops one
    resolution step (never below your minimum quality) and doesn't go back up for 5 minutes; the
    dock shows its usual amber notice;
  - if OBS skips a frame for encoding lag, or one of its outputs stops on an encoder error, the
    buffer goes back to MJPEG at once, without cutting or shifting the delay;
  - when OBS starts streaming, recording or the Replay Buffer, the buffer hands over its NVENC
    session first and takes the hardware back a few seconds later.

  Any error of the hardware encoder or decoder also sends the buffer back to MJPEG at once. Back on
  MJPEG it stays there until you turn the delay off and on again, and its RAM grows to the MJPEG
  figure over the next delay seconds, always within the budget.
- **More seconds fit in the same RAM when it's short.** Once the hardware codec has worked in this
  OBS session, the ring is sized assuming 12x compression instead of 5x, so when RAM can't hold the
  whole delay about 2.4 times more frames fit. The first time you turn the delay on in each OBS
  session it is sized conservatively (MJPEG's 5x), since no hardware encoder has been checked yet.
- **Stored at your stream encoder's resolution when every encoder rescales below the output.** For
  example a 1440p output streamed by an encoder rescaled to 1080p, with no recording above 1080p:
  the buffer is stored at 1080p, since nothing uses the extra lines. With a virtual camera or
  another raw output on, or `buffer_output_cap` off, it stays at the output resolution.
- **Smaller memory blocks and at most two compression threads per context**, which also lighten
  MJPEG: ~15–35 MB less in our runs, and up to ~60 MB (1440p) or ~130 MB (2160p) less when the
  plugin had added threads.
- **No new controls, texts or RAM warnings.** A hidden support setting, `buffer_codec`, forces MJPEG
  (steps in `docs/TROUBLESHOOTING.md`).
- **Linux and macOS don't change**: Linux keeps MJPEG and macOS stores the buffer uncompressed.

**Storage codec (technical detail, `src/storage-codec*`, `src/hw-storage-codec.*`):**
- **Interface.** `storage::Encoder` is asynchronous (`Send()`/`Receive()`, frames in flight,
  `Drain()`) and `storage::Decoder` decodes one stored packet and flushes on a discontinuity. MJPEG,
  raw and the hardware codec implement them; `VideoCodec` and `CodecFactory` are gone. Decoded
  frames are always I420, so playback doesn't know which codec was behind them.
- **Hardware encoders** (`TRIGGLOW_HAVE_HW_STORAGE`, Windows only), tried in this order and only for
  the GPU vendors present: `hevc_nvenc`, `hevc_amf`, `hevc_qsv`, `h264_nvenc`, `h264_amf`,
  `h264_qsv`; engines OBS doesn't encode on (an iGPU, say) first. NVENC only while OBS runs at most
  3 NVENC encoders, so one session stays free even with the old 5-session limit. Constant QP 20 for
  HEVC and 18 for H.264, a key every half second (30 frames at 60 fps), no B-frames, the fastest
  preset, no lookahead, forced IDRs and parameter sets in band (copied in front of a key that lacks
  them). NVENC and AMF open on a D3D11 device of their vendor's adapter (OBS's own when it's that
  vendor's).
- **Hardware decode**: D3D11VA on OBS's adapter, low delay, downloaded as NV12 and split into I420
  on the decode thread, never a silent software fallback. libavcodec's software HEVC/H.264 decoder
  is only an emergency path: when the hardware decoder fails, or when the buffer leaves an engine it
  shares with OBS for decoding (AMD, Intel; NVIDIA decodes on NVDEC, a separate engine).
- **Setup thread** (`trigglow-codec-setup`): opens and probes an encoder without ever delaying a
  capture: 8 synthetic frames, keys forced at 0 and 5 with their parameter sets in band, a D3D11VA
  decode of 0–7 and, after a flush, of 5–7, sizes and PSNR-Y above 30 dB. A session registry
  remembers what passed (a new frame size then only needs a short reopen), what failed at which
  size, and what failed hard (not tried again until OBS restarts). Meanwhile the buffer stores MJPEG
  from the first frame (the "bridge"); the hardware encoder takes over at a frame boundary, on a new
  chain that starts on a key. On an RX 7800 XT: probe 330–380 ms at 1440p, reopen about 100 ms.
- **Ring with chains.** Every packet carries its codec, whether it's a key, its chain and its seq;
  one ring holds several codecs. A P-frame is only decoded right after its reference; a packet lost
  after its encode forces the next key and the P-frames until then aren't stored; captures dropped
  before their encode don't break anything. For inter-frame storage the ring keeps a GOP of slack
  (30 slots at 60 fps instead of 15), so the key of the frame due is always still there. When
  playback starts, resumes after a pause or jumps, the decoder catches up unshown from the key or
  waits for the next one, holding the frame on screen; those holds don't count as slow decoding.
- **Encode worker.** One hardware context (bitstream in capture order), packets matched to frames by
  seq. A packet out of order, an encoder error, or 500 ms with frames in flight and no packet while
  frames keep coming is a hard error. The encoder's latency (about one frame) never counts as load
  for the degradation, which only sees the worker's own time. A new frame size (a rung, an OBS video
  reset) drains the old encoder; MJPEG bridges until the new one is open.

**Storage codec policy (technical detail, `src/storage-codec-policy.*`):**
- States `Mjpeg`, `Probing`, `Hw`, `HwGuarded`, `Yielded` and `Evacuated`, evaluated every 250 ms on
  the graphics thread next to the degradation policy. Pure and unit-tested.
- **Shared engine**: the buffer's encoder is of the same family as an active OBS video encoder, or
  its decode runs on an AMD or Intel GPU whose engine also encodes for OBS. OBS's encoders are
  listed at Enable, on every stream, recording, Replay Buffer and virtual camera start or stop, and
  once a second while the delay runs, which also sees outputs other plugins start (Aitum Vertical,
  multi-rtmp, Source Record). Unknown encoders count as shared.
- **Back to MJPEG at once** (`Evacuated`, until the next Enable): any hard error of the hardware
  encoder or decoder (an encoder that failed isn't tried again this session); and, on a shared
  engine, one frame or more skipped by OBS for encoding lag in 2 s (main mix and its encoders' own
  mixes), an OBS output stopping on an encoder error, or an early warning with no rung left. With
  the decode on the shared engine, the hardware frames left in the ring are decoded in software.
- **Early warning** (shared engine, held for 1 s over at least 20 packets, 2 s after the previous
  change): latency p95 above 2.5 capture intervals (42 ms at 60 fps), or 3 frames in flight. One
  rung down (fewer pixels is what frees the engine) and no going back up for 5 minutes.
- **NVENC sessions**: on the stream, recording and Replay Buffer "starting" events, if the buffer
  may hold an NVENC session it closes it (the UI waits up to 100 ms, only when OBS's NVENC encoders,
  the starting one and the buffer's add up to 4 or more), stores MJPEG meanwhile and sets the
  hardware up again 3 s after the output started (10 s at most).

**Changed (memory and sizing):**
- **Arena blocks** of expected/64 in whole MiB, between 4 and 8 MiB, with one spare (was
  expected/16, 4–32 MiB, two spares, at least two raw frames): the memory committed past the live
  packets drops from 35–71 MB to about two blocks. A raw frame bigger than a block takes the
  oversized path.
- **At most two slice threads per MJPEG context**: the parallelism steps are 1×1 → 2×1 → 2×2 with
  workers and 1 → 2 slices in sync mode; the hardware codec has only 1×1.
- **Storage class, fixed per delay cycle** (`storage_class` filter setting): `intra` (5x, a quarter
  second of slack; with `buffer_codec` set to `mjpeg`, `codec_workers` off, no GPU, or Linux; 1x on
  macOS), `intra-gop` (5x with a GOP of slack: a cycle the hardware may take over before any encoder
  passed its probe this session) and `inter` (12x and a GOP of slack, once one has). The ring, the
  dock's estimate and the audio delay go through the same class, so they keep one whole-second
  delay. 12x holds even if the cycle ends on MJPEG (13–17x measured); with worse content the arena
  drops frames instead of growing.
- **Encoders' ceiling** (`encodercheck::EncoderCeiling()`): the active encoders that see the main
  canvas (exactly or by aspect ratio, every Enhanced Broadcasting track included, other canvases
  never) cap the buffer at the largest of them when that is below OBS's output. No ceiling with none
  of them, with a raw output on the main mix (virtual camera, NDI), with a GPU-rescaled encoder of
  another aspect ratio, or with `buffer_output_cap` off. A soft ceiling: the ring, its RAM limit and
  the audio delay stay sized for the output, so it can lift at any time; the dock shows nothing new.

**Added (support and diagnostics):**
- **Hidden `buffer_codec` setting** in `buffer-mode-settings.json`: absent means `"auto"` (hardware
  where it works); the exact string `"mjpeg"` forces MJPEG. Read only when OBS starts, as strictly
  as the other hidden settings: anything else (`"MJPEG"`, `"hevc"`, `false`, `0`) keeps auto with a
  WARN, and is dropped from the file when OBS closes. `codec_workers` off wins over it (MJPEG). The
  `settings loaded` and `enabled` lines show `buffer_codec=auto|mjpeg`, and `enabled` the cycle's
  `storage_class`.
- **`pipeline check`** names the codec storing the frames after `mode=` (`codec=mjpeg`,
  `codec=hevc_nvenc`…; `raw` on macOS).
- **`storage codec check`** line after it, every 10 s: the encoder with its QP and GOP, the decoder
  (`d3d11va`, `sw` or `mjpeg`), the hardware encoder's latency p50/p95/max, frames in flight and
  `Send()` p95, keys and forced IDRs, chain breaks and orphan P-frames dropped, catch-ups and no-key
  waits, bridge frames, codec changes, hard errors, the policy state and its veto, and the frames
  OBS skipped for encoding lag in the main mix and in its encoders' mixes. One short line when the
  cycle is all MJPEG.
- **Other lines**: `storage codec: auto -> hevc_amf (…)` when a hardware encoder passes its probe
  (QP, GOP, decode adapter, probe times, what was tried, engine sharing, OBS's NVENC sessions), or
  why it stays on MJPEG; a WARN `delay buffer codec … -> mjpeg: <why>` on every return to MJPEG;
  INFO lines for yields; `delay pipeline: …` names the storage class; `encoder ceiling WxH: …` or
  `no encoder ceiling: …`.
- **`PipelineHealth` v2**: the codec, the encoder's name, hardware decode, the policy state and its
  change count, for support diagnostics only. The dock is unchanged.

**Security:**
- **No Quick Sync in an OBS running as administrator.** Its runtime is loaded through a dispatcher
  that searches DLL paths an elevated process shouldn't trust; the log says so when an Intel GPU is
  present. NVENC and AMF load their runtimes from the driver, and the decode is D3D11VA.
- **`get_pipeline_state` copies only what the caller's struct holds** (a `size` parameter), so a
  caller built against the smaller v1 struct never has memory past it overwritten.
- Only the GPU vendors present are probed, so no other vendor's runtime is loaded. The plugin only
  decodes bitstreams it produced itself.

**Packaging / Build / CI:**
- **`data/licenses/LICENSE-ffmpeg.txt`**: the BtbN FFmpeg build pinned in `buildspec.json` (release
  tag, archive, SHA-256, FFmpeg commit), links to its source and build scripts, the MIT/BSD/Apache
  components it links in (libvpl, AMF, ffnvcodec) and the LGPL v3 and GPL v3 texts. The installer
  lists each notice, so a package without one fails to build, and the third-party tables of the
  README name the FFmpeg DLLs.
- **The Windows build checks the downloaded FFmpeg stays LGPL**: it fails if its configure line has
  `--enable-gpl` or `--enable-nonfree`, or if libavcodec doesn't report an LGPL license.
- **No new DLLs**: the hardware encoders and decoders are in the `avcodec-63.dll` and
  `avutil-61.dll` already shipped, and their runtimes come with the GPU driver.
- The ThreadSanitizer job also runs `pipeline_gop_stress_tests`; `TRIGGLOW_STRESS_CI=1` loosens its
  timing checks too.
- Version 0.7.0 in `buildspec.json` and the Windows installer script.

**Tests** (23 suites, no OBS needed, plus `hw_storage_codec_tests` on local Windows builds;
`ctest`):
- New: `storage_codec_tests` (frames in flight from 0 to 16, chain bookkeeping, parameter sets in
  band, NV12, the strict `buffer_codec` parse, the ranking with and without Quick Sync, OBS encoder
  families, engine sharing), `storage_policy_tests` (priorities, the shared-engine rule, cooldown,
  veto, yields, too few samples, an hour at the edge of the thresholds with at most one transition),
  `pipeline_gop_stress_tests` (the pipeline with a fake inter-frame codec: latency, GOPs, losses,
  codec and size changes, render pauses, setups that fail; never a P-frame without its reference,
  chains from a key, one thread per codec object and never the graphics thread),
  `adversarial_gop_tests` (evacuations inside a GOP, packets lost after their encode, Disable and
  shutdown with a setup blocked, sizes flapping, yields, half frame rate, pauses with frames held in
  the encoder, a broken hardware decoder, a slow software decode, an arena too small) and
  `hw_storage_codec_tests` (the real FFmpeg with failures injected: failed opens, a cancelled setup,
  a decoder that fails to open).
- Extended: `frame_ring_tests` (decodable packets, decode start plans, mixed codecs),
  `frame_arena_tests` (8 MiB blocks, one spare), `buffer_fit_tests` (storage classes, GOP slack,
  2.4x the frames at the floor, 4K60 for 60 s on 16 GB at full resolution), `encoder_check_tests`
  (the ceiling), `degradation_policy_tests` (two-slice ladders, hardware steps, the forced rung
  down, the veto, latency never counted as load) and `support_diagnostics_tests` (`PipelineHealth`
  sizes).

**Known limitations:**
- **NVENC hasn't been validated inside a real OBS yet.** The hardware path was measured with AMD's
  AMF outside OBS. NVIDIA's NVENC and NVDEC, Intel's Quick Sync and the VRAM the hardware codec
  takes aren't measured, and bytes per frame (so RAM) can differ on NVIDIA. Every encoder is probed
  before use, and any error, or a frame OBS skips on a shared engine, brings MJPEG back; if you
  still see a problem, set `buffer_codec` to `"mjpeg"` and send a report.
- **The emergency software decode is slower, and can't keep up at 4K60.** If the hardware decoder
  fails, or the buffer leaves an engine it shares with OBS for decoding (AMD, Intel), the hardware
  frames already in the buffer are decoded on the CPU: at 2160p60 the delayed picture can repeat
  frames (never a shift of the delay) until the MJPEG frames that follow reach the screen, at most
  the length of the delay.
- **Linux and macOS are unchanged**: MJPEG on Linux, uncompressed on macOS. Sign-in, Trigi and
  reports from OBS are still Windows-only.
- After a return to MJPEG the buffer doesn't try the hardware again until you turn the delay off and
  on. The first activation of each OBS session sizes the ring with MJPEG's 5x, so with RAM short it
  can hold fewer seconds than later ones.
- When the buffer lowers a rung because the GPU's video engine is busy, the dock's notice gives its
  existing cause, "compressing the frames takes a lot of CPU"; the log's WARN says it was the
  engine.
- v0.6.0's limitations still apply (switching Program to the live scene by hand or deleting an
  "(auto)" filter while the delay is on, render jitter, the canvas-sized keep-alive target).

## v0.6.0 — 2026-10-19 (Early Access)

The delay no longer competes with OBS for its graphics thread, it adapts to what your computer can
do without ever cutting, and the plugin takes the look of trigglow.com. "Report a problem" now
starts with Trigi, Trigglow's AI support assistant, sign-in moves to a safer flow with the plugin's
own encrypted sign-in, and the plugin goes through a security and privacy pass. A v0.5.1
performance hotfix was prepared but never published: everything in it ships here. **Updating
from v0.5.x is required** (see "Before you update"). This entry lists what you'll notice first, then
the technical detail, and the known limitations last.

**Before you update:**
- **Updating is required to keep using the delay.** The sign-in of v0.5.x and earlier is switched
  off when this version is published: from then on those versions can no longer sign in to Trigglow,
  and the sign-ins they already have stop working after a transition period. The delay needs a
  signed-in account, so update. When you install 0.6.0 over your current version, your sign-in moves
  to the new format by itself on the first start; if the dock asks you to sign in, do it once.
- **Sign-in, Trigi and sending reports from OBS work only on Windows in this version**; macOS and
  Linux get them in v0.6.1 (see "Known limitations").
- **The objects the plugin manages are renamed from "(no tocar)" to "(auto)"**, automatically.
  **Point any Stream Deck, obs-websocket or other remote-control action that uses the old names at
  the new ones** (list below).
- **Going back to v0.5.0 or earlier after updating duplicates those objects** (see "Names" below).

**What changes for you:**
- **Trigi, Trigglow's AI support assistant, in "Report a problem"** (Windows, signed in). Ask about
  Dynamic Delay and get an answer right in the dialog; if it doesn't sort it out, **Create a ticket
  with this conversation** hands it to the team with the context. The first time, a short notice
  says what Trigi is and what happens with your data, and you can always write to the team instead.
  **Your OBS log is never sent to the AI.** The OBS technical data is optional and you see exactly
  what is sent before you tick it.
- **Safer sign-in.** The dock shows a code, and the trigglow.com page must show the same one before
  you confirm; **It wasn't me** on the page cancels the request. The plugin keeps its own sign-in,
  encrypted for your Windows user, and **Sign out** also disconnects it on trigglow.com. Your v0.5.x
  sign-in moves over by itself.
- **Reports from OBS go to your account when you're signed in**: no name or email to type, and you
  follow the ticket in your support panel on trigglow.com. The OBS log goes only if you tick its box,
  which you can do once you've seen it in **See what is sent**: your Windows user name, URLs, keys,
  emails and IPs are hidden, and the dialog tells you what can still show up.
- **No more "Encoding overloaded" as soon as the delay is on with a canvas larger than the
  output.** Reported live with a 2560x1440 canvas and a 1920x1080 output at 60 fps. The delay
  buffer is now stored at OBS's output resolution at most (Settings → Video → Output (Scaled)
  Resolution), so you can go back to your larger canvas.
- **1440p output at 60 fps is supported.** Compressing and decompressing the buffered frames no
  longer happens on OBS's graphics thread (the one that has to finish every frame of your stream in
  time) but on the plugin's own threads. At 1440p60 the plugin's work on that thread drops from
  ~14/17 ms to ~1.2–1.7/1.6–2.1 ms per frame (p50/p95, offline microbenchmark on an
  8-core/16-thread CPU).
- **Minimum quality 1440p (2K) and 2160p (4K)** in the dock, after 480p, 720p and 1080p. With a
  1440p output and a 1440p minimum the buffer stays at 1440p when RAM is short (the delay gets
  shorter instead, as with 1080p) and when your computer can't keep up (only the capture frame
  rate halves, with an output at 48 fps or more). It never goes above your OBS output: with a 1080p
  output it buffers at 1080p. So a 2560x1440 canvas can go out at 1080p, or at 1440p60.
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
  "Turn the delay off to change it." **The dock is the only place to set the delay**: the "(auto)"
  filters no longer have delay or quality fields in their own properties, ignore values written from
  anywhere else, and switch themselves back on if something turns them off while the delay runs.
- **Switching scene collection turns the delay off**, and the dock says why: the helper scene and
  the filters belong to the collection you left. Turn it on again when you're ready.
- **The countdown and the figures add up**: while the delay fills, the dock counts the seconds that
  really apply ("41 / 60 s" when RAM only fits 41), and the countdown no longer starts over when you
  toggle the overlay or change its corner. The RAM line under the fields now reads as the RAM
  estimate it is ("RAM only fits 41 s of 60 s…"), in whole seconds.
- **New design** of the dock, the report dialog and the live overlay, aligned with trigglow.com, on
  top of your OBS theme (dark, light, high contrast), with AA contrast and visible keyboard focus.
  The live overlay is one image the plugin draws, signed with a small "By Trigglow" under the icon.
- **Fixes**, among others: delay filters left on by closing OBS while Active are switched off at
  startup and when you switch scene collection; 59.94 fps no longer loses a frame per second;
  changing OBS's video settings while the delay runs shows the loading scene instead of black;
  renaming the live scene no longer floods the log nor keeps Disable from working; closing OBS
  while Trigi is answering or a ticket is being sent no longer waits on the network.

**Added / Trigi, the AI support assistant (Windows, signed in):**
- **"Report a problem" opens on Trigi** when it's available: on Windows, signed in, with a plugin
  version Trigi supports. Otherwise the dialog opens straight on the ticket form, as before; signed
  out, it offers **Sign in to ask Trigi** next to the form.
- **First use: "Before you start with Trigi".** The notice says that Trigi is an AI and can make
  mistakes; that your messages and, if you turn it on, the OBS technical data are processed by
  Trigglow and an AI provider listed in the [Privacy Policy](https://www.trigglow.com/legal/privacy);
  that conversations are kept for up to 12 months; not to type passwords, stream keys, overlay URLs
  or payment details; that the OBS log is never sent to the AI; and that you can always write to the
  team. **Got it** or **I'd rather write to the team** (the ticket form). Nothing is sent before you
  accept it and send your first question: opening the dialog makes no call to Trigi. If a later
  version changes the notice, it's shown again.
- **Optional OBS technical data.** **Include OBS technical data** is unticked until you tick it (the
  choice is remembered), and **See what is sent** shows the exact data. Only numbers and states, sent
  once when a conversation starts: OBS and Windows versions, architecture, renderer, GPU vendor,
  total RAM, encoder family, the delay's state, seconds and minimum quality, canvas and output size,
  fps, OBS's lagged and skipped frames, the buffer's health and the plugin's recent problem codes.
  Never scene, source, filter, encoder or output names, paths, URLs, stream details, your user name
  or email, the GPU's name, hardware IDs or IPs (`src/support-diagnostics.*`,
  `src/diagnostics-collector.*`, which never waits on OBS's graphics thread).
- **The conversation.** Enter sends, Ctrl+Enter or Shift+Enter adds a line, with a character
  counter and the fixed line "Trigi is an AI and can make mistakes. Don't share sensitive data."
  "Trigi is thinking…" shows a still spinner when Windows asks for less motion, and "This is taking
  longer than usual…" after 15 s. **Was this helpful?** under each answer: Yes closes the
  conversation as solved, No offers the ticket. **Clear conversation** only clears the view. What
  you see in OBS is kept in memory only and is gone when you close the dialog.
- **From the conversation to a ticket.** **Create a ticket with this conversation** asks Trigi to
  sum it up (or uses your own words if it can't) and opens the form filled in, for you to check and
  edit, with the conversation linked so the team has the context, the version footer and the log
  box. When the answer is about something outside Dynamic Delay, the button is **Open support on
  trigglow.com** instead.
- **Every state says what to do next**: no connection, Trigi took too long, trigglow.com isn't
  answering, too many questions in a row (Send stays off with a countdown), session ended (what you
  typed is kept), conversation closed (a new one starts with your next question), "Trigi can't
  answer right now" (with **Create a ticket**), Trigi turned off (the form opens quietly, with your
  text) and "Update the plugin to use Trigi" (with **Get the update**). A question that didn't go
  through is marked "Not sent" and can be tried again by hand; nothing is retried automatically.
- **Safe rendering.** Answers are shown as plain text with bold, `code`, bullets and numbered steps
  only, built from escaped text (`src/chat-text.*`, `src/trigi-widgets.*`); links, images and HTML in
  an answer stay plain, non-clickable text, and the view never loads anything. The only links are
  the plugin's own fixed buttons.
- **Accessibility**: logical tab order, a name for every control, one screen reader announcement
  per answer and per state change (the countdown isn't read out every second), AA contrast in dark,
  light and high contrast, and button rows that stack in narrow dialogs.
- **In the OBS log**, one line per Trigi event (conversation started, question sent with its length
  and whether technical data went with it, answer, unavailable, rate limited, HTTP code); never the
  text of your questions or of the answers, IDs or the technical data.
- English and Spanish strings. Pages and states in `src/trigi-page.*` and `src/trigi-states.*`
  (tested without Qt), client in `src/support-client.*` and `src/support-service.*`.

**Changed / Sign-in:**
- **Sign in** opens trigglow.com in your browser and the dock shows a code: "The page must show the
  code KD7F-29QM." The page shows the app, its version and system, when the request was made, the
  same code and, if the request seems to come from a different network than yours, a warning;
  **Confirm** or **It wasn't me**. **Copy link** and **Cancel** stay in the dock. The secret the
  plugin uses to wait for your confirmation never leaves it and never goes in a URL, and neither the
  link, the codes nor the secret are written to the OBS log.
- **The plugin's own sign-in.** Once you confirm, the plugin gets its own token, which only works
  for what the plugin does (checking your account, renewing itself, signing out, Trigi and your own
  Dynamic Delay tickets), not for the rest of your account. It's renewed automatically when
  trigglow.com says so. On Windows it's stored encrypted with Windows' data protection, for your
  Windows user only, apart from the plugin's settings; your email isn't stored. If it can't be
  decrypted (another Windows user, a restored profile), it's dropped and you sign in again.
- **Sign out revokes it on trigglow.com** as well as deleting it from your computer (the local copy
  goes even without a connection). You can also disconnect the plugin from the web: Settings →
  Active sessions → Connected apps.
- **v0.5.x sign-ins move over by themselves** on the first start of 0.6.0: the old sign-in is
  exchanged once for the new one, and the old plain-text file is overwritten and deleted (also when
  a new sign-in already exists). If trigglow.com can't be reached, it's tried again on the next
  start and the delay keeps working meanwhile.
- **Sign out always sticks.** Signing out while the plugin is renewing its token, exchanging a
  v0.5.x sign-in or waiting for the browser never brings the sign-in back at the next start: a token
  the server hands out after the sign-out isn't kept, and it's revoked too. Closing "Report a
  problem" while it renews the token no longer risks the sign-in: the renewal finishes in the
  background and its token is kept.
- **One check per OBS start** (plus a renewal when it's due). Without a connection you stay signed
  in and the delay works; if your account is suspended or not active, **Enable delay** is blocked
  with "Your Trigglow account isn't active." and **Open trigglow.com**.
- New messages in the dock: sign-in cancelled from the web, too many attempts, "This plugin version
  can't sign in any more. Update the plugin." and the account not being active. `src/auth-client.*`,
  `src/token-store.*` and `src/api-client.*`; `docs/ACCOUNT_GATE.md` describes the flow.

**Changed / Report a problem:**
- **Signed-in tickets.** Signed in, the ticket comes from your account (no name or email to type),
  with an optional subject and a footer with the plugin, OBS and Windows versions that you can see
  and edit. "Ticket sent" offers **View ticket on trigglow.com**; replies arrive by email and in your
  support panel.
- **The OBS log, redacted and with your consent.** **Attach OBS's log** starts unticked and can only
  be ticked once you've opened **See what is sent**, which shows the log already redacted (its first
  and last 200 lines; the whole log goes). The redaction (`src/log-redact.*`, line by line, tested)
  hides the user name in Windows, macOS and Linux paths (moved profiles, network shares and work
  OneDrive folders too), stream URLs, any other URL (only the host stays), overlay links, emails,
  keys, tokens, passwords, cookies and credential headers, JWTs, IPv4 and IPv6 addresses and long
  token-like strings, and keeps timestamps, versions and hotkeys. Names of scenes, sources, audio
  devices and windows can still show up, and the dialog says so. Above 4.5 MB after redaction, the
  start and the end are kept.
- **Only this OBS's log.** The plugin marks its own OBS's log when it loads and attaches only the
  file with that mark, never the newest log of another OBS (a portable one, or a second instance).
- **No more copies in `%TEMP%`.** The log is read and redacted in memory, and the copies earlier
  versions left in `%TEMP%\Trigglow` are deleted when the plugin loads.
- If the ticket is created but the log can't be attached, the dialog says so and offers **Save
  redacted log…** so you can add it from the web.
- **Signed out**, the guest form stays (name, email, message). If trigglow.com asks guests for a
  human check (captcha), which OBS can't show, the dialog says "To send the report from OBS, sign
  in. You can also use the web form." with **Sign in**, **Open web support** and **Save redacted
  log…**, and never retries on its own. The guest ticket's access key travels in a header,
  not in the URL, and is never logged.
- On macOS and Linux the form is shown disabled with a link to the web form (no HTTP client there
  yet).

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
  is gone, the web has no green); errors get a red rule and an alert icon. Preparing delay and Delay
  on both count the seconds that really apply, the same as the live overlay: with 60 s requested
  and 41 s fitting in RAM, the chip reads "41 / 60 s" in amber, the message "Saving 41 s of stream…:
  only 41 of your 60 s fit in RAM" and the fill window lasts 41 s. The fill countdown starts over
  only for a new fill window: toggling the overlay or changing its corner while it fills no longer
  restarts it (and the switch to the delayed scene with it).
- **RAM estimate line** under the delay and quality fields, worded as what it is: "RAM fits the
  full 30 s at 1920×1080", "RAM only fits 41 s of 60 s at …", in the same whole seconds as the chip.
  It hides while the degradation notice shows (the notice then says what the buffer holds), and
  gives no "lower the seconds or the min. quality" advice while those fields are locked.
- **"Buffer quality lowered" notice** at the bottom of the status card while the buffer runs below
  full quality: a sliders icon and the title in amber, then what was lowered (resolution against the
  full size, capture fps, or both), why (OBS's graphics thread under heavy load, compressing or
  decompressing the frames) and that the delay is unchanged; repeated and dropped frames per minute
  follow when above zero. It goes away as soon as the buffer is back at full quality, and nothing is
  polled while the delay is off. Screen readers hear it once: when it appears, when its cause
  changes, and "Buffer back to full quality." when it goes with the delay still on (Qt 6.8+
  announcements; older Qt keeps the card's name change), not on every rung.
- **Locked while the delay runs:** the live and loading scene pickers, the delay seconds and the
  minimum quality. The seconds used to stay editable while Active and applied on every keystroke,
  resizing the buffer under a live stream. Locked fields lose their arrows, take the card's
  background and show a lock; a line underneath, "Turn the delay off to change it.", is also their
  accessible description; they are out of the tab order until the delay is off, and their value
  stays readable (muted text, AA in every theme).
- **Minimum quality 1440p (2K) and 2160p (4K)**, after 480p, 720p and 1080p. The tooltip says they
  only apply with an output that big; the sizing and the degradation already took any floor.
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
  Dynamic Delay mark, Archivo), written to the plugin's config folder
  (`overlay/delay-badge-r3-30s.png`) and shown by a single image source. It looks the same on
  Windows, macOS and Linux (the old text used Segoe UI, Windows only) and moves in one piece. It
  sits 2.2% of the canvas's short side away from the chosen corner and keeps its size relative to
  that side: the same plate as before on landscape canvases (200x58 at 1080p), and on a 1080x1920
  vertical canvas the 1080p size instead of a third of the frame's width. It shows the delay really
  in effect.
- **Signed "By Trigglow"**: small, quiet text under the icon, outside the badge plate (Geist Mono,
  11 px on a 1080p canvas and 7.3 px at 720p, mist at 75 % with a faint dark halo and no box), the
  same in every language. In a bottom corner the margin is kept below the signature. The file names
  carry a render version, so a badge an earlier build left in that folder is never shown, and it is
  deleted.
- **Move or resize it in OBS and it stays that way.** The plugin places it in the chosen corner the
  first time and again only when you pick another corner in the dock (or when the canvas size or
  the badge's own size changes, as long as you haven't moved or resized it). An existing image
  source already called "Trigglow Delay Badge (auto)" is reused instead of adding "... 2" copies.
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

**Changed:**
- **"Minimum quality" is a floor for the RAM reduction and for the degradation, bounded by OBS's
  output.** When RAM is short it still decides how far the buffer may shrink (rather than go below
  it, fewer seconds are stored), and the degradation never goes below it either; but it no longer
  raises the buffer above the output resolution: with a 720p output the buffer is 720p even with a
  1080p minimum.
- **The `compression check` log line is replaced by `perf check` and `pipeline check`** (below).
- **Sign-in, reports and the update check: see "Changed / Sign-in" and "Changed / Report a
  problem"** above and "Security and privacy" below.

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
- **A buffer restart no longer shows black.** A change of the delay or the minimum quality that
  resizes the buffer throws the buffered frames away. The filter now signals it
  (`buffer_restarted`) and Program goes back to the loading scene, with audio re-synced, while the
  buffer refills, instead of a blank delayed view for `delay` seconds. Same after a reset of OBS's
  video settings. With no loading scene set, Program stays on the (blank) wrapper scene, never on
  the undelayed live scene. The dock locks those fields while the delay runs, so in practice this
  covers resets of OBS's video settings.
- **The delay filters only ever apply the dock's delay.** Their own properties could bypass the
  plugin: 90 s in the video filter's dialog put video at 90 s and audio at 60, and 0 s passed the
  live scene through undelayed while the dock said "Delay on". The "(auto)" filters' properties now
  hold only a note that the dock manages them. The dock pushes its delay and quality floor through
  new `set_managed_*` procs, and from then on a filter ignores any other value written to its
  settings (OBS's Defaults button, an older dialog, obs-websocket), with one WARN per push. While
  the delay is on, a delay filter switched off from outside the dock (its eye icon in the Filters
  dialog, obs-websocket) is switched back on at once, with one WARN per activation.
- **A 0 s delay** (only reachable now for a filter the dock doesn't manage yet, e.g. one added by
  hand) passes the video through undelayed, with no buffer and no codec. Before, it encoded every
  frame into a one-slot ring and never drew it: black output at full CPU cost.
- **Switching scene collection with the delay on left the dock saying "Delay on"** while the new
  collection ran without any delay. On `OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING` (switch, new,
  remove) the delay is now stopped like Disable while the old collection is still loaded, so it's
  saved with its filters off and its scene back on Program, and the dock goes Inactive with a line
  saying why. OBS 31 sends no `COLLECTION_CHANGING` when duplicating or renaming a collection, only
  `COLLECTION_CHANGED` with the copy already loaded: the same stop runs then, switching off the
  copy's filters and leaving the helper scene if the copy opened on it.
- **Renaming the live scene with the delay on** flooded the log (the dock's once-a-second health
  poll looked the filter up by name and warned on every miss) and left the filter enabled after
  Disable. The bridge now keeps weak references to the filters it switched on, for the poll, the
  setters and switching them off.
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
- **Dock tooltip for the minimum quality corrected.** It said the delayed video "never drops
  below" that resolution, which stopped being true with the output cap. The filter's properties no
  longer have a quality field at all: they only show the note that the dock manages the filter.

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

**Security and privacy:**
- **One hardened network path.** Every call goes through WinHTTP (`src/win-http.*`): HTTPS with TLS
  1.2 or newer, to api.trigglow.com, the only host the sign-in token is ever sent to, plus the
     update check on api.github.com (without the token). Redirects are never followed (a redirect is
     an error, so the token can't be bounced to another host), no cookies or Windows auto-logon,
     timeouts on every step plus an overall deadline, a cap on the response size, and a User-Agent
     with the real plugin version and system. Nothing that creates something (a conversation, a
     question, a ticket, an attachment) is retried automatically.
- **The plugin only opens its own fixed pages**: the sign-in, support, privacy and plugin pages on
  www.trigglow.com and this repository on GitHub, checked by prefix, with encoded dots and slashes
  read as a browser would (no `..` or `%2e%2e` way out of the page). Anything else, for example a
  link inside an answer, isn't opened.
- **Less personal data in the OBS log.** The sign-in link, codes and secret, tokens, ticket and
  conversation IDs, and the text of questions and answers are never written to it, and lines that
  mention a file name it without the full path, which included your Windows user name.
- **The update notice only opens this repository's release pages**: any other link in GitHub's
  answer is replaced by the latest-release page. The update check uses the same hardened client.
- **No build machine paths in the DLL**: the Windows DLL records only its PDB's file name, and the
  binary holds no path-like or host-like strings it doesn't need.
- **OBS closes cleanly while a call is in flight.** Every network job (sign-in, Trigi, tickets, the
  log for a ticket, the update check) runs as a cancellable background job
  (`src/background-tasks.*`, `src/cancel-token.*`, `src/ui-tasks.*`), and WinHTTP runs
  asynchronously, so a call is aborted at once. Unloading the plugin cancels every job and waits at
  most 2 s; on Windows no plugin code runs after OBS unloads it, and no result reaches a closed
  dialog. A timeout is reported as "Trigi took too long" instead of "no connection".

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
- **`TRIGGLOW_STRESS_CI=1`**, set on both CI test jobs, relaxes only `pipeline_stress_tests`'
  timing limits (50% of due frames shown exactly instead of 90%, 100 ms of lag after a render pause,
  at least 10 frames shown behind a slow decoder), which shared runners and ThreadSanitizer can't
  always meet. Its correctness checks stay strict everywhere.
- Version 0.6.0 in `buildspec.json` and the Windows installer script.
- **Windows links `winhttp`, `crypt32` (the encrypted sign-in) and `dxgi` (the GPU vendor ID of the
  optional technical data).** `TRIGGLOW_ALLOW_API_HOST_OVERRIDE` (off by default) is for local test
  builds only: the configure step fails if it's on in CI, so no release can point anywhere but
  api.trigglow.com.
- **The Windows DLL records only its PDB's file name** (`/PDBALTPATH`), not the build machine's
  absolute path (`cmake/windows/helpers.cmake`).

**Tests** (19 suites, no OBS needed; `ctest`):
- `buffer_fit_tests` (sizing: canvas = output, 1440p → 1080p, 4K → 1080p, output above canvas,
  mismatched aspect ratios, a floor above the output, RAM reduction, rounding, widths multiple of
  8, odd dimensions, degenerate input, 59.94 fps, the whole-second effective delay, the 1440p and
  2160p floors, budget/output sweeps), `encoder_check_tests` (the encoder WARN's decision), `ui_tokens_tests` (contrast on OBS
  31's palettes), `auto_names_tests` (names and their migration), `overlay_layout_tests` (overlay
  placement), `frame_arena_tests`, `frame_ring_tests` (slot state machine, generations, age-based
  selection), `degradation_policy_tests` (ladder, every trigger, cooldown, 30 s hysteresis, no
  oscillation; the 1440p and 2160p floors), `pipeline_stress_tests` (ring + arena + codec pipeline
  under random detaches, resizes, rung and rate changes: the delay never shrinks, frames are never
  torn or shown out of order, and no ring is ever freed on the simulated graphics thread) and
  `background_tasks_tests` (cancellation, results never delivered to a destroyed owner, the bounded
  shutdown).
- New for the account, Trigi and tickets, all with a scripted network stand-in that never touches
  the network: `json_tests` (strict, bounded JSON), `log_redact_tests` (every redaction rule, paths
  of the three systems, a 2 MB hostile line and a 5 MB log), `chat_text_tests` (script and image
  tags, markdown links, code fences, 10,000 characters, joined emojis, bidi overrides),
  `support_diagnostics_tests` (only the documented keys, types and values, within 1 KB),
  `api_client_tests`, `token_store_tests` (the encrypted round trip on Windows), `auth_client_tests`
  (sign-in, renewal, sign-out, the v0.5.x exchange and every 401/403 rule), `support_client_tests`
  (every answer shape and error, request bodies and headers) and `trigi_states_tests` (the state,
  button and countdown each answer or error leads to).

**Known limitations:**
- **Switching Program to the live scene by hand while the delay is Active** (instead of the
  plugin's helper scene, "Trigglow Delay Buffer (auto)") sends the live video without delay: the
  delay filter sits on the helper scene. This was already the case before 0.6.0. Leave Program on
  the scene the plugin chose while the delay runs, and turn the delay off from the dock first.
- **Deleting an "(auto)" delay filter by hand while the delay is on** lets the live video out
  without delay (the helper scene then shows the live scene with no filter) until you disable and
  re-enable the delay from the dock, which recreates it. Don't delete the plugin's filters; turn the
  delay off from the dock.
- Captures are deduplicated by a wall-clock cell per frame interval, not by OBS's frame time, so
  heavy render jitter can occasionally cost a captured frame (a repeated frame, never a shift of the
  delay). Planned next.
- While the buffer fills, keeping the live scene rendering still uses a canvas-sized render target
  whose content is never used (GPU only). Planned next.
- With a canvas larger than the output, the delayed frame is drawn back at canvas size and OBS
  scales it down again, which can look slightly softer than the live picture. If you don't need the
  larger canvas, setting canvas = output avoids it.
- macOS still has no FFmpeg build: the buffer is stored uncompressed (raw I420) there.
- **macOS and Linux have no HTTP client in the plugin yet**, so there is no sign-in there (and
  without it, Enable delay doesn't work), no Trigi and no reports from OBS; the report dialog links
  to the web form. Planned for v0.6.1.
- The redacted log can still contain names of scenes, sources, audio devices and windows: they
  can't be told apart reliably, and the preview says so. Check it with **See what is sent** before
  you tick the box.
- The conversation with Trigi in OBS isn't kept once you close the dialog. On trigglow.com it's
  kept for up to 12 months and you can delete it before (Settings → Privacy & data →
  Conversations with Trigi); what was copied into a ticket stays in that ticket.

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
  used on the Trigglow website — instead of Inno Setup's generic defaults.

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
