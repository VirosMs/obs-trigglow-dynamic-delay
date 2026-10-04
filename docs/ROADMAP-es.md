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

## v0.4.0 (entregado — 2026-09-07)
- **Reportar un problema**: un diálogo nativo en el dock que crea un ticket de soporte real en
  trigglow.com (categoría `dynamic_delay`) y adjunta automáticamente el log actual de OBS — sin
  que el usuario tenga que buscarlo a mano. Nuevo `HttpsPostMultipartFile()` sobre el cliente
  WinHTTP ya existente (`src/win-http.cpp`) para la subida del adjunto.
- **Chequeo de actualizaciones dentro del plugin**: al cargar, compara la última release de
  GitHub de este repositorio contra la versión en ejecución (público, sin autenticación) y
  muestra un aviso en el dock si hay una más nueva — silencioso ante cualquier fallo o si ya está
  actualizado, nunca como un error.
- **Localización real**: todo el texto visible (dock, diálogo de reportar un problema, nombres de
  filtro, descripciones de hotkeys, mensajes de estado del buffer) pasa ahora por el sistema de
  idiomas propio de OBS (`obs_module_text()`) en vez de literales en español fijos en el código —
  `data/locale/en-US.ini`/`es-ES.ini` ahora están totalmente traducidos en vez de tener una sola
  clave `PluginName` cada uno. Antes, un usuario con OBS en cualquier idioma que no fuera español
  veía igualmente el plugin 100% en español.
- **Rediseño del dock**: secciones agrupadas en tarjetas, paleta de colores de estado semántica
  consistente, y controles de delay/calidad uno junto al otro.
- **RAM medida por primera vez, no asumida**: una línea de log `compression check`
  (`src/video-delay-filter.cpp`) reveló un ratio de compresión MJPEG real de ~11-15x sobre
  gameplay en vivo, muy por encima del 3x asumido de forma conservadora desde v0.3.0 —
  `kAssumedCompressionRatio` reajustado a 5.0 con esos datos, más la eliminación de una
  asignación de memoria por cada frame en la codificación. Medido en vivo a 30s/1080p60: la
  memoria total del proceso de OBS bajó de ~2.8GB a ~2.1GB.

## v0.5.0 (entregado — 2026-10-04)
- **Overlay "Delay Ns"** (insignia con logo + texto, al 40% de tamaño) sobre la salida retrasada
  mientras está Activo, con checkbox y selector de cuatro esquinas en el dock.
- **Un solo botón Activar/Desactivar** y un **dock responsivo** (área con scroll + filas que se
  apilan cuando es estrecho).
- **Delay de vídeo por reloj real.** Un log de producción mostró el ring por número de frames a
  48-90 slots/s con 60 fps (delay real ~20-38s frente a 30s exactos del audio). Ahora se captura
  un slot por celda de 1/fps y la reproducción elige por hora de captura; el audio también sigue
  timestamps.
- **La escena envoltorio contiene siempre la escena en directo elegida**, y los filtros que
  quedaron encendidos al cerrar OBS estando Activo se apagan al cargar.
- **Fix de empaquetado del instalador de Windows:**
- **El instalador de Windows nunca incluía `es-ES.ini`.** Encontrado probando en directo en una
  instalación de OBS configurada en español: el plugin parecía atascado en inglés sin importar el
  idioma de OBS. La sección `[Files]` de `installers/windows/trigglow-dynamic-delay-setup.iss` solo
  listaba `en-US.ini` — un descuido de empaquetado anterior a esta sesión, invisible desde que
  v0.4.0 convirtió `es-ES.ini` en una traducción real por primera vez (antes tenía una sola clave
  placeholder, así que un archivo faltante no cambiaba nada visible para el usuario).
  `obs_module_text()` cae de vuelta al idioma por defecto siempre que el `.ini` del idioma pedido no
  esté en disco, lo cual se ve idéntico a "este plugin no soporta español" desde el lado del
  usuario. Ahora incluye ambos. Es el siguiente release después de v0.4.0 (actualmente en
  producción) — lo bastante pequeño como para no esperar a las features no relacionadas de abajo.

