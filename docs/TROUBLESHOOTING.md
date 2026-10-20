*[Versión en español](TROUBLESHOOTING-es.md)*

# Basic troubleshooting — Trigglow Dynamic Delay

## The dock doesn't show up in OBS
- Go to `View → Docks` and check that "Trigglow Dynamic Delay" is checked.
- Check the OBS log (`Help → Logs and Profiles → View Current Log`) for the line
  `[obs-trigglow-dynamic-delay] loaded successfully`. If it's not there, the plugin didn't load —
  check that the binary is in the correct plugins folder for your OS (see README) and that the
  architecture (64-bit) matches your OBS installation.

## The log says "requires OBS Studio 31.1.0 or newer"
Since v0.7.1 the plugin checks the OBS it runs in and stops loading in an OBS older than 31.1.0, with
this line in the log (`Help → Logs and Profiles → View Current Log`):

```
[obs-trigglow-dynamic-delay] Trigglow Dynamic Delay v0.7.1 requires OBS Studio 31.1.0 or newer, running 30.2.3: not loaded
```

Update OBS Studio (on Ubuntu 24.04, from the OBS PPA; see the Linux section of
`docs/INSTALL_GUIDE.md`). The first line the plugin logs always says which OBS it found and which
file it was loaded from:

```
[obs-trigglow-dynamic-delay] loading Trigglow Dynamic Delay v0.7.1 on OBS Studio 32.2.2, from C:/ProgramData/obs-studio/plugins/obs-trigglow-dynamic-delay/bin/64bit/obs-trigglow-dynamic-delay.dll
```

On Windows the installer already refuses an older OBS, so this mostly shows up after OBS was
replaced by an older version, or with a portable OBS.

## OBS 33 lists the plugin as "Legacy"
OBS 33 marks plugins it finds in the old locations as "Legacy" in its plugin manager, and OBS 34
won't load them at all. v0.7.0 and earlier installed there (inside `C:\Program Files\obs-studio`
on Windows; `obs-plugins` and `~/.config/obs-studio/plugins/.../bin/64bit` on Linux).

