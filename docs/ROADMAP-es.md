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

## v0.6.0 (entregado — 2026-10-19)
Rendimiento, adaptación y el aspecto de Trigglow, además de Trigi, un inicio de sesión más seguro
y una revisión de seguridad. Se preparó un hotfix de rendimiento v0.5.1 que nunca se publicó; todo
lo que llevaba sale aquí. Lo primero que se reportó en directo: sobrecarga al codificar ("Encoding
overloaded") en cuanto se activaba el delay con un lienzo 2560x1440 y una salida 1920x1080 a 60 fps.
- **Buffer limitado a la resolución de salida de OBS.** El ring se dimensionaba a partir del lienzo,
  así que un lienzo 1440p emitido a 1080p se codificaba/decodificaba en MJPEG a 1440p en el hilo
  gráfico de OBS (14,3/17,4 ms p50/p95 por frame). La calidad mínima pasa a ser un suelo de RAM que
  nunca supera la salida. La aritmética de dimensionado está en `src/buffer-fit.*`, con tests; anchos
  múltiplos de 8 para la lectura en OpenGL. Ajuste oculto de soporte `buffer_output_cap`, y un aviso
  en el log para encoders por encima del buffer.
- **Compresión/descompresión en hilos propios, fuera del hilo gráfico de OBS — salida 1440p60
  soportada.** Uno o dos hilos de compresión, un hilo de descompresión que trabaja por adelantado, un
  hilo liberador de memoria, un ring de cabeceras de slot con máquina de estados y una arena por
  bloques. A 1440p60 el trabajo del plugin en el hilo gráfico pasó de ~14/17 ms a ~1,2–1,7/1,6–2,1 ms
  por frame (p50/p95, microbenchmark fuera de OBS). Interruptor oculto `codec_workers` para volver al
  códec en el hilo gráfico.
- **Degradación automática que nunca corta el delay**: más hilos de compresión, después peldaños de
  menos resolución hasta la calidad mínima, después la mitad de fps de captura; vuelve a subir con
  30 s de histéresis; aviso ámbar en el dock con qué se redujo y por qué. La calidad mínima llega
  hasta 1440p (2K) y 2160p (4K).
- **Menos RAM, nada pesado en el hilo gráfico**: sin relleno de ceros, arena por bloques (~1,9 GB →
  ~0,6–0,8 GB para 30 s a 1440p60 en simulación), memoria liberada en el hilo liberador, códecs y
  objetos de GPU liberados al desactivar, conversión I420 en la GPU, tablas de Huffman estándar, cada
  frame decodificado y subido una sola vez.
- **Un único delay en segundos enteros para vídeo y audio**, también cuando la RAM recorta el ring
  (antes hasta ~1 s de diferencia); intervalo de frame exacto a 59,94 fps; un reinicio del buffer se
  rellena detrás de la escena de carga en vez de mostrar negro; 0 s deja pasar el vídeo.
- **Los filtros de delay que quedaban encendidos al cerrar OBS estando Activo se apagan de verdad**,
  al arrancar y al cambiar de colección de escenas; corregidas la fuga de referencias del filtro y el
  destructor sin contexto gráfico; un archivo de ajustes ilegible se aparta como `.invalid`.
- **Telemetría `perf check` y `pipeline check`** cada 10 s en el log de OBS (sustituye a
  `compression check`).
- **Rediseño de la interfaz alineado con trigglow.com**: dock, diálogo de reporte y overlay del
  directo sobre el tema de OBS, Archivo y Geist Mono empaquetadas (OFL), iconos SVG, contraste WCAG
  AA y foco de teclado, errores de inicio de sesión visibles, los segundos de delay bloqueados
  mientras el delay está en marcha. El overlay es una sola imagen generada, igual en todos los
  sistemas, firmada "By Trigglow", que conserva la posición y el tamaño que le des.
- **Nombres "(auto)"** para la escena, los filtros y el overlay que gestiona el plugin, migrados
  automáticamente.
- **Trigi, el asistente de soporte con IA de Trigglow, en "Reportar un problema"** (Windows, con
  sesión iniciada): aviso de IA y privacidad en el primer uso, datos técnicos de OBS opcionales
  (solo cifras y estados, con vista previa), el log de OBS nunca va a la IA, cada estado de error con
  su siguiente paso, render seguro de las respuestas y la conversación convertida en ticket con un
  resumen.
- **Inicio de sesión v2**: un código que comparar en la página de trigglow.com con "No he sido yo",
  un inicio de sesión propio del plugin y con alcance limitado, cifrado con la API de protección de
  datos de Windows, renovado solo y revocado con Cerrar sesión (o desde las apps conectadas de
  trigglow.com), y un paso automático y único desde la v0.5.x. El inicio de sesión de la v0.5.x y
  anteriores se apaga cuando se publica esta versión, así que actualizar es obligatorio.
- **Tickets con la cuenta**, con asunto opcional y un pie con las versiones; el log de OBS redactado
  en memoria, con vista previa y adjuntado solo con consentimiento; las copias antiguas de `%TEMP%`
  se borran; los invitados pasan al formulario web cuando trigglow.com pide una comprobación humana.
- **Endurecimiento**: un único camino WinHTTP (sin redirecciones, TLS 1.2+, timeouts, tope de
  tamaño, el token solo para api.trigglow.com), solo páginas fijas, sin códigos, URLs ni rutas con
  el usuario en el log de OBS, el aviso de actualización limitado a las releases de este repo, sin
  rutas de compilación en la DLL, y todas las llamadas de red se pueden cancelar, así que OBS se
  cierra limpio aunque Trigi esté respondiendo.
- **Build/CI**: FFmpeg fijado a un build de fin de mes de BtbN, 19 suites de tests unitarios en CI
  en todas las plataformas, un job de ThreadSanitizer, `Qt6::Svg`.

## v0.7.0 (entregado — 2026-10-20)
Una versión de optimización: el buffer del delay ocupa más o menos la mitad de RAM en Windows. La
v0.6.1 prevista (cliente HTTP para macOS y Linux) no se publicó; va a continuación.
- **Códec de almacenamiento por hardware en Windows.** El buffer se guarda como HEVC (H.264 en una
  GPU sin HEVC) con el encoder de la GPU (NVENC, AMF, Quick Sync) y se decodifica con D3D11VA, en
  vez de MJPEG: a 1440p60 pasa de ~675 MB a ~325 MB con 30 s y de ~1,2 GB a ~520 MB con 60 s, el
  PSNR-Y de los frames guardados sube de ~40 a ~45 dB y la CPU del códec baja de ~0,8 a ~0,2–0,3
  núcleos (fuera de OBS, AMF de AMD). MJPEG se queda como puente mientras se prepara un encoder y
  como alternativa. Sin DLL nuevas.
- **Un ring que admite códecs inter-frame**: códec, keyframe, cadena y seq por paquete, un GOP de
  margen, decode desde un keyframe con puesta al día, varios códecs en un mismo ring y una interfaz
  de encoder asíncrona.
- **Protección frente a "Encoding overloaded"** cuando el motor de la GPU se comparte con los
  encoders de OBS: un peldaño menos ante un aviso temprano de latencia (veto de 5 minutos), vuelta a
  MJPEG al momento ante un frame que OBS se salta por retraso de codificación, un error de un
  encoder de OBS o cualquier error del hardware, y la sesión NVENC cedida a una salida que arranca.
- **Dimensionado**: clase de almacenamiento fija por ciclo (12x cuando el códec por hardware ya ha
  funcionado en la sesión, 2,4 veces los frames cuando falta RAM; el 5x de MJPEG en el primer ciclo
  de la sesión), el buffer limitado a la resolución de los encoders del stream cuando todos
  reescalan por debajo de la salida, bloques de arena de 8 MiB con uno de repuesto y como mucho dos
  hilos de slice por contexto MJPEG.
- **Soporte**: ajuste oculto `buffer_codec` (`"auto"` o `"mjpeg"`), `codec=` en `pipeline check`,
  una línea `storage codec check` y `PipelineHealth` v2. Sin UI, textos ni avisos de RAM nuevos.
- **Seguridad y licencias**: sin Quick Sync en un OBS elevado, `get_pipeline_state` limitado al
  tamaño de la estructura de quien llama, `LICENSE-ffmpeg.txt` incluido y una comprobación LGPL de
  la build de FFmpeg en CI.
- **Tests**: 23 suites en CI (estrés y pruebas adversariales con un códec inter-frame falso, la
  política del códec), más el códec por hardware sobre el FFmpeg real en builds locales de Windows.

## Siguiente
- **Un cliente HTTP en macOS y Linux** (previsto como v0.6.1, no publicado), para que el inicio de
  sesión (y con él Activar delay), Trigi y los reportes desde OBS funcionen también allí, con el
  inicio de sesión guardado en el llavero o el almacén de secretos del sistema. Hasta entonces, las
  funciones de cuenta del plugin son solo para Windows.
- **Validar el códec de almacenamiento por hardware en NVIDIA dentro de un OBS real**: NVENC
  compartido con el encoder del stream (y Enhanced Broadcasting) sin ningún frame saltado por
  retraso de codificación, NVDEC, la VRAM que ocupa y los bytes por frame frente a las cifras de
  AMD. También Quick Sync, en hardware Intel.
- **Después: copia cero en la GPU.** Codificar directamente desde la textura de OBS y decodificar a
  una textura D3D11, para que desaparezcan las copias de cada frame en la CPU (lectura, frames sin
  comprimir, la reserva de decodificados): según nuestra estimación, otros 50–120 MB menos y menos
  trabajo en el hilo gráfico de OBS. Tiene más riesgo (compartir el dispositivo D3D11 de OBS con
  FFmpeg entre hilos), así que va después del cliente HTTP.
- **Deduplicar las capturas por la hora de frame de OBS** (`obs_get_video_frame_time()`) en vez de
  por una celda de reloj por intervalo de frame: exactamente una captura por frame de OBS, sin
  depender de la fase de la celda, para que el jitter del render no meta dos renders en una celda y se
  pierda un frame real.
- **Un render target más pequeño para el keep-alive.** Mientras el buffer se llena, mantener la
  escena en directo renderizando todavía la renderiza en una textura del tamaño del lienzo cuyo
  contenido nunca se usa (14,7 MB a 1440p, con un clear por frame); bastaría un target diminuto con la
  misma proyección.
- **Rediseño del delay a nivel de output** — el siguiente gran paso de arquitectura; ver "Más
  adelante" abajo para lo que implica y por qué no ha empezado.
- Verificar la v0.6.0 y la v0.7.0 en directo: salida 1440p60 y la degradación dentro de una sesión
  real de OBS (las cifras de arriba salen de benchmarks fuera de OBS en CPU y GPU de AMD), y la
  alineación A/V con un reloj y una palmada.
- Presets de delay guardados (p. ej. "Delay corto 5s", "Delay largo 30s"), seleccionables desde el
  dock y desde hotkeys adicionales.
- **Verificación en directo** de macOS/Linux — ambos ya compilan en verde en CI; esto trata de
  ejecutar y confirmar el plugin de verdad en esas plataformas, no de trabajo nuevo de build.
- Revisar los ratios de compresión supuestos (5x para MJPEG, 12x para el códec por hardware desde la
  v0.7.0) con las cifras de `ratio` y de la arena de la línea `pipeline check` de más sesiones y
  juegos. Solo deciden cuántos segundos caben en el presupuesto, así que subirlos daría duración, no
  memoria residente.

## Replay Buffer en directo real (pausado — acotado, sin empezar, 2026-09-08)
Se planteó como v0.7.0 cuando se acotó; la v0.7.0 acabó siendo la versión de RAM de arriba, y esto
no tiene número de versión hasta que se retome.

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
  bufferiza el bitstream ya codificado y lo reenvía mediante su propio sender RTMP multi-destino, de
  modo que Programa, el cambio de escena, el Replay Buffer y la grabación nunca se tocan — con una
  huella de memoria menor que nuestro ring de frames, ya que el bitstream del propio stream es
  todavía más pequeño que los frames MJPEG (o, desde la v0.7.0, HEVC de alta calidad) del buffer. Es
  arquitectónicamente la solución más limpia al objetivo del Replay Buffer en directo real Y al
  cambio de escena sin recarga (ver "Fuera de alcance por ahora" abajo) a la vez, pero implica
  sustituir el output RTMP nativo de OBS por uno propio (reconexión, manejo de keyframes, envío
  multi-destino) — varias sesiones de trabajo y un riesgo notablemente mayor. Solo merece
  priorizarse si el fix más acotado del Replay Buffer no llega a cubrir el uso real una vez
  entregado.
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
