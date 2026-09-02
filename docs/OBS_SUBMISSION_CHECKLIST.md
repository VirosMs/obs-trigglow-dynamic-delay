# OBS Plugins Catalog Submission Checklist

Practical checklist for actually submitting Trigglow Dynamic Delay to the OBS Studio plugins
store/catalog.

**v0.3.x note:** v0.3.0 (real MJPEG compression of the RAM ring buffer, see `docs/SPEC.md` §4),
v0.3.1 (a scene-selector persistence fix + real installer branding — replacing Inno Setup's generic
icon/wizard images, see `installers/windows/branding/`), and v0.3.2 (fixed audio drifting behind a
RAM-shortened video delay, see `docs/SPEC.md` §3.3) are all feature/fix updates on top of the same
0.2.0 submission this checklist was written for — they follow the same process and don't change
anything below; the items already marked "Done" for 0.2.0 still hold.

## Done

- Documentation is now bilingual: README, CHANGELOG, FAQ, Troubleshooting, ROADMAP, SPEC, and the
  new INSTALL_GUIDE each have an English version (default, at the plain filename) and a Spanish
  version (`*-es.md` sidecar), cross-linked in both directions.
- On 2026-08-26, the docs were substantially corrected for technical accuracy, not just translated:
  they had previously described an abandoned reconnect-based delay mechanism (change the delay
  value, OBS briefly reconnects the stream to apply it) instead of the no-reconnect buffer mode
  that v0.2.0 actually ships (see `docs/SPEC.md` §0 and §2 for the full history of that change).
- **Real screenshots exist and are in use**, taken from an actual 0.2.0 install
  (`docs/images/`): the Windows SmartScreen warning and how to get past it, every step of the
  installer wizard, and enabling/showing the plugin's dock inside a running OBS Studio. All
  embedded in `docs/INSTALL_GUIDE.md` / `docs/INSTALL_GUIDE-es.md`, linked from `README.md`.
- `LICENSE` (GPL-2.0-or-later) is present. This is not actually a licensing inconsistency: the
  `LICENSE` file itself contains the standard GPLv2 text (expected — that's normal practice), and
  the "-or-later" grant comes from the SPDX header in each source file (e.g.
  `src/plugin-main.cpp`), not from different text in the LICENSE file. Nothing to fix here.
- `buildspec.json` has `author`, `website`, and `email` filled in.
- `obs_module_description()` and `OBS_MODULE_AUTHOR()` are implemented in `src/plugin-main.cpp`.
- CI (`.github/workflows/push.yaml`) builds installers for Windows, macOS, and Linux and attaches
  them to a GitHub Release.
- **The `0.2.0` GitHub Release is published** (no longer a draft) — the Windows installer's
  filename version used to be hardcoded and drifted out of sync with `buildspec.json`, which
  silently dropped the `.exe` out of the release entirely; fixed in `installers/windows/
  trigglow-dynamic-delay-setup.iss` (now reads the version from CI instead of a hardcoded literal).
  Verified the public download URL resolves (200, not 404) before publishing.

## Still needed before submitting

- **A dedicated icon/logo image for the store listing**, if the submission form asks for one
  (many catalog/marketplace listings do). Needs actual design work, not just reusing the OBS
  default plugin icon, and not the same thing as the dock/installer screenshots above. **Who:** a
  human (or a designer) — this isn't something to auto-generate for a real product listing.

- **Verify the current OBS submission process directly on obsproject.com/forum.** Submission
  portals and requirements change over time; treat this checklist as a starting point, not the
  final word. Before submitting, check the live forum thread/guidelines for the plugin
  submission process (required fields, review turnaround, forum account requirements, etc.).
  **Who:** whoever is doing the submission, right before submitting.

- **CA-issued signing is still deferred** (see `docs/ROADMAP.md` and
  [#10](https://github.com/VirosMs/obs-trigglow-dynamic-delay/issues/10)) — a SignPath Foundation
  application is in progress. As of v0.3.3 the installer is signed with a *self-signed* certificate
  as a stopgap (proves the file wasn't tampered with post-build), but this does **not** remove the
  SmartScreen "Unknown publisher" warning documented in `docs/INSTALL_GUIDE.md`, and does **not**
  stop Microsoft Defender's `Wacatac.B!ml` cloud-heuristic false positive (confirmed live,
  2026-09-02, on the official v0.3.2 release asset — not specific to unofficial builds). Flag both
  clearly in the forum submission text so reviewers and users aren't surprised.

- **Disclose the free-account requirement and network use in the forum listing text.** As of this
  change, Enable (dock button, hotkey, and Stream Deck) requires having signed in to a free
  trigglow.com account once — see `docs/ACCOUNT_GATE.md` for the full design and why it exists.
  The plugin now makes outbound HTTPS calls to trigglow.com (`src/auth-manager.hpp`), which it
  didn't before v0.4.0 — say so plainly in the resource description alongside the "100% free" claim,
  since some reviewers/users specifically look for undisclosed network activity in OBS plugins.
  **Who:** whoever writes the forum submission text.