## v0.6.0 (propuesto)
- Presets de delay guardados (p. ej. "Delay corto 5s", "Delay largo 30s"), seleccionables desde el
  dock y desde hotkeys adicionales.
- **Verificación en directo** de macOS/Linux — ambos ya compilan en verde en CI; esto trata de
  ejecutar y confirmar el plugin de verdad en esas plataformas, no de trabajo nuevo de build.
- Medir el reajuste de RAM de v0.4.0 (`kAssumedCompressionRatio = 5.0`) contra más
  sesiones/juegos usando el nuevo contador de crecimiento por slot (`encodeGrowthCount_`) antes de
  plantearse subirlo más — ver el comentario de esa constante en `src/video-delay-filter.cpp`.
  usuario. Ahora incluye ambos.

## v0.7.0 (pausado — acotado, sin empezar, 2026-09-08)
Objetivo: un Replay Buffer "en directo real", propio de Trigglow, guardado con su propio hotkey (el
hotkey nativo "Save Replay" de OBS no se puede redirigir a un output propio, así que esto nunca iba a
reutilizarlo) que siga funcionando mientras el modo buffer está Active, en vez de capturar la escena
wrapper delayed como hace el output de streaming. Investigado el 2026-09-08, pausado deliberadamente
antes de escribir código — ver abajo.

- **Idea de primera pasada, y por qué está mal.** Esta entrada originalmente proponía el patrón
  "Branch Output" de `exeldro/obs-source-record` tal cual: un `obs_view` + encoder +
  output `"replay_buffer"` dedicados (todo API pública de OBS — `obs_view_create`/`obs_view_add2`,
  `obs_video_encoder_create`, `obs_output_create("replay_buffer", ...)`, leído de la implementación
  real de Source Record) apuntando a la escena en directo real, corriendo en paralelo al output de
  streaming que sigue mostrando la escena wrapper delayed. Eso está mal específicamente para este
  plugin: los filtros son una propiedad del propio objeto fuente, no de quien lo renderiza/lee —
  verificado directamente contra `libobs/obs-source.c` (`obs_source_output_audio()` ejecuta toda la
  cadena de filtros vía `filter_async_audio()` *antes* de que `source_signal_audio_data()` llegue a
  CUALQUIER callback de captura o consumidor; `obs_source_default_render()` funciona igual para
  vídeo). Como `VideoDelayFilter`/`AudioDelayFilter` están adjuntos directamente a los objetos de la
  escena en directo/fuentes hoja (anidar nunca los duplica — ver el comentario de cabecera de
  `EnsureBufferWrapperScene`), un `obs_view` o encoder nuevo apuntando a "la escena en directo real"
  vería la MISMA salida ya delayed que ve el lado de streaming, justo en el momento en que el modo
  buffer pasa a Active — exactamente cuando un replay en directo real más importaría. Source Record
  en sí nunca se topa con esto, porque su objetivo es capturar una fuente *con* los filtros que
  tenga.
- **Lo que arreglarlo de verdad requeriría.** `VideoDelayFilter`/`AudioDelayFilter` ya capturan una
  copia sin retrasar de cada frame/buffer de audio en cada tick, antes de escribirla en su ring de
  delay — el fix correcto es que TAMBIÉN empujen esa misma copia sin retrasar a un `video_output_t`/
  `audio_output_t` dedicado (`video_output_open()`/`video_output_lock_frame()`, igual para audio) que
  un encoder+output de replay buffer nuevo lea, en vez de construir algo aislado al lado. Eso implica
  cambiar los internos de los dos archivos de los que depende la garantía central de delay de este
  plugin — no una feature nueva autocontenida junto a ellos.
