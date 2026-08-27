*[English version](ROADMAP.md)*

# Roadmap — Trigglow Dynamic Delay

## v0.2.0 (MVP — este entregable)
- **Modo buffer** sin reconexión: el output de streaming no se toca nunca, en ningún momento, por
  ningún motivo — el plugin retrasa vídeo y audio acumulándolos en RAM del sistema y cambiando el
  Programa de OBS internamente entre la escena en directo, una escena de carga opcional y una
  escena wrapper delayed.
- Enable / Disable / Toggle Delay, con la escena en directo obligatoria y la escena de carga
  opcional.
- Calidad mínima seleccionable (480p/720p/1080p); el tiempo pedido se acorta en vez de bajar nunca
  de esa calidad, si el presupuesto de RAM no da para ambos.
- Estimación de ajuste de buffer en vivo y presupuesto de RAM detectado, mostrados en el dock y
  actualizados según el usuario ajusta segundos o calidad, con un aviso no bloqueante antes de
  Enable si la petición completa no cabe.
- Vídeo y audio delayed juntos, en sincronía.
- Buffer de RAM liberado automáticamente al pulsar Disable.
- Estado `Inactive` / `Filling` (con cuenta atrás en vivo) / `Active` / `Error` visible en un dock
  nativo de OBS.
- 3 hotkeys nativas de OBS (Toggle obligatoria, Enable/Disable opcionales), utilizables
  directamente desde Stream Deck vía su propia acción "System: Hotkey".
- Persistencia de settings por perfil de OBS.
- Manejo de errores sin crashear (sin escena en directo elegida, escena en directo que no resuelve
  a nada, etc.).
- Windows como plataforma prioritaria; macOS/Linux compilan en verde en CI pero todavía no se han
  probado en directo.

## v0.3.0 (entregado — 2026-08-27)
- **Compresión** real del buffer: cada slot del ring ahora se codifica en MJPEG (todo-intra) al
  capturarlo y se decodifica al reproducirlo, integrando FFmpeg para ambas direcciones
  (`obs_video_encoder_create` solo alimenta el propio pipeline de output de OBS, y libobs no
  expone ninguna API pública de decodificación — también se investigó y descartó una vía de
  compute shader de GPU para v0.2.0, ya que libobs no tiene API de compute/dispatch en su interfaz
  gráfica pública). Fallback automático a NV12 sin comprimir siempre que el codec no esté
  disponible o un frame falle.
- Los slots del ring reservan RAM según un tamaño presupuestado (comprimido) en vez del techo
  crudo completo, creciendo solo según lo que cada frame necesita. Medido en vivo a 30s@1080p:
  ~2,9GB con el buffer activo, frente a los ~6,3GB de antes de comprimir.
- Solo Windows + Linux por ahora — macOS no tiene ningún build estático de FFmpeg de confianza
  equivalente disponible y sigue funcionando exactamente igual que en v0.2.0 (sin comprimir).
- Sigue abierto: medir el ratio de compresión real contra gameplay real prolongado (el único dato
  de hoy vino de una escena de carga estática, no representativo); el crecimiento de RAM de un
  slot es permanente durante la sesión una vez ocurre (no baja hasta Disable()).
- `main` ahora es una rama protegida (PR obligatorio, incluso para el mantenedor); `develop` es
  donde ocurre el trabajo en curso.

## v0.4.0 (propuesto)
- Presets de delay guardados (p. ej. "Delay corto 5s", "Delay largo 30s"), seleccionables desde el
  dock y desde hotkeys adicionales.
- **Verificación en directo** de macOS/Linux — ambos ya compilan en verde en CI; esto trata de
  ejecutar y confirmar el plugin de verdad en esas plataformas, no de trabajo nuevo de build.

## Más adelante
- Instaladores firmados para Windows/macOS (el instalador de Windows actual no está firmado —
  Early Access).
- Un plugin de Stream Deck propio de Trigglow, además del flujo de hotkey nativa (que se mantiene
  siempre disponible como opción sin dependencias) — sobre todo para mostrar el estado
  ON/OFF/Filling con color directamente en el propio botón del Stream Deck.

## Fuera de alcance por ahora
- Delay distinto por escena.
- Delay del Replay Buffer o de la grabación local como control separado del streaming (hoy ambos
  siguen el mismo output de Programa que el stream — ver `docs/FAQ.md`).
- Panel web externo como forma principal de control (decisión de producto: todo vive nativo en
  OBS).
