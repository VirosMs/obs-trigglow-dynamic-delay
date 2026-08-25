# Changelog — Trigglow Dynamic Delay for OBS

## v0.1.0 — 2026-08-23 (MVP / Early Access)

Primera versión pública. Plugin nativo de OBS Studio (C++, plantilla oficial
`obsproject/obs-plugintemplate`) para activar/desactivar un delay configurable del stream sin salir
de OBS ni depender de una app externa.

**Incluido:**
- Enable / Disable / Toggle Delay desde un dock nativo de OBS, con estado visible
  (`Inactive` / `Applying` / `Active` / `Error`).
- 3 hotkeys nativas de OBS (Toggle, Enable, Disable) — compatibles de forma directa con Stream Deck
  vía su acción "Hotkey" estándar, sin plugin de Stream Deck propio.
- Campo de segundos de delay configurable y "modo seguro" (evita reintentos en bucle si la
  reconexión no se confirma a tiempo).
- Persistencia de settings por perfil de OBS.
- Manejo de errores sin crashear (sin output de streaming válido, timeout de reconexión, etc.).

**Limitación conocida y documentada (no es un bug):** cambiar el delay mientras ya estás en directo
provoca una reconexión breve del stream — es como funciona OBS a día de hoy, no una limitación de
este plugin. Detalle técnico completo en `docs/SPEC.md`.

**Fuera de alcance en v0.1.0:** perfiles/presets de delay, instaladores firmados, plugin de Stream
Deck propio, modo "sin reconexión". Ver `docs/ROADMAP.md`.
