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
Independently, by design — you pick both freely, and the plugin never silently lowers your chosen
quality floor (480p/720p/1080p). What it *can* do is shorten the actual buffered duration if your
requested seconds don't fit the RAM budget detected on your machine at that quality. The dock shows
a live, non-blocking estimate of what a given combination will actually achieve as you adjust
either setting, before you ever press Enable. See `docs/SPEC.md` §3.5.

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
No. All control lives inside OBS. The Trigglow website is only for downloading it and checking the
documentation.

**Do I need Trigglow's Stream Deck plugin?**
No — the plugin uses native OBS hotkeys, which Stream Deck can trigger directly with its own
"System: Hotkey" action. See `docs/STREAM_DECK.md`.

**Is it a finished product?**
No — it's explicitly marked as **MVP / Early Access**. It's functional and meant for real live use,
but with a deliberately limited scope (see `docs/ROADMAP.md` for what comes next), and one risk
area — a GPU crash observed under an earlier design — that's believed reduced but not yet
specifically re-confirmed (`docs/SPEC.md` §7).
