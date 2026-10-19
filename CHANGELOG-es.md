*[English version](CHANGELOG.md)*

# Changelog — Trigglow Dynamic Delay for OBS

## v0.7.0 — 2026-10-20 (Early Access)

Una versión de optimización: en Windows el buffer del delay ocupa más o menos la mitad de RAM. Los
frames que esperan el delay se guardan con el encoder de vídeo por hardware de tu GPU y se
decodifican en la GPU, en vez de comprimirse uno a uno como imágenes MJPEG, y la imagen con delay
sale más parecida a la original. No hay nada que configurar: el dock, sus textos y sus avisos son
los mismos. Esta entrada recoge primero lo que vas a notar, después el detalle técnico y al final
las limitaciones conocidas.

**Antes de actualizar:**
- No hay que cambiar nada. Instala encima de la v0.6.0 con OBS cerrado; tus escenas, ajustes,
  atajos e inicio de sesión se conservan.
- Si vienes de la v0.5.x o anterior, lee "Antes de actualizar" de la v0.6.0, más abajo: actualizar
  es obligatorio, y los objetos que gestiona el plugin pasan a llamarse "(auto)".

**Qué cambia para ti:**
- **Más o menos la mitad de RAM para el buffer del delay en Windows.** Con un encoder de vídeo por
  hardware (NVENC de NVIDIA, AMF de AMD o Quick Sync de Intel) el buffer se guarda como vídeo HEVC,
  o H.264 en una GPU sin HEVC, y se decodifica en la GPU. A 1440p60 pasa de ~675 MB a ~325 MB con
  30 s de delay, y de ~1,2 GB a ~520 MB con 60 s: entre un −49 % y un −62 % según la resolución y el
  delay. Son cifras de referencia medidas fuera de OBS en una GPU AMD (AMF), con contenido tipo
  gameplay; NVENC aún no está medido. Tabla por resolución en `docs/FAQ-es.md`.
- **La imagen con delay se parece más a la original.** En las mismas mediciones, los frames
  guardados vuelven con un PSNR-Y de ~45 dB en vez de ~40 dB con MJPEG, así que el encoder de tu
  stream parte de una imagen más limpia.
- **Menos CPU.** La compresión la hace el motor de vídeo de la GPU: el trabajo de códec del plugin
  baja de ~0,8 a ~0,2–0,3 núcleos de CPU a 1440p60, y de ~2 a ~0,4–0,65 a 2160p60.
- **El encoder de tu stream va primero (protección frente a "Encoding overloaded").** Cuando el
  buffer codifica en el mismo motor de la GPU que alguno de los encoders de OBS:
  - si sube la latencia del encoder del buffer, o se le acumulan frames, el buffer primero baja un
    peldaño de resolución (nunca por debajo de tu calidad mínima) y no vuelve a subir en 5 minutos;
    el dock muestra su aviso ámbar de siempre;
  - si OBS se salta un frame por retraso de codificación, o una de sus salidas se para por un error
    del encoder, el buffer vuelve a MJPEG al momento, sin cortar ni mover el delay;
  - cuando OBS empieza a emitir, a grabar o a usar el Replay Buffer, el buffer le cede antes su
    sesión NVENC y recupera el hardware unos segundos después.

  Cualquier error del encoder o del decoder por hardware también devuelve el buffer a MJPEG al
  momento. Una vez en MJPEG se queda así hasta que desactivas y vuelves a activar el delay, y su RAM
  sube hasta la cifra de MJPEG durante los siguientes segundos de delay, siempre dentro del
  presupuesto.
- **Caben más segundos en la misma RAM cuando falta.** Cuando el códec por hardware ya ha funcionado
  en esta sesión de OBS, el ring se dimensiona suponiendo una compresión de 12x en vez de 5x, así
  que cuando la RAM no da para todo el delay caben unas 2,4 veces más frames. La primera vez que
  activas el delay en cada sesión de OBS se dimensiona con un margen conservador (el 5x de MJPEG),
  porque aún no se ha comprobado ningún encoder de hardware.
- **Se guarda a la resolución del encoder de tu stream cuando todos los encoders reescalan por
  debajo de la salida.** Por ejemplo, una salida 1440p que se emite con un encoder reescalado a
  1080p, sin ninguna grabación por encima de 1080p: el buffer se guarda a 1080p, porque nadie usa
  las líneas de más. Con la cámara virtual u otra salida sin encoder activa, o con
  `buffer_output_cap` desactivado, se queda a la resolución de salida.
- **Bloques de memoria más pequeños y como mucho dos hilos de compresión por contexto**, que también
  aligeran MJPEG: ~15–35 MB menos en nuestras pruebas, y hasta ~60 MB (1440p) o ~130 MB (2160p)
  menos cuando el plugin había añadido hilos.
- **Sin controles, textos ni avisos de RAM nuevos.** Un ajuste oculto de soporte, `buffer_codec`,
  fuerza MJPEG (pasos en `docs/TROUBLESHOOTING-es.md`).
- **Linux y macOS no cambian**: Linux sigue con MJPEG y macOS guarda el buffer sin comprimir.

**Códec de almacenamiento (detalle técnico, `src/storage-codec*`, `src/hw-storage-codec.*`):**
- **Interfaz.** `storage::Encoder` es asíncrono (`Send()`/`Receive()`, frames en vuelo, `Drain()`) y
  `storage::Decoder` decodifica un paquete guardado y se vacía (flush) ante una discontinuidad.
  MJPEG, el modo sin comprimir y el códec por hardware las implementan; `VideoCodec` y
  `CodecFactory` desaparecen. Los frames decodificados son siempre I420, así que la reproducción no
  sabe qué códec había detrás.
- **Encoders por hardware** (`TRIGGLOW_HAVE_HW_STORAGE`, solo Windows), en este orden y solo de los
  fabricantes de GPU presentes: `hevc_nvenc`, `hevc_amf`, `hevc_qsv`, `h264_nvenc`, `h264_amf`,
  `h264_qsv`; primero los motores en los que OBS no codifica (una iGPU, por ejemplo). NVENC solo
  mientras OBS tenga como mucho 3 encoders NVENC en marcha, así queda una sesión libre incluso con
  el antiguo límite de 5. QP constante 20 en HEVC y 18 en H.264, un keyframe cada medio segundo
  (30 frames a 60 fps), sin B-frames, el preset más rápido, sin lookahead, IDR forzados y parámetros
  en banda (copiados delante de un keyframe que no los lleve). NVENC y AMF se abren en un
  dispositivo D3D11 del adaptador de su fabricante (el de OBS si es de ese fabricante).
- **Decode por hardware**: D3D11VA en el adaptador de OBS, de baja latencia, descargado como NV12 y
  separado a I420 en el hilo de decode, nunca con un paso silencioso a software. El decoder
  HEVC/H.264 por software de libavcodec es solo una vía de emergencia: cuando falla el decoder por
  hardware, o cuando el buffer abandona un motor que comparte con OBS para decodificar (AMD, Intel;
  NVIDIA decodifica en NVDEC, un motor aparte).
- **Hilo de preparación** (`trigglow-codec-setup`): abre y sondea un encoder sin retrasar nunca una
  captura: 8 frames sintéticos, keyframes forzados en el 0 y el 5 con sus parámetros en banda, un
  decode D3D11VA del 0 al 7 y, tras vaciar el decoder, del 5 al 7, tamaños y PSNR-Y por encima de
  30 dB. Un registro de la sesión recuerda qué pasó la prueba (un tamaño nuevo solo necesita
  entonces una reapertura corta), qué falló y a qué tamaño, y qué falló de forma grave (no se vuelve
  a probar hasta reiniciar OBS). Mientras tanto el buffer guarda en MJPEG desde el primer frame (el
  "puente"); el encoder por hardware toma el relevo en una frontera de frame, en una cadena nueva
  que empieza en un keyframe. En una RX 7800 XT: sondeo de 330–380 ms a 1440p y reapertura de unos
  100 ms.
- **Ring con cadenas.** Cada paquete lleva su códec, si es keyframe, su cadena y su seq; un mismo
  ring admite varios códecs. Un P-frame solo se decodifica justo después de su referencia; un
  paquete perdido después de codificarse fuerza el siguiente keyframe y los P-frames hasta entonces
  no se guardan; las capturas descartadas antes de codificarse no rompen nada. Con almacenamiento
  inter-frame el ring guarda un GOP de margen (30 slots a 60 fps en vez de 15), así que el keyframe
  del frame que toca siempre sigue ahí. Al empezar la reproducción, al reanudarla tras una pausa o
  al saltar, el decoder se pone al día sin mostrar desde el keyframe o espera al siguiente,
  manteniendo el frame en pantalla; esas repeticiones no cuentan como decode lento.
- **Hilo de codificación.** Un único contexto por hardware (bitstream en orden de captura), paquetes
  emparejados con sus frames por seq. Un paquete fuera de orden, un error del encoder, o 500 ms con
  frames en vuelo y ningún paquete mientras siguen llegando frames es un error grave. La latencia
  del encoder (más o menos un frame) nunca cuenta como carga para la degradación, que solo ve el
  tiempo propio del hilo. Un tamaño de frame nuevo (un peldaño, un reinicio del vídeo de OBS) vacía
  el encoder anterior; MJPEG hace de puente hasta que el nuevo está abierto.

**Política del códec de almacenamiento (detalle técnico, `src/storage-codec-policy.*`):**
- Estados `Mjpeg`, `Probing`, `Hw`, `HwGuarded`, `Yielded` y `Evacuated`, evaluados cada 250 ms en
  el hilo gráfico junto a la política de degradación. Pura y con tests.
- **Motor compartido**: el encoder del buffer es de la misma familia que un encoder de vídeo activo
  de OBS, o su decode va en una GPU AMD o Intel cuyo motor también codifica para OBS. Los encoders
  de OBS se listan al activar, en cada inicio o parada de la emisión, la grabación, el Replay Buffer
  y la cámara virtual, y una vez por segundo mientras el delay está activo, lo que también ve las
  salidas que arrancan otros plugins (Aitum Vertical, multi-rtmp, Source Record). Los encoders
  desconocidos cuentan como compartidos.
- **Vuelta a MJPEG al momento** (`Evacuated`, hasta la siguiente activación): cualquier error grave
  del encoder o del decoder por hardware (un encoder que ha fallado no se vuelve a probar en esta
  sesión); y, con el motor compartido, uno o más frames saltados por OBS por retraso de codificación
  en 2 s (mezcla principal y mezclas propias de sus encoders), una salida de OBS que se para por un
  error del encoder, o un aviso temprano sin peldaño que bajar. Con el decode en el motor
  compartido, los frames por hardware que quedan en el ring se decodifican por software.
- **Aviso temprano** (motor compartido, mantenido durante 1 s con al menos 20 paquetes, 2 s después
  del cambio anterior): latencia p95 por encima de 2,5 intervalos de captura (42 ms a 60 fps), o
  3 frames en vuelo. Un peldaño menos (menos píxeles es lo que libera el motor) y sin volver a subir
  durante 5 minutos.
- **Sesiones NVENC**: en los eventos de "inicio" de la emisión, la grabación y el Replay Buffer, si
  el buffer puede tener una sesión NVENC la cierra (la UI espera como mucho 100 ms, y solo cuando
  los encoders NVENC de OBS, el que arranca y el del buffer suman 4 o más), guarda en MJPEG mientras
  tanto y vuelve a preparar el hardware 3 s después de que la salida arranque (10 s como mucho).

**Cambiado (memoria y dimensionado):**
- **Bloques de la arena** de lo esperado/64 en MiB enteros, entre 4 y 8 MiB, con un bloque de
  repuesto (antes lo esperado/16, entre 4 y 32 MiB, dos de repuesto y al menos dos frames sin
  comprimir): la memoria reservada de más sobre los paquetes vivos baja de 35–71 MB a unos dos
  bloques. Un frame sin comprimir mayor que un bloque va por la vía de tamaño excesivo.
- **Como mucho dos hilos de slice por contexto MJPEG**: los pasos de paralelismo son 1×1 → 2×1 → 2×2
  con hilos de códec y 1 → 2 slices en modo síncrono; el códec por hardware solo tiene 1×1.
- **Clase de almacenamiento, fija por ciclo de delay** (ajuste del filtro `storage_class`): `intra`
  (5x y un cuarto de segundo de margen; con `buffer_codec` en `mjpeg`, `codec_workers` desactivado,
  sin GPU, o en Linux; 1x en macOS), `intra-gop` (5x con un GOP de margen: un ciclo en el que el
  hardware puede tomar el relevo antes de que ningún encoder haya pasado su sondeo en esta sesión) e
  `inter` (12x y un GOP de margen, cuando ya alguno lo ha pasado). El ring, la estimación del dock y
  el delay del audio pasan por la misma clase, así que mantienen un único delay en segundos enteros.
  El 12x aguanta aunque el ciclo acabe en MJPEG (13–17x medido); con contenido peor la arena
  descarta frames en vez de crecer.
