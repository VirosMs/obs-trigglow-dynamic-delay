# Free account gate

Trigglow Dynamic Delay is, and stays, 100% free -- no subscription, no paid tier, no feature ever
locked behind payment. Starting with this change, using the delay (the dock's Enable button, its
hotkey, and Stream Deck) requires having signed in to a free trigglow.com account at least once.
The purpose is discovery, not monetization or lead generation: Trigglow ships several other free
(with-limits) products for streamers, and most people who install this plugin from the OBS
resources directory would otherwise never find out they exist.

## What this does NOT do

- No password, email, or any personal data is ever typed into or stored by the plugin itself --
  see "How login works" below. The plugin only ever holds an opaque session token, the same kind
  the web app itself stores in its own browser session.
- No plugin configuration (scenes, delay seconds, quality floor) is gated. Only Enable/Toggle are.
- No account data collected here is used for marketing or resold to third parties without separate,
  explicit consent -- that's a Trigglow product/privacy-policy commitment, not something enforced
  in this code, but it's the reason the gate is scoped this narrowly.

## How login works

1. `AuthManager::StartLogin()` (`src/auth-manager.hpp`/`.cpp`) calls `POST /auth/plugin/start` on
   trigglow.com, which returns a one-time device code and a `verificationUrl`. This call (and every
   other HTTPS call this feature makes) goes through `src/win-http.hpp` (WinHTTP, native Windows
   API) rather than Qt's `QNetworkAccessManager` -- see that header's comment for why: Qt's TLS
   backend plugin is tied to the exact Qt version already loaded in the host process (OBS's own
   bundled Qt, not the older one this plugin builds against), and shipping a mismatched one just
   gets silently rejected at runtime ("No functional TLS backend was found", hit live 2026-09-01).
2. The plugin opens `verificationUrl` in the user's system browser (`QDesktopServices::openUrl`) --
   never an embedded browser, never a form inside OBS.
3. The user logs in (or creates a free account) using trigglow.com's normal login page -- the exact
   same one the web app itself uses (email/password or Kick OAuth). The plugin is never involved in
   that exchange at all.
4. Once confirmed there, the plugin's poll loop (`GET /auth/plugin/poll`, every 3s, capped at 5
   minutes) picks up a session token, exactly once (the code is consumed server-side on first
   successful poll).
5. The token is stored locally in this plugin's own OBS module config directory
   (`trigglow-account.json`), in plaintext -- consistent with how `buffer-mode-settings.json`
   already stores this plugin's other local settings (see `plugin-main.cpp`). No new crypto/keychain
   machinery for an Early Access plugin whose installer isn't code-signed yet either.

On every OBS start, `AuthManager::Init()` does exactly one fire-and-forget `GET /auth/me` to confirm
the stored token still works (and to roll its rolling server-side TTL forward) -- not a loop, not a
background heartbeat. Outside of an active `StartLogin()` wait, the plugin makes zero network calls
related to this feature.

## Enforcement point

`BufferModeController::SetAuthorizationCheck()` (`src/buffer-mode-controller.hpp`) is the single
choke point: both the dock's Enable button (`settings-ui.cpp`) and the hotkey/Stream Deck path
(`hotkeys.cpp`) call straight into `BufferModeController::Enable()`/`Toggle()`, so gating there
covers both without duplicating the check.
