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
whatever video is really able to deliver instead of drifting ahead. Since v0.6.0 both use exactly
the same delay in whole seconds in that case too (before, they could end up to ~1 s apart), and the
dock and the live overlay show that same figure. See `docs/SPEC.md` §3.2/§3.3.

**How do the "Delay (seconds)" and "Minimum quality" settings interact?**
Independently, by design — you pick both freely. The minimum quality (480p to 2160p (4K)) is how far
the plugin may lower the buffer's resolution to save RAM: it never goes below it, and shortens the
actual buffered duration instead if your requested seconds don't fit the RAM budget detected on
your machine at that quality. Since v0.6.0 it never raises the buffer above OBS's output resolution,
though (Settings → Video → Output (Scaled) Resolution): with a 720p output the buffer is 720p even
if you pick 1080p, because OBS has nothing above its output to send. It's also the floor for the
automatic quality reduction when your computer can't keep up (see below). The dock shows a live,
non-blocking estimate of what a given combination will actually achieve (seconds and resolution) as
you adjust either setting, before you ever press Enable. See `docs/SPEC.md` §3.5.

**What happens if my requested delay + quality doesn't fit in RAM?**
You get a clear warning in the dock *before* enabling — it never fails silently and never crashes.
The plugin simply buffers fewer seconds than requested at your chosen quality rather than ever
dropping below it; lowering the seconds or the quality floor gets you back to the full requested
duration.

**Why can't I change the delay while it's on?**
Since v0.6.0 the scenes, the delay seconds and the minimum quality are locked while the delay is
preparing or on, with a line that says "Turn the delay off to change it." Before, the seconds stayed
editable and every keystroke resized the buffer in the middle of a live stream. Turn the delay off,
change it and turn it on again; it fills again for the new number of seconds.

**What's the maximum delay it supports?**
The dock's spinner accepts 1–60 seconds as a UI sanity range. The real ceiling is whatever fits
your machine's detected RAM budget (roughly half of total system RAM, floored at 1GB and capped at
24GB) at your chosen quality — if 60s at your chosen quality doesn't fit, the dock's live estimate
will tell you before you enable it, and the plugin buffers as many seconds as actually fit instead.

**Does it compress the buffer? Does that lower quality?**
Yes, since v0.3.0 — on Windows and Linux, each buffered frame is MJPEG-compressed in RAM and
decompressed back on playback, with an automatic, safe fallback to uncompressed storage if the codec
isn't available for any reason. The compression is visually close to lossless, and the quality floor
you pick (480p to 2160p (4K)) works exactly as before: this only changes how the buffered frames are
stored in memory. Real gameplay compresses about 11–15x (measured live since v0.4.0), though it
depends on what's on screen. Since v0.6.0 the compression runs on the plugin's own threads, not on
OBS's graphics thread, and memory follows what the frames really weigh: about 0.6–0.8 GB for 30 s at
1440p60 in our offline simulation. See `docs/SPEC.md` §4. macOS isn't part of this yet and stores
the buffer uncompressed.

**OBS shows "Encoding overloaded" as soon as I enable the delay. Why?**
Up to v0.5.0, compressing and decompressing the buffered frames ran inside OBS's graphics thread,
which has 16.7 ms per frame at 60 fps, and the buffer was stored at your canvas resolution: with a
canvas larger than the output (for example 2560x1440 canvas, 1920x1080 output) that work took most
of the frame and OBS fell behind. **v0.6.0 fixes it**: the buffer is never stored above OBS's output
resolution, and compression runs on the plugin's own threads, so what's left on OBS's graphics
thread is a millisecond or two per frame. Update to v0.6.0; if you lowered your canvas to work
around it, you can set it back. If it still happens, see the "Encoding overloaded" section of
`docs/TROUBLESHOOTING.md`.

**Does it only work at 1080p? What about 2K (1440p)?**
It works at any resolution — the buffer follows OBS's output resolution. What matters for
performance is the *output* resolution and frame rate, not the canvas:
- **Canvas larger than the output** (e.g. 1440p canvas, 1080p output): fine since v0.6.0, same cost
  as a 1080p output.
