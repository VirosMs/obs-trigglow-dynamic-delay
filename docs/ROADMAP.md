# Roadmap — Trigglow Dynamic Delay

## v0.1.0 (MVP — este entregable)
- Enable / Disable / Toggle Delay con reconexión controlada cuando ya estás en directo.
- Estado Inactive / Active / Error visible en un dock nativo de OBS.
- 3 hotkeys nativas de OBS (Toggle obligatoria, Enable/Disable opcionales).
- Persistencia de settings por perfil de OBS.
- Uso con Stream Deck vía la acción nativa "Hotkey" de Stream Deck sobre la hotkey de OBS.
- Windows como plataforma prioritaria; proyecto preparado para macOS/Linux.

## v0.2.0 (propuesto)
- Presets guardados (p. ej. "Delay corto 5s", "Delay largo 30s") seleccionables desde el dock y desde hotkeys adicionales.
- Indicador de cuenta atrás/objetivo mientras el output se reconecta, en vez de solo "Applying...".
- Confirmación configurable: opción de saltarse el diálogo de aviso de reconexión para usuarios avanzados.
- Logs exportables a fichero desde el propio dock (para depurar en directo sin ir a buscar el log de OBS).

## v0.3.0 (propuesto)
- Exploración de un modo "sin reconexión" mediante un filtro de vídeo/audio propio con buffer acotado
  en memoria (ver `docs/SPEC.md` §2.3) — detrás de un flag experimental, no por defecto, con límites
  claros de segundos máximos según RAM disponible.
- Integración opcional con un plugin de Stream Deck propio de Trigglow (además del flujo de hotkey
  nativa, que se mantiene siempre como opción sin dependencias) para mostrar el estado ON/OFF con
  color en el propio botón del Stream Deck.
- Builds firmados e instaladores para Windows/macOS.
- Soporte Linux verificado en CI.

## Fuera de alcance por ahora
- Delay distinto por escena.
- Delay del Replay Buffer o de la grabación local (solo streaming en v0.1–v0.3).
- Panel web externo como forma principal de control (decisión de producto: todo vive nativo en OBS).