- **Techo de los encoders** (`encodercheck::EncoderCeiling()`): los encoders activos que ven el
  lienzo principal (exactamente o por relación de aspecto, incluidas todas las pistas de Enhanced
  Broadcasting y nunca otros lienzos) limitan el buffer al mayor de ellos cuando queda por debajo de
  la salida de OBS. No hay techo si no hay ninguno, con una salida sin encoder sobre la mezcla
  principal (cámara virtual, NDI), con un encoder reescalado por GPU de otra relación de aspecto, o
  con `buffer_output_cap` desactivado. Es un techo blando: el ring, su límite de RAM y el delay del
  audio siguen dimensionados para la salida, así que puede levantarse en cualquier momento; el dock
  no muestra nada nuevo.

**Añadido (soporte y diagnóstico):**
- **Ajuste oculto `buffer_codec`** en `buffer-mode-settings.json`: si falta vale `"auto"` (hardware
  donde funcione); el texto exacto `"mjpeg"` fuerza MJPEG. Solo se lee al arrancar OBS, con la misma
  lectura estricta que los demás ajustes ocultos: cualquier otro valor (`"MJPEG"`, `"hevc"`,
  `false`, `0`) mantiene auto con un WARN, y desaparece del archivo al cerrar OBS. `codec_workers`
  desactivado manda sobre él (MJPEG). Las líneas `settings loaded` y `enabled` muestran
  `buffer_codec=auto|mjpeg`, y `enabled` además la `storage_class` del ciclo.
- **`pipeline check`** nombra el códec que guarda los frames después de `mode=` (`codec=mjpeg`,
  `codec=hevc_nvenc`…; `raw` en macOS).
- **Línea `storage codec check`** justo después, cada 10 s: el encoder con su QP y su GOP, el
  decoder (`d3d11va`, `sw` o `mjpeg`), la latencia p50/p95/máx del encoder por hardware, los frames
  en vuelo y el p95 de `Send()`, keyframes e IDR forzados, cadenas rotas y P-frames huérfanos
  descartados, puestas al día y esperas de keyframe, frames de puente, cambios de códec, errores
  graves, el estado de la política y su veto, y los frames que OBS se saltó por retraso de
  codificación en la mezcla principal y en las de sus encoders. Una línea corta cuando todo el ciclo
  es MJPEG.
- **Otras líneas**: `storage codec: auto -> hevc_amf (…)` cuando un encoder por hardware pasa su
  sondeo (QP, GOP, adaptador de decode, tiempos del sondeo, qué se probó, si comparte motor con OBS
  y las sesiones NVENC de OBS), o por qué se queda en MJPEG; un WARN `delay buffer codec … -> mjpeg:
  <motivo>` en cada vuelta a MJPEG; líneas INFO para las cesiones; `delay pipeline: …` nombra la
  clase de almacenamiento; `encoder ceiling WxH: …` o `no encoder ceiling: …`.
- **`PipelineHealth` v2**: el códec, el nombre del encoder, el decode por hardware, el estado de la
  política y su contador de cambios, solo para el diagnóstico de soporte. El dock no cambia.

**Seguridad:**
- **Sin Quick Sync en un OBS ejecutado como administrador.** Su runtime se carga a través de un
  cargador que busca DLL en rutas de las que un proceso elevado no debería fiarse; el log lo dice
  cuando hay una GPU Intel. NVENC y AMF cargan su runtime desde el driver, y el decode es D3D11VA.
- **`get_pipeline_state` solo copia lo que cabe en la estructura de quien llama** (un parámetro
  `size`), así que a quien se compiló con la estructura v1, más pequeña, nunca se le sobrescribe
  memoria más allá de ella.
- Solo se sondean los fabricantes de GPU presentes, así que no se carga el runtime de ningún otro.
  El plugin solo decodifica bitstreams que ha producido él mismo.

**Empaquetado / Build / CI:**
- **`data/licenses/LICENSE-ffmpeg.txt`**: la build de FFmpeg de BtbN fijada en `buildspec.json`
  (etiqueta de la release, archivo, SHA-256, commit de FFmpeg), enlaces a su código fuente y a sus
  scripts de build, los componentes MIT/BSD/Apache que enlaza (libvpl, AMF, ffnvcodec) y los textos
  de la LGPL v3 y la GPL v3. El instalador lista cada aviso, así que un paquete sin alguno no
  compila, y las tablas de terceros del README nombran las DLL de FFmpeg.
- **La build de Windows comprueba que el FFmpeg descargado sigue siendo LGPL**: falla si su línea de
  configuración lleva `--enable-gpl` o `--enable-nonfree`, o si libavcodec no declara una licencia
  LGPL.
- **Sin DLL nuevas**: los encoders y decoders por hardware están en las `avcodec-63.dll` y
  `avutil-61.dll` que ya se distribuyen, y sus runtimes vienen con el driver de la GPU.
- El job de ThreadSanitizer también ejecuta `pipeline_gop_stress_tests`; `TRIGGLOW_STRESS_CI=1`
  relaja también sus comprobaciones de tiempo.
- Versión 0.7.0 en `buildspec.json` y en el script del instalador de Windows.

**Tests** (23 suites, sin OBS, más `hw_storage_codec_tests` en builds locales de Windows; `ctest`):
- Nuevos: `storage_codec_tests` (frames en vuelo de 0 a 16, contabilidad de cadenas, parámetros en
  banda, NV12, la lectura estricta de `buffer_codec`, el orden de candidatos con y sin Quick Sync,
  familias de encoders de OBS, motor compartido), `storage_policy_tests` (prioridades, la regla del
  motor compartido, enfriamiento, veto, cesiones, pocas muestras, una hora en el límite de los
  umbrales con como mucho una transición), `pipeline_gop_stress_tests` (el pipeline con un códec
  inter-frame falso: latencia, GOP, pérdidas, cambios de códec y de tamaño, pausas de render,
  preparaciones que fallan; nunca un P-frame sin su referencia, cadenas que empiezan en un keyframe,
  un hilo por objeto de códec y nunca el hilo gráfico), `adversarial_gop_tests` (evacuaciones dentro
  de un GOP, paquetes perdidos después de codificarse, desactivar y cerrar con una preparación
  bloqueada, tamaños que cambian sin parar, cesiones, mitad de fps, pausas con frames retenidos en
  el encoder, un decoder por hardware roto, un decode por software lento, una arena demasiado
  pequeña) y `hw_storage_codec_tests` (el FFmpeg real con fallos inyectados: aperturas fallidas, una
  preparación cancelada, un decoder que no llega a abrirse).
- Ampliados: `frame_ring_tests` (paquetes decodificables, planes de arranque del decode, códecs
  mezclados), `frame_arena_tests` (bloques de 8 MiB, un repuesto), `buffer_fit_tests` (clases de
  almacenamiento, margen de GOP, 2,4 veces los frames en el suelo, 4K60 durante 60 s con 16 GB a
  resolución completa), `encoder_check_tests` (el techo), `degradation_policy_tests` (escaleras de
  dos slices, pasos por hardware, la bajada forzada de peldaño, el veto, la latencia que nunca
  cuenta como carga) y `support_diagnostics_tests` (tamaños de `PipelineHealth`).

**Limitaciones conocidas:**
- **NVENC aún no se ha validado dentro de un OBS real.** La vía por hardware se midió con AMF de AMD
  fuera de OBS. NVENC y NVDEC de NVIDIA, Quick Sync de Intel y la VRAM que ocupa el códec por
  hardware no están medidos, y los bytes por frame (y con ellos la RAM) pueden variar en NVIDIA.
  Cada encoder se sondea antes de usarlo, y cualquier error, o un frame que OBS se salte con el
  motor compartido, devuelve el buffer a MJPEG; si aun así ves un problema, pon `buffer_codec` en
  `"mjpeg"` y envía un reporte.
- **El decode por software de emergencia es más lento y no llega a 4K60.** Si falla el decoder por
  hardware, o el buffer abandona un motor que comparte con OBS para decodificar (AMD, Intel), los
  frames por hardware que ya están en el buffer se decodifican en la CPU: a 2160p60 la imagen con
  delay puede repetir frames (nunca mover el delay) hasta que lleguen a pantalla los frames MJPEG
  siguientes, como mucho lo que dure el delay.
- **Linux y macOS no cambian**: MJPEG en Linux y sin comprimir en macOS. El inicio de sesión, Trigi
  y los reportes desde OBS siguen siendo solo de Windows.
- Tras volver a MJPEG, el buffer no vuelve a probar el hardware hasta que desactivas y activas el
  delay. La primera activación de cada sesión de OBS dimensiona el ring con el 5x de MJPEG, así que
  con poca RAM puede guardar menos segundos que las siguientes.
- Cuando el buffer baja un peldaño porque el motor de vídeo de la GPU va saturado, el aviso del dock
  da la causa que ya existía, "comprimir los fotogramas consume mucha CPU"; el WARN del log dice que
  fue el motor.
- Siguen valiendo las limitaciones de la v0.6.0 (cambiar Program a la escena en directo a mano o
  borrar un filtro "(auto)" con el delay activo, los tirones de render, el render target del tamaño
  del lienzo para mantener viva la escena).

## v0.6.0 — 2026-10-19 (Early Access)

El delay deja de competir con OBS por su hilo gráfico, se adapta a lo que da tu equipo sin cortarse
nunca, y el plugin adopta el aspecto de trigglow.com. "Reportar un problema" empieza ahora por
Trigi, el asistente de soporte con IA de Trigglow, el inicio de sesión pasa a un flujo más seguro
con un inicio de sesión propio del plugin y cifrado, y el plugin pasa una revisión de seguridad y
privacidad. Se preparó un hotfix de rendimiento v0.5.1 que nunca se publicó: todo lo que llevaba
sale aquí. **Actualizar desde la v0.5.x es obligatorio** (ver "Antes de actualizar"). Esta entrada
recoge primero lo que vas a notar, después el detalle técnico y al final las limitaciones conocidas.

**Antes de actualizar:**
- **Actualizar es obligatorio para seguir usando el delay.** El inicio de sesión de la v0.5.x y
  anteriores se apaga cuando se publica esta versión: desde ese momento esas versiones ya no pueden
  iniciar sesión en Trigglow, y las sesiones que ya tienen dejan de funcionar tras un periodo de
  transición. El delay necesita una cuenta con la sesión iniciada, así que actualiza. Al instalar la
  0.6.0 encima de tu versión, tu inicio de sesión pasa solo al formato nuevo en el primer arranque;
  si el dock te pide iniciar sesión, hazlo una vez.
- **El inicio de sesión, Trigi y el envío de reportes desde OBS solo funcionan en Windows en esta
  versión**; macOS y Linux los tendrán en la v0.6.1 (ver "Limitaciones conocidas").
- **Los objetos que gestiona el plugin pasan de "(no tocar)" a "(auto)"**, de forma automática.
  **Apunta a los nombres nuevos cualquier acción de Stream Deck, obs-websocket u otro control remoto
  que use los antiguos** (lista abajo).
- **Volver a la v0.5.0 o anterior después de actualizar duplica esos objetos** (ver "Nombres" más
  abajo).

**Qué cambia para ti:**
- **Trigi, el asistente de soporte con IA de Trigglow, en "Reportar un problema"** (Windows, con
  sesión iniciada). Pregunta por Dynamic Delay y recibe la respuesta en el propio diálogo; si no lo
  resuelve, **Crear ticket con esta conversación** se lo pasa al equipo con el contexto. La primera
  vez, un aviso breve explica qué es Trigi y qué pasa con tus datos, y siempre puedes escribir al
  equipo en su lugar. **El log de OBS nunca se envía a la IA.** Los datos técnicos de OBS son
  opcionales y ves exactamente qué se envía antes de marcarlos.
- **Inicio de sesión más seguro.** El dock muestra un código, y la página de trigglow.com debe
  mostrar el mismo antes de que confirmes; **No he sido yo** en la página cancela la solicitud. El
  plugin guarda su propio inicio de sesión, cifrado para tu usuario de Windows, y **Cerrar sesión**
  también lo desconecta en trigglow.com. Tu inicio de sesión de la v0.5.x se pasa solo.
