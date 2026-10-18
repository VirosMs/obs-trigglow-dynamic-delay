*[English version](CHANGELOG.md)*

# Changelog — Trigglow Dynamic Delay for OBS

## v0.5.1 — 2026-10-18 (Early Access)

Hotfix de rendimiento. Con un lienzo mayor que la salida (reportado en directo: lienzo 2560x1440,
salida 1920x1080 a 60 fps), OBS mostraba **"Encoding overloaded" / sobrecarga al codificar** en
cuanto se activaba el delay.

**Corregido / Rendimiento:**
- **Sobrecarga al codificar con el delay activo cuando el lienzo es mayor que la salida.** El
  buffer de vídeo se guardaba a la resolución del lienzo, así que con un lienzo 1440p emitido a
  1080p el plugin comprimía y descomprimía en MJPEG cada frame del buffer a 1440p dentro del hilo
  gráfico de OBS: 14,3/17,4 ms p50/p95 por frame en un benchmark del mismo pipeline fuera de OBS,
  la mayor parte de los 16,7 ms que tiene un frame a 60 fps, y OBS se quedaba atrás. Esos píxeles
  de más los descartaba de todos modos el propio reescalado de salida de OBS. Ahora el buffer nunca
  supera la resolución de salida de OBS (Ajustes → Vídeo → Resolución de salida (escalada)) y,
  junto con la compresión con varios hilos de abajo, el mismo montaje cuesta ~5,2/5,9 ms p50/p95
  (mismo benchmark). Puedes volver a usar un lienzo 2560x1440 con salida 1920x1080.
  `EnsureRingSized()` y la estimación del dock pasan ambos por la nueva función pura de
  dimensionado `bufferfit::Compute()` (`src/buffer-fit.{hpp,cpp}`, sin dependencia
  de libobs/FFmpeg) con la geometría del lienzo (`obs_video_info::base_*`) y de la salida
  (`output_*`). El techo es una escala uniforme, `min(1, max(outW/baseW, outH/baseH))`, que
  conserva la relación de aspecto y nunca baja de la salida en ningún eje; solo se aplica cuando el
  objetivo del buffer tiene el tamaño del lienzo (la escena en directo). Ver `docs/SPEC-es.md`
  §3.5.
- **Compresión MJPEG con varios hilos.** El codificador divide cada frame en slices que se
  comprimen en paralelo, hasta 4 hilos según la CPU (uno por cada 4 CPU lógicas: 4 en una CPU de 8
  núcleos/16 hilos, 1 en una de 4 hilos). A 1080p el coste del plugin en el hilo gráfico de OBS
  baja de 8,6/10,4 a 5,2/5,9 ms p50/p95 por frame. `thread_type = FF_THREAD_SLICE` se fija de forma
  explícita: con solo `thread_count > 1`, FFmpeg elige threading por frames para MJPEG,
  `receive_packet()` devuelve `EAGAIN` en el mismo tick y todos los frames se guardarían en crudo.
  Si aun así FFmpeg arranca con threading por frames, el codificador se reabre con un solo hilo y
  un WARN. Solo la codificación a 1080p: de 5,1 a 1,8 ms p50, el mismo CPU total, frames ~3,5% más
  grandes.
- **Cada frame retrasado se descomprime y sube una sola vez, haya las vistas que haya.** OBS
  renderiza la escena una vez por vista (Programa, vista previa, proyectores, multiview), y cada
  una de esas llamadas volvía a descomprimir y subir a la GPU el mismo frame retrasado. Ahora
  ocurre como mucho una vez por frame; las vistas extra solo lo vuelven a dibujar. Si un frame no
  se puede mostrar (nunca se capturó, o falla al descomprimir), se repite el último en vez de
  dibujar negro: un tirón de un frame que nunca alarga el delay. La caché se indexa por slot +
  hora de captura y se invalida en `ReleaseRing()`/`ReleaseSizedGpuObjects()`, así que un frame
  repetido nunca viene de un ciclo anterior.
- **Intervalo de frame exacto con frame rates fraccionarios.** A 59,94 fps el intervalo de
  captura era `1e9 / 59` ns, así que se perdía casi un frame real por segundo. Ahora es
  `1e9 × fps_den / fps_num` (16.683.333 ns a 59,94), y se usa para la captura, el tramo del ring y
  su dimensionado. El delay efectivo con el que se sincroniza el audio
  (`get_effective_delay_seconds`) también usa aritmética racional: multiplicar por el intervalo
  truncado podía redondear a 29 s un tramo de exactamente 1800 frames a 60 fps (30 s).