- **Windows:** run the v0.7.1 installer (with OBS closed). It moves the plugin to
  `C:\ProgramData\obs-studio\plugins\obs-trigglow-dynamic-delay\` and removes the old copy from
  OBS's folder. If the plugin still shows as Legacy, look in
  `C:\Program Files\obs-studio\obs-plugins\64bit\` for a leftover `obs-trigglow-dynamic-delay.dll`
  (for example from a `.zip` extracted there by hand) and delete it with OBS closed.
- **Linux:** install the v0.7.1 package (or run the `.tar.xz`'s `install.sh`). It installs into OBS
  33's location and only adds symlinks in the legacy ones for OBS 31 and 32, which OBS 33 skips. A
  copy you made by hand in `~/.config/obs-studio/plugins/obs-trigglow-dynamic-delay/` (not a
  symlink) is still a legacy plugin: delete it.
- **macOS** is not affected: the plugin's location doesn't change in OBS 33.

The log line above (`loading ... from <path>`) tells you which copy OBS actually loaded.

## The Linux package doesn't install, or the plugin doesn't load on Linux
- **`obs-studio (>= 31.1.0)` can't be installed** (apt, dnf or pacman refuses): your distro's OBS
  is older than the minimum. On Ubuntu 24.04 add the OBS PPA
  (`sudo add-apt-repository ppa:obsproject/obs-studio`, then `sudo apt update`) and install again.
  Debian 13 ships OBS 30.2 and has no newer one: the plugin can't run with it.
- **Other dependencies can't be installed** (`libqt6...`): the package needs Qt 6.4 or newer, on
  x86_64. Ubuntu 22.04 and older, Debian 12 and older and 32-bit or ARM systems aren't supported.
- **The Flatpak OBS** doesn't see plugins installed by the packages or by `install.sh`, and isn't
  supported.
- **Installed but no dock:** check `/usr/lib/x86_64-linux-gnu/obs-modules/plugins/` (`/usr/lib64/...`
  on Fedora, `/usr/lib/...` on Arch) or `~/.local/share/obs-studio/plugins/` for
  `obs-trigglow-dynamic-delay.so`, and run `ldd` on it: no line may say `not found`. Then look for
  the plugin's lines in the OBS log, as above.
- **Two copies:** a package and the `.tar.xz` install at the same time. Keep one: remove the package,
  or run `./install.sh --uninstall`.

## The "Enable delay" button is greyed out
- `Enable delay` stays disabled until you've signed in with your free Trigglow account and picked a
  **live scene** in the dock. While it's disabled, a checklist under it shows which of the two is
  missing. The plugin's own helper scene ("Trigglow Delay Buffer (auto)") is never offered as a
  live scene, and a saved scene that no longer exists counts as not chosen.
- While the delay is preparing or on, `Enable delay` is replaced by `Disable delay`: there is only
  ever one button.

## I can't change the delay seconds, the quality or the scenes
- **This is on purpose.** While the delay is preparing (`Preparing delay`) or on (`Delay on`), the
  live and loading scene pickers, the delay seconds and the minimum quality are locked: they show a
  lock instead of their arrows, and the line under them reads "Turn the delay off to change it."
  Before v0.6.0 the seconds stayed editable while the delay was on and every keystroke resized the
  buffer in the middle of a live stream.
- Press `Disable delay`, change what you need and press `Enable delay` again. The buffer fills
  again for the new number of seconds (with the loading scene on Program, if you set one).
- **The dock is the only place to set the delay.** The plugin's own filters ("Trigglow Buffer Mode
  Delay (auto)" and the "Trigglow Audio Delay (auto) - …" ones) have no delay or quality fields in
  their properties any more, only a note that the dock manages them. A value written to them from
  anywhere else (OBS's Defaults button, obs-websocket) is ignored, with a WARN in the log. While the
  delay is on, switching one of them off from outside the dock (its eye icon in the Filters dialog,
  obs-websocket) switches it straight back on, also with a WARN: turn the delay off from the dock.

## The dock shows an error
- The status card turns red, with an alert icon, the headline "Couldn't enable" and a detail line
  explaining what went wrong (for example: sign in first, choose a live scene, or the helper scene
  couldn't be created). This is a controlled, crash-free state, not a hang.
- Fix what the detail line describes (usually: re-pick the live scene, since a saved scene name may
  no longer exist) and press `Enable delay` again.
- If the detail line is unclear or seems wrong, check the OBS log for lines prefixed
  `[obs-trigglow-dynamic-delay]` around the time the error appeared — they carry more detail than
  the short message the dock can show.

## Signing in from the dock
- **Sign-in works only on Windows for now (v0.6.0 and v0.7.0).** On macOS and Linux the plugin has
  no HTTP client yet (planned next, see `docs/ROADMAP.md`), so sign-in, and with it `Enable delay`,
  Trigi and reports from OBS, aren't available there.
- **How it goes.** Press **Sign in**: your browser opens trigglow.com (sign in or create a free
  account there) and the dock shows "The page must show the code KD7F-29QM." (your code is
  different). Check that the page shows the same code and press **Confirm**; the dock picks it up by
  itself a few seconds later.
- **The browser didn't open** ("Couldn't open your browser. Copy the link and open it yourself."):
  press **Copy link** and paste it into a browser on this computer.
- **The codes don't match, or you didn't start it**: press **It wasn't me** on the page. Nothing is
  connected, and the dock says "Sign-in cancelled from the web." Start again from the dock if you did
  mean to sign in.
- **"This request comes from a different network than yours"** on the page: the request from OBS and
  your browser seem to come from different networks. That's normal with a VPN or proxy on only one of
  them, or with the page open on your phone or on another connection. If you pressed **Sign in**
  yourself just now and the codes match, you can confirm; if not, press **It wasn't me**.
- **"The sign-in link expired. Try again."**: the request wasn't confirmed in time, or was already
  used. Press **Sign in** again for a new code.
- **"Couldn't reach trigglow.com. Check your connection and try again."**: OBS can't reach
  trigglow.com (offline, firewall, proxy). The plugin only talks to api.trigglow.com over HTTPS;
  allow OBS through.
- **"Too many attempts. Wait a moment and try again."**: wait a minute and try again.
- **"Couldn't sign in. Try again."**: an unexpected answer. Try again in a moment; if it keeps
  happening, send a report from [trigglow.com/support](https://www.trigglow.com/support).
- **"This plugin version can't sign in any more. Update the plugin."**: install the latest version
  (see "The plugin says to update" below).
- **"Your session ended. Sign in again to use the delay."**: the plugin's sign-in was revoked or
  expired, for example because you disconnected the plugin on trigglow.com, changed or reset your
  password there, or OBS wasn't opened for a long time. Sign in again from the dock.
- **"Your Trigglow account isn't active."**: the account is suspended or not active, and `Enable
  delay` stays blocked. Press **Open trigglow.com** to see why, or write to support from the web.

## Where the sign-in is stored, and how to sign out or disconnect the plugin
- On Windows the plugin keeps its own sign-in, encrypted with Windows' data protection for your
  Windows user, apart from the plugin's settings. Another Windows user, or a copy of the files on
  another computer, can't use it: if it can't be decrypted, the plugin drops it and asks you to sign
  in. Your email isn't stored, and the sign-in is never attached to a report.
- **Sign out** in the dock revokes it on trigglow.com and deletes it from this computer (the local
  copy is deleted even without a connection).
- **From trigglow.com**, for example if you no longer have that computer: Settings → Active sessions
  → Connected apps → **Disconnect** next to "Trigglow Dynamic Delay (OBS)". The plugin asks you to
  sign in again the next time it checks.
- **Coming from v0.5.x**: the old sign-in, stored as plain text, is exchanged for the new one on the
  first start of 0.6.0, and the old file is overwritten and deleted. There's nothing to delete by
  hand.

## The plugin says to update
- "This plugin version can't sign in any more. Update the plugin." (dock), "Update the plugin to use
  Trigi" (Report a problem) and "Update required" (trigglow.com sign-in page) all mean that your
  version is too old. Once v0.6.0 is published, v0.5.x and earlier can no longer sign in, and the
  sign-ins they already have stop working after a transition period, so those versions can't use
  the delay any more.
- Download the latest version (**Get the update** in the dialog, the dock's update notice or the
  [latest release](https://github.com/VirosMs/obs-trigglow-dynamic-delay/releases/latest)) and
  install it with OBS closed. Your settings are kept, and a v0.5.x sign-in moves over by itself; if
  the dock asks you to sign in, do it once.

## Asking Trigi (Report a problem)
- **Trigi doesn't show up and the dialog opens on the ticket form.** Trigi needs Windows, a
  signed-in account and a plugin version it supports. Signed out, the dialog offers **Sign in to ask
  Trigi** and **Send a report without Trigi**; on macOS and Linux, use the web form. If Trigi is off
  for the moment, the form says "Trigi isn't available right now. Your message goes straight to the
  support team." and your message reaches a person.
- **What each message means:**
  - "No connection to trigglow.com. Check your connection and try again.": check the connection
    (firewall, proxy, VPN) and press **Try again**. Nothing is retried automatically.
  - "Trigi took too long to answer. Try again.": try again; if it keeps happening, create a ticket.
  - "trigglow.com isn't answering right now. Try again in a few minutes.": a temporary problem on our
    side.
  - "You've asked a lot of questions in a row. Try again in a few minutes.": **Send** stays off with a
    countdown, and "You can ask Trigi again." when it ends. Meanwhile, **Create a ticket** works.
  - "Trigi can't answer right now. Create a ticket and the team will help you.": Trigi isn't
    available for the moment; the ticket reaches the team.
  - "I can only help with Trigglow and Dynamic Delay.": ask about Dynamic Delay, or write to the team.
  - "Your Trigglow session ended. Sign in again to keep asking Trigi…": sign in again (it continues
    in the dock); what you typed is kept.
  - "This conversation was closed. Try again to start a new one.": your next question starts a new
    conversation.
  - "Update the plugin to use Trigi.": see "The plugin says to update" above; you can still send a
    report from the same dialog.
  - **Not sent** under a question: it didn't reach trigglow.com. **Try again** sends it again.
- **Trigi can be wrong.** Check what it says before changing your setup. If an answer doesn't help,
  press **No** under "Was this helpful?" and create a ticket with the conversation.
- **Links in Trigi's answers aren't clickable**, on purpose. The dialog's own buttons open the right
  pages (support on trigglow.com, the update on GitHub).

## Sending a report
- **Signed in (Windows)**: the ticket is created from your account, so you only write what happened
  (the subject is optional). To send the team OBS's log, open **See what is sent**, check it, and
  then tick **Attach OBS's log** (it starts unticked and can't be ticked before you've seen it):
  paths with your Windows user name, URLs, keys, emails and IPs are hidden, but names of scenes,
  sources, audio devices and windows may remain. "Ticket sent" offers **View ticket on
  trigglow.com**; replies arrive by email and in your support panel.
- **"OBS log not found. The ticket will be sent without it."**: the plugin only attaches the log of
  the OBS it runs in, and couldn't find it among that OBS's logs (for example, after deleting the log
  files while OBS was open). Send the report without it, or restart OBS and try again.
- **"The ticket was created, but the log couldn't be attached."**: the ticket exists. Press **Save
  redacted log…** and add that file to the ticket on trigglow.com.
- **Signed out**: the guest form asks for a name and an email to reply to. If the dialog says "To
  send the report from OBS, sign in. You can also use the web form.", trigglow.com is asking guests
  for a human check (captcha), which OBS can't show: sign in, or press **Open web support** and send
  it from your browser. **Save redacted log…** saves the redacted log so you can attach it there
  (check it first: names of scenes, sources, audio devices and windows may remain).
- **"Too many reports in a row. Try again in a few minutes."**: wait and send it again.
- **"Your Trigglow session has expired. Sign in again from the dock."**: sign in again and send it.
- **macOS and Linux**: the form is shown disabled (v0.6.0 and v0.7.0). Use
  [trigglow.com/support](https://www.trigglow.com/support) and attach OBS's log yourself (check it
  first: the plugin can't hide personal data from it there).

## "Encoding overloaded" (or lagged frames) as soon as the delay is on

**What caused it before v0.6.0.** While the delay is on, the plugin compresses every captured frame
(MJPEG) and decompresses the delayed one. Up to v0.5.0 that ran inside OBS's graphics thread — the
same thread that has to finish each frame of your stream in time (16.7 ms per frame at 60 fps,
33.3 ms at 30 fps) — and the buffer was stored at your **canvas** resolution. With a 2560x1440
canvas and a 1920x1080 output that cost 14.3/17.4 ms p50/p95 per frame in our benchmarks, most of a
60 fps frame, even though OBS throws those extra pixels away when it scales to the output. When that
work takes too long, OBS falls behind and shows "Encoding overloaded" (frames skipped due to
encoding lag) or rendering lag (frames missed).

**v0.6.0 fixes it.** Update first (the dock shows a notice when a newer version exists).
- The buffer is never stored above OBS's output resolution (`Settings → Video → Output (Scaled)
  Resolution`). If you lowered your canvas to work around the problem, you can set it back.
- Compression and decompression run on the plugin's own threads, not on OBS's graphics thread. What
  the plugin still does there (reading the frame back from the GPU and uploading the delayed one)
  costs ~1.2–1.7/1.6–2.1 ms p50/p95 per frame at 1440p60 in our offline benchmark, instead of
  ~14/17 ms.
- **A 1440p output at 60 fps is supported.** If your computer can't keep up with it, the plugin
  lowers the buffer's quality step by step instead of letting OBS fall behind, and tells you so in
  the dock (next section). The delay itself never changes.

**v0.7.0: the buffer and your GPU's video encoder.** On Windows the buffer now stores the delayed
frames with the GPU's hardware encoder (NVENC, AMF or Quick Sync) when there is one, which can be
the same engine your stream encodes on. On a shared engine it's built to give way to OBS's encoder:
when that engine gets busy it drops one resolution step, and if OBS skips even one frame for
encoding lag, or one of its outputs stops on an encoder error, it goes back to MJPEG at once, which
doesn't touch the engine. That shows in the log as a WARN like:

```
[obs-trigglow-dynamic-delay] [video-delay-filter] delay buffer codec hevc_nvenc -> mjpeg: OBS skipped frames for encoding lag, on an engine the buffer shares (3 frames skipped in 2 s); buffer encoder latency p95 38 ms -- the delay itself is unchanged, the buffer only takes more RAM until the delay is turned off
```

(illustrative values). The delay never cuts or shifts; the buffer just stays on MJPEG, with more
RAM, until you turn the delay off and on. A few skipped frames right before that line can come from
the buffer; if OBS keeps skipping frames after it, the buffer isn't what's overloading the encoder.
NVENC hasn't been validated inside a real OBS yet, so if you suspect the buffer:
- Set `"buffer_codec": "mjpeg"` (see "Hidden settings for support" below), restart OBS and compare
  with the same scene and settings. If "Encoding overloaded" goes away only with MJPEG, send a
  report with both logs.
- Look at the log (see "Reading the log lines"): `engine shared with OBS` in the
  `storage codec: auto -> …` line, and in the `storage codec check` lines the buffer encoder's
  latency (`hwenc lat`) and `obs encode-lag skipped main+N mixes+N`.

**If it still happens on v0.6.0 or later:**
- **Rule out the encoder itself:** if "Encoding overloaded" also shows up with the delay off, the
  plugin isn't the cause — check your encoder preset and settings.
- **Look at the dock while it happens.** An amber "Buffer quality lowered" notice means the plugin
  is already lightening its own work (see the next section).
- **Make the canvas equal to the output if you can** (`Settings → Video`: Base (Canvas) Resolution =
  Output (Scaled) Resolution). A larger canvas no longer costs the plugin anything extra, but OBS
  itself still renders every scene at canvas resolution.
- **Close the projectors, multiview and extra previews you don't need.** Each delayed frame is
  decompressed only once however many views are open, but every open view is still extra rendering
  work for OBS.
- Still stuck: use the dock's **Report a problem** button after having the delay on for at least
  30 seconds in the same OBS session, and attach the current OBS log (check it with **See what is
  sent**, then tick **Attach OBS's log**): it includes the `perf check`, `pipeline check` and
  `storage codec check` lines described below.

## The dock says "Buffer quality lowered" (amber notice)

**What it means.** Your computer couldn't keep up with the delay buffer at full quality, so the
plugin lightened it instead of letting OBS drop frames. The notice says what it lowered and why, for
example "Buffering at 1592×896 instead of 1920×1080 because compressing the frames takes a lot of
CPU. Your 30 s delay is unchanged." The causes it can name are OBS's graphics thread being under
heavy load, compressing the frames, or decompressing them. If any frames were repeated or dropped
in the last 10 seconds, a second line gives how many per minute; on stream they show as a repeated
frame, never as a change of the delay.

**What the plugin does, in order:**
1. Adds compression threads (up to two compression contexts with up to two threads each, since
   v0.7.0). You don't see a notice for this: quality is untouched. With the hardware storage codec
   (v0.7.0, Windows) there are no threads to add: the GPU does the compression.
2. Buffers at a lower resolution, in steps of about 17%, **never below the minimum quality** you
   chose in the dock.
3. As a last step, captures every other frame (30 fps on a 60 fps stream), only when your output
   runs at 48 fps or more.

It never shortens the delay and never empties the buffer for this. When the load drops and the
next step up has stayed comfortably within budget for 30 seconds in a row, it goes back up one
step at a time, and the notice disappears once the buffer is back at full quality.

**Since v0.7.0, a busy GPU video engine also lowers a step.** When the buffer's hardware encoder
shares the GPU's video engine with OBS's encoder and that engine gets busy (the buffer encoder's
latency rises, or frames queue up in it), the plugin lowers one resolution step so your stream's
encoder keeps its share, and doesn't go back up for 5 minutes. The notice then says the cause is
compressing the frames, and the log has a WARN starting with `storage codec hevc_nvenc: the buffer
encoder's latency rose on an engine OBS shares` (or `frames queued up in the buffer encoder`). If
that happens at your minimum quality, the buffer goes back to MJPEG instead (previous section).