- **1440p output at 30 or 60 fps:** supported since v0.6.0. In our offline benchmark at 1440p60 (an
  8-core/16-thread CPU) the plugin's work on OBS's graphics thread is ~1.2–1.7/1.6–2.1 ms p50/p95 per
  frame, and compression and decompression keep under one CPU core busy in total on their own
  threads.
- **Minimum quality 1440p (2K) or 2160p (4K):** in the dock since v0.6.0. With a 1440p output and
  a 1440p minimum the buffer never drops below 1440p, neither to save RAM nor when your computer
  can't keep up (then only the capture frame rate halves): if the seconds don't fit in RAM, the
  delay gets shorter instead, as with 1080p. Above your output it changes nothing: with a 1080p
  output the buffer stays 1080p. To see the RAM the buffer really takes, look for
  `arena=…MB committed` in the `pipeline check` lines of the OBS log (every 10 s while the delay
  runs): at 1440p60 with 30 s expect roughly 600–900 MB with gameplay (`ratio=` around 11–15x). The
  plugin plans for up to ~1.9 GB there (5x), and the dock warns before you enable it if that doesn't
  fit.
- **If your computer can't keep up** at your settings, the plugin lowers the buffer's resolution
  (never below your minimum quality) and, as a last step, its capture frame rate, instead of
  letting OBS fall behind or cutting the delay. The dock tells you with an amber notice (next
  question).

**What does the amber "Buffer quality lowered" notice in the dock mean?**
Your computer couldn't keep up with the delay buffer at full quality, so the plugin lightened it:
first it adds compression threads (no notice for that), then it buffers at a lower resolution, never
below your minimum quality, and as a last step it captures every other frame (only with an output at
48 fps or more). The notice says what was lowered and why. The delay in seconds never changes and
the stream never cuts; repeated frames, if any, show as a brief stutter. When the load drops, it goes
back up by itself after 30 s with room to spare, and the notice disappears. What to do about it, if
anything: `docs/TROUBLESHOOTING.md`.

**Does it affect local recording or the Replay Buffer?**
Indirectly, yes, and this is worth understanding: buffer mode works by switching what OBS's Program
is showing, and both local recording and the Replay Buffer capture that same Program output — so if
you're recording (or have the Replay Buffer running) while the delay is `Active`, what gets saved is
the delayed content too, same as what viewers see. There's currently no way to delay the stream
without also delaying recording/Replay Buffer, or vice versa — that per-output split is explicitly
out of scope for now (see `docs/ROADMAP.md`).

**The plugin's scene and filters have new names. Do I need to do anything?**
Since v0.6.0 they end in "(auto)" instead of "(no tocar)": **Trigglow Delay Buffer (auto)**,
**Trigglow Buffer Mode Delay (auto)**, `Trigglow Audio Delay (auto) - <source>` and **Trigglow Delay
Badge (auto)**. Your scene collections are renamed automatically. Only update Stream Deck,
obs-websocket or other remote-control actions that use those objects **by name**; the plugin's
hotkeys aren't affected. If you go back to v0.5.0 or earlier afterwards, it creates its own
"(no tocar)" set next to the "(auto)" one: delete the set you don't use.

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
100% free) — see `docs/ACCOUNT_GATE.md`. **In v0.6.0, sign-in (and with it Enable delay and Trigi)
works only on Windows.** macOS and Linux don't have the plugin's HTTP client yet; it's planned for
v0.6.1.

**How does signing in work, and what does the plugin keep?**
Signing in always happens on trigglow.com, in your browser: the plugin never asks for your password.
Press **Sign in** in the dock; it shows a code ("The page must show the code KD7F-29QM."), and the
trigglow.com page shows the same code with the app, its version and when it was requested. Confirm
only if the codes match and you started it; **It wasn't me** cancels it. From then on the plugin
keeps its own sign-in, which only works for what the plugin does (checking your account, Trigi and
your own Dynamic Delay tickets), encrypted with Windows' data protection for your Windows user. Your
email isn't stored. **Sign out** in the dock also disconnects it on trigglow.com, and you can
disconnect it from the web too: Settings → Active sessions → Connected apps.