- **Sin parón ni pico de RAM al empezar el delay.** Al dimensionar el buffer se rellenaba con
  ceros cada slot en el hilo gráfico de OBS, lo que tocaba toda su memoria de golpe: a 1440p60 eran
  ~1,9 GB residentes de una vez y un parón de ~381 ms (unos 23 frames perdidos) en el primer frame
  tras Enable. Ahora los slots se reservan sin inicializar (`Slot::pixels` es un
  `std::unique_ptr<uint8_t[]>` sin inicializar), así que la memoria solo pasa a ser residente a
  medida que se escriben frames: en la simulación fuera de OBS, 651-896 MB en vez de 1923 MB a
  1440p60, dimensionar cuesta ~5 ms y liberar el buffer al desactivar también es más rápido
  (87 → 30 ms). La memoria reservada (comprometida), y la cifra `ring` de `perf check`, no cambian.
  Nada lee más allá de los bytes que escribió cada frame.
- **Compresión más rápida en CPU con menos de 8 núcleos lógicos.** El codificador MJPEG usa ahora
  las tablas de Huffman estándar de JPEG (`huffman=default`) en vez de las óptimas por frame de
  FFmpeg, que cuestan una segunda pasada sobre cada frame. Con slices FFmpeg ya lo hacía, así que
  cambia el codificador de un hilo: un 10-17% menos de tiempo de codificación a 1440p a cambio de
  frames ~3% más grandes, y la imagen decodificada es idéntica (la codificación Huffman no pierde
  información). La línea de log `MJPEG encoder/decoder open` dice ahora `huffman=default` (o por
  qué no se pudo fijar).
- **Delay que se quedaba puesto tras cerrar OBS estando Activo (el fix de v0.5.0 no funcionaba).**
  Si OBS se cerraba con el delay activo, en el siguiente arranque la escena en directo quedaba con
  delay permanente mientras el dock decía "Inactivo". La v0.5.0 intentaba apagar esos filtros al
  cargar, pero los buscaba con `obs_enum_sources()`, que nunca devuelve filtros, así que no apagaba
  nada. Ahora recorre los filtros (`obs_source_enum_filters`) de inputs, grupos, escenas y fuentes
  de audio globales, y los apaga fuera de los locks de libobs. Limitación conocida: se ejecuta una
  vez, cuando OBS termina de cargar, no al cambiar de colección de escenas sin reiniciar OBS.
- **Corregidos el tooltip del dock y el texto de la propiedad del filtro para la calidad mínima.**
  Decían que el vídeo retrasado "nunca baja de" esa resolución, lo que dejó de ser cierto con el
  techo de salida (ver Cambiado).
- **Se perdían referencias al filtro de delay en cada Enable.** `obs_source_get_filter_by_name()`
  devuelve una referencia nueva, pero el plugin la trataba como prestada y nunca la liberaba
  (filtro de vídeo: varias por Enable; filtros de audio: una por fuente de audio y llamada), así que
  el filtro de vídeo nunca se destruía. Ahora cada búsqueda libera su referencia
  (`OBSSourceAutoRelease`), y el destructor del filtro, que libobs ejecuta sin contexto gráfico,
  entra en uno para liberar sus objetos de GPU.
- **Un archivo de ajustes con JSON inválido ya no se sobrescribe.** Si `buffer-mode-settings.json`
  no se podía leer (por ejemplo tras editarlo a mano y dejarse una coma), el plugin cargaba los
  valores por defecto y los escribía encima al cerrar OBS, y se perdía lo que había. Ahora se aparta
  como `buffer-mode-settings.json.<yyyyMMdd-HHmmss>.invalid` (se copia si no se puede renombrar) con
  un WARN que da las dos rutas; si no funciona ninguna de las dos cosas, el archivo no se toca al
  salir.

**Cambiado:**
- **La "Calidad mínima" es un suelo para la reducción por RAM, acotado por la salida de OBS.**
  Cuando falta RAM sigue decidiendo hasta dónde puede encogerse el buffer (antes que bajar de ahí,
  se guardan menos segundos), pero ya no eleva el buffer por encima de la resolución de salida:
  con una salida de 720p el buffer es 720p aunque el mínimo sea 1080p. Guardar más filas de las que
  OBS emite no aportaba nada.
- **La línea de log `compression check` se sustituye por `perf check`** (abajo), que incluye los
  mismos datos de compresión.