**What you can do:**
- Nothing, if the delayed picture looks fine to you: it's working as designed.
- Close what's competing for the CPU (browser tabs with video, background encodes, a second
  recording) or lower the game's CPU load.
- Lower the output resolution or frame rate in `Settings → Video` if the notice shows up all the
  time at your current settings.
- A higher minimum quality keeps the buffer sharper, but leaves the plugin fewer steps before it
  has to halve the capture rate. With the minimum at your output resolution (for example 1440p
  (2K) with a 1440p output) there are no resolution steps left: only the capture rate can halve.
- If you think the notice is wrong, send a report with **Report a problem** with the delay on for
  at least 30 seconds: every rung change is a WARN line in the log, with its cause and the numbers
  behind it.

## Reading the log lines (for support)

While the delay is on (Filling or Active), the plugin writes three lines to the OBS log every 10
seconds (two before v0.7.0), plus a last set when you disable the delay, set it to 0 s or close OBS.
Example (illustrative values — yours will differ; each is one line in the log):

```
[obs-trigglow-dynamic-delay] [video-delay-filter] perf check (periodic, 10.0s): buf=1920x1080 (capped to OBS output 1920x1080, canvas 2560x1440) fps=60.00 | render p50/p95/max=2.10/2.90/4.80ms own=0.70/1.00/1.90 readback=0.25/0.40/0.90 upload=0.15/0.22/0.60 | calls=60.0/s captures=60.0/s uploads=60.0/s holds=0 (late decode 0, dropped 0, encoding 0) | obs frames+600 lagged+0 skipped+0
[obs-trigglow-dynamic-delay] [video-delay-filter] pipeline check (periodic): mode=workers codec=hevc_nvenc rung=0/4 capture=1920x1080@60.00fps enc=1x1 L=3 S=3 | enc0 p50/p95/max=2.40/3.70/6.10ms busy=15% enc1=0.00/0.00/0.00 busy=0% dec=2.40/2.90/4.80 busy=15% | drops reserve=0 backpressure=0 arena=0 capture=0 rawFallback=0 encFail=0 decFail=0 staleEnc=0 decJumps=0 staleDec=0 | queue enc max=1 avg=0.05 dec ahead avg=2.90 min=2 | ratio=54.0x arena=104MB committed (live 98MB, peak 112MB, 13 blocks of 8MB, limit 16384MB) ring=1831 slots (ready 1828, dropped 0, in flight 1, pinned 0)
[obs-trigglow-dynamic-delay] [video-delay-filter] storage codec check (periodic): codec=hevc_nvenc(qp20,gop30) dec=d3d11va | hwenc lat p50/p95/max=19.8/20.6/21.5ms inflight max=1 send p95=3.70ms | keys=20 forcedIdr=0 chainBreaks=0 orphanP=0 catchUps=0 (frames 0, max 0ms) noKeyWaits=0 noKeySkips=0 | bridge frames=0 codecChanges=0 hardErrors=0 | policy=Hw guard=off | obs encode-lag skipped main+0 mixes+0
```

