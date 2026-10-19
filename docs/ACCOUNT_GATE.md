# Free account gate

Trigglow Dynamic Delay is, and stays, 100% free -- no subscription, no paid tier, no feature ever
locked behind payment. Starting with this change, using the delay (the dock's Enable button, its
hotkey, and Stream Deck) requires having signed in to a free trigglow.com account at least once.
The purpose is discovery, not monetization or lead generation: Trigglow ships several other free
(with-limits) products for streamers, and most people who install this plugin from the OBS
resources directory would otherwise never find out they exist.

## What this does NOT do

- No password, email, or any personal data is ever typed into or stored by the plugin itself --
  see "How login works" below. The plugin only ever holds its own opaque token, which works for
  nothing but the few calls the plugin makes.
- No plugin configuration (scenes, delay seconds, quality floor) is gated. Only Enable/Toggle are.
- No account data collected here is used for marketing or resold to third parties without separate,
  explicit consent -- that's a Trigglow product/privacy-policy commitment, not something enforced
  in this code, but it's the reason the gate is scoped this narrowly.

## How login works

1. `AuthManager::StartLogin()` (`src/auth-manager.hpp`/`.cpp`, protocol in `src/auth-client.hpp`)
   calls `POST /auth/plugin/start` on trigglow.com, which returns a short code (like `KD7F-29QM`),
   the confirmation page to open, and a secret that never leaves the plugin. This call (and every
   other HTTPS call this feature makes) goes through `src/win-http.hpp` (WinHTTP, native Windows
   API) rather than Qt's `QNetworkAccessManager` -- see that header's comment for why: Qt's TLS
   backend plugin is tied to the exact Qt version already loaded in the host process (OBS's own
   bundled Qt, not the older one this plugin builds against), and shipping a mismatched one just
   gets silently rejected at runtime ("No functional TLS backend was found", hit live 2026-09-01).
   Redirects are never followed, so the plugin's token can't be bounced to another host.
2. The plugin opens the page in the user's system browser (`QDesktopServices::openUrl`) -- never an
   embedded browser, never a form inside OBS -- and shows the code in the dock. The page shows the
   same code, the app's name as trigglow.com knows it (not text taken from the link), its version
   and system, how long ago the request was made and, when the request seems to come from a
   different network than the browser's, a warning: confirm only if the codes match. "It wasn't
   me" on the page cancels the request. "Copy link" in the dock covers a browser that didn't open.
3. The user logs in (or creates a free account) using trigglow.com's normal login page -- the exact
   same one the web app itself uses (email/password or Kick OAuth). The plugin is never involved in
   that exchange at all.
4. While the dock waits, the plugin asks `POST /auth/plugin/token` at the interval the server gives,
   until the request expires. The secret travels in the request body, never in a URL, and neither
   the page, the code nor the secret is ever written to OBS's log.
5. Once confirmed, the plugin receives its own token. It only works for the plugin's own calls
   (account check, renewal, sign-out, Trigi and the user's own support tickets), it is renewed
   automatically when the server says so, and "Sign out" revokes it on the server as well as
   deleting it locally (the local copy goes even without a connection). The user can also revoke it
   from trigglow.com: Settings -> Active sessions -> Connected apps.
6. The token is stored encrypted with the Windows data protection API, for the current Windows
   user only, apart from the plugin's settings. The email is not stored. If it can't be decrypted
   (another Windows user, a restored profile), it is deleted and the dock goes back to "Sign in".
   It is never read, attached or mentioned by "Report a problem".

Sign-ins made with version 0.5.x or earlier are exchanged once, silently, for a plugin token on the
first start of 0.6.0, and the old plain-text sign-in file is overwritten and deleted. Until that
exchange succeeds (for example while offline) the old sign-in keeps Enable working.

The sign-in of 0.5.x and earlier is switched off on trigglow.com when 0.6.0 is published: from
then on those versions can no longer sign in, and the sign-ins they already have stop working after
a transition period, during which 0.6.0 can still exchange them. Updating is therefore required to
keep using the delay; on 0.6.0 the dock says "This plugin version can't sign in any
more. Update the plugin." if trigglow.com ever reports the same for it.

On every OBS start, `AuthManager::Init()` makes one `GET /auth/plugin/me` call to confirm the stored
token still works (plus a renewal when it is due) -- not a loop, not a background heartbeat. If
trigglow.com can't be reached, the plugin stays signed in and Enable keeps working; if the server
says the account is suspended or not active, Enable is blocked and the dock says why. Outside of an
active `StartLogin()` wait, the plugin makes no other network calls related to this feature.

Sign-in (and with it Enable and Trigi) works only on Windows in 0.6.0: macOS and Linux get an HTTP
client, with the sign-in kept in the system's keychain or secret store, in 0.6.1 (planned).

## Enforcement point

`BufferModeController::SetAuthorizationCheck()` (`src/buffer-mode-controller.hpp`) is the single
choke point: both the dock's Enable button (`settings-ui.cpp`) and the hotkey/Stream Deck path
(`hotkeys.cpp`) call straight into `BufferModeController::Enable()`/`Toggle()`, so gating there
covers both without duplicating the check.