**Añadido:**
- **Línea `perf check` en el log de OBS.** Cada 10 s con el delay activo (Filling y Active), y una
  última al desactivar el delay o cerrar OBS. Indica la resolución del buffer y si está limitada
  (`capped to OBS output …`, `not capped …`, `cap off …`), tiempos p50/p95/max por fase (render
  completo, lectura, codificación, decodificación, subida), llamadas/capturas/subidas por segundo,
  frames repetidos (`holds`), fallbacks a crudo, los contadores propios de OBS de frames
  renderizados/perdidos/saltados en la misma ventana, ratio de compresión y memoria del ring. Es lo
  que lee soporte cuando el delay parece costar demasiado; guía campo a campo en
  `docs/TROUBLESHOOTING-es.md`. Arrays fijos de 4096 muestras por fase, sin asignaciones por frame.
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
  `buffer-mode-settings.json` del plugin recupera la forma de dimensionar de v0.5.0 (resolución de
  la fuente/lienzo, reducida solo por RAM) para quien graba a resolución de lienzo y prefiere esa
  calidad a costa de CPU. Solo un `false` literal de JSON quita el límite: cualquier otro valor
  (`"false"` entre comillas, `0`...) lo deja activado y escribe un WARN. Sin control en el dock a
  propósito; solo se lee al cargar OBS, y el log lo confirma con `output_cap=off`. Pasos en
  `docs/TROUBLESHOOTING-es.md`.
- **Tests unitarios del cálculo de dimensionado del buffer** (`tests/buffer_fit_tests.cpp`,
  desactivados por defecto: configura con `-DTRIGGLOW_BUILD_TESTS=ON`, o por separado con
  `cmake -S tests -B build_tests` sin ninguna dependencia de OBS; ejecuta con `ctest`). Cubren
  lienzo = salida, 1440p → 1080p, 4K → 1080p, salida mayor que el lienzo, relaciones de aspecto
  distintas, un suelo por encima de la salida, reducción por RAM, redondeo, dimensiones impares,
  entradas degeneradas, 59,94 fps y barridos de presupuesto/salida. Un segundo programa de tests,
  `tests/encoder_check_tests.cpp`, cubre la decisión del aviso de encoders: un encoder del lienzo
  con y sin escalado, encoders reescalados en GPU por encima y por debajo del buffer, pistas de
  Enhanced Broadcasting, lienzos verticales (720x1280, 1080x1920) y un buffer al que el límite no
  afecta.

**Build / CI:**
- **FFmpeg de Windows fijado a otra versión**: el build de fin de mes de BtbN
  `autobuild-2026-09-30-13-08` (`ffmpeg-n9.0.2-17-g2a571b6068`, misma rama 9.0 y mismas DLL:
  avcodec-63, avutil-61, swresample-7). La versión fijada anterior, `autobuild-2026-10-03-18-14`
  (`n9.0.2-22`), se borró en origen y devolvía 404, lo que rompía el build de Windows al configurar.
  BtbN borra sus autobuilds diarios a las dos semanas más o menos pero conserva los de fin de mes,
  así que a partir de ahora solo se fijan builds de fin de mes (`buildspec.json`,
  `cmake/windows/buildspec.cmake`).
- **CI compila y ejecuta los tests unitarios** en Ubuntu, Windows y macOS: nuevo job `unit-tests` en
  `.github/workflows/build-project.yaml`, con el nuevo `tests/CMakeLists.txt`, que se configura por
  separado sin descargar OBS, Qt ni FFmpeg (`cmake -S tests -B build_tests`) y también sirve para la
  ruta `-DTRIGGLOW_BUILD_TESTS=ON` del proyecto principal. Se ejecuta en PRs y pushes, y bloquea el
  job de release.
- Versión subida a 0.5.1 en `buildspec.json` y en el script del instalador de Windows.

**Limitaciones conocidas:**
- **Salida 1440p a 60 fps.** Con una *salida* de 2560x1440 (no solo el lienzo) el buffer también es
  1440p, y la compresión sigue ejecutándose dentro del hilo gráfico de OBS: a 30 fps debería
  funcionar, a 60 fps todavía puede sobrecargar OBS. Sacar la compresión del hilo gráfico (objetivo
  1440p60) es el plan para la v0.6.0 — ver `docs/ROADMAP-es.md`.
- Cuando el presupuesto de RAM recorta el ring, el vídeo se retrasa el tramo exacto (fraccionario)
  y el audio en segundos enteros, así que pueden quedar hasta ~1 s separados. Alinearlos también
  está previsto para la v0.6.0.

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
  a partir de la misma marca usada en trigglow.virosms.com — en vez de los
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