**`perf check`** (what the plugin costs on OBS's graphics thread):

| Field | What it tells you |
|---|---|
| `perf check (reason, Ns)` | Why the line was written (`periodic`, `resumed after a pause`, `filter disabled`, `filter destroyed`, `delay set to 0`) and how many seconds it covers. |
| `buf=WxH (…)` | Full-quality buffer resolution, and why: `capped to OBS output …` (the output cap is active), `not capped …` (usually: the canvas is already equal to or smaller than the output), `cap off (buffer_output_cap=false) …` (cap disabled by hand, see below). `, reduced for RAM` means it also had to shrink to fit the delay in the RAM budget. What is captured right now, after any degradation, is `capture=` in `pipeline check`. |
| `render p50/p95/max` | Total time of each plugin call on the graphics thread, in ms, **including rendering your live scene**, which OBS would do anyway. |
| `own` | The plugin's own part of it: conversion, readback and copy, upload (and the codec in sync mode). This is what the degradation judges. Compare it with the frame budget: 16.7 ms at 60 fps, 33.3 ms at 30 fps. |
| `readback` / `upload` | GPU→CPU copy of each capture, and upload of the delayed frame to the GPU. |
| `calls`, `captures`, `uploads` (per second) | Plugin render calls (one per open view: Program, preview, projectors…), frames captured (close to your fps, or half of it on the last degradation step) and delayed frames uploaded. |
| `holds (late decode, dropped, encoding)` | Times the last frame was repeated because the one due wasn't ready, split by why: not decompressed in time, dropped at capture, or still being compressed. An occasional one is harmless; a steadily growing count is what makes the degradation step in. |
| `obs frames+ lagged+ skipped+` | OBS's own counters over the same window: frames rendered, frames missed due to rendering lag, frames skipped due to encoding lag. Non-zero `lagged`/`skipped` with a high `own` points at the plugin; with a low `own`, look elsewhere (game, encoder, GPU load). |

**`pipeline check`** (the compression threads, the buffer memory and the degradation):

| Field | What it tells you |
|---|---|
| `mode` | `workers` (normal) or `sync` (`codec_workers` turned off, see below: the codec runs on the graphics thread). |
| `codec` (v0.7.0) | What stores the frames right now: `mjpeg`; a hardware encoder on Windows, named as FFmpeg names it (`hevc_nvenc`, `hevc_amf`, `hevc_qsv`, or `h264_nvenc`, `h264_amf`, `h264_qsv` on a GPU without HEVC); `raw` on macOS (uncompressed). `mjpeg` on Windows can mean no hardware encoder, the first seconds while one is being checked, a return to MJPEG (a WARN says why) or `buffer_codec` set to `mjpeg`: the `storage codec check` line below tells them apart. |
| `rung=N/M` | Degradation step: `0` is full quality, `M` the last step. Anything above 0 is what the dock's amber notice is about. |
| `capture=WxH@fps` | What is being captured right now. |
| `enc=CxS`, `L`, `S` | Compression contexts × threads per context, frames decompressed ahead (`L`) and staging slots (`S`). |
| `enc0`, `enc1`, `dec` | p50/p95/max ms per frame of each compression thread and of the decompression thread, and how busy each was over the window. |
| `drops reserve/backpressure/arena/capture` | Captures that never made it into the buffer: no free slot, compression behind (the newest capture is dropped), buffer memory at its limit, or a GPU readback that failed. Each shows as a repeated frame, never as a shift of the delay. |
| `rawFallback`, `encFail`, `decFail` | Frames stored uncompressed because compression failed, and compression/decompression failures. Should be 0. |
| `staleEnc`, `decJumps`, `staleDec` | Compressions thrown away because the slot was reused, times the decompressor jumped ahead to catch up, and frames decompressed ahead that were too old to show after a pause in rendering. |
| `queue enc`, `dec ahead` | Compression queue depth (max, average) and frames ready ahead of what's on screen (average, minimum). |
| `ratio`, `arena`, `ring` | Compression ratio, buffer memory (committed, live, peak, blocks and their size, limit) and slot states. `committed` is the stored frames, the biggest part of what Task Manager shows for the buffer: at 1440p60 with 30 s and gameplay, roughly 200–250 MB with the hardware codec (`ratio=` around 40–60x) and 600–900 MB with MJPEG (around 11–15x). The codecs and frame pools add about 50 MB (MJPEG) to 110 MB (hardware) on top. |

**`storage codec check`** (v0.7.0: how the frames are stored, and the guard for OBS's encoder):

| Field | What it tells you |
|---|---|
| `codec=…` | The encoder storing the frames, with its settings: `hevc_nvenc(qp20,gop30)` (constant QP 20, a key every 30 frames), or `mjpeg(q5,huff=default)`. `bridging` after MJPEG means a hardware encoder is being set up: MJPEG stores meanwhile. When the whole cycle is MJPEG the line is short: `codec=mjpeg(…) dec=mjpeg policy=Mjpeg` and the OBS counters. |
| `dec=` | What decodes them: `d3d11va` (the GPU), `sw` (the CPU, only in an emergency: the GPU decoder failed, or the buffer left an engine it shared with OBS), or `mjpeg`. |
| `hwenc lat p50/p95/max`, `inflight max`, `send p95` | Time from handing a frame to the hardware encoder to getting its packet (about one frame, ~20 ms at 60 fps, is normal), the most frames waiting inside it, and how long handing a frame over took. A latency p95 above 2.5 frames (42 ms at 60 fps), or 3 frames waiting, on an engine OBS also encodes on, lowers a step. |
| `keys`, `forcedIdr`, `chainBreaks`, `orphanP` | Key frames stored, keys forced (after a lost packet or a change), packets lost after their encode (the arena full) and the P-frames dropped after them until the next key. Should stay near 0 apart from `keys`. |
| `catchUps (frames, max)`, `noKeyWaits`, `noKeySkips` | Times the decoder started from an earlier key to reach the frame due (after a pause or a jump), and times it waited for the next key instead: the frame on screen is held meanwhile. |
| `bridge frames`, `codecChanges`, `hardErrors` | Frames MJPEG stored while a hardware encoder was being set up, changes of the codec in use, and failures of the hardware encoder or decoder (each one sends the buffer back to MJPEG). |
| `policy=`, `guard=` | The guard's state: `Probing` (checking an encoder), `Hw` (hardware in use), `HwGuarded` (a step was lowered for the engine), `Yielded` (NVENC handed over to an OBS output starting), `Evacuated` (back on MJPEG until the next Enable), `Mjpeg` (MJPEG only). `guard=on` while going back up is vetoed (5 minutes after an engine warning). |
| `obs encode-lag skipped main+N mixes+N` | Frames OBS skipped for encoding lag in the window, in its main mix and in its encoders' own mixes (GPU rescaling, Enhanced Broadcasting tracks). With the engine shared, one is enough to send the buffer back to MJPEG. |

Other lines worth knowing: `settings loaded` (when OBS starts) and `enabled` (each time you press
Enable) show `output_cap=on|off`, `codec_workers=on|off` and `buffer_codec=auto|mjpeg`, and
`enabled` the cycle's `storage_class`; `delay pipeline: …` (each time the buffer starts) gives the
configuration chosen, the storage class (`intra`, `intra-gop` or `inter`, see `docs/SPEC.md` §4.8)
and the degradation ladder; a WARN starting with `delay buffer lower rung` or `delay buffer higher
rung` marks each degradation step, with its cause and the last 2 seconds of numbers. Since v0.7.0
also: `storage codec: auto -> …` when a hardware encoder passes its check (with `engine shared with
OBS: yes|no|unknown` and OBS's NVENC sessions) or `storage codec: auto -> mjpeg at … (…)` with the
reason; the WARN `delay buffer codec … -> mjpeg: …` on a return to MJPEG; `storage codec: … yielded
its engine to an OBS output starting`; and `encoder ceiling WxH: …` or `no encoder ceiling: …` when
the stream's encoder sets the buffer's size.

**Sign-in, Trigi and report lines** (since v0.6.0) say what happened and, on errors, the HTTP code:
`[auth-manager] signed in`, `signed out`, `session ended`, `token renewed`, `migrated legacy login`;
`[trigi] session started (available=true)`, `[trigi] message sent (len=…, diagnostics=on|off)`,
`[trigi] reply (HTTP 200, len=…, action=…)`, `[trigi] rate limited`; `[support] ticket created
(attachment=yes|no)`. They never contain the sign-in link or codes, tokens, ticket or conversation
IDs, what you asked, Trigi's answers or the technical data.

## Hidden settings for support: `buffer_output_cap`, `codec_workers` and `buffer_codec`

Three settings with no dock control on purpose. Use them only if support asks you to, or if you know
exactly why you need one.

**`buffer_output_cap`: turning the output cap off.** Only for someone who records (or sends an
Enhanced Broadcasting track) above the output resolution and prefers that quality for the delayed
part at the cost of CPU and RAM. It brings back v0.5.0's sizing (canvas resolution, only reduced
for RAM): more pixels to compress per frame, and with more to store per second the same RAM holds
less, so if RAM runs short the buffer is reduced or the delay ends up shorter than requested. When
it would make a difference, the log warns with a line like `video encoder "…" (output "…") is
2560x1440, above the delay buffer's 1920x1080 …: its delayed video is upscaled`, which also gives
the size the buffer would have without the cap (and, if it wouldn't fit, how many seconds it would
hold).

**`codec_workers`: running the codec on OBS's graphics thread again (kill switch).** Turns the
compression threads off: compression and decompression go back to OBS's graphics thread, as before
v0.6.0 (with the slice-threaded compressor). Memory is still freed off that thread, and the
degradation still works (it adds compression threads first). It exists to find out whether a
problem comes from the compression threads — for example a frozen or out-of-order delayed picture,
or a crash in a thread named `trigglow-enc`, `trigglow-dec` or `trigglow-reap`. It brings back the
graphics-thread cost, so at 1440p60 expect "Encoding overloaded" or the amber notice much sooner.
With the compression threads off, the buffer always uses MJPEG, whatever `buffer_codec` says.

**`buffer_codec`: forcing MJPEG instead of the hardware storage codec (v0.7.0).** By default
(`"auto"`) the buffer uses the GPU's hardware encoder on Windows when there is one, and MJPEG
otherwise. `"buffer_codec": "mjpeg"` makes it use MJPEG always, as before v0.7.0: about twice the
RAM and more CPU, but the GPU's video engine untouched. It exists to rule the hardware codec in or
out of a problem, for example "Encoding overloaded" with the delay on, a GPU driver issue, or a
crash in the thread named `trigglow-codec-setup`. It has no effect on Linux and macOS, which never
use the hardware codec.

**How to set any of them:**
1. **Close OBS completely.** The settings are only read when OBS starts, and OBS rewrites this file
   when it exits, so a change made with OBS open is lost.
2. Open `%APPDATA%\obs-studio\plugin_config\obs-trigglow-dynamic-delay\buffer-mode-settings.json`
   in a text editor (portable OBS: `config\obs-studio\plugin_config\obs-trigglow-dynamic-delay\`
   inside the OBS folder; macOS/Linux: the same `plugin_config/obs-trigglow-dynamic-delay/` folder
   inside OBS's config directory). If the file doesn't exist yet, open and close OBS once with the
   plugin installed.
3. Add `"buffer_output_cap": false`, `"codec_workers": false` and/or `"buffer_codec": "mjpeg"` as
   one more key, keeping everything else as it is (mind the comma on the line before). The first two
   have to be a literal `false`, without quotes: `"false"` (a string), `0` or anything else keeps
   the setting on and the log says why (`"codec_workers" … is not a JSON boolean`). `buffer_codec`
   has to be exactly the string `"mjpeg"`, in quotes and lowercase: `"MJPEG"`, `"hevc"`, `false` or
   anything else keeps `"auto"` and the log says why (`"buffer_codec" … is neither "auto" nor
   "mjpeg" -- keeping auto`); without the quotes the file isn't valid JSON (see below). A value that
   isn't taken is dropped from the file when OBS closes.

   ```json
   {
       "delay_seconds": 30,
       "overlay_corner": 0,
       "buffer_codec": "mjpeg"
   }
   ```

   (Your file has more keys and your own values; only the last line is new.)
4. Start OBS. The `settings loaded` line shows `output_cap=off`, `codec_workers=off` and/or
   `buffer_codec=mjpeg`, and so does the `enabled` line when you press Enable. With the cap off,
   `perf check` says `cap off (buffer_output_cap=false)`; with the compression threads off,
   `pipeline check` says `mode=sync`; with `buffer_codec` at `mjpeg`, it says `codec=mjpeg`, and the
   log says `storage codec: MJPEG (buffer_codec=mjpeg)` when the delay starts.

To undo it: with OBS closed, delete the key (or set `true` for the first two, `"auto"` for
`buffer_codec`) and start OBS again.

**If the file ends up as invalid JSON** (a missing comma or quote is enough), the plugin doesn't
overwrite it: at start it moves it aside as `buffer-mode-settings.json.<date-time>.invalid` in the
same folder, uses the defaults (your live and loading scenes, delay and overlay settings too, until
you set them again) and writes a WARN with both paths (`settings file … is not valid JSON -- moved
it to …`). Fix the copy, close OBS, rename it back to `buffer-mode-settings.json` and start OBS
again.

## The buffer takes more RAM than the FAQ's figures

The table in `docs/FAQ.md` ("How much RAM does the delay buffer take?") is for the hardware storage
codec on Windows. Look at `codec=` in the `pipeline check` lines and at the `storage codec` lines of
the OBS log:
- **`codec=mjpeg` the whole time**, with `storage codec: auto -> mjpeg at … (no hardware encoder to
  try …)` or `(no hardware encoder works …; tried: …)`: no GPU encoder could be used (no NVIDIA, AMD
  or Intel GPU with a video encoder, or it failed its check). The buffer works as before, with
  MJPEG's RAM. Linux always uses MJPEG and macOS stores the buffer uncompressed.
- **`storage codec: MJPEG (buffer_codec=mjpeg)`** or `codec_workers=false`: MJPEG on purpose (see
  "Hidden settings for support").
- **A WARN `delay buffer codec … -> mjpeg: …`**: the buffer went back to MJPEG while the delay was
  on (OBS skipped frames on a shared engine, an OBS encoder error, or an error of the hardware
  codec). Its RAM then grows to MJPEG's figure over the next delay seconds, within the budget, until
  you turn the delay off. Turning it off and on tries the hardware again (an encoder that failed
  itself isn't tried again until OBS restarts).
- **`policy=Yielded`** for a few seconds after you start streaming, recording or the Replay Buffer:
  the buffer gave its NVENC session to that output and stores MJPEG meanwhile.
- Content that changes a lot every frame (fast camera, particles, grain) compresses worse with any
  codec: a lower `ratio=` in `pipeline check`.

Whatever the codec, the buffer never takes more than its RAM budget: if the frames don't fit, it
drops frames (a repeated frame on screen) instead of growing.

## The delayed video looks lower quality than expected
- **Check the dock first.** An amber "Buffer quality lowered" notice means the plugin is buffering
  at a lower resolution or frame rate for now because your computer can't keep up (see above). It
  goes back to full quality by itself when the load drops.
- Otherwise it's buffer sizing, not a bug: `EnsureRingSized()` computes a RAM budget from your
  machine's total system RAM (50% of it, clamped between 1GB and 24GB) and never lets the buffered
  video drop below the **minimum quality** you selected (480p to 2160p (4K)) — instead, if your
  requested seconds × that quality don't fit the budget, it shortens the **actual buffered
  duration**. And the buffer is never stored above OBS's output resolution, so with a 720p output
  it's 720p even with a 1080p minimum — OBS doesn't send anything above its output anyway.
- Since v0.7.0, when every encoder that sees your canvas encodes below the output (for example a
  stream rescaled to 1080p from a 1440p output, with no recording above it), the buffer is stored at
  that encoder's size, since nothing uses more: the log says `encoder ceiling WxH: …`. Starting a
  recording at a higher resolution lifts it again by itself; its first `delay` seconds are upscaled.
- With a canvas larger than the output, the delayed frame is drawn back at canvas size and OBS
  scales it down again, which can look slightly softer than the live picture. If you don't need the
  larger canvas, set canvas = output.
- If you **record** at a resolution above your output (e.g. recording rescaled to the canvas), the
  delayed part of that recording is upscaled from the output-sized buffer; the log warns about it.
  See `buffer_output_cap` above if you prefer the old behavior.
- To see whether your requested combination of seconds + quality actually fits your RAM, watch the
  RAM estimate line under the dock's delay and quality fields *before* pressing Enable — "RAM fits
  the full 30 s at 1920×1080", or "RAM only fits 41 s of 60 s at …" (non-blocking). While the delay
  runs it gives the same whole seconds as the status chip, and it hides while the amber "Buffer
  quality lowered" notice is shown, which then says what the buffer really holds. See
  `docs/SPEC.md` §3.5 for the full sizing logic.
- If you want the full requested duration guaranteed, lower the quality floor, lower the requested
  seconds, or free up system RAM (close other RAM-heavy applications) before pressing Enable.

## After updating, a Stream Deck or obs-websocket action no longer works
- v0.6.0 renamed the objects the plugin manages: "(no tocar)" / "(auto, no tocar)" became "(auto)".
  The scene is now **Trigglow Delay Buffer (auto)**, the filters **Trigglow Buffer Mode Delay
  (auto)** and `Trigglow Audio Delay (auto) - <source>`, and the overlay **Trigglow Delay Badge
  (auto)**. Your collections were renamed automatically the first time they loaded.
- Any action that refers to them **by name** (switch to the helper scene, toggle a delay filter or
  the overlay source) must be pointed at the new names. Actions that use the plugin's hotkeys
  ("Trigglow: Toggle Dynamic Delay"…) aren't affected.

## I went back to an older version and now there are two helper scenes
- v0.5.0 and earlier don't know the "(auto)" names, so they create their own "(no tocar)" helper
  scene, delay filters and overlay next to the renamed ones (the "(auto)" filters are switched off
  when the collection loads, so they stay inert).
- Delete the set you don't use. If you come back to v0.6.0 later, the "(auto)" set is used, and a
  leftover "(no tocar)" set isn't renamed while its new name is taken: delete it.

## The delay turned off when I switched scene collection
- **That's on purpose.** The helper scene and the delay filters belong to the collection you were
  in, so the new one would run without any delay while the dock still said "Delay on". Since v0.6.0
  the plugin turns the delay off before OBS leaves the old collection (switching, creating or
  removing a collection), saves it with its filters off and its scene back on Program, and the dock
  says: "The delay turned off because you switched scene collection. Turn it on again when you're
  ready."
- Duplicating or renaming a collection works the same way, with one difference: OBS 31 doesn't
  announce those in advance, so the plugin stops the delay right after the copy has loaded,
  switching off the copy's filters and leaving the helper scene if the copy opened on it.
- Press `Enable delay` again in the new collection when you're ready (pick its live scene first if
  it's different).

## I deleted one of the plugin's filters while the delay was on (known issue)
- Deleting an "(auto)" delay filter by hand while the delay is on lets the live video out without
  delay: the helper scene then shows your live scene with no filter. It stays that way until you
  press `Disable delay` and `Enable delay` again in the dock, which recreates the filter (the buffer
  fills again first).
- Don't delete or rename the plugin's scene and filters; to stop the delay, use the dock.

## I switched Program to my live scene and the stream went out without delay (known issue)
- While the delay is on, Program has to stay on the scene the plugin chose: the loading scene while
  it fills, then its helper scene "Trigglow Delay Buffer (auto)", which holds your live scene with
  the delay filter. If you click your live scene in OBS's scene list (or a Stream Deck/obs-websocket
  action switches to it), Program shows the live scene directly and the stream gets it **without
  delay**. This was already the case before v0.6.0.
- Leave Program on the scene the plugin chose while the delay runs. To change scenes, turn the delay
  off from the dock first, switch, and turn it on again (the buffer fills again).

## The buffer never seems to fill / `Preparing delay` never reaches `Delay on`
- Check the OBS log for `[obs-trigglow-dynamic-delay]`-prefixed warnings around the time you pressed
  Enable. Two known causes that log a specific warning instead of silently doing nothing:
  - `Render(): obs_filter_get_target() returned null` — the wrapper scene's filter target
    disappeared (e.g. the live scene was deleted or renamed out from under the plugin after Enable).
  - `Render(): target's base size is 0x0, skipping until it's real` — the live scene hasn't produced
    a real frame size yet (this is normal for the first render or two right after Enable and should
    resolve itself; if it persists, check that the live scene actually has visible, active sources in
    it).
- If neither warning appears and the countdown just seems slow, that's expected: the fill window is
  exactly as long as the delay you requested — a 60-second delay takes 60 real seconds to fill
  before switching to `Delay on`.
- If the dock goes back to `Preparing delay` while the delay was on, the buffer had to restart (an
  OBS video settings change). The log says `the video buffer restarted empty`; Program shows the
  loading scene until it refills.

## The status stays stuck in `Preparing delay` far longer than the requested seconds
- This is a symptom, not a documented mode — the plugin has no reconnection or "Applying" state to
  wait on. If it genuinely never reaches `Delay on`, follow the section above and check the OBS log
  for warnings.

## The hotkey doesn't do anything
- Confirm you assigned it in `Settings → Hotkeys` (searching for "Trigglow") — the plugin registers
  3 hotkeys (`Trigglow: Toggle Dynamic Delay`, `Trigglow: Enable Dynamic Delay`,
  `Trigglow: Disable Dynamic Delay`; in Spanish, `Trigglow: Activar/desactivar Dynamic Delay`,
  `Trigglow: Activar Dynamic Delay` and `Trigglow: Desactivar Dynamic Delay`) with no key assigned
  by default.
- Try the dock button first to rule out an issue with the plugin's own configuration (e.g. no live
  scene chosen, which also blocks the hotkey's Enable/Toggle from doing anything), as opposed to the
  hotkey/Stream Deck binding itself.

## The Stream Deck button doesn't do anything
- Verify that the combination configured in Stream Deck exactly matches the one assigned in
  OBS.
- If the button switches scenes or toggles filters by name instead of using the hotkey, see "After
  updating, a Stream Deck or obs-websocket action no longer works" above.
- See `docs/STREAM_DECK.md` for the full step-by-step flow.

## The plugin doesn't compile
- Check `docs/BUILD_VALIDATION.md` — it documents what's been verified (real CI builds against the
  real OBS SDK and Qt6 on Windows/macOS/Ubuntu, plus live testing inside a real OBS instance on
  Windows) and what hasn't (live testing on macOS/Linux, and a confirmed fix for the GPU
  Device-Remove/Reset risk described in `docs/SPEC.md` §7). The unit tests (23 suites, no OBS
  needed) build with `-DTRIGGLOW_BUILD_TESTS=ON` or on their own with `cmake -S tests -B
  build_tests`, and CI runs them on every platform; a 24th, `hw_storage_codec_tests`, is added on
  Windows when the plugin's FFmpeg is already in `.deps` (a local build of the plugin).
- A compile error on your own machine is valuable information — it's likely a difference between
  your exact OBS/Qt version and the ones pinned in `buildspec.json`. Open an issue in the repository
  with the full error log.

## I don't know if the problem is the plugin or my streaming configuration
- Disable the plugin (temporarily remove it from the plugins folder) and confirm that streaming
  works fine without it. If the problem persists without the plugin, it's not the plugin's fault —
  and this is a particularly clean test for this plugin specifically, since it never touches the
  streaming output at all; if you have streaming trouble, it did not cause it.
