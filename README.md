# Trigglow Dynamic Delay

Native OBS plugin to enable/disable stream delay on the fly — from a dock button, a native OBS hotkey, or your Stream Deck (via that same hotkey).

**Status:** Early Access — this repository is the dedicated, source-available home for the plugin (see [License](#license)). Product page, install guide, and FAQ: **https://trigglow.com/dynamic-delay**

## Why this exists

Streamers often need to raise or drop stream delay mid-broadcast — for moderation, to blunt stream-sniping in competitive games, or just to buy reaction time. Doing that today usually means digging through OBS Settings, or running a separate app alongside OBS. Trigglow Dynamic Delay turns it into a dock button, a hotkey, or a Stream Deck press.

## How it works

- **Before you go live:** change the delay freely — it's applied cleanly the next time the stream output starts. No cuts.
- **While already live:** OBS locks a stream output's delay value the moment it starts, and there is no public (or internal) API to recompute the buffer of an already-running output. Applying a new value live currently requires reconnecting the output — a real OBS limitation, not a shortcut this plugin takes. The plugin warns before doing this.

## Controls

- Dock button (Enable / Disable / Toggle), with visible state: Inactive / Applying / Active / Error
- 3 native OBS hotkeys (Toggle, Enable, Disable) — map these directly in a Stream Deck "Hotkey" action, no separate Stream Deck plugin required
- Per-OBS-profile persistent settings

## Scope (current release)

**In:** Enable/Disable/Toggle delay, dock status, native hotkeys, Stream Deck via hotkey, safe mode (no retry loops), per-profile settings.

**Not yet:** saved delay presets, a no-reconnect live-apply mode, a dedicated Trigglow Stream Deck plugin, signed installers, verified macOS/Linux builds (build tooling is in place; binaries aren't yet verified).

## License

GPL-2.0-or-later — required because the plugin links against `libobs`. See [LICENSE](LICENSE).

## Links

- Product page & downloads: https://trigglow.com/dynamic-delay
- Trigglow: https://trigglow.com
