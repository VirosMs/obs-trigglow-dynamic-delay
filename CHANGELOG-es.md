*[English version](CHANGELOG.md)*

# Changelog — Trigglow Dynamic Delay for OBS

## v0.6.0 — 2026-10-18 (Early Access)

El delay deja de competir con OBS por su hilo gráfico, se adapta a lo que da tu equipo sin cortarse
nunca, y el plugin adopta el aspecto de trigglow.com. Se preparó un hotfix de rendimiento v0.5.1
que nunca se publicó: todo lo que llevaba sale aquí.

**Qué cambia para ti:**
- **Se acabó el "Encoding overloaded" (sobrecarga al codificar) al activar el delay con un lienzo
  mayor que la salida.** Reportado en directo con un lienzo 2560x1440 y una salida 1920x1080 a
  60 fps. El buffer del delay se guarda ahora como mucho a la resolución de salida de OBS (Ajustes →
  Vídeo → Resolución de salida (escalada)), así que puedes volver a tu lienzo grande.
- **Salida 1440p a 60 fps soportada.** Comprimir y descomprimir los frames del buffer ya no ocurre
  en el hilo gráfico de OBS (el que tiene que terminar a tiempo cada frame de tu stream) sino en
  hilos propios del plugin. A 1440p60 el trabajo del plugin en ese hilo baja de ~14/17 ms a
  ~1,2–1,7/1,6–2,1 ms por frame (p50/p95, microbenchmark fuera de OBS en una CPU de 8 núcleos/16
  hilos).
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
  teclado visible.
- **Correcciones**, entre otras: los filtros de delay que quedaron activos al cerrar OBS con el delay
  Activo se desactivan al arrancar y al cambiar de colección de escenas; a 59,94 fps ya no se pierde
  un frame por segundo; cambiar los ajustes de vídeo de OBS con el delay en marcha muestra la escena
  de carga en vez de negro; renombrar la escena en directo ya no llena el log ni impide desactivar el
  delay.

**Antes de actualizar:**
- **Los objetos que gestiona el plugin pasan de "(no tocar)" a "(auto)"**, de forma automática.
  **Apunta a los nombres nuevos cualquier acción de Stream Deck, obs-websocket u otro control remoto
  que use los antiguos** (lista abajo).
- **Volver a la v0.5.0 o anterior después de actualizar duplica esos objetos** (ver "Nombres" más
  abajo).

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

**Cambiado:**
- **La "Calidad mínima" es un suelo para la reducción por RAM y para la degradación, acotado por la
  salida de OBS.** Cuando falta RAM sigue decidiendo hasta dónde puede encogerse el buffer (antes que
  bajar de ahí, se guardan menos segundos), y la degradación tampoco baja nunca de ella; pero ya no
  eleva el buffer por encima de la resolución de salida: con una salida de 720p el buffer es 720p
  aunque el mínimo sea 1080p.
- **La línea de log `compression check` se sustituye por `perf check` y `pipeline check`** (abajo).

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

**Tests** (9 suites, sin OBS; `ctest`):
- `buffer_fit_tests` (dimensionado: lienzo = salida, 1440p → 1080p, 4K → 1080p, salida mayor que
  el lienzo, relaciones de aspecto distintas, un suelo por encima de la salida, reducción por RAM,
  redondeo, anchos múltiplos de 8, dimensiones impares, entradas degeneradas, 59,94 fps, el delay
  efectivo en segundos enteros, barridos de presupuesto/salida), `encoder_check_tests` (la decisión
  del aviso de encoders), `ui_tokens_tests` (contraste en las paletas de OBS 31),
  `auto_names_tests` (los nombres y su migración), `overlay_layout_tests` (colocación del overlay),
  `frame_arena_tests`, `frame_ring_tests` (máquina de estados de los slots, generaciones, selección
  por antigüedad), `degradation_policy_tests` (escalera, cada disparador, enfriamiento, histéresis
  de 30 s, sin oscilaciones) y `pipeline_stress_tests` (ring + arena + pipeline de códec con
  desconexiones, redimensionados y cambios de peldaño y de fps al azar: el delay nunca encoge, los
  frames nunca salen rotos ni desordenados y ningún ring se libera nunca en el hilo gráfico
  simulado).

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