**I'm on v0.5.x or earlier. Do I have to update?**
Yes. The sign-in of v0.5.x and earlier is retired with v0.6.0: those versions can no longer sign in
to Trigglow, and the sign-ins they already have stop working after a transition period, so the
delay stops working there. Install v0.6.0 over your current version, with OBS closed. On the first
start your sign-in moves to the new format by itself; if the dock asks you to sign in, do it once.
Your scenes and settings are kept.

**The dock or "Report a problem" says to update the plugin. What do I do?**
"This plugin version can't sign in any more. Update the plugin." in the dock, "Update the plugin to
use Trigi" in "Report a problem" or "Update required" on the trigglow.com sign-in page all mean the
same: your version is too old for what you're trying to do. Download the latest version (**Get the
update** in the dialog, the update notice in the dock, or the
[latest release](https://github.com/VirosMs/obs-trigglow-dynamic-delay/releases/latest)) and install
it with OBS closed; your settings and your sign-in are kept. Meanwhile you can still send a report
from the form in the dialog or from [trigglow.com/support](https://www.trigglow.com/support).

**What is Trigi?**
Trigglow's AI support assistant, the same one as on trigglow.com, now in the plugin: open **Report a
problem** in the dock and ask about Dynamic Delay (setting it up, what the dock says, performance,
signing in). It answers right in the dialog. It's an AI and can make mistakes, so check what it
tells you before changing your setup. If it doesn't sort it out, **Create a ticket with this
conversation** sends it to the team with the context, and you can always skip Trigi and write to the
team directly. Trigi is available on Windows when you're signed in; otherwise the dialog opens on
the ticket form. The first time, a short notice ("Before you start with Trigi") explains all this,
and nothing is sent until you accept it and send your first question.

**What does Trigi see, and what doesn't it?**
It sees what you type and, only if you tick **Include OBS technical data**, a small set of numbers
and states sent once when the conversation starts: OBS and Windows versions, renderer, GPU vendor,
total RAM, encoder family, the delay's state, seconds and minimum quality, canvas and output size,
fps, OBS's lagged and skipped frames, the buffer's health and the plugin's recent problem codes.
**See what is sent** shows the exact data before you send it. The box is unticked until you tick it.
Trigi never gets your OBS log (not whole, not in parts, not summarized), and the technical data
never includes scene, source or filter names, paths, URLs, stream keys or stream details, your user
name or email, your GPU's name, hardware IDs or IPs. Your messages and the technical data are
processed by Trigglow and an AI provider listed in the
[Privacy Policy](https://www.trigglow.com/legal/privacy). Don't type passwords, stream keys, overlay
URLs or payment details. The OBS log only goes with a ticket if you tick its box, which you can do
once you've seen it in **See what is sent**. Paths with your Windows user name, URLs, keys, emails
and IPs are hidden in it; names of scenes, sources, audio devices and windows can still show up, so
check it there first.

**How long are my conversations with Trigi kept, and how do I delete them?**
Conversations with Trigi and the technical data you choose to send are kept for up to 12 months and
then deleted. The history you see in OBS is only in memory and disappears when you close the
dialog (**Clear conversation** only clears that view). To delete them earlier, on trigglow.com go to
Settings → Privacy & data → Conversations with Trigi → **Delete my Trigi conversations**, or ask
support. A conversation you turned into a ticket stays in that ticket's thread, like the rest of the
ticket.

**What network calls does the plugin make?**
On every OBS start, one check of your sign-in with api.trigglow.com (plus a renewal when it's due)
and one check of this repository's latest release on api.github.com. Everything else happens only
when you use it: signing in, signing out, asking Trigi and sending a report. All of it over HTTPS,
the sign-in only ever sent to api.trigglow.com, and the plugin only opens its own fixed pages on
www.trigglow.com and GitHub.

**Do I need Trigglow's Stream Deck plugin?**
No — the plugin uses native OBS hotkeys, which Stream Deck can trigger directly with its own
"System: Hotkey" action. See `docs/STREAM_DECK.md`.

**Is it a finished product?**
No — it's explicitly marked as **MVP / Early Access**. It's functional and meant for real live use,
but with a deliberately limited scope (see `docs/ROADMAP.md` for what comes next), and one risk
area — a GPU crash observed under an earlier design — that's believed reduced but not yet
specifically re-confirmed (`docs/SPEC.md` §7).
