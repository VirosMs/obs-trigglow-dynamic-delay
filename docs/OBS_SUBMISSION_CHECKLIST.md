# OBS Plugins Catalog Submission Checklist

Practical checklist for actually submitting Trigglow Dynamic Delay to the OBS Studio plugins
store/catalog.

## Done

- Documentation is now bilingual: README, CHANGELOG, FAQ, Troubleshooting, ROADMAP, and SPEC each
  have an English version (default, at the plain filename) and a Spanish version (`*-es.md`
  sidecar), cross-linked in both directions.
- On 2026-08-26, the docs were substantially corrected for technical accuracy, not just translated:
  they had previously described an abandoned reconnect-based delay mechanism (change the delay
  value, OBS briefly reconnects the stream to apply it) instead of the no-reconnect buffer mode
  that v0.2.0 actually ships (see `docs/SPEC.md` §0 and §2 for the full history of that change).
- `LICENSE` (GPL-2.0-or-later) is present. This is not actually a licensing inconsistency: the
  `LICENSE` file itself contains the standard GPLv2 text (expected — that's normal practice), and
  the "-or-later" grant comes from the SPDX header in each source file (e.g.
  `src/plugin-main.cpp`), not from different text in the LICENSE file. Nothing to fix here.
- `buildspec.json` has `author`, `website`, and `email` filled in.
- `obs_module_description()` and `OBS_MODULE_AUTHOR()` are implemented in `src/plugin-main.cpp`.
- CI (`.github/workflows/push.yaml`) builds installers for Windows, macOS, and Linux and attaches
  them to a GitHub Release.

## Still needed before submitting

- **A real screenshot of the plugin's dock UI running inside OBS Studio.** This requires someone
  to actually launch OBS with the plugin installed and take a screenshot — it cannot be faked or
  generated. Put it in `docs/images/` (e.g. `docs/images/dock-screenshot.png`) and reference it
  from the top of `README.md`; the OBS catalog listing will very likely also want one directly.
  **Who:** a human with a working OBS + plugin install.

- **A dedicated icon/logo image for the store listing**, if the submission form asks for one
  (many catalog/marketplace listings do). Needs actual design work, not just reusing the OBS
  default plugin icon. **Who:** a human (or a designer) — this isn't something to auto-generate
  for a real product listing.

- **Publish the draft GitHub Release.** CI currently creates releases with `draft: true` (see
  `.github/workflows/push.yaml`), so nothing is public until someone reviews the generated
  installers and clicks "Publish release" on GitHub. **Who:** a human with repo write access.

- **Verify the current OBS submission process directly on obsproject.com/forum.** Submission
  portals and requirements change over time; treat this checklist as a starting point, not the
  final word. Before submitting, check the live forum thread/guidelines for the plugin
  submission process (required fields, review turnaround, forum account requirements, etc.).
  **Who:** whoever is doing the submission, right before submitting.

- **Signed installers are explicitly deferred to v0.3** (see `docs/ROADMAP-es.md`). Until then,
  the unsigned Windows installer may trigger a SmartScreen "Unknown publisher" warning for anyone
  who downloads it from the forum — the same tradeoff already documented on the streampulse
  website and in `installers/windows/trigglow-dynamic-delay-setup.iss`. Consider flagging this
  clearly in the forum submission text so reviewers and users aren't surprised by it.
