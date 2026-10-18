*[Versión en español](FAQ-es.md)*

# FAQ — Trigglow Dynamic Delay

**Does the stream cut out or reconnect when I enable, disable, or use the delay?**
No — never, at any point, for any reason. That's the entire point of this plugin: it never calls
into OBS's own streaming-output delay setting at all. Instead, two OBS filters buffer video and
audio in a system-RAM ring, and the plugin switches OBS's Program between your live scene, an
optional loading scene, and a delayed "wrapper" scene entirely internally — the streaming output
itself never sees any of that happen. Full technical detail, including why an earlier
reconnect-based design was tried and abandoned, in `docs/SPEC.md` §2–§3.

**Is the audio delayed too, in sync with the video?**
Yes. Video and audio use the same configured delay in seconds, so they land back in sync — audio
uses a separate filter attached to each individual audio-capable source inside your live scene
(OBS doesn't allow an audio filter to attach directly to a scene itself), but both run off the same
`delaySeconds` value. As of v0.3.2, this stays true even if the RAM budget forces video to actually
buffer less than the full requested duration (see the next question) — audio automatically matches
whatever video is really able to deliver instead of drifting ahead. See `docs/SPEC.md` §3.2/§3.3.

**How do the "Delay (seconds)" and "Minimum quality" settings interact?**
Independently, by design — you pick both freely. The minimum quality (480p/720p/1080p) is how far
the plugin may lower the buffer's resolution to save RAM: it never goes below it, and shortens the
actual buffered duration instead if your requested seconds don't fit the RAM budget detected on
your machine at that quality. Since v0.5.1 it never raises the buffer above OBS's output resolution,
though (Settings → Video → Output (Scaled) Resolution): with a 720p output the buffer is 720p even
if you pick 1080p, because OBS has nothing above its output to send. The dock shows a live,
non-blocking estimate of what a given combination will actually achieve (seconds and resolution) as
you adjust either setting, before you ever press Enable. See `docs/SPEC.md` §3.5.

**What happens if my requested delay + quality doesn't fit in RAM?**
You get a clear warning in the dock *before* enabling — it never fails silently and never crashes.
The plugin simply buffers fewer seconds than requested at your chosen quality rather than ever
dropping below it; lowering the seconds or the quality floor gets you back to the full requested
duration.

**What's the maximum delay it supports?**
The dock's spinner accepts 1–60 seconds as a UI sanity range. The real ceiling is whatever fits
your machine's detected RAM budget (roughly half of total system RAM, floored at 1GB and capped at
24GB) at your chosen quality — if 60s at your chosen quality doesn't fit, the dock's live estimate
will tell you before you enable it, and the plugin buffers as many seconds as actually fit instead.

**Does it compress the buffer now? Does that lower quality?**
As of v0.3.0, yes — on Windows and Linux, each buffered frame is now MJPEG-compressed in RAM and
decompressed back on playback, with an automatic, safe fallback to uncompressed storage if the codec
isn't available for any reason. No quality loss: the quality floor you pick (480p/720p/1080p) works
exactly as before, this only changes how the buffered frames are stored in memory. On a real 30s@1080p60
test session, this cut the buffer's RAM usage roughly in half (~6.3GB → ~2.9GB). The exact reduction
depends heavily on what's on screen and hasn't been measured against sustained real gameplay yet — see
`docs/SPEC.md` §4. macOS isn't part of this yet and keeps working exactly as before, uncompressed.

**OBS shows "Encoding overloaded" as soon as I enable the delay. Why?**
Compressing and decompressing the buffered frames runs inside OBS's graphics thread, which has
16.7 ms per frame at 60 fps. Up to v0.5.0 the buffer was stored at your canvas resolution, so with a
canvas larger than the output (for example 2560x1440 canvas, 1920x1080 output) that work took most
of the frame and OBS fell behind. **v0.5.1 fixes that case**: the buffer is never stored above OBS's
output resolution and compression uses several threads, so a 1440p canvas with a 1080p output costs
about the same as a plain 1080p setup. Update to v0.5.1; if you lowered your canvas to work around
it, you can set it back. If it still happens, see the "Encoding overloaded" section of
`docs/TROUBLESHOOTING.md`.

**Does it only work at 1080p? What about 2K (1440p)?**
It works at any resolution — the buffer follows OBS's output resolution. What matters for
performance is the *output* resolution and frame rate, not the canvas:
- **Canvas larger than the output** (e.g. 1440p canvas, 1080p output): fine since v0.5.1, same cost
  as a 1080p output.
- **1440p output at 30 fps:** should work.
- **1440p output at 60 fps:** may still overload OBS on many machines, because compression still runs
  inside OBS's graphics thread. Moving it off that thread, with 1440p60 as the target, is the main
  goal of the next version (v0.6.0, see `docs/ROADMAP.md`); there's no date for it yet. Until then,
  use a 1080p output for 60 fps, or 30 fps for a 1440p output.

**Does it affect local recording or the Replay Buffer?**
Indirectly, yes, and this is worth understanding: buffer mode works by switching what OBS's Program
is showing, and both local recording and the Replay Buffer capture that same Program output — so if
you're recording (or have the Replay Buffer running) while the delay is `Active`, what gets saved is
the delayed content too, same as what viewers see. There's currently no way to delay the stream
without also delaying recording/Replay Buffer, or vice versa — that per-output split is explicitly
out of scope for now (see `docs/ROADMAP.md`).

**What triggers the `Error` state?**
Anything that would otherwise leave the plugin in a broken but silent condition: no live scene
chosen, or a chosen live scene that no longer resolves to anything (e.g. it was deleted). The dock
shows a clear message explaining what's wrong instead of hanging or crashing.

**Does it work with Twitch/YouTube/Kick/any platform?**
Yes — buffer mode works entirely on OBS's own Program output, never on the streaming connection to
any specific platform, so the behavior is identical no matter where you're streaming to.

**Do I need a web panel or a Trigglow account to use it?**
No web panel: all control lives inside OBS. Since v0.3.3 you do need to sign in once to a free
trigglow.com account before Enable works (the sign-in happens in your browser, and the plugin stays
100% free) — see `docs/ACCOUNT_GATE.md`.

**Do I need Trigglow's Stream Deck plugin?**
No — the plugin uses native OBS hotkeys, which Stream Deck can trigger directly with its own
"System: Hotkey" action. See `docs/STREAM_DECK.md`.

**Is it a finished product?**
No — it's explicitly marked as **MVP / Early Access**. It's functional and meant for real live use,
but with a deliberately limited scope (see `docs/ROADMAP.md` for what comes next), and one risk
area — a GPU crash observed under an earlier design — that's believed reduced but not yet
specifically re-confirmed (`docs/SPEC.md` §7).