- **Por qué pausado en vez de intentado ahora.** La propia historia de `docs/SPEC.md` (las entradas
  v0.3.1/v0.3.2 de §3.3, el crash de GPU sin resolver de §7) muestra que este mismo par de archivos
  ya ha sacado a la luz repetidamente bugs sutiles, solo encontrados en directo, incluso con cambios
  acotados y específicos del propio delay. Tocar sus internos para un segundo camino de captura no
  relacionado arriesga la garantía central de "nunca corta, jamás" por una feature que es aditiva, no
  esencial. Retomar esto debería empezar por "cómo validamos que el camino de delay existente no se
  vea afectado", no directamente por "añadir el segundo tap".

## Más adelante
- **Rediseño del delay a nivel de output** (estudiado el 2026-09-08, no iniciado). `obsdelay.com`
  bufferiza el bitstream ya codificado y lo reenvía mediante su propio sender RTMP
  multi-destino, de modo que Programa, el cambio de escena, el Replay Buffer y la grabación nunca
  se tocan — con una huella de memoria mucho menor que nuestro ring de frames NV12/MJPEG, ya que un
  bitstream comprimido es mucho más pequeño que frames decodificados. Es arquitectónicamente la
  solución más limpia al objetivo de v0.7.0 Y al cambio de escena sin recarga (ver "Fuera de
  alcance por ahora" abajo) a la vez, pero implica sustituir el output RTMP nativo de OBS por uno
  propio (reconexión, manejo de keyframes, envío multi-destino) — varias sesiones de trabajo y un
  riesgo notablemente mayor. Solo merece priorizarse si el fix más acotado de v0.7.0 no llega a
  cubrir el uso real una vez entregado.
- Instaladores firmados para Windows/macOS (el instalador de Windows actual no está firmado —
  Early Access).
- Un plugin de Stream Deck propio de Trigglow, además del flujo de hotkey nativa (que se mantiene
  siempre disponible como opción sin dependencias) — sobre todo para mostrar el estado
  ON/OFF/Filling con color directamente en el propio botón del Stream Deck.

## Fuera de alcance por ahora
- Delay *distinto* por escena — una duración de delay diferente configurada por cada escena.
- **Seguir los cambios de escena del streamer mientras está delayed, sin un hueco de recarga**
  (investigado y descartado, 2026-09-08). Un primer intento hacía que la escena wrapper se
  retargeteara a la escena a la que Programa cambiara (primero desde el dock, luego siguiendo
  automáticamente `OBS_FRONTEND_EVENT_SCENE_CHANGED`) — técnicamente correcto, pero cada cambio
  necesariamente volvía a entrar en `Filling` primero, ya que el ring de la escena recién elegida
  empieza vacío: nuestros filtros de vídeo/audio solo capturan los frames de UNA fuente a la vez,
  así que "el buffer ya tiene historial de la escena a la que acabas de cambiar" no es posible sin
  bufferizar continuamente todas las escenas a las que el usuario podría cambiar (multiplicando el
  coste de RAM/GPU por el número de escenas sin ningún beneficio el resto del tiempo). Esa recarga
  choca directamente con la promesa central de este plugin — "nunca corta, jamás" — así que se
  revirtió en vez de entregarse como compromiso. Se revisó contra los plugins de referencia por si
  había alguna forma de evitarlo: el filtro por-fuente de `exeldro/obs-dynamic-delay` tiene la
  misma limitación exacta (su propio tracker de issues tiene una petición de feature pendiente para
  bufferizar el padre de una fuente invisible, es decir, exactamente este hueco) y
  `ne0lines/comp-delay-for-obs` usa la misma arquitectura de 3 escenas fuente/transición/delay que
  nosotros, sin indicios de que resuelva esto tampoco. Solo `obsdelay.com` lo evita, y únicamente
  porque nunca retrasa los frames de una escena específica — bufferiza el propio bitstream del
  stream, ya compuesto y ya codificado, corriente abajo de Programa, así que un cambio de escena es
  solo parte de esa única señal continua, nunca "un buffer distinto sin historial". Eso es el
  "Rediseño del delay a nivel de output" ya listado en Más adelante — vale la pena revisitarlo solo
  como parte de ese rediseño mayor, no como una adición aislada a la arquitectura actual de filtro
  por-escena.
- Panel web externo como forma principal de control (decisión de producto: todo vive nativo en
  OBS).