- **Los reportes desde OBS van a tu cuenta si tienes la sesión iniciada**: sin nombre ni email que
  escribir, y sigues el ticket en tu panel de soporte de trigglow.com. El log de OBS solo va si marcas
  su casilla, cosa que puedes hacer después de verlo en **Ver qué se envía**: tu usuario de Windows,
  URLs, claves, emails e IPs van ocultos, y el diálogo te dice qué puede seguir apareciendo.
- **Se acabó el "Encoding overloaded" (sobrecarga al codificar) al activar el delay con un lienzo
  mayor que la salida.** Reportado en directo con un lienzo 2560x1440 y una salida 1920x1080 a
  60 fps. El buffer del delay se guarda ahora como mucho a la resolución de salida de OBS (Ajustes →
  Vídeo → Resolución de salida (escalada)), así que puedes volver a tu lienzo grande.
- **Salida 1440p a 60 fps soportada.** Comprimir y descomprimir los frames del buffer ya no ocurre
  en el hilo gráfico de OBS (el que tiene que terminar a tiempo cada frame de tu stream) sino en
  hilos propios del plugin. A 1440p60 el trabajo del plugin en ese hilo baja de ~14/17 ms a
  ~1,2–1,7/1,6–2,1 ms por frame (p50/p95, microbenchmark fuera de OBS en una CPU de 8 núcleos/16
  hilos).
- **Calidad mínima 1440p (2K) y 2160p (4K)** en el dock, tras 480p, 720p y 1080p. Con una salida
  1440p y un mínimo de 1440p el buffer se queda en 1440p cuando falta RAM (el delay se acorta,
  igual que con 1080p) y cuando tu equipo no da más (solo se captura a la mitad de fps, con una
  salida a 48 fps o más). Nunca supera la salida de OBS: con una salida 1080p el buffer es 1080p.
  Así, un lienzo 2560x1440 puede salir a 1080p, o a 1440p60.
- **Si tu equipo no da más, el delay sigue sin cortarse.** El plugin primero añade hilos de
  compresión, después guarda el buffer a menos resolución (nunca por debajo de tu calidad mínima) y
  solo entonces captura a la mitad de fps; vuelve a subir solo en cuanto hay margen. Mientras
  tanto, el dock muestra un aviso ámbar con qué se redujo y por qué. Los segundos de delay no cambian
  nunca.
- **Menos RAM y sin parón al empezar el delay.** Unos 0,6–0,8 GB en vez de ~1,9 GB para 30 s a
  1440p60 (simulación fuera de OBS). La memoria se toma a medida que llegan los frames en vez de
  toda de golpe, y al desactivar el delay se libera fuera del hilo gráfico de OBS, junto con los
  códecs y la memoria de vídeo que usaba el delay.
- **Audio y vídeo comparten siempre el mismo delay**, en segundos enteros, también cuando la RAM no
  da para el delay completo (antes podían quedar hasta ~1 s separados). El dock y el overlay del
  directo muestran esa misma cifra.
- **Los segundos de delay se bloquean mientras el delay está en marcha**, como las escenas y la
  calidad mínima: "Desactiva el delay para cambiarlo." **El dock es el único sitio donde se elige el
  delay**: los filtros "(auto)" ya no tienen campos de delay ni de calidad en sus propias
  propiedades, ignoran los valores que se escriban desde fuera y se vuelven a activar solos si algo
  los desactiva mientras el delay está en marcha.
- **Cambiar de colección de escenas desactiva el delay**, y el dock dice por qué: la escena auxiliar
  y los filtros pertenecen a la colección que has dejado. Vuelve a activarlo cuando quieras.
- **La cuenta atrás y las cifras cuadran**: mientras el delay se llena, el dock cuenta los segundos
  que de verdad se aplican ("41 / 60 s" cuando en la RAM solo caben 41), y la cuenta atrás ya no
  vuelve a empezar al activar o desactivar el overlay o cambiar su esquina. La línea de RAM bajo los
  campos se lee ahora como la estimación de RAM que es ("En la RAM solo caben 41 s de 60 s…"), en
  segundos enteros.
- **Nuevo diseño** del dock, el diálogo de reporte y el overlay del directo, alineado con
  trigglow.com, sobre tu tema de OBS (oscuro, claro, alto contraste), con contraste AA y foco de
  teclado visible. El overlay del directo es una sola imagen que pinta el plugin, firmada con un
  pequeño "By Trigglow" bajo el icono.
- **Correcciones**, entre otras: los filtros de delay que quedaron activos al cerrar OBS con el delay
  Activo se desactivan al arrancar y al cambiar de colección de escenas; a 59,94 fps ya no se pierde
  un frame por segundo; cambiar los ajustes de vídeo de OBS con el delay en marcha muestra la escena
  de carga en vez de negro; renombrar la escena en directo ya no llena el log ni impide desactivar el
  delay; cerrar OBS mientras Trigi responde o se envía un ticket ya no espera a la red.

**Añadido / Trigi, el asistente de soporte con IA (Windows, con sesión iniciada):**
- **"Reportar un problema" se abre en Trigi** cuando está disponible: en Windows, con la sesión
  iniciada y con una versión del plugin que Trigi admita. Si no, el diálogo abre directamente el
  formulario de ticket, como antes; sin sesión, ofrece **Inicia sesión para preguntar a Trigi** junto
  al formulario.
- **Primer uso: "Antes de empezar con Trigi".** El aviso dice que Trigi es una IA y puede
  equivocarse; que Trigglow y un proveedor de IA indicado en la
  [Política de Privacidad](https://www.trigglow.com/legal/privacy) procesan tus mensajes y, si lo
  activas, los datos técnicos de OBS; que las conversaciones se guardan hasta 12 meses; que no
  escribas contraseñas, claves de stream, URLs de overlay ni datos de pago; que el log de OBS nunca se
  envía a la IA; y que siempre puedes escribir al equipo. **Entendido** o **Prefiero escribir al
  equipo** (el formulario de ticket). No se envía nada hasta que lo aceptas y mandas tu primera
  pregunta: abrir el diálogo no hace ninguna llamada a Trigi. Si una versión futura cambia el aviso,
  vuelve a mostrarse.
- **Datos técnicos de OBS opcionales.** **Incluir datos técnicos de OBS** está desmarcada hasta que la
  marques (se recuerda tu elección), y **Ver qué se envía** muestra los datos exactos. Solo cifras y
  estados, que se envían una vez al empezar una conversación: versiones de OBS y de Windows,
  arquitectura, renderizador, fabricante de la GPU, RAM total, familia del codificador, el estado,
  los segundos y la calidad mínima del delay, tamaño del lienzo y de la salida, fps, los frames
  retrasados y saltados de OBS, la salud del buffer y los códigos de problemas recientes del plugin.
  Nunca nombres de escenas, fuentes, filtros, codificadores ni salidas, rutas, URLs, datos de tu
  emisión, tu nombre de usuario o tu email, el nombre de la GPU, IDs de hardware ni IPs
  (`src/support-diagnostics.*`, `src/diagnostics-collector.*`, que nunca espera al hilo gráfico de
  OBS).
- **La conversación.** Enter envía, Ctrl+Enter o Mayús+Enter añade una línea, con un contador de
  caracteres y la línea fija "Trigi es una IA y puede equivocarse. No compartas datos sensibles."
  "Trigi está pensando…" muestra un spinner quieto si Windows pide menos movimiento, y "Está tardando
  más de lo normal…" a los 15 s. **¿Te ha servido?** bajo cada respuesta: Sí cierra la conversación
  como resuelta, No ofrece el ticket. **Borrar conversación** solo limpia la vista. Lo que ves en OBS
  solo está en memoria y desaparece al cerrar el diálogo.
- **De la conversación al ticket.** **Crear ticket con esta conversación** pide a Trigi un resumen
  (o usa tus propias palabras si no puede) y abre el formulario ya relleno, para que lo revises y lo
  edites, con la conversación enlazada para que el equipo tenga el contexto, el pie con las versiones
  y la casilla del log. Si la respuesta trata de algo que no es Dynamic Delay, el botón es **Abrir
  soporte en trigglow.com**.
- **Cada estado dice qué hacer a continuación**: sin conexión, Trigi ha tardado demasiado,
  trigglow.com no responde, demasiadas preguntas seguidas (Enviar queda desactivado con una cuenta
  atrás), sesión terminada (se conserva lo que escribiste), conversación cerrada (se empieza una nueva
  con tu siguiente pregunta), "Trigi no puede responder ahora" (con **Crear ticket**), Trigi apagado
  (se abre el formulario sin más, con tu texto) y "Actualiza el plugin para usar Trigi" (con
  **Descargar la actualización**). Una pregunta que no llegó se marca como "No enviado" y se puede
  reintentar a mano; nada se reintenta solo.
- **Render seguro.** Las respuestas se muestran como texto plano con negrita, `código`, viñetas y
  pasos numerados, nada más, construidos a partir de texto escapado (`src/chat-text.*`,
  `src/trigi-widgets.*`); los enlaces, imágenes y HTML de una respuesta se quedan como texto plano no
  clicable, y la vista nunca carga nada. Los únicos enlaces son los botones fijos del propio plugin.
- **Accesibilidad**: orden de tabulación lógico, un nombre para cada control, un anuncio del lector
  de pantalla por respuesta y por cambio de estado (la cuenta atrás no se lee cada segundo),
  contraste AA en oscuro, claro y alto contraste, y filas de botones que se apilan en diálogos
  estrechos.
- **En el log de OBS**, una línea por evento de Trigi (conversación iniciada, pregunta enviada con su
  longitud y si llevaba datos técnicos, respuesta, no disponible, límite alcanzado, código HTTP);
  nunca el texto de tus preguntas ni de las respuestas, IDs ni los datos técnicos.
- Textos en inglés y español. Páginas y estados en `src/trigi-page.*` y `src/trigi-states.*`
  (probados sin Qt), cliente en `src/support-client.*` y `src/support-service.*`.

**Cambiado / Inicio de sesión:**
- **Iniciar sesión** abre trigglow.com en tu navegador y el dock muestra un código: "La página debe
  mostrar el código KD7F-29QM." La página muestra la app, su versión y su sistema, cuándo se hizo la
  solicitud, el mismo código y, si la solicitud parece venir de una red distinta a la tuya, un aviso;
  **Confirmar** o **No he sido yo**. **Copiar enlace** y **Cancelar** siguen en el dock. El secreto
  con el que el plugin espera tu confirmación nunca sale de él ni va en una URL, y ni el enlace, ni
  los códigos, ni el secreto se escriben en el log de OBS.
- **Un inicio de sesión propio del plugin.** Cuando confirmas, el plugin recibe su propio token, que
  solo sirve para lo que hace el plugin (comprobar tu cuenta, renovarse, cerrar sesión, Trigi y tus
  propios tickets de Dynamic Delay), no para el resto de tu cuenta. Se renueva solo cuando
  trigglow.com lo indica. En Windows se guarda cifrado con la protección de datos de Windows, solo
  para tu usuario de Windows y aparte de los ajustes del plugin; tu email no se guarda. Si no se puede
  descifrar (otro usuario de Windows, un perfil restaurado), se descarta y vuelves a iniciar sesión.
- **Cerrar sesión lo revoca en trigglow.com** además de borrarlo de tu equipo (la copia local se
  borra aunque no haya conexión). También puedes desconectar el plugin desde la web: Ajustes →
  Sesiones activas → Apps conectadas.
- **Los inicios de sesión de la v0.5.x se pasan solos** en el primer arranque de la 0.6.0: el inicio
  de sesión antiguo se cambia una vez por el nuevo, y el fichero antiguo en texto plano se sobrescribe
  y se borra (también si ya existe un inicio de sesión nuevo). Si no se puede conectar con
  trigglow.com, se vuelve a intentar en el siguiente arranque y mientras tanto el delay sigue
  funcionando.
- **Cerrar sesión siempre se mantiene.** Cerrar sesión mientras el plugin renueva su token, cambia
  un inicio de sesión de la v0.5.x o espera al navegador nunca hace que la sesión vuelva en el
  siguiente arranque: un token que el servidor entregue después del cierre no se guarda, y además se
  revoca. Cerrar "Reportar un problema" mientras renueva el token ya no pone en riesgo la sesión: la
  renovación termina en segundo plano y su token se guarda.
- **Una comprobación en cada arranque de OBS** (más una renovación cuando toca). Sin conexión sigues
  con la sesión iniciada y el delay funciona; si tu cuenta está suspendida o no está activa, **Activar
  delay** queda bloqueado con "Tu cuenta de Trigglow no está activa." y **Abrir trigglow.com**.
- Mensajes nuevos en el dock: inicio de sesión cancelado desde la web, demasiados intentos, "Esta
  versión del plugin ya no puede iniciar sesión. Actualiza el plugin." y la cuenta no activa.
  `src/auth-client.*`, `src/token-store.*` y `src/api-client.*`; `docs/ACCOUNT_GATE.md` describe el
  flujo.

**Cambiado / Reportar un problema:**
- **Tickets con tu cuenta.** Con la sesión iniciada, el ticket sale de tu cuenta (sin nombre ni
  email que escribir), con un asunto opcional y un pie con las versiones del plugin, de OBS y de
  Windows que ves y puedes editar. "Ticket enviado" ofrece **Ver ticket en trigglow.com**; las
  respuestas llegan por email y a tu panel de soporte.
- **El log de OBS, redactado y con tu consentimiento.** **Adjuntar el registro (log) de OBS** empieza
  desmarcada y solo se puede marcar después de abrir **Ver qué se envía**, que muestra el log ya
  redactado (sus primeras y últimas 200 líneas; se envía completo). La redacción (`src/log-redact.*`,
  línea a línea, con tests) oculta el nombre de usuario en rutas de Windows, macOS y Linux (también
  perfiles movidos, recursos de red y carpetas de OneDrive de empresa), las URLs de stream, cualquier
  otra URL (solo se conserva el host), los enlaces de overlay, emails, claves, tokens, contraseñas,
  cookies y cabeceras de credenciales, JWT, las direcciones IPv4 e IPv6 y las cadenas largas con forma
  de token, y conserva las marcas de tiempo, las versiones y los atajos. Pueden seguir apareciendo
  nombres de escenas, fuentes, dispositivos de audio y ventanas, y el diálogo lo avisa. Si después de
  redactar pasa de 4,5 MB, se conservan el principio y el final.
- **Solo el log de este OBS.** El plugin marca el log de su propio OBS al cargar y solo adjunta el
  fichero con esa marca, nunca el log más reciente de otro OBS (uno portable o una segunda
  instancia).
- **Se acabaron las copias en `%TEMP%`.** El log se lee y se redacta en memoria, y las copias que
  dejaron versiones anteriores en `%TEMP%\Trigglow` se borran al cargar el plugin.
- Si el ticket se crea pero no se puede adjuntar el log, el diálogo lo dice y ofrece **Guardar log
  redactado…** para que lo añadas desde la web.
- **Sin sesión**, se mantiene el formulario de invitado (nombre, email, mensaje). Si trigglow.com
  pide a los invitados una comprobación humana (captcha), que OBS no puede mostrar, el diálogo dice
  "Para enviar el reporte desde OBS, inicia sesión. También puedes usar el formulario web." con
  **Iniciar sesión**, **Abrir soporte web** y **Guardar log redactado…**, y nunca
  reintenta solo. La clave de acceso del ticket de invitado va en una cabecera, no en la URL, y nunca
  se escribe en el log.
- En macOS y Linux el formulario aparece desactivado con un enlace al formulario web (allí todavía no
  hay cliente HTTP).

**Cambiado / Dock:**
- **Aspecto de marca sobre tu tema de OBS.** Tarjetas, campos y fondos siguen el tema activo de OBS
  (oscuro, claro, alto contraste de Windows); la marca entra por tarjetas cuadradas con hairlines,
  titulares en Archivo y etiquetas y cifras en Geist Mono (empaquetadas, ver Empaquetado), iconos
  SVG en lugar de glifos de texto y el degradado cian→magenta solo en la acción principal, la casilla
  marcada y el estado Activo. Los colores se derivan de la paleta del tema en un único sitio
  (`src/ui-tokens.*`, `src/ui-theme.*`): todo texto, placeholders incluidos, llega a WCAG AA (4,5:1)
  y todo borde de campo y anillo de foco a 3:1 en las paletas de los temas de OBS 31 (Yami y sus
  variantes Grey, Light, Classic, Acri y Rachni), comprobado en `tests/ui_tokens_tests.cpp`. El foco
  de teclado se ve en todos los controles, y el área de desplazamiento del dock ya no es una primera
  parada del tabulador invisible.
- **Tarjeta de estado**: icono, titular y chip con los segundos en cada estado, nunca solo color.
  Preparando delay muestra una barra de progreso con "12 / 30 s" y "Faltan 18 s"; Delay activo va
  en cian con un escudo (adiós al verde, la web no tiene verde); los errores llevan regla roja e
  icono de alerta. Preparando delay y Delay activo cuentan los segundos que de verdad se aplican,
  los mismos que el overlay del directo: con 60 s pedidos y 41 s que caben en la RAM, el chip pone
  "41 / 60 s" en ámbar, el mensaje "Guardando 41 s de directo…: de tus 60 s solo caben 41 en la RAM"
  y la ventana de llenado dura 41 s. La cuenta atrás solo vuelve a empezar con una ventana de
  llenado nueva: activar o desactivar el overlay o cambiar su esquina mientras se llena ya no la
  reinicia (ni, con ella, el paso a la escena retrasada).
- **Línea de estimación de RAM** bajo los campos de delay y calidad, redactada como lo que es: "En la
  RAM caben los 30 s completos a 1920×1080", "En la RAM solo caben 41 s de 60 s a …", en los mismos
  segundos enteros que el chip. Se oculta mientras se muestra el aviso de degradación (que entonces
  dice qué guarda el buffer), y no aconseja "baja los segundos o la calidad mínima" mientras esos
  campos están bloqueados.
- **Aviso "Calidad del buffer reducida"** al pie de la tarjeta de estado mientras el buffer va por
  debajo de la calidad completa: un icono de controles deslizantes y el título en ámbar, y después
  qué se redujo (la resolución frente al tamaño completo, los fps de captura o ambos), por qué (el
  hilo gráfico de OBS va muy cargado, comprimir o descomprimir los fotogramas consume mucha CPU) y
  que el delay sigue intacto; si hay fotogramas repetidos o descartados por minuto, se indican
  debajo. Desaparece en cuanto el buffer vuelve a la calidad completa, y con el delay desactivado no
  se consulta nada. Los lectores de pantalla lo oyen una sola vez: cuando aparece, cuando cambia su
  causa, y "El buffer vuelve a la calidad completa." cuando se va con el delay todavía en marcha
  (anuncios de Qt 6.8+; con Qt anterior cambia el nombre de la tarjeta), no en cada peldaño.
- **Bloqueados mientras el delay está en marcha:** los selectores de escena en directo y de carga,
  los segundos de delay y la calidad mínima. Los segundos seguían editables con el delay Activo y se
  aplicaban con cada tecla, cambiando el tamaño del buffer en pleno directo. Los campos bloqueados
  pierden sus flechas, toman el fondo de la tarjeta y muestran un candado; una línea debajo,
  "Desactiva el delay para cambiarlo.", es además su descripción accesible; el tabulador los salta
  hasta que el delay se desactiva, y su valor sigue legible (texto atenuado, AA en todos los temas).
- **Calidad mínima 1440p (2K) y 2160p (4K)**, tras 480p, 720p y 1080p. El tooltip dice que solo
  cuentan con una salida así de grande; el dimensionado y la degradación ya admitían cualquier
  suelo.
- **Activar delay se explica**: mientras está deshabilitado, una lista indica si falta la cuenta
  gratuita o la escena en directo.
- **Selector de escena en directo**: no hay nada preseleccionado ("Elige una escena…") hasta que
  eliges una, así que también se puede elegir la primera y el campo nunca muestra una escena que la
  lista da por pendiente; una escena guardada que ya no existe cuenta como no elegida. La escena
  auxiliar del plugin ya no se ofrece como escena en directo ni de carga.
- **Los fallos de inicio de sesión ya no son mudos**: la tarjeta de cuenta dice si no se pudo
  conectar con trigglow.com, si el enlace caducó, si el navegador no se abrió (con **Copiar
  enlace**) o si la sesión terminó, y la espera se puede cancelar.
- La rueda del ratón sobre la escena, los segundos o la calidad ya no los cambia si no tienen el
  foco; desplaza el dock. El campo de segundos dibuja sus propias flechas, así que los temas sin
  estilo para ese control (el tema System de OBS) ya no muestran dos puntos.
- Los números siguen el idioma de OBS (`15.910 MB`, `41,2 s` en español), las resoluciones usan
  `×` y las unidades espacio duro (`30 s`). En docks estrechos (menos de 300 px) el botón de cuenta
  y el pie se apilan.
- **Reportar un problema**: cabecera, etiquetas encima de su campo, errores bajo el campo con el
  foco en él, errores de red en un banner y una fila de adjunto que dice si el log de OBS va con el
  ticket. Los errores técnicos de red quedan en el log de OBS. Se abre con 460 px de ancho y se
  puede estrechar hasta 340 px: Nombre y Email se apilan por debajo de 400 px y la nota del
  navegador pasa encima de los botones; la ayuda del email va bajo el campo de email.
- **Textos**: microcopy reescrito en inglés y español (tildes, sin `--`, sin glifos, tooltips más
  cortos). En español los atajos se llaman "Trigglow: Activar/desactivar Dynamic Delay", "Trigglow:
  Activar Dynamic Delay" y "Trigglow: Desactivar Dynamic Delay"; las asignaciones de teclas
  existentes se conservan.

**Cambiado / Overlay del directo:**
- El overlay "DELAY 30 s" es ahora **una única imagen que pinta el propio plugin** (placa tinta,
  regla de marca, marca de Dynamic Delay, Archivo), guardada en la carpeta de configuración del
  plugin (`overlay/delay-badge-r3-30s.png`) y mostrada por una sola fuente de imagen. Se ve igual
  en Windows, macOS y Linux (el texto anterior usaba Segoe UI, solo Windows) y se mueve de una
  pieza. Queda a un 2,2% del lado corto del lienzo de la esquina elegida y mantiene su tamaño
  relativo a ese lado: la misma placa que antes en lienzos horizontales (200x58 a 1080p), y en un
  lienzo vertical 1080x1920 el tamaño de 1080p en vez de un tercio del ancho del encuadre. Muestra
  el delay que de verdad se aplica.
- **Firmado "By Trigglow"**: un texto pequeño y discreto bajo el icono, fuera de la placa de la
  insignia (Geist Mono, 11 px en un lienzo de 1080p y 7,3 px a 720p, mist al 75 % con un halo
  oscuro muy suave y sin caja), igual en todos los idiomas. En una esquina inferior el margen se
  mantiene por debajo de la firma. Los nombres de archivo llevan una versión de dibujo, así que
  nunca se muestra una insignia que dejó en esa carpeta una compilación anterior, y se borra.
- **Si lo mueves o lo escalas en OBS, se queda así.** El plugin lo coloca en la esquina elegida la
  primera vez y de nuevo solo cuando eliges otra esquina en el dock (o cuando cambia el tamaño del
  lienzo o el de la propia insignia, siempre que no lo hayas movido ni escalado). Si ya existe una
  fuente de imagen llamada "Trigglow Delay Badge (auto)", se reutiliza en vez de añadir
  copias "... 2".
- La fuente de texto antigua ("Trigglow Delay Overlay Text (auto, no tocar)") se elimina sola. Su
  texto ya no se puede editar: si le habías cambiado la fuente o el color, ese ajuste se pierde; la
  imagen se puede mover o escalar y lo conserva.

**Cambiado / Nombres de los objetos que gestiona el plugin:**
- Adiós a "(no tocar)" en todos los idiomas: la escena pasa a ser **Trigglow Delay Buffer (auto)**,
  los filtros **Trigglow Buffer Mode Delay (auto)** y `Trigglow Audio Delay (auto) - <fuente>`, y el
  overlay **Trigglow Delay Badge (auto)**. Las colecciones existentes se renombran una vez, en su
  sitio, al cargarse (se conservan posición, filtros y ajustes); si un nombre nuevo ya está en uso se
  queda el antiguo y sigue funcionando.
- **Todo lo que se refiera a estos objetos por su nombre hay que actualizarlo**: una acción de
  Stream Deck, obs-websocket u otro control remoto que cambie a la escena con el nombre antiguo, o
  active los filtros o el overlay antiguos por su nombre, hay que apuntarla a los nombres "(auto)".
- **Volver a la v0.5.0 o anterior después del cambio de nombre los duplica**: la versión antigua no
  conoce los nombres "(auto)", así que vuelve a crear su escena auxiliar, sus filtros de delay y su
  overlay "(no tocar)" junto a ellos (al cargar la colección desactiva los filtros "(auto)", que se
  quedan inertes). Borra el juego que no uses; de vuelta en la 0.6.0 se usa el juego "(auto)" y el
  "(no tocar)" sobrante no se renombra mientras su nombre nuevo esté ocupado.

**Cambiado:**
- **La "Calidad mínima" es un suelo para la reducción por RAM y para la degradación, acotado por la
  salida de OBS.** Cuando falta RAM sigue decidiendo hasta dónde puede encogerse el buffer (antes que
  bajar de ahí, se guardan menos segundos), y la degradación tampoco baja nunca de ella; pero ya no
  eleva el buffer por encima de la resolución de salida: con una salida de 720p el buffer es 720p
  aunque el mínimo sea 1080p.
- **La línea de log `compression check` se sustituye por `perf check` y `pipeline check`** (abajo).
- **Inicio de sesión, reportes y comprobación de actualizaciones: ver "Cambiado / Inicio de
  sesión" y "Cambiado / Reportar un problema"** más arriba y "Seguridad y privacidad" más abajo.

**Corregido:**
- **Audio y vídeo comparten un único delay en segundos enteros.** Cuando la RAM recortaba el ring,
  el vídeo se retrasaba el tramo exacto (fraccionario) y el audio en segundos enteros, así que podían
  quedar hasta ~1 s separados. `bufferfit::Compute()` devuelve ahora un único delay efectivo en
  segundos enteros, conservando el margen de un cuarto de segundo contra el jitter, y vídeo y audio
  usan ese. `get_effective_delay_seconds` del filtro de vídeo lo calcula a partir de los ajustes
  actuales del filtro en el hilo que llama (sin saltar al hilo gráfico), así que el audio recibe el
  valor nuevo en cuanto el dock cambia un ajuste, desde Enable. La tarjeta de estado del dock muestra
  la misma cifra que el overlay del directo: con 60 s pedidos y 35 s que caben, el chip pone
  "35 / 60 s" en ámbar y la línea de detalle dice que de los 60 s solo caben 35 en la RAM.
- **Un reinicio del buffer ya no muestra negro.** Un cambio del delay o de la calidad mínima que
  redimensiona el buffer tira los frames guardados. El filtro ahora lo avisa (`buffer_restarted`) y
  el Programa vuelve a la escena de carga, con el audio resincronizado, mientras el buffer se
  rellena, en vez de una vista retrasada en blanco durante `delay` segundos. Igual tras un reset de
  los ajustes de vídeo de OBS. Si no hay escena de carga, el Programa se queda en la escena
  contenedora (en blanco), nunca en la escena en directo sin delay. El dock bloquea esos campos
  mientras el delay está en marcha, así que en la práctica esto cubre los resets de los ajustes de
  vídeo de OBS.
- **Los filtros de delay solo aplican el delay del dock.** Sus propias propiedades permitían
  saltarse el plugin: 90 s en el diálogo del filtro de vídeo dejaban el vídeo a 90 s y el audio a 60,
  y 0 s dejaba pasar la escena en directo sin delay mientras el dock decía "Delay activo". Las
  propiedades de los filtros "(auto)" ahora solo tienen una nota de que los gestiona el dock. El dock
  envía su delay y su suelo de calidad con nuevos procs `set_managed_*`, y a partir de ahí un filtro
  ignora cualquier otro valor que se escriba en sus ajustes (el botón de valores por defecto de
  OBS, un diálogo antiguo, obs-websocket), con un WARN por cada envío. Con el delay en marcha, un filtro de
  delay desactivado desde fuera del dock (su icono del ojo en el diálogo de Filtros, obs-websocket)
  se vuelve a activar al momento, con un WARN por activación.
- **Un delay de 0 s** (ahora solo posible en un filtro que el dock todavía no gestiona, p. ej. uno
  añadido a mano) deja pasar el vídeo sin retraso, sin buffer y sin códec. Antes comprimía cada frame
  en un ring de un slot y nunca lo dibujaba: salida en negro con todo el coste de CPU.
- **Cambiar de colección de escenas con el delay en marcha dejaba el dock diciendo "Delay activo"**
  mientras la colección nueva funcionaba sin ningún delay. En
  `OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING` (cambiar, nueva, eliminar) el delay se detiene ahora
  como con Desactivar mientras la colección antigua sigue cargada, así que se guarda con sus filtros
  apagados y su escena de vuelta en el Programa, y el dock pasa a Inactivo con una línea que dice
  por qué. OBS 31 no envía `COLLECTION_CHANGING` al duplicar o renombrar una colección, solo
  `COLLECTION_CHANGED` con la copia ya cargada: entonces se hace la misma parada, que apaga los
  filtros de la copia y sale de la escena auxiliar si la copia se abrió en ella.
- **Renombrar la escena en directo con el delay en marcha** llenaba el log (la consulta de salud del
  dock, una vez por segundo, buscaba el filtro por su nombre y avisaba cada vez que no lo
  encontraba) y dejaba el filtro activado tras Desactivar. El puente guarda ahora referencias débiles
  a los filtros que activó, para la consulta, los ajustes y para apagarlos.
- **Delay que se quedaba puesto tras cerrar OBS estando Activo.** Si OBS se cerraba con el delay
  activo, en el siguiente arranque la escena en directo quedaba con delay permanente mientras el
  dock decía "Inactivo". La v0.5.0 intentaba apagar esos filtros al cargar, pero los buscaba con
  `obs_enum_sources()`, que nunca devuelve filtros, así que no apagaba nada. Ahora recorre los
  filtros (`obs_source_enum_filters`) de inputs, grupos, escenas y fuentes de audio globales, y los
  apaga fuera de los locks de libobs, cuando OBS termina de cargar y también al cambiar a otra
  colección de escenas (en ambos casos solo con el delay desactivado).
- **Se perdían referencias al filtro de delay en cada Enable.** `obs_source_get_filter_by_name()`
  devuelve una referencia nueva, pero el plugin la trataba como prestada y nunca la liberaba, así que
  el filtro de vídeo nunca se destruía. Ahora cada búsqueda libera su referencia
  (`OBSSourceAutoRelease`), y el destructor del filtro, que libobs ejecuta sin contexto gráfico,
  entra en uno para liberar sus objetos de GPU (después de esperar a que terminen los hilos de
  códec).
- **Un archivo de ajustes con JSON inválido ya no se sobrescribe.** Si `buffer-mode-settings.json`
  no se podía leer (por ejemplo tras editarlo a mano y dejarse una coma), el plugin cargaba los
  valores por defecto y los escribía encima al cerrar OBS. Ahora se aparta como
  `buffer-mode-settings.json.<yyyyMMdd-HHmmss>.invalid` (se copia si no se puede renombrar) con un
  WARN que da las dos rutas; si no funciona ninguna de las dos cosas, el archivo no se toca al salir.
- **Corregido el tooltip del dock para la calidad mínima.** Decía que el vídeo retrasado "nunca
  baja de" esa resolución, lo que dejó de ser cierto con el límite de salida. Las propiedades del
  filtro ya no tienen ningún campo de calidad: solo muestran la nota de que el filtro lo gestiona el
  dock.

**Pipeline del buffer del delay (detalle técnico):**
- **Límite a la resolución de salida.** Hasta la v0.5.0 el buffer de vídeo se guardaba a la
  resolución del lienzo, así que con un lienzo 1440p emitido a 1080p el plugin comprimía y
  descomprimía en MJPEG cada frame del buffer a 1440p dentro del hilo gráfico de OBS: 14,3/17,4 ms
  p50/p95 por frame en un benchmark del mismo pipeline fuera de OBS, la mayor parte de los 16,7 ms
  que tiene un frame a 60 fps. Esos píxeles de más los descartaba de todos modos el propio reescalado
  de salida de OBS. `EnsureRingSized()` y la estimación del dock pasan ambos por la nueva función pura
  de dimensionado `bufferfit::Compute()` (`src/buffer-fit.{hpp,cpp}`, sin dependencia de
  libobs/FFmpeg) con la geometría del lienzo (`obs_video_info::base_*`) y de la salida
  (`output_*`). El techo es una escala uniforme, `min(1, max(outW/baseW, outH/baseH))`, que
  conserva la relación de aspecto y nunca baja de la salida en ningún eje; solo se aplica cuando el
  objetivo del buffer tiene el tamaño del lienzo (la escena en directo). Los anchos del buffer son
  múltiplos de 8 y las alturas pares, así que cada fila de cada plano I420 ocupa un número entero de
  palabras de 4 bytes (OpenGL rellena las filas de la lectura a 4 bytes en Linux y macOS). Ver
  `docs/SPEC-es.md` §3.5.
- **Hilos de códec** (`src/codec-pipeline.{hpp,cpp}`, C++ puro con el códec inyectado). El hilo
  gráfico ahora solo renderiza la captura, la convierte, la lee de vuelta (tres slots de staging,
  mapeados dos capturas después para que nunca se bloquee) y sube el frame que toca, solo cuando
  cambia. Uno o dos hilos de compresión (`trigglow-enc-N`, cada uno con su propio contexto MJPEG)
  toman los frames de una cola acotada; un hilo de descompresión (`trigglow-dec`) descomprime hasta
  3 frames por delante de lo que hay en pantalla, salta directamente al frame que toca cuando se
  queda atrás y empieza por los frames más antiguos durante el último cuarto de segundo de la ventana
  de llenado, para que el primer frame retrasado esté listo cuando toca. Un hilo liberador
  (`trigglow-reap`) libera rings, pools de frames y bloques de la arena: el hilo gráfico nunca libera
  cientos de MB ni espera a un hilo. Cuando la compresión se queda atrás, se descarta la captura más
  nueva (la vista retrasada repite un frame); la cola nunca crece. Medido fuera de OBS a 1440p60
  (Ryzen 7 7700X, FFmpeg n9.0.2 vendorizado): hilo gráfico ~1,2–1,7/1,6–2,1 ms p50/p95 por frame,
  compresión ~8,8/10,4 ms y descompresión ~6,2/7,5 ms en sus propios hilos (ocupados un 53% y un 37%
  aproximadamente, menos de un núcleo en total). Ver `docs/SPEC-es.md` §4.6.
- **Conversión de color a I420 en la GPU.** El shader escribe Y, U y V como tres planos separados
  (BT.601, rango de estudio), así que la CPU ya no desentrelaza NV12 en ningún sentido: unos
  1,1–1,3 ms menos de CPU por frame a 1440p. Los frames MJPEG se etiquetan ahora con el rango de
  estudio que contienen (`AVCOL_RANGE_MPEG`; solo metadatos, los planos decodificados son
  idénticos).
- **Ring con máquina de estados por slot y arena por bloques.** El ring (`src/frame-ring.*`) es un
  conjunto fijo de cabeceras de slot, cada una Free, Reserved, Encoding, Ready o Dropped, con su hora
  de captura asignada al reservar el slot, así que las marcas de tiempo siguen en orden termine en el
  orden que termine la compresión. Los datos viven en una arena por bloques (`src/frame-arena.*`):
  paquetes uno detrás de otro en bloques de 4–32 MiB, nunca rellenados con ceros; un bloque se
  recicla entero cuando el ring ha sobrescrito todos los frames que tenía, y solo los bloques vacíos
  por encima de dos de reserva vuelven al sistema, en el hilo liberador. La memoria comprometida y la
  residente siguen lo que de verdad pesan los frames: ~576–768 MB para 30 s a 1440p60 en la
  simulación fuera de OBS, frente a ~1,9 GB residentes y un parón de ~381 ms (unos 23 frames
  perdidos) en el primer frame tras Enable cuando cada slot se rellenaba con ceros de entrada. La
  arena nunca crece por encima del presupuesto de RAM: una racha larga de frames que comprimen mucho
  peor de lo previsto descarta frames (un frame repetido en pantalla) en vez de hacer crecer la
  memoria.
- **Desactivar libera todo lo que usaba el delay.** El ring y el pool de frames van al hilo
  liberador, cada hilo cierra su códec y se liberan los objetos de GPU (~100 MB de RAM medidos y
  ~37 MB de VRAM estimados a 1440p mientras el delay está desactivado); reabrirlos en el siguiente
  Enable cuesta del orden de un milisegundo.
- **Tablas de Huffman estándar de JPEG** (`huffman=default`) en vez de las óptimas por frame de
  FFmpeg, que cuestan una segunda pasada sobre cada frame: un 10–17% menos de tiempo de compresión
  a 1440p a cambio de frames un 3–3,5% más grandes; la imagen decodificada es idéntica (la
  codificación Huffman no pierde información). La línea de log `MJPEG encoder open` dice
  `huffman=default` (o por qué no se pudo fijar).
- **Cada frame retrasado se descomprime y se sube una sola vez, haya las vistas que haya**
  (Programa, vista previa, proyectores, multiview). Si el frame que toca no está listo (descartado,
  o no descomprimido a tiempo), se repite el último en vez de dibujar negro: un tirón de un frame que
  nunca alarga el delay. Justo después de una pausa del render, los frames descomprimidos por
  adelantado antes de la pausa se descartan en vez de mostrarse tarde.
- **Intervalo de frame exacto con frame rates fraccionarios.** A 59,94 fps el intervalo de captura
  era `1e9 / 59` ns, así que se perdía casi un frame real por segundo. Ahora es
  `1e9 × fps_den / fps_num` (16.683.333 ns a 59,94), y se usa para la captura, el tramo del ring y su
  dimensionado.
- **Modo síncrono** (el interruptor `codec_workers`, abajo): el códec se ejecuta en el hilo gráfico
  de OBS como en la v0.5.0, pero con el compresor por slices (`thread_type = FF_THREAD_SLICE`,
  `hardware_concurrency() / 4` hilos, de 1 a 4): a 1080p pasó de 8,6/10,4 a 5,2/5,9 ms p50/p95 en
  ese hilo. `FF_THREAD_SLICE` tiene que fijarse de forma explícita: con solo `thread_count > 1`
  FFmpeg elige threading por frames para MJPEG, `receive_packet()` devuelve `EAGAIN` y todos los
  frames se guardarían en crudo; si aun así arranca con threading por frames, el compresor se reabre
  con un solo hilo y un WARN.

**Degradación automática (detalle técnico, `src/degradation-policy.*`):**
- Se evalúa cada 250 ms sobre los últimos 2 s, y como pronto 2 s después del cambio anterior. Mira
  el trabajo propio del plugin en el hilo gráfico (conversión, lectura y copia, subida; no el render
  de la escena, que OBS hace de todos modos), el tiempo de compresión y de descompresión por frame,
  las capturas descartadas y los frames que el descompresor no tuvo listos a tiempo.
- **Bajada**, de un paso en un paso, cuando: el p95 del hilo gráfico supera el 40% del intervalo de
  frame (o el 25% mientras OBS pierde frames por retraso de render, o su p50 de los últimos 0,5 s
  supera el 50%); el p95 de compresión supera el 80% de lo que tienen los contextos de compresión
  por frame, o se descartan más del 2% de las capturas; el p95 de descompresión supera el 80% del
  intervalo de captura, o más del 5% de los frames que tocan no se descomprimen a tiempo.
- **La escalera.** La carga de compresión añade paralelismo primero (contextos de compresión ×
  hilos de slice: 1×1, 2×1, 2×2, 2×4) y después baja de peldaño; la carga de descompresión o del
  hilo gráfico baja de peldaño directamente (el descompresor MJPEG no tiene threading, y lo que queda
  en el hilo gráfico escala con los píxeles). Los peldaños: el tamaño completo del buffer, después
  pasos de ×0,83 hasta tu calidad mínima (nunca por debajo), y después ese suelo capturado a la mitad
  de fps (solo cuando la salida va a 48 fps o más). Todos los peldaños conservan el mismo ring y el
  mismo número de slots: el delay nunca se mueve y el buffer nunca se vacía.
- **Subida**, de un paso, solo cuando la carga prevista un peldaño más arriba se mantiene por debajo
  del objetivo (hilo gráfico ≤ 25% del intervalo de frame, hilos de códec ≤ 50%) durante 30 s
  seguidos; primero vuelve la calidad y después se devuelven los hilos. Las métricas de
  descompresión solo cuentan cuando el descompresor trabaja ya con frames capturados en el peldaño
  actual (va `delay` segundos por detrás de la captura).
- El punto de partida se predice según la resolución: 1440p60 empieza con un contexto de compresión
  y un slice, 4K60 con dos contextos.
- Cada cambio de peldaño es un WARN en el log con su causa y las métricas de los últimos 2 s,
  terminado en "The delay itself is unchanged."; los cambios de hilos son líneas INFO. El dock lee
  el estado una vez por segundo con el proc `get_pipeline_state` del filtro, que nunca espera al
  hilo gráfico.

**Seguridad y privacidad:**
- **Un único camino de red endurecido.** Todas las llamadas pasan por WinHTTP (`src/win-http.*`):
  HTTPS con TLS 1.2 o superior, a api.trigglow.com, el único host al que se envía el token de inicio
  de sesión, más la comprobación de actualizaciones en api.github.com (sin el token). Las
  redirecciones nunca se siguen (una redirección es un error, así que el token no puede rebotar a
  otro host), sin cookies ni inicio de sesión automático de Windows, con timeouts en cada paso y un
  límite total, un tope al tamaño de la respuesta y un User-Agent con la versión real del plugin y
  el sistema. Nada que cree algo (una conversación, una pregunta, un ticket, un adjunto) se
  reintenta solo.
- **El plugin solo abre sus propias páginas fijas**: las de inicio de sesión, soporte, privacidad y
  la del plugin en www.trigglow.com y este repositorio en GitHub, comprobadas por prefijo, leyendo los
  puntos y barras codificados como lo haría un navegador (sin salida por `..` ni `%2e%2e`). Cualquier
  otra cosa, por ejemplo un enlace dentro de una respuesta, no se abre.
- **Menos datos personales en el log de OBS.** El enlace, los códigos y el secreto del inicio de
  sesión, los tokens, los IDs de tickets y de conversaciones, y el texto de las preguntas y las
  respuestas nunca se escriben en él, y las líneas que mencionan un fichero lo nombran sin la ruta
  completa, que incluía tu nombre de usuario de Windows.
- **El aviso de actualización solo abre las páginas de releases de este repositorio**: cualquier
  otro enlace en la respuesta de GitHub se sustituye por la página de la última release. La
  comprobación de actualizaciones usa el mismo cliente endurecido.
- **Sin rutas de la máquina de compilación en la DLL**: la DLL de Windows solo registra el nombre de
  su PDB, y el binario no lleva cadenas con forma de ruta o de host que no necesite.
- **OBS se cierra limpio con una llamada en curso.** Cada trabajo de red (inicio de sesión, Trigi,
  tickets, el log de un ticket, la comprobación de actualizaciones) va como un trabajo en segundo
  plano cancelable (`src/background-tasks.*`, `src/cancel-token.*`, `src/ui-tasks.*`), y WinHTTP
  funciona en modo asíncrono, así que una llamada se corta al momento. Descargar el plugin cancela
  todos los trabajos y espera como mucho 2 s; en Windows ningún código del plugin se ejecuta después
  de que OBS lo descargue, y ningún resultado llega a un diálogo cerrado. Un timeout se indica como
  "Trigi ha tardado demasiado" en vez de "sin conexión".

**Añadido (soporte y diagnóstico):**
- **Línea `perf check` en el log de OBS**, cada 10 s con el delay en marcha (Filling y Active), y
  una última al desactivar el delay, ponerlo a 0 s o cerrar OBS. Indica la resolución del buffer y
  si está limitada (`capped to OBS output …`, `not capped …`, `cap off …`), p50/p95/max de la
  llamada de render completa, de la parte propia del plugin (`own`), de la lectura y de la subida,
  llamadas/capturas/subidas por segundo, frames repetidos (`holds`, separados en descompresión
  tardía, descartados y aún comprimiéndose) y los contadores propios de OBS de frames
  renderizados/perdidos/saltados en la misma ventana. Arrays fijos de 4096 muestras por fase, sin
  asignaciones por frame.
- **Línea `pipeline check`** justo después: modo (`workers` o `sync`), peldaño de degradación,
  tamaño y fps de captura, contextos de compresión × hilos de slice, profundidad de descompresión
  anticipada y de staging, p50/p95/max de compresión y descompresión con lo ocupado que estuvo cada
  hilo, descartes (reserva, contrapresión, arena, captura), fallbacks a crudo, fallos de códec,
  compresiones obsoletas, saltos del descompresor, frames descomprimidos obsoletos, profundidad de la
  cola de compresión, frames descomprimidos por adelantado, ratio de compresión, memoria de la arena
  (comprometida, viva, pico, bloques, límite) y estados de los slots. Cada ciclo empieza además con
  una línea `delay pipeline: …` con la configuración elegida y la escalera de degradación. Guía campo
  a campo en `docs/TROUBLESHOOTING-es.md`.
- **Aviso cuando una salida codifica por encima del buffer.** Si una salida activa tiene un encoder
  de vídeo del lienzo principal por encima de la resolución del buffer (por ejemplo una grabación
  reescalada a la resolución del lienzo con Reescalar salida de la salida avanzada, o pistas de
  Enhanced Broadcasting por encima de la salida), el log avisa una vez por encoder, tamaño y ciclo de
  delay de que su vídeo diferido se reescala hacia arriba, con ambas resoluciones, el tamaño que
  tendría el buffer sin el límite y cómo quitarlo, y añade que quitarlo cuesta CPU y RAM y puede
  acortar el delay si falta RAM (con la cifra cuando el buffer sin límite no guardaría el delay
  completo). Se comprueba al activar, al llenarse el buffer y al empezar el streaming, la grabación
  o el Replay Buffer; solo cuando quitar el límite cambiaría de verdad el buffer. Los encoders de
  otros lienzos con nombre (p. ej. un lienzo vertical) se reconocen y se ignoran. Los encoders
  reescalados en GPU, que libobs pasa a una mezcla que no expone, se atribuyen al lienzo principal
  cuando tienen su relación de aspecto (o la de la salida), y el WARN lo indica; un segundo lienzo
  con la misma forma podría tomarse por el principal, y un encoder del lienzo principal reescalado a
  otra relación de aspecto no se detecta. Decisión en `src/encoder-check.{hpp,cpp}`, detalles en
  `docs/SPEC-es.md` §3.5.
- **Ajuste oculto `buffer_output_cap`, para soporte.** `"buffer_output_cap": false` en el
  `buffer-mode-settings.json` del plugin recupera la forma de dimensionar de la v0.5.0 (resolución
  de la fuente/lienzo, reducida solo por RAM) para quien graba a resolución de lienzo y prefiere esa
  calidad a costa de CPU y RAM. El log lo confirma con `output_cap=off`.
- **Interruptor oculto `codec_workers`, para soporte.** `"codec_workers": false` en el mismo
  archivo vuelve a ejecutar la compresión y la descompresión en el hilo gráfico de OBS, sin hilos de
  códec (el hilo liberador sigue liberando la memoria fuera de ese hilo, y la degradación sigue
  activa, añadiendo primero hilos de slice). Sirve para descartar o confirmar que un problema viene
  de los hilos de códec; devuelve el coste al hilo gráfico. El log lo confirma con
  `codec_workers=off` en las líneas `settings loaded` y `enabled`, y `mode=sync` en
  `pipeline check`.
- Ninguno de los dos ajustes ocultos tiene control en el dock a propósito; solo se leen al arrancar
  OBS y hay que editarlos con OBS cerrado (OBS reescribe el archivo al salir). Solo un `false`
  literal de JSON desactiva uno: cualquier otro valor (`"false"` entre comillas, `0`...) lo deja
  activado y escribe un WARN. Pasos en `docs/TROUBLESHOOTING-es.md`.

**Empaquetado / Build / CI:**
- Nuevo `data/fonts/` (subconjuntos de Archivo SemiBold/Bold y Geist Mono Medium, SIL OFL 1.1 con
  sus `OFL-*.txt`) y `data/licenses/` (iconos de Lucide, ISC), ambos en el instalador, que ahora no
  se compila sin ellos. Si faltan en una instalación, el plugin usa la fuente de la interfaz de OBS
  y lo indica una vez en el log. El plugin enlaza ahora `Qt6::Svg`.
- **FFmpeg de Windows fijado a otra versión**: el build de fin de mes de BtbN
  `autobuild-2026-09-30-13-08` (`ffmpeg-n9.0.2-17-g2a571b6068`, misma rama 9.0 y mismas DLL:
  avcodec-63, avutil-61, swresample-7). La versión fijada anterior, `autobuild-2026-10-03-18-14`
  (`n9.0.2-22`), se borró en origen y devolvía 404, lo que rompía el build de Windows al configurar.
  BtbN borra sus autobuilds diarios a las dos semanas más o menos pero conserva los de fin de mes,
  así que a partir de ahora solo se fijan builds de fin de mes (`buildspec.json`,
  `cmake/windows/buildspec.cmake`).
- **CI compila y ejecuta los tests unitarios** en Ubuntu, Windows y macOS: nuevo job `unit-tests` en
  `.github/workflows/build-project.yaml`, con `tests/CMakeLists.txt`, que se configura por separado
  sin descargar OBS, Qt ni FFmpeg (`cmake -S tests -B build_tests`) y también sirve para la ruta
  `-DTRIGGLOW_BUILD_TESTS=ON` del proyecto principal. Se ejecuta en PRs y pushes, y bloquea el job de
  release.
- **Job de ThreadSanitizer** (`unit-tests-tsan`, Ubuntu, GCC): compila `frame_arena_tests`,
  `frame_ring_tests` y `pipeline_stress_tests` con `-fsanitize=thread` y los ejecuta. Por ahora solo
  informa (anotación de aviso y resumen del job) en vez de bloquear la release, porque TSAN puede
  fallar sin que haya una carrera en este código (libstdc++ sin instrumentar, su lentitud dispara las
  comprobaciones de tiempo del test de estrés); el paso de compilación sí bloquea.
- **`TRIGGLOW_STRESS_CI=1`**, puesto en los dos jobs de tests de CI, relaja solo los límites de
  tiempo de `pipeline_stress_tests` (50% de los frames que tocan mostrados exactos en vez de 90%,
  100 ms de retraso tras una pausa del render, al menos 10 frames mostrados detrás de un descompresor
  lento), que los runners compartidos y ThreadSanitizer no siempre cumplen. Sus comprobaciones de
  corrección siguen siendo estrictas en todas partes.
- Versión 0.6.0 en `buildspec.json` y en el script del instalador de Windows.
- **Windows enlaza `winhttp`, `crypt32` (el inicio de sesión cifrado) y `dxgi` (el ID del fabricante
  de la GPU de los datos técnicos opcionales).** `TRIGGLOW_ALLOW_API_HOST_OVERRIDE` (desactivada por
  defecto) es solo para builds de prueba en local: la configuración falla si está activada en CI,
  así que ninguna release puede apuntar a otro sitio que no sea api.trigglow.com.
- **La DLL de Windows solo registra el nombre de su PDB** (`/PDBALTPATH`), no la ruta absoluta de
  la máquina de compilación (`cmake/windows/helpers.cmake`).

**Tests** (19 suites, sin OBS; `ctest`):
- `buffer_fit_tests` (dimensionado: lienzo = salida, 1440p → 1080p, 4K → 1080p, salida mayor que
  el lienzo, relaciones de aspecto distintas, un suelo por encima de la salida, reducción por RAM,
  redondeo, anchos múltiplos de 8, dimensiones impares, entradas degeneradas, 59,94 fps, el delay
  efectivo en segundos enteros, los suelos 1440p y 2160p, barridos de presupuesto/salida), `encoder_check_tests` (la decisión
  del aviso de encoders), `ui_tokens_tests` (contraste en las paletas de OBS 31),
  `auto_names_tests` (los nombres y su migración), `overlay_layout_tests` (colocación del overlay),
  `frame_arena_tests`, `frame_ring_tests` (máquina de estados de los slots, generaciones, selección
  por antigüedad), `degradation_policy_tests` (escalera, cada disparador, enfriamiento, histéresis
  de 30 s, sin oscilaciones; los suelos 1440p y 2160p), `pipeline_stress_tests` (ring + arena +
  pipeline de códec con desconexiones, redimensionados y cambios de peldaño y de fps al azar: el
  delay nunca encoge, los frames nunca salen rotos ni desordenados y ningún ring se libera nunca en
  el hilo gráfico simulado) y `background_tasks_tests` (cancelación, resultados que nunca llegan a
  un dueño destruido, el cierre acotado).
- Nuevas para la cuenta, Trigi y los tickets, todas con un sustituto de red guionizado que nunca
  sale a la red: `json_tests` (JSON estricto y acotado), `log_redact_tests` (cada regla de redacción,
  rutas de los tres sistemas, una línea hostil de 2 MB y un log de 5 MB), `chat_text_tests`
  (etiquetas script e img, enlaces markdown, vallas de código, 10.000 caracteres, emojis unidos,
  caracteres de dirección bidi), `support_diagnostics_tests` (solo las claves, tipos y valores
  documentados, en menos de 1 KB), `api_client_tests`, `token_store_tests` (el cifrado de ida y
  vuelta en Windows), `auth_client_tests` (inicio de sesión, renovación, cierre de sesión, el cambio
  desde la v0.5.x y cada regla de 401/403), `support_client_tests` (cada forma de respuesta y cada
  error, cuerpos y cabeceras de las peticiones) y `trigi_states_tests` (el estado, el botón y la
  cuenta atrás a los que lleva cada respuesta o error).

**Limitaciones conocidas:**
- **Cambiar a mano el Programa a la escena en directo con el delay Activo** (en vez de la escena
  auxiliar del plugin, "Trigglow Delay Buffer (auto)") emite el vídeo en directo sin delay: el filtro
  de delay está en la escena auxiliar. Ya pasaba antes de la 0.6.0. Deja el Programa en la escena que
  eligió el plugin mientras el delay está en marcha, y desactiva antes el delay desde el dock.
- **Borrar a mano un filtro de delay "(auto)" con el delay en marcha** deja salir el vídeo en
  directo sin delay (la escena auxiliar muestra entonces la escena en directo sin filtro) hasta que
  desactivas y vuelves a activar el delay desde el dock, que lo vuelve a crear. No borres los filtros
  del plugin; desactiva el delay desde el dock.
- Las capturas se deduplican por una celda de reloj por intervalo de frame, no por la hora de frame
  de OBS, así que con mucho jitter de render se puede perder de vez en cuando un frame capturado (un
  frame repetido, nunca un desplazamiento del delay). Previsto para lo siguiente.
- Mientras el buffer se llena, mantener la escena en directo renderizando sigue usando un render
  target del tamaño del lienzo cuyo contenido nunca se usa (solo GPU). Previsto para lo siguiente.
- Con un lienzo mayor que la salida, el frame retrasado se vuelve a dibujar a tamaño de lienzo y OBS
  lo reduce otra vez, lo que puede verse algo más blando que la imagen en directo. Si no necesitas el
  lienzo grande, poner lienzo = salida lo evita.
- macOS sigue sin build de FFmpeg: allí el buffer se guarda sin comprimir (I420 en crudo).
- **macOS y Linux todavía no tienen cliente HTTP en el plugin**, así que allí no hay inicio de
  sesión (y sin él no funciona Activar delay), ni Trigi, ni reportes desde OBS; el diálogo de
  reporte enlaza al formulario web. Previsto para la v0.6.1.
- El log redactado puede seguir conteniendo nombres de escenas, fuentes, dispositivos de audio y
  ventanas: no se pueden distinguir de forma fiable, y la vista previa lo avisa. Revísalo con **Ver
  qué se envía** antes de marcar la casilla.
- La conversación con Trigi en OBS no se conserva al cerrar el diálogo. En trigglow.com se guarda
  hasta 12 meses y puedes borrarla antes (Ajustes → Privacidad y datos → Conversaciones con Trigi);
  lo que se copió a un ticket se queda en ese ticket.

## v0.5.0 — 2026-10-04 (Early Access)

**Añadido:**
- **Overlay "Delay Ns".** Mientras el delay está Activo, se dibuja sobre la salida retrasada una
  pequeña insignia con el logo de Dynamic Delay y el delay real ("Delay 30s"); desaparece en cuanto
  desactivas el delay. El dock tiene un checkbox **"Mostrar overlay"** y un **selector de esquina**
  (arriba izquierda, arriba derecha, abajo izquierda, abajo derecha; arriba izquierda por defecto)
  para que nunca tape tu escena. Es una fuente normal de OBS dentro de la escena envoltorio del
  plugin (`data/images/delay-badge.png` + una fuente de texto), al 40% de tamaño. Si el presupuesto
  de RAM acorta el delay de vídeo, el overlay muestra el delay realmente aplicado, no el pedido.

**Cambiado:**
- **Un solo botón Activar/Desactivar.** El dock muestra solo el botón que tiene sentido:
  **Activar** mientras está inactivo, **Desactivar** desde que empieza a llenar hasta que se apaga.
- **Dock responsivo.** El panel va dentro de un área con scroll, así que reducirlo ya no aplasta
  los controles unos sobre otros; las filas en paralelo (cuenta, delay/calidad, botones) se apilan
  en columnas cuando el dock es estrecho. Corregido el texto cortado en los campos de delay/calidad
  y ajustados márgenes y paddings.

**Corregido:**
- **El delay de vídeo se desviaba del tiempo real (medido en un log de producción).** El ring de
  vídeo escribía un slot por llamada a `Render()` y asumía que eran exactamente `fps` por segundo;
  en vivo iba a 48-90 slots/s a 60 fps, así que un delay de vídeo de "30s" eran en realidad
  ~20-38s mientras el audio iba a 30s exactos. Ahora se captura como máximo una vez por celda de
  tiempo de 1/fps y se reproduce por hora de captura, así que el vídeo mantiene el delay
  configurado y queda sincronizado con el audio.
- **El delay de audio también sigue el reloj real.** Las fuentes que dejan de enviar audio cuando
  están en silencio (captura loopback/de aplicación, medios en pausa, navegadores) ya no alargan el
  delay de audio: los huecos se rellenan con silencio y una fuente que retrocede o se reinicia
  reinicia su historial. El audio viejo se descarta en cada Activar, la primera salida tras el
  calentamiento entra con fundido (sin clic), los planos de audio ausentes escriben silencio y el
  ajuste de delay es seguro entre hilos.
- **La escena en directo elegida se ignoraba al reutilizar la escena envoltorio.** Tras cambiar la
  escena en directo (p. ej. de "Talk" a "In Game") el envoltorio seguía mostrando la anterior, así
  que el filtro nunca almacenaba lo que había en pantalla y la salida retrasada parecía congelada.
  Ahora el envoltorio contiene siempre exactamente la escena en directo seleccionada.
- **Delay en marcha tras cerrar OBS estando Activo.** Los filtros se guardan encendidos en la
  colección de escenas; ahora se apagan al terminar de cargar OBS si el plugin está Inactivo.
- **El instalador de Windows nunca incluía `es-ES.ini`**, así que OBS en español mostraba el plugin
  en inglés. Ahora incluye ambos idiomas, más la imagen del overlay.

## v0.4.0 — 2026-09-07 (Early Access)

**Añadido:**
- **Botón "Reportar un problema" en el dock.** Abre un pequeño diálogo nativo (nombre, email, una
  breve descripción) que crea un ticket de soporte real en trigglow.com (categoría
  `dynamic_delay`) y le adjunta automáticamente el log actual de OBS — el usuario no tiene que
  buscar el archivo de log por su cuenta. Construido sobre el cliente WinHTTP ya existente
  (`src/win-http.cpp`), ampliado con un nuevo `HttpsPostMultipartFile()` para la subida del
  adjunto; ficheros nuevos `src/bug-report.{hpp,cpp}` (localiza/copia el log actual de OBS, solo
  Windows por ahora) y `src/report-bug-dialog.{hpp,cpp}` (el diálogo en sí). Nuevo uso de red
  saliente — ver la nota de divulgación en `docs/OBS_SUBMISSION_CHECKLIST.md`.
- **Chequeo de actualizaciones dentro del plugin.** Al cargar, el plugin compara la última
  release publicada de este repositorio en GitHub contra su propia versión (un GET público sin
  autenticación, sin necesidad de un endpoint nuevo en el backend de trigglow.com) y muestra un
  pequeño aviso en la parte superior del dock si existe una versión más nueva, con enlace directo
  a la página de la release. Silencioso ante cualquier fallo o cuando ya está actualizado — nunca
  se muestra como un error. Fichero nuevo `src/update-checker.{hpp,cpp}`. Nuevo uso de red
  saliente — ver `docs/OBS_SUBMISSION_CHECKLIST.md`.
- **Localización real.** Todo el texto visible (el dock, el diálogo de reportar un problema, el
  nombre de ambos filtros en el propio diálogo de Filtros de OBS, las 3 descripciones de hotkeys,
  y los mensajes de estado del buffer) pasa ahora por el sistema de idiomas propio de OBS
  (`obs_module_text()`, vía un nuevo helper `Str()` en `src/i18n.hpp`) en vez de ser un literal
  en español fijo en el código C++. `data/locale/en-US.ini` y `es-ES.ini` pasaron de tener una
  sola clave `PluginName` cada uno (lo único que realmente se usaba antes de este cambio) a
  contener la traducción completa real del plugin. Antes, un usuario con OBS en inglés (o
  cualquier idioma que no fuera español) veía igualmente el plugin 100% en español.

**Cambiado:**
- **Rediseño del dock**: los controles ahora se agrupan en tarjetas (estado / cuenta /
  configuración) en vez de una columna larga de filas sueltas, con una paleta de colores
  semántica consistente (verde/ámbar/rojo/azul para éxito/aviso/error/acento, independiente del
  tema de OBS del usuario — todo lo demás sigue usando `palette(...)` para que el dock encaje con
  el tema activo) y Enable/Disable coloreados para que se entiendan de un vistazo. Delay y calidad
  mínima ahora van uno junto al otro en vez de apilados. Los tamaños de fuente pasaron de píxeles
  fijos a puntos, para que el dock escale bien con el escalado de pantalla de Windows.

**Corregido / Rendimiento:**
- **Consumo real de RAM a 30s/1080p medido en vivo por primera vez** (línea de log
  `compression check`, `src/video-delay-filter.cpp`) en vez de asumido: una sesión completa
  mantuvo un ratio de compresión MJPEG real de ~11-15x sobre gameplay real, muy por encima del 3x
  que se asumía de forma conservadora desde v0.3.0. Se retuneó `kAssumedCompressionRatio` de 3.0 a
  5.0 con esos datos reales — la memoria propia del ring bajó de ~1.78GB a un estimado de ~1.0GB
  con ese ajuste, y la memoria total del proceso de OBS en las pruebas bajó de ~2.8GB a ~2.1GB. Un
  nuevo contador de crecimiento por slot (`encodeGrowthCount_`) se registra junto al ratio para
  que un futuro reajuste tenga datos reales de frecuencia de crecimiento, no otra suposición.
- Se eliminó una asignación de memoria real por cada frame: `EncodeScratchNv12Into()` hacía
  `av_frame_unref()` + `av_frame_get_buffer()` (un malloc+free completo del buffer de píxeles del
  frame de codificación) en cada frame codificado; ahora el buffer se reserva una sola vez al
  abrir el contexto del códec y se reutiliza vía `av_frame_make_writable()` en cada tick. No
  cambia la memoria propia del ring (independiente del reajuste anterior), pero elimina la
  fragmentación/churn de heap en el camino de codificación a lo largo de una sesión larga.

## v0.3.3 — 2026-09-02 (Early Access)

**Añadido:**
- Enable (botón del dock, hotkey y Stream Deck) ahora requiere haber iniciado sesión una vez en
  una cuenta gratuita de trigglow.com — ver `docs/ACCOUNT_GATE.md`. El plugin sigue siendo 100%
  gratuito; esto es una puerta de descubrimiento, no un muro de pago. El login ocurre
  completamente en el navegador del sistema del usuario (el mismo login que ya usa la web),
  nunca dentro de OBS — el plugin solo guarda un token de sesión opaco después. Nuevo uso de red
  saliente: antes ninguno, ahora un puñado de llamadas HTTPS a trigglow.com alrededor del inicio
  de sesión (ver la nota de divulgación en `docs/OBS_SUBMISSION_CHECKLIST.md`), hechas vía un
  cliente WinHTTP nativo (`src/win-http.cpp`) en vez de la red de Qt — ver el comentario de ese
  fichero para el porqué (el backend TLS de Qt está ligado exactamente a la build de Qt que OBS ya
  tiene cargada, que no coincide con la que usa este plugin para compilar).

**Cambiado:**
- Nuevo icono dedicado para la marca del instalador de Windows (antes reutilizaba el logo general
  de Trigglow) — ver `installers/windows/branding/generate-assets.py`.

**Problema conocido:**
- El instalador de Windows ha sido marcado directamente por Microsoft Defender
  (`Trojan:Win32/Wacatac.B!ml`) como amenaza detectada, confirmado en el propio asset de la
  release oficial v0.3.2, no solo en builds no oficiales — ver
  [#10](https://github.com/VirosMs/obs-trigglow-dynamic-delay/issues/10). Casi con toda
  seguridad un falso positivo (binario nuevo, sin reputación). CI ahora firma el instalador con un
  certificado **autofirmado** como solución provisional (prueba que el archivo no fue manipulado
  después de que la propia build del proyecto lo produjera) — esto **no** elimina el aviso de
  SmartScreen ni detiene el falso positivo de Defender, ambos necesitan una identidad validada por
  una CA; una solicitud a SignPath Foundation (firma real de CA gratuita para proyectos de código
  abierto que califiquen) está en curso como solución definitiva.

## v0.3.2 — 2026-08-27 (Early Access)

**Arreglado:**
- El audio podía notarse claramente por detrás del vídeo en delays largos
  y/o de alta calidad (30s+, 1080p) — reportado en directo como "el audio
  sale después de la acción del vídeo". Causa raíz: cuando el presupuesto
  de RAM no da para el delay completo pedido a la calidad elegida,
  `VideoDelayFilter` acorta en silencio su propia duración real de buffer
  (así ha sido siempre, desde antes incluso de que existiera la
  compresión — es por diseño, la calidad siempre gana a la duración), pero
  `AudioDelayFilter` siempre guarda exactamente los segundos completos
  pedidos (PCM barato, sin presupuesto de RAM aplicado) — así que cada vez
  que el vídeo tenía que acortarse, el audio seguía retrasando más tiempo
  del que el vídeo realmente aplicaba, y la brecha crecía cuanto más largo
  o de más calidad era el pedido. Arreglado haciendo que
  `BufferModeController` le pregunte al filtro de vídeo cuánto puede
  realmente guardar (una nueva consulta `get_effective_delay_seconds`) en
  cuanto el buffer se llena de verdad, y recortando el delay del audio para
  que coincida siempre que el vídeo haya tenido que acortarse. Efecto
  secundario puntual: si esta corrección se dispara, el ring de audio se
  reinicia y queda en silencio brevemente (bastante menos de un segundo)
  justo cuando el buffer pasa a `Active`, mientras se vuelve a llenar hasta
  la duración corregida — un buen cambio a cambio de no quedar
  desincronizado el resto de la sesión.

## v0.3.1 — 2026-08-27 (Early Access)

**Arreglado:**
- Los selectores de escena en directo/de carga del dock aparecían vacíos
  cada vez que se abría OBS por primera vez (o se reinstalaba el plugin),
  aunque los nombres guardados se cargaban correctamente por detrás —
  dando la sensación de que el plugin "olvidaba" la configuración y
  obligando a reelegirla cada vez. Causa raíz: `obs_module_load()` (y por
  tanto la primera vez que el dock rellena esos desplegables) corre antes
  de que OBS termine de cargar la colección de escenas, así que el primer
  intento de listar escenas siempre volvía vacío. El dock ahora también
  vuelve a rellenar ambos desplegables en cuanto OBS termina de cargar,
  reseleccionando lo que estuviera guardado.

**Cambiado:**
- El instalador de Windows ahora usa branding real de Trigglow — icono
  propio del `.exe`/barra de tareas y banner/logo del asistente, generados
  a partir de la misma marca usada en la web de Trigglow — en vez de los
  gráficos genéricos por defecto de Inno Setup.

## v0.3.0 — 2026-08-27 (Early Access)

Compresión real del buffer. El ring buffer ahora codifica cada frame con MJPEG (todo-intra, acorde
a los resets abruptos y las lecturas por índice del ring) antes de guardarlo, y lo decodifica de
vuelta al reproducirlo — reduciendo el uso real de RAM por encima del ya logrado con NV12 en
v0.2.0, sin bajar nunca la calidad mínima que elijas.

**Incluido:**
- FFmpeg (avcodec/avutil) vendorizado y enlazado en Windows + Linux; cada slot del ring se
  codifica en MJPEG al capturarlo y se decodifica al reproducirlo.
- Fallback automático y seguro a NV12 sin comprimir (idéntico a v0.2.0) siempre que el codec no
  pueda abrirse o un frame concreto falle al codificar/decodificar — la compresión nunca es un
  requisito imprescindible para que el buffer funcione.
- Los slots del ring ahora reservan RAM según un tamaño presupuestado (comprimido) en vez del
  techo crudo completo, creciendo un slot concreto solo cuando un frame realmente necesita más
  sitio. Medido en vivo a 30s@1080p: **~2,9GB en total con el buffer activo, frente a los ~6,3GB**
  de antes de esta versión — el contenido real de gameplay comprime algo peor que el ratio
  asumido, así que esto varía según lo que se vea en pantalla.
- macOS todavía no forma parte de esta versión — no hay ningún build estático de FFmpeg de
  confianza equivalente disponible ahí hoy (ver `docs/ROADMAP.md`); el plugin sigue funcionando
  exactamente igual que en v0.2.0 en macOS, sin comprimir.

**Limitaciones conocidas (no son bugs):**
- El ratio de compresión real todavía no se ha medido contra gameplay real prolongado — el único
  número registrado hasta ahora (muy alto) vino de una escena de carga mayormente estática y no es
  representativo. El uso real de RAM en la práctica puede ser mayor o menor que los ~2,9GB medidos
  aquí según el contenido.
- Un slot que en algún momento necesitó más sitio del presupuestado mantiene esa capacidad mayor
  durante el resto de la sesión (por diseño, para evitar una reasignación en cada frame) — el RAM
  solo puede crecer durante una sesión, nunca bajar, hasta que Disable() libera el buffer entero.

**Cambios en el repo:** `main` ahora es una rama protegida (pull request obligatorio, incluso para
el mantenedor) — el desarrollo ocurre en `develop`, integrado vía PR. Ver `README.md` si vas a
contribuir.

## v0.2.0 — 2026-08-26 (MVP / Early Access)

Primera versión que refleja el diseño realmente enviado. Plugin nativo de OBS Studio (C++,
plantilla oficial `obsproject/obs-plugintemplate`) que retrasa el vídeo y el audio de tu stream
juntos, sin tocar nunca el output de streaming — sin reconexión, sin corte, en ningún momento, por
ningún motivo.

Antes se publicó un `0.1.0` (2026-08-23) que describía un delay **basado en reconexión**,
totalmente abandonado durante el desarrollo a favor del modo buffer sin reconexión de más abajo —
ver `docs/SPEC.md` §0/§2 para el por qué. Los assets de esa release `0.1.0` son de antes de esta
reescritura por completo; `0.2.0` es la primera release que refleja de verdad lo que se distribuye.

**Incluido:**
- **Modo buffer** sin reconexión: dos filtros de OBS acumulan vídeo y audio en un anillo de RAM del
  sistema, y el plugin cambia el Programa de OBS entre la escena en directo, una escena de carga
  opcional y una escena wrapper delayed, internamente. El delay nativo del output de streaming
  nunca se toca.
- Enable / Disable / Toggle desde un dock nativo de OBS, con estado visible y cuenta atrás en vivo
  mientras se llena el buffer: `Inactive` / `Filling (Ns restantes)` / `Active` / `Error`.
- Vídeo y audio delayed juntos, en sincronía, usando los mismos segundos configurados.
- Calidad mínima seleccionable (480p/720p/1080p) para el vídeo delayed. Si el delay y la calidad
  pedidos no caben juntos en el presupuesto de RAM detectado en la máquina, el plugin acorta el
  tiempo real de buffer en vez de bajar nunca de la calidad elegida.
- Estimación de ajuste de buffer en vivo, no bloqueante, y presupuesto de RAM detectado, mostrados
  en el dock y actualizados según el usuario ajusta segundos o calidad — antes incluso de pulsar
  Enable.
- Buffer de RAM liberado automáticamente al pulsar Disable.
- 3 hotkeys nativas de OBS (Toggle, Enable, Disable) — utilizables directamente desde Stream Deck
  vía su propia acción "System: Hotkey", sin necesitar un plugin de Stream Deck propio.
- Persistencia de settings por perfil de OBS.
- Manejo de errores sin crashear (sin escena en directo elegida, escena en directo que no resuelve
  a nada, etc. → estado `Error` con un mensaje claro en el dock, nunca un crash).

**Limitaciones conocidas (no son bugs):**
- El ring buffer almacena frames NV12 sin comprimir — una compresión real de vídeo/audio (un
  pipeline de encode/decode basado en FFmpeg) se investigó y se aplazó explícitamente como trabajo
  futuro; ver `docs/SPEC.md` §5 (la compresión de vídeo se entregó en v0.3.0 más abajo — ver
  `docs/SPEC.md` §4 — la de audio sigue aplazada).
- Un diseño anterior con una textura de GPU persistente por frame almacenado produjo un crash real
  de "Device Remove/Reset" al pulsar Disable durante pruebas en directo. La reescritura actual a
  RAM/NV12 usa un pool pequeño y fijo de objetos GPU en su lugar y probablemente reduce ese riesgo,
  pero todavía no se ha vuelto a probar específicamente para confirmar que no reaparece — ver
  `docs/SPEC.md` §7.

**Fuera de alcance en v0.2.0:** compresión real del buffer, presets de delay guardados, elegir una
fuente individual en vez de una escena completa, un plugin de Stream Deck propio de Trigglow,
instaladores firmados, verificación en directo de macOS/Linux (ambos compilan en verde en CI pero
no se han probado en directo). Ver `docs/ROADMAP.md`.
