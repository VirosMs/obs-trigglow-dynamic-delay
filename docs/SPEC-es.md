*[English version](SPEC.md)*

# Especificación técnica — Trigglow Dynamic Delay para OBS

Estado: **MVP / v0.5.1 — Early Access**
Última actualización: 2026-10-18 (v0.5.1: §3.3, §3.5, §4.2–§4.5, §8; reescrita desde cero el
2026-08-26 — ver §0)
Nombre interno del plugin: `obs-trigglow-dynamic-delay`
Web: https://www.trigglow.com/dynamic-delay

---

## 0. Por qué se reescribió este documento

La spec original de v0.1.0 (fechada el 2026-08-23, conservada en el historial de git) describía un
delay **basado en reconexión**: cambias el valor y, si ya estás en directo, OBS reconecta brevemente
el stream para aplicarlo. Ese diseño se construyó y luego se abandonó por completo el 2026-08-24,
después de que las pruebas en directo dejaran claro que la reconexión en sí resultaba inaceptable
para uso real de streaming, en favor del **modo buffer**: un delay que nunca toca el output de
streaming, en ningún momento. El modo buffer es lo que v0.2.0 realmente distribuye hoy. La spec
antigua siguió describiendo el mecanismo abandonado (incluso calificando el modo buffer de
"candidato para v2/v3, no apto para un MVP fiable") durante todo el desarrollo del modo buffer — este
documento la sustituye por lo que realmente hay en el código.

## 1. Resumen del producto

Trigglow Dynamic Delay es un plugin nativo de OBS Studio (C++, CMake, plantilla oficial
[`obsproject/obs-plugintemplate`](https://github.com/obsproject/obs-plugintemplate)) que retrasa el
vídeo y el audio de tu stream un número configurable de segundos — con un botón, una hotkey nativa de
OBS, o un botón de Stream Deck mapeado a esa hotkey — **sin que el output de streaming se toque
jamás**. Ni reconexión, ni corte, en ningún momento, por ningún motivo. Sin app externa, sin panel
web, sin proceso aparte: todo vive dentro del proceso de OBS.

## 2. Por qué se rechazó la reconexión

Antes de construir nada, verificamos el comportamiento real de OBS leyendo directamente el código
fuente de `libobs` (no solo la documentación, que en este punto está incompleta), contrastado con el
hilo del foro de OBS
["how to change stream delay without restarting OBS while live"](https://obsproject.com/forum/threads/how-to-change-stream-delay-without-restarting-obs-while-live.194645/)
y la petición de funcionalidad todavía sin resolver
["Allow adding/removing Output Delay while live"](https://ideas.obsproject.com/posts/541/allow-adding-removing-output-delay-while-live).

**Hallazgo clave (verificado en `libobs/obs-output.c`, `hook_data_capture()`):**

```c
if (output->delay_sec) {
    output->active_delay_ns = (uint64_t)output->delay_sec * 1000000000ULL;
    ...
    os_atomic_set_bool(&output->delay_active, true);
}
```

`obs_output_set_delay(output, delay_sec, flags)` es un simple setter: guarda `delay_sec`/`flags` en
la estructura del output pero **nunca toca el buffer que ya está en curso**. El valor solo se lee y se
convierte en el delay real (`active_delay_ns`) **una vez, en el instante en que el output arranca** —
`hook_data_capture()` se ejecuta al iniciar el stream, no en cada llamada al setter. Esto confirma,
desde el código fuente y no desde folclore de foros:

> **OBS fija el valor del delay en el instante en que el stream arranca. Llamar a
> `obs_output_set_delay()` mientras el stream ya está activo no cambia lo que está ocurriendo ahora
> mismo — solo "arma" el valor para la próxima vez que el output arranque.** Es una limitación
> arquitectónica de `libobs`, no algo solucionable desde la Frontend API ni desde ningún plugin — la
> misma limitación que tiene el propio ajuste de delay nativo de OBS.

Una versión anterior de este plugin (la primera build de v0.1.0, ya abandonada) sorteaba esto llamando
a `obs_frontend_streaming_stop()` + un reinicio automático para volver a "armar" el valor — un enfoque
real y funcional, pero que provoca una reconexión breve y visible en la plataforma de destino cada vez
que el delay cambia en directo. Las pruebas en directo dejaron claro que reconectar en cada cambio no
era una experiencia aceptable para una herramienta pensada para que el streamer "reaccione al
instante", así que se abandonó por completo en favor del modo buffer descrito abajo. (El código de
`delay-controller.*`/`hotkeys.*` del modo reconexión sigue en este repositorio, inactivo — ver §6 — por
si algún día se quiere un modo opcional de menor consumo de recursos, pero `plugin-main.cpp` no lo
instancia y no se distribuye en ninguna build actual.)

## 3. Cómo funciona realmente el modo buffer

El modo buffer nunca llama a `obs_output_set_delay()` ni toca el output de streaming en absoluto. En
su lugar, retrasa lo que OBS está *mostrando en Program*, usando dos filtros de OBS más una pequeña
máquina de estados:

### 3.1. `VideoDelayFilter` — vídeo

Un filtro de OBS solo `OBS_SOURCE_VIDEO`, adjunto a una **escena contenedora** oculta que contiene la
escena en directo elegida por el usuario. En cada llamada a `video_render`:

1. Renderiza la escena en directo en un único render target de captura RGBA reutilizable.
2. Convierte ese frame RGBA a **NV12** (Y + planos UV a media resolución, promediados por bloques,
   BT.601) mediante un pequeño shader propio compilado una sola vez con `gs_effect_create()` — libobs
   no tiene ninguna API pública de compute/dispatch (comprobado directamente contra `graphics.h`), así
   que un shader de píxeles normal de dos pasadas es el único mecanismo público para esta conversión.
3. Lanza una **lectura asíncrona de GPU a CPU** hacia uno de 2 pares rotatorios de staging surfaces
   (`gs_stage_texture` + un `gs_stagesurface_map` *diferido* un frame después, para que el hilo de CPU
   nunca se bloquee esperando una copia que acaba de empezar).
4. Copia los bytes Y/UV ya terminados en el slot de escritura actual de un **ring buffer en RAM del
   sistema** (`std::vector<Slot>`, `Slot::pixels` — NV12, 1.5 bytes/píxel, no los 4 de RGBA).
5. Sube el slot que tiene `delaySeconds` de antigüedad a 2 pequeñas texturas de reproducción
   reutilizables y lo dibuja mediante la técnica inversa del mismo shader (NV12→RGBA).

Solo existe un **número pequeño y fijo de objetos de GPU**, sin importar cuántos segundos se estén
almacenando — un render target de captura, los dos render targets de conversión, 2 pares de staging
surfaces, 2 texturas de reproducción. Esto sustituyó a un diseño anterior (una textura de GPU
persistente por cada frame almacenado) después de que el feedback en directo cuestionara si de verdad
hacía falta tanta VRAM, y después de un crash real de "Device Remove/Reset" del juego al pulsar
Disable, que elevó el coste de mantener docenas de texturas grandes de GPU vivas a la vez — no
demostrado de forma concluyente como causado por ese diseño, pero plausiblemente reducido al pasar a
una huella de GPU fija y pequeña (ver §7).

### 3.2. `AudioDelayFilter` — audio

Retrasar el audio necesitó un **punto de enganche distinto**, descubierto en directo:
`obs_source_filter_add()` (`obs-source.c`) se niega silenciosamente a adjuntar CUALQUIER filtro que
pida `OBS_SOURCE_AUDIO` a una **escena** — el propio registro de `obs-scene.c` demuestra que las
escenas solo anuncian `OBS_SOURCE_VIDEO` en su propio `output_flags` (el audio de una escena pasa por
un callback `audio_render` aparte, nunca expuesto al chequeo `filter_compatible()` que ejecuta
`obs_source_filter_add()`). Esto no se puede arreglar renombrando cosas ni con trucos de keep-alive —
es una limitación dura de `libobs`.

Así que `AudioDelayFilter` (un filtro solo `OBS_SOURCE_AUDIO`) se adjunta en su lugar a **cada fuente
de audio hoja individual** dentro de la escena en directo (micrófono, audio de escritorio, etc.), una
instancia por fuente, cada una ejecutando la misma lógica de ring buffer con ventana de delay que el
vídeo, en espacio de muestras (`ring_[channel][frame]`). El vídeo y el audio usan el mismo
`delaySeconds`, así que ambos llegan sincronizados.

### 3.3. `BufferModeController` — la máquina de estados

Estados: `Inactive → Filling → Active`, con `Error` alcanzable desde cualquier estado. En **Enable()**:

1. Se asegura de que exista una escena contenedora oculta, anidando la escena en directo elegida.
2. Adjunta/habilita `VideoDelayFilter` en la escena contenedora y `AudioDelayFilter` en las fuentes
   hoja con audio de la escena en directo.
3. Fuerza a la escena en directo a seguir renderizando en segundo plano durante la ventana de llenado
   (`obs_source_inc_showing`/`dec_showing`) — una fuente que no está en Program/Preview normalmente no
   renderiza nada en absoluto, así que sin esto el ring buffer se quedaría vacío durante toda la
   espera. (El audio no necesita esto: el propio pipeline de audio de una fuente hoja se ejecuta de
   forma continua independientemente de la visibilidad en Program — solo el renderizado de vídeo
   necesitaba forzarse.)
4. Cambia el Program a la escena de carga opcional (o deja la escena en directo si no se eligió
   ninguna).
5. Cuando el temporizador de llenado propio del dock llega a `delaySeconds`, cambia el Program a la
   escena contenedora, que ya está mostrando el contenido retrasado almacenado — **un cambio de escena
   puramente interno**, el output de streaming nunca ve que esto ocurre.

En **Disable()**: vuelve a poner en Program la escena que se mostraba antes de Enable, deshabilita
ambos filtros y libera el ring de RAM (ver §3.4) — volviendo al comportamiento instantáneo, sin delay,
sin buffer.

**Efecto secundario importante de cambiar el Programa:** el paso 5 llama a
`obs_frontend_set_current_scene()` — exactamente la misma función que usa la propia lista de escenas
de OBS. Esto significa que el modo buffer retrasa **todo lo que observa el Programa**, no solo el
output de streaming: la grabación local y el Replay Buffer, si cualquiera de los dos está configurado
para capturar el Programa (la configuración habitual/por defecto), también mostrarán la escena de
carga y después el contenido delayed mientras el modo buffer esté Active, en vez del directo real.
Es una diferencia real respecto al diseño de reconexión abandonado (§2), que solo afectaba al delay
del propio output de streaming y dejaba el Programa/la grabación intactos. Hoy no es configurable —
un streamer que quiera una grabación local sin delay junto a un stream con delay no está cubierto tal
cual.

**v0.3.1: restaurar los selectores de escena del dock al abrir OBS.** `LoadSettings()` (llamado desde
`obs_module_load()` en `plugin-main.cpp`) restaura `liveSceneName`/`loadingSceneName` desde disco
correctamente en cada arranque de OBS, pero el constructor de `TrigglowDelayDock` — que corre justo
después, todavía dentro de `obs_module_load()` — rellena sus desplegables de escena llamando a
`ListAvailableScenes()` en ese mismo instante. Encontrado en directo, 2026-08-27: OBS todavía no ha
terminado de cargar la colección de escenas en ese punto (la carga de módulos ocurre antes que la
carga de la colección de escenas), así que ese primer rellenado siempre volvía vacío, dejando ambos
desplegables en blanco aunque los ajustes guardados estuvieran bien — indistinguible, desde el punto
de vista del usuario, de que el plugin hubiera olvidado la configuración. Arreglado conectando por
fin el callback `FrontendEvent::FinishedLoading` que ya existía en `ObsFrontendBridge` (huérfano
hasta ahora — ver §2, antes lo usaba `DelayController`): `BufferModeController` ahora se suscribe a
él en su constructor y expone `SetSceneListRefreshCallback()`, que el dock usa para volver a rellenar
ambos desplegables — usando el mismo `status_.liveSceneName`/`loadingSceneName` ya correcto — en
cuanto OBS termina de cargar de verdad.

**v0.3.2: evitar que el audio se desincronice tras un delay de vídeo acortado.** El §3.5 de más
abajo explica que `VideoDelayFilter` acorta su propia duración REAL de buffer (nunca el suelo de
calidad) siempre que el presupuesto de RAM no dé para el `delaySeconds` completo pedido a la calidad
elegida — esto ha sido así desde antes incluso de que existiera la compresión. `AudioDelayFilter`,
en cambio, siempre guarda exactamente los segundos completos pedidos (PCM barato, sin ningún chequeo
de presupuesto de RAM) — así que cada vez que el vídeo tenía que acortarse, el audio seguía
retrasando la duración larga sin tocar, y ambos se desincronizaban exactamente por esa diferencia.
Reportado en directo, 2026-08-27, en pedidos largos/de alta calidad (30s+, 1080p) como "el audio sale
después de la acción del vídeo". Arreglado con una nueva consulta entre hilos,
`get_effective_delay_seconds` (un `proc_handler` en el `obs_source_t` del filtro de vídeo, siguiendo
el mismo patrón `obs_queue_task(OBS_TASK_GRAPHICS, ..., wait=true)` que ya usaba `release_buffers` —
`ring_`/`currentFps_` solo son seguros de leer desde el hilo de gráficos):
`ObsFrontendBridge::GetVideoEffectiveDelaySeconds()` la llama, y
`BufferModeController::SyncAudioDelayToVideoEffective()` recorta el delay del audio a lo que esa
consulta devuelva siempre que sea menor que el valor pedido. Se llama desde `Enable()` (no hace nada
ahí, porque el ring todavía no está dimensionado), `OnFillTimerElapsed()` (el punto real de
corrección — para entonces el ring ya está dimensionado seguro), y cualquier cambio en vivo de delay
o calidad mientras está `Active`. Un efecto secundario aceptado: `AudioDelayFilter::EnsureRingSized`
reinicia el historial de ese ring cada vez que sus segundos configurados cambian de verdad, así que
una corrección en `OnFillTimerElapsed()` provoca un silencio breve (bastante menos de un segundo)
justo cuando el Programa cambia a la escena wrapper delayed, mientras el ring de audio se vuelve a
llenar hasta la duración ahora coincidente — un cambio claramente mejor que quedar desincronizado
el resto de la sesión. (Límite conocido, todavía abierto en v0.5.1: la consulta devuelve el tramo
del ring redondeado hacia abajo a segundos enteros, mientras que `Render()` limita el delay de vídeo
al tramo sin redondear, así que cuando el presupuesto de RAM recorta el ring el vídeo y el audio
pueden quedar hasta ~1s separados. Con el delay completo en el ring ambos son exactamente el delay
pedido. Alinearlos es trabajo de v0.6.0. Desde v0.5.1 el tramo se calcula con aritmética racional
a los `fps_num/fps_den` exactos — multiplicar por un intervalo de frame truncado podía redondear a
29s un tramo de 1800 frames a 60fps.)

**v0.5.1: apagar los filtros de delay que dejó encendidos una sesión anterior.** Los filtros se
guardan encendidos en la colección de escenas cuando OBS se cierra con el delay Active, así que en
el siguiente arranque la escena en directo quedaba con delay permanente mientras el dock mostraba
`Inactive`. La v0.5.0 añadió `ObsFrontendBridge::DisableAllDelayFilters()` en
`FrontendEvent::FinishedLoading` (solo con el controlador en `Inactive`), pero enumeraba con
`obs_enum_sources()`, que nunca devuelve filtros (libobs 31.1.1: solo inputs no privados y grupos),
así que no apagaba nada. Ahora reúne todas las fuentes que pueden llevar uno de nuestros filtros —
inputs y grupos (`obs_enum_sources`), las escenas del lienzo principal (`obs_enum_scenes`) y las
fuentes de audio globales de los canales de salida (`obs_get_output_source`) —, recorre sus filtros
con `obs_source_enum_filters` y solo entonces apaga los que coinciden, fuera de todo lock de libobs
(`obs_source_set_enabled()` emite una señal cuyos handlers pueden tomar esos locks). Limitación
conocida: solo se ejecuta en `FinishedLoading`, no cuando el usuario cambia de colección de escenas
sin reiniciar OBS.

### 3.4. Liberar el ring de RAM al hacer Disable

OBS se salta por completo el `video_render` de un filtro **deshabilitado** — `VideoDelayFilter::Render()`
no vuelve a ejecutarse una vez deshabilitado, así que nada reduciría/liberaría su ring de RAM por sí
solo. `VideoDelayFilter` registra un proc handler `"release_buffers"` en su propio `obs_source_t`
(`obs_source_get_proc_handler`); `ObsFrontendBridge::SetBufferFilterEnabled(false)` lo llama. Como esa
llamada puede producirse desde cualquier hilo (en la práctica, el hilo del clic de botón del dock de
Qt) mientras que el ring solo se toca desde el hilo de gráficos/vídeo, el propio `ring_.clear()` se
encola en `OBS_TASK_GRAPHICS` mediante `obs_queue_task(..., wait=true)` en vez de ejecutarse in situ —
se ejecuta estrictamente después de cualquier llamada a Render() todavía en curso, sin necesitar
ningún lock.

### 3.5. Dimensionado del buffer: techo de salida, después suelo de calidad, después duración

El usuario elige dos cosas, ambas de libre elección, nunca restringidas:

- **Delay (segundos)** — de 1 a 60 en el spinner del dock (un límite de cordura de la interfaz; el
  límite real es la aritmética de RAM de abajo).
- **Calidad mínima** — 480p / 720p / 1080p. El suelo para la reducción por RAM de abajo: antes que
  guardar el vídeo retrasado por debajo de esta altura, el plugin guarda menos segundos. Eso sí,
  desde v0.5.1 nunca eleva el buffer por encima de la resolución de salida de OBS (regla 1 de
  abajo): con una salida de 720p el buffer es 720p aunque el mínimo sea 1080p, porque guardar más
  filas de las que OBS emite no aporta nada.

`EnsureRingSized()` calcula un **presupuesto de RAM** a partir de la RAM total del sistema
(`GlobalMemoryStatusEx` en Windows; sin implementar en otras plataformas, donde se usa un valor de
respaldo conservador) — el 50% de la RAM total, acotado entre un suelo de 1GB y un techo de 24GB.

Desde v0.5.1 el dimensionado en sí es una función pura, `bufferfit::Compute()`
(`src/buffer-fit.hpp`/`.cpp`, sin libobs ni FFmpeg para poder probarla con tests unitarios), a la
que llaman con la misma entrada `EnsureRingSized()` (que redimensiona el ring de verdad) y
`VideoDelayFilter::EstimateBufferFit()` (la estimación del dock), así que lo que predice el dock es
lo que ocurre. Su entrada es el tamaño del objetivo, la geometría del lienzo
(`obs_video_info::base_*`) y de la salida (`output_*`), los `fps_num/fps_den` exactos, el delay, la
calidad mínima, el presupuesto de RAM y el ratio de compresión asumido (§4.3). Aplica tres reglas,
en orden:

1. **Techo de salida (v0.5.1).** El buffer nunca guarda más píxeles de los que emite la mezcla
   principal de OBS (Ajustes → Vídeo → Resolución de salida (escalada)). Lo que esté por encima lo
   descarta el propio reescalado de salida de OBS, pero le costaba CPU al plugin: la codificación y
   decodificación MJPEG (§4.2) escalan más o menos linealmente con los píxeles y se ejecutan en el
   hilo gráfico de OBS. Un usuario con un lienzo 2560x1440 y una salida 1920x1080 a 60fps veía
   "Encoding overloaded" en OBS con v0.5.0, que guardaba el buffer al tamaño del lienzo; en un
   benchmark fuera de OBS con el mismo FFmpeg y el mismo pipeline, un buffer 2560x1440 cuesta
   14,3/17,4ms p50/p95 de codificación + decodificación por frame (codificador de un hilo). El techo
   es una escala uniforme, `capScale = min(1, max(outW/baseW, outH/baseH))`: conserva la relación de
   aspecto del lienzo y nunca baja de la salida en ningún eje (inclinándose por la calidad cuando las
   relaciones de aspecto de lienzo y salida difieren). Solo
   se aplica cuando el objetivo tiene el tamaño del lienzo (el caso normal de la escena en directo) y
   la geometría se conoce; cualquier otro objetivo se deja sin límite, la opción conservadora.
2. **Suelo de calidad.** Si el delay completo no cabe en el presupuesto en el techo, la escala se
   reduce, pero nunca por debajo de `minResolutionHeight` filas — y ese suelo nunca supera el techo.
   La reducción se calcula contra el coste continuo y después se baja de fila en fila de la fuente
   hasta que el frame redondeado cabe de verdad, así que un ring reducido por RAM sigue guardando el
   delay completo y el resultado es monótono respecto al presupuesto y al tamaño de salida. Las
   dimensiones se redondean con `llround` y se fuerzan pares (submuestreo de croma de NV12), con un
   mínimo de 2.
3. **Duración al final.** Si ni siquiera el suelo cabe, el ring recibe menos frames: el plugin acorta
   la **duración realmente almacenada** en vez de bajar del suelo.

El número de frames usa el ritmo exacto: `ceil(delay × fps_num / fps_den)` más un cuarto de segundo
de holgura (el jitter de captura nunca debe dejar el slot más antiguo más joven que el delay) más un
slot, así que 30s a 60000/1001 necesitan 1815 slots en vez de un recuento truncado a 59fps. La celda
de captura es `1e9 × fps_den / fps_num` ns (16.683.333ns a 59,94fps); antes de v0.5.1 era
`1e9 / 59` en ese caso, lo que perdía casi un frame real por segundo.

El dock muestra una estimación en vivo y no bloqueante de qué logrará realmente una combinación
dada de (segundos, calidad) — duración y resolución del buffer, techo incluido — *antes* de que el
usuario pulse Enable — "aconsejar según el hardware, pero a su elección": esta estimación nunca
bloquea ni recorta las elecciones del usuario, solo avisa. `EnsureRingSized()` registra
`buffering at WxH (capped to OBS output WxH, canvas WxH)` siempre que se aplica el techo, y un WARN
si el ring tiene que reconstruirse mientras guarda frames (ajustes de vídeo de OBS, delay o calidad
cambiados con el delay activo — el ring vuelve a llenarse desde vacío).

**Válvula de escape: `buffer_output_cap` (v0.5.1).** Un ajuste oculto del plugin:
`"buffer_output_cap": false` en `buffer-mode-settings.json` (la carpeta de configuración del plugin
que da `obs_module_get_config_path()`; en Windows
`%APPDATA%\obs-studio\plugin_config\obs-trigglow-dynamic-delay\`) quita la regla 1 y dimensiona
como v0.5.0: resolución de la fuente, reducida solo por RAM, nunca por debajo de la calidad mínima.
No es idéntico byte a byte, eso sí: el ritmo exacto y el cuarto de segundo de holgura redondeado
hacia arriba dan un slot más que v0.5.0 a 25/30/50fps, y cuando falta RAM la reducción fila a fila
de la regla 2 puede quedar hasta 2px por debajo o por encima del tamaño de v0.5.0. Existe para
soporte — por ejemplo quien graba a resolución de lienzo y prefiere esa calidad a costa de CPU —,
así que no tiene propiedad en el filtro ni control en el dock. `plugin-main.cpp` lo lee solo al
cargar, de forma estricta: solo un `false` literal de JSON quita el límite (se comprueba con
`obs_data_item_gettype() == OBS_DATA_BOOLEAN`); ausente, `true` o `null` (libobs lo descarta al
cargar) lo dejan activado, y cualquier otro tipo — `"false"` como texto, `0` — también, con un WARN.
Un archivo de ajustes que existe pero no es JSON válido se renombra (o, si no se puede, se copia) a
`buffer-mode-settings.json.<yyyyMMdd-HHmmss>.invalid` con un WARN que da las dos rutas antes de
usar los valores por defecto, así que el guardado al salir nunca lo sustituye en silencio; si no
funciona ninguna de las dos cosas, el archivo no se toca al salir. `BufferModeController` lo envía al
filtro en cada `Enable()` mediante `ObsFrontendBridge::SetBufferFilterOutputCap()`, así que el
archivo de ajustes siempre gana sobre lo que el filtro guardó con la colección de escenas, y la
estimación del dock también lo usa. `SaveBufferSettings()` reescribe el archivo desde cero al salir
de OBS y solo vuelve a escribir la clave cuando es `false` (el archivo del resto de usuarios queda
igual), y por eso mismo hay que editarlo con OBS cerrado. Las líneas de log `settings loaded` y
`enabled` indican `output_cap=on|off`, y el log de dimensionado y la línea `perf check` (§4.5) dicen
cuándo está desactivado. Pasos para el usuario: `docs/TROUBLESHOOTING-es.md`.

**Encoders por encima del buffer (v0.5.1).** Con el techo activo, una salida cuyo encoder de vídeo
trabaja por encima del buffer — una grabación reescalada al lienzo, o una pista de Enhanced
Broadcasting por encima de la salida — recibe vídeo diferido reescalado hacia arriba.
`BufferModeController::WarnAboutEncodersAboveBuffer()` registra un WARN por encoder, tamaño y ciclo
de delay (un encoder que se reinicia a otro tamaño vuelve a avisar), con el nombre del encoder y de
la salida, ambas resoluciones, el tamaño sin límite y el ajuste `buffer_output_cap`, y añade que
quitar el límite cuesta CPU y RAM y puede acortar el delay si falta RAM (con la cifra real cuando el
buffer sin límite no guardaría el delay completo). Revisa cada slot de encoder de vídeo
(`MAX_OUTPUT_VIDEO_ENCODERS`) de cada salida activa, y solo cuando quitar el límite guardaría de
verdad más. Se ejecuta solo en el hilo de UI — en `Enable()`, al llenarse el buffer y en los eventos
de frontend de inicio de streaming / grabación / replay buffer —, nunca desde el hilo gráfico, porque
`obs_enum_outputs()` mantiene `outputs_mutex`.

Qué encoders ven el lienzo principal (`ObsFrontendBridge::ListActiveVideoEncoders()`; la decisión
es la función pura de `src/encoder-check.*`): en libobs 31.1.1 un encoder renderiza el `video_t` que
recibió y `obs_encoder_parent_video()` lo devuelve, pero un encoder reescalado en GPU (Ajustes →
Salida → Avanzado → Reescalar salida, y todas las pistas de Enhanced Broadcasting, que además fijan
un formato preferido) se pasa al arrancar a una mezcla propia de la misma vista al tamaño escalado
(`maybe_set_up_gpu_rescale()` en `obs-encoder.c`), un `video_t` que ninguna API pública relaciona
con su lienzo. Así que:

- `obs_get_video()` → el lienzo principal (exacto).
- el vídeo de otro lienzo con nombre (`obs_enum_canvases()` + `obs_canvas_get_video()`, p. ej. un
  lienzo vertical) → no es el lienzo principal (exacto).
- cualquier otra mezcla → el lienzo principal si el tamaño del encoder tiene la relación de aspecto
  del lienzo o de la salida (tolerancia del 2%, suficiente para los tamaños pares que ofrece OBS,
  como 852x480); si no, no. El WARN dice cuándo se ha basado en esto.

Límites de esa heurística: un encoder reescalado en GPU de otro lienzo, de un lienzo privado o de la
vista de un plugin con la misma relación de aspecto que el lienzo principal (un segundo lienzo 16:9)
se toma por el principal — un WARN que no aplica —, y un encoder del lienzo principal reescalado a
otra relación de aspecto (estirado) se pasa por alto. Un lienzo vertical (9:16) con un lienzo
principal 16:9 siempre se distingue.

**Tests (v0.5.1).** `tests/buffer_fit_tests.cpp` prueba `bufferfit::Compute()` sin OBS: lienzo =
salida, 1440p → 1080p, 4K → 1080p, salida mayor que el lienzo, relaciones de aspecto distintas, un
suelo por encima de la salida, reducción por RAM dentro de rango, un suelo activo, redondeo,
dimensiones impares, entradas degeneradas, un objetivo que no es el lienzo, delay cero, 59,94fps, el
límite desactivado y barridos de presupuesto/salida. `tests/encoder_check_tests.cpp` prueba la
decisión del WARN de encoders (`encodercheck`), con los buffers calculados por `bufferfit::Compute()`
con y sin límite: coincidencia de relación de aspecto, un encoder del lienzo sin escalar y escalado
en CPU, encoders reescalados en GPU por encima/igual/por debajo del buffer, pistas de Enhanced
Broadcasting, lienzos verticales (720x1280, 1080x1920) y un lienzo principal vertical, y un buffer al
que el límite no afecta (límite desactivado, salida = lienzo, limitado por RAM). Desactivados por
defecto, así que el empaquetado del plugin no cambia. Dos formas de compilarlos
(`tests/CMakeLists.txt`):

```
# Desde el build del propio plugin (mismos flags de compilación que el plugin):
cmake --preset <preset> -DTRIGGLOW_BUILD_TESTS=ON
cmake --build <dir> --target buffer_fit_tests encoder_check_tests --config RelWithDebInfo
ctest --test-dir <dir> -C RelWithDebInfo --output-on-failure

# Por separado, sin descargar ninguna dependencia de OBS/Qt/FFmpeg:
cmake -S tests -B build_tests
cmake --build build_tests --config RelWithDebInfo
ctest --test-dir build_tests -C RelWithDebInfo --output-on-failure
```

El job `unit-tests` de CI (`.github/workflows/build-project.yaml`) ejecuta la forma por separado en
Ubuntu, Windows y macOS, y bloquea el job de release si fallan.

## 4. v0.3.0: compresión real MJPEG del ring de RAM

La aritmética del presupuesto de RAM del §3.5 anterior sigue asumiendo un ring NV12 sin comprimir
para el mecanismo central del §3 — eso era exacto para v0.2.0, pero a partir de v0.3.0 cada slot del
ring (`VideoDelayFilter::Slot`, `src/video-delay-filter.hpp`) se comprime además con MJPEG al
capturarlo y se descomprime al reproducirlo, en las plataformas donde eso está disponible. Esta
sección describe ese pipeline; la descripción del bucle de captura/reproducción y el pool fijo de
objetos de GPU del §3 no cambia en lo demás.

**Alcance de plataforma:** solo donde está definido `TRIGGLOW_HAVE_FFMPEG` — Windows y Linux, vía un
build de FFmpeg vendorizado, de release fijada, de BtbN LGPL-shared (`avcodec`/`avutil`/`swresample`;
nunca "latest", para mantener los builds reproducibles). Todavía no existe un build estático de
FFmpeg de confianza equivalente para macOS, así que macOS no forma parte de esto: sigue funcionando
exactamente igual que en v0.2.0, sin comprimir. En cualquier sitio donde el codec no esté disponible
(macOS hoy, o un build sin FFmpeg enlazado), todos los caminos de código de abajo caen
automáticamente al comportamiento NV12 sin comprimir de antes de v0.3.0 — la compresión es una
adición sobre el mecanismo existente, nunca un requisito nuevo para que funcione.

### 4.1. Por qué MJPEG, todo intra

La invariante central del ring (§3.1, §3.4) es que cualquier slot puede sobrescribirse, desalojarse o
leerse de forma independiente mediante aritmética de índices en cualquier momento —
`EnsureRingSized()` puede redimensionar/vaciar todo el ring de golpe ante un cambio de resolución o de
delay, y la reproducción lee el slot que tiene `delaySeconds` de antigüedad sin ninguna noción de "el
frame anterior" que entre en juego. Un codec predictivo/GOP real (frames P/B que referencian frames
anteriores, lógica de búsqueda por keyframe, un orden de decodificación distinto del orden de
visualización) arrastra exactamente el tipo de suposiciones de máquina de estados entre frames que no
encajan con eso: decodificar el slot N requeriría también conservar aquello de lo que dependía el
slot N, incluso después de que el ring haya avanzado o se haya redimensionado más allá de él. MJPEG
evita esto por construcción — cada frame se codifica y decodifica de forma independiente (todo
intra, sin ninguna predicción entre frames), así que los bytes comprimidos de un slot son
autocontenidos e independientes del resto del bitstream, encajando exactamente con el contrato de
"cualquier slot, en cualquier momento" del ring. El coste es un ratio de compresión peor que el que
lograría una estructura GOP real; eso se acepta como el precio de encajar con esta arquitectura
concreta, no como un codec de entrega de propósito general.

### 4.2. Flujo de codificación/decodificación

El paso de captura en `VideoDelayFilter::Render()` no cambia hasta la lectura asíncrona de GPU a CPU
(§3.1, pasos 1–3): los bytes Y/UV recogidos en el tick actual terminan en `scratchNv12_`, el NV12
crudo de un frame completo, reutilizado cada tick como espacio de scratch. A partir de ahí:

- **Codificar (captura):** `EncodeScratchNv12Into()` de-interleava el plano UV de NV12 de
  `scratchNv12_` en dos buffers de scratch U y V separados y compactos (`encodeScratchU_`/
  `encodeScratchV_`) — el codificador de MJPEG quiere 4:2:0 planar, y NV12 es semi-planar (UV
  interleaved), un layout de memoria distinto. Esos planos se copian en un `AVFrame` reutilizado
  (`encodeFrame_`, `AV_PIX_FMT_YUV420P` con `AVCOL_RANGE_JPEG`) y se pasan por `encoderCtx_`, el
  `AVCodecContext` de MJPEG abierto por `EnsureCodecContextsOpen()` al `bufferWidth_`/`bufferHeight_`
  actual del ring, con calidad fija vía `AV_CODEC_FLAG_QSCALE` (`qmin`/`qmax` fijados ambos a
  `kMjpegQuality = 5` — un punto dulce de MJPEG comúnmente citado, visualmente cercano a lossless
  mientras sigue comprimiendo de forma significativa; todavía no es un ajuste del dock). Los bytes del
  paquete resultante se copian en `pixels` del `Slot` destino, con `usedBytes` puesto al tamaño del
  paquete y `compressed = true`.
- **Decodificar (reproducción):** `DecodeSlotIntoScratchNv12()` hace lo inverso — pasa el paquete
  MJPEG del slot (`src.compressed` debe ser true) por `decoderCtx_` y escribe los planos decodificados
  de vuelta en `scratchNv12_` como bytes NV12 planos, listos para la subida de reproducción existente
  con `gs_texture_set_image()` (§3.1, paso 5) exactamente como si ese slot nunca se hubiera
  comprimido.
- **Fallback:** cualquiera de las dos funciones devuelve `false` (destino intacto) si el contexto del
  codec correspondiente no está abierto — no encontrado en el FFmpeg enlazado, o `avcodec_open2()`
  falló a esta resolución — o si un frame concreto genuinamente falla al codificar/decodificar.
  Quienes las llaman entonces guardan/leen `scratchNv12_` en crudo, exactamente el mismo camino de
  código que usan siempre las plataformas sin `TRIGGLOW_HAVE_FFMPEG`. La compresión, por tanto, nunca
  es un requisito duro para que el ring funcione — solo una optimización de RAM añadida encima.

**Dónde se ejecuta esto y cuánto cuesta (v0.5.1).** Tanto la codificación como la decodificación se
ejecutan de forma síncrona dentro de `Render()`, en el hilo gráfico de OBS — el hilo que tiene que
terminar cada frame en 1/fps (16,7ms a 60fps). Su coste escala más o menos con el número de píxeles
del buffer, por eso v0.5.1 limita el buffer a la resolución de salida (§3.5, regla 1) y añade los
cambios de abajo. En un benchmark fuera de OBS con el mismo FFmpeg (codificación + decodificación
por frame, p50/p95): 1080p de 8,6/10,4ms (v0.5.0) a 5,2/5,9ms; un lienzo 1440p con salida 1080p de
14,3/17,4ms a ~5,2/5,9ms. Una *salida* 1440p se sigue guardando a 1440p: se espera que vaya bien a
30fps, todavía no a 60fps. Llevar la codificación y decodificación a hilos worker, fuera del hilo
gráfico (objetivo 1440p60), es el siguiente paso — ver `docs/ROADMAP-es.md`, v0.6.0.

- **Codificador con hilos por slices (v0.5.1).** `EnsureCodecContextsOpen()` abre el codificador
  MJPEG con `thread_type = FF_THREAD_SLICE` y `thread_count = clamp(hardware_concurrency() / 4, 1,
  kMaxEncodeSliceThreads = 4)` — 4 en una CPU de 8 núcleos/16 hilos, 1 en una de 4 hilos —, así que
  los slices de cada frame se codifican en paralelo y baja el tiempo real en el hilo gráfico.
  `thread_type` hay que fijarlo de forma explícita: con solo `thread_count > 1`, FFmpeg elige
  threading por frames para MJPEG (`QSCALE` se salta su excepción de un solo hilo),
  `receive_packet()` devuelve entonces `EAGAIN` en el mismo tick y todos los frames se guardarían en
  crudo. Si `active_thread_type` sigue indicando threading por frames, el codificador se reabre con
  un solo hilo (WARN), y si abrirlo con slices falla, se reintenta con un hilo. Medido con el n9.0.2
  vendorizado: la codificación a 1080p pasa de 5,1 a 1,8ms p50 con 4 slices, el mismo CPU total,
  paquetes ~3,5% más grandes. El decodificador se queda en `thread_count = 1` (el decodificador MJPEG
  de n9.0 no tiene threading). La línea de log `MJPEG encoder/decoder open` indica los hilos, las CPU
  lógicas y `active_thread_type`.
- **Tablas de Huffman estándar (v0.5.1).** El codificador se abre con `huffman=default`
  (`av_opt_set()` sobre sus opciones privadas) en vez de las tablas "óptimas" que FFmpeg usa por
  defecto, que necesitan una segunda pasada por frame. Con más de un slice FFmpeg ya usa las tablas
  estándar, así que esto cambia el codificador de un hilo (menos de 8 CPU lógicas, o la reapertura con
  un hilo): un 10-17% menos de tiempo de codificación (p50-p95, 1440p, n9.0.2 vendorizado) a cambio
  de paquetes ~3% más grandes. La codificación Huffman no pierde información, así que los frames
  decodificados son idénticos, y cada paquete sigue llevando sus tablas (DHT), así que el
  decodificador no cambia. El resultado se comprueba y aparece en la línea de log de apertura
  (`huffman=default`, o por qué no se pudo fijar).
- **Decodificar y subir cada slot como mucho una vez (v0.5.1).** `Render()` se ejecuta una vez por
  vista de la escena (Programa, vista previa, proyectores, multiview) y antes decodificaba y hacía
  `gs_texture_set_image()` del slot que tocaba en cada una de esas llamadas. Ahora el filtro recuerda
  para qué slot (índice + hora de captura) intentó la última subida; las llamadas siguientes para el
  mismo slot solo dibujan las texturas de reproducción que ya hay, y un slot que falló al decodificar
  no se reintenta en cada llamada. Si el slot que toca nunca recibió un frame o falla al decodificar,
  se vuelve a dibujar el último frame subido en este ciclo (un "hold") en vez de no dibujar nada; el
  siguiente slot se sigue eligiendo por antigüedad, así que un hold es un tirón de un frame, nunca
  una deriva del delay. Mientras el ring se está llenando no se dibuja nada, como antes. La caché se
  invalida en `ReleaseRing()` (Disable, cambio de tamaño del ring) y `ReleaseSizedGpuObjects()`
  (cambio de resolución, destructor), así que un hold nunca puede mostrar un frame de un ciclo
  anterior ni una textura destruida.

### 4.3. Asignación presupuestada, solo-crecimiento

Comprimir los bytes almacenados dentro de un slot solo compensa en la práctica si la propia
asignación de memoria del slot también se reduce — de lo contrario, un `usedBytes` más pequeño dentro
de un vector `pixels` que sigue siendo de tamaño completo es una victoria de papel, no una real.
`EnsureRingSized()` ahora asigna `pixels` de cada slot a un tamaño **presupuestado** —
`frameBytes / kAssumedCompressionRatio` — en vez del techo NV12 bruto completo.
`kAssumedCompressionRatio` es una constante conservadora, elegida de antemano deliberadamente. Valía
`3.0` en plataformas `TRIGGLOW_HAVE_FFMPEG` cuando se introdujo (elegida conservadora porque el
contenido de captura de OBS está desproporcionadamente lleno de justo lo que peor comprime — bordes
afilados de UI/HUD, texto en pantalla — aunque MJPEG sobre vídeo natural a menudo lo hace mucho
mejor), y vale `5.0` desde v0.4.0 (ver la nota de reajuste de abajo); en cualquier otro sitio vale
`1.0` (sin ninguna compresión asumida), lo que mantiene la asignación de esa plataforma idéntica al
comportamiento anterior a v0.3.0.

`EncodeScratchNv12Into()` y el fallback de almacenamiento en crudo hacen crecer el `pixels` de un
slot más allá de su presupuesto (`Slot::EnsureCapacity()`) cada vez que un frame concreto necesita
genuinamente más espacio del asumido — contenido difícil de comprimir, o el codec no disponible/
fallido para ese frame. Ese crecimiento es **permanente** durante el resto de la vida del slot: la
capacidad nunca se reduce de vuelta, para evitar pagar un realloc+memcpy en cada frame. Esto es un
trinquete deliberado de un solo sentido, no un bug — en el peor caso, un slot que comprime peor de lo
asumido de forma consistente termina creciendo hasta el tamaño bruto completo (idéntico a
almacenarlo todo sin comprimir), y la única forma de liberar ese crecimiento es liberar todo el ring
(`Disable()`, o un cambio de resolución que reasigna `ring_` desde cero).

**Sin relleno de ceros (v0.5.1).** Los slots eran `std::vector<uint8_t>` dimensionados con
`assign(budgetedBytes, 0)`, que toca todas las páginas del ring en el hilo gráfico en cuanto se
dimensiona: la memoria residente subía de golpe al ring entero (~1,9GB a 1440p60) y el primer
`Render()` tras `Enable()` se paraba ~381ms. `Slot::pixels` es ahora un
`std::unique_ptr<uint8_t[]>` sin inicializar con una `capacity` explícita, así que una página solo
pasa a ser residente cuando se escribe un frame en ella: la memoria residente sigue el tamaño real de
los paquetes (651-896MB en vez de 1923MB a 1440p60 en la simulación fuera de OBS) y dimensionar
cuesta ~5ms. La memoria comprometida no cambia. Esto se basa en que nada lee nunca más allá de
`usedBytes`: el decodificador copia exactamente `usedBytes` (comprobado contra `capacity`), y un slot
en crudo solo se sube si contiene un frame NV12 completo.

**Reajuste de v0.4.0.** `kAssumedCompressionRatio` vale `5.0` en plataformas
`TRIGGLOW_HAVE_FFMPEG` desde v0.4.0, tras la primera medición real (una sesión en directo de
30s/1080p60 con gameplay mantuvo un ratio estable de ~11-15x, y ningún slot creció nunca por encima
del presupuesto de 3.0). Sigue siendo conservador a propósito: viene de una sesión y un juego, y un
slot que necesite más simplemente crece. Ver `CHANGELOG-es.md`, v0.4.0, y el comentario de la
constante en `src/video-delay-filter.cpp`.

### 4.4. Resultado medido, y qué queda abierto

Una medición real, en directo — 30 segundos de vídeo 1080p60 almacenado, buffer `Active` — mostró que
el uso total de RAM del ring pasó de **~6.3GB** (antes del cambio de asignación presupuestada, es
decir, comprimiendo los bytes pero reservando todavía el techo bruto completo por slot) a **~2.9GB**
(después de él) en la misma máquina, la misma sesión: una reducción real de **~2.2x**, medida
directamente en vez de asumida.

Queda abierto: el ratio de compresión MJPEG real por frame **todavía no** se ha medido contra
contenido de gameplay real sostenido. El único ratio registrado hasta ahora (`loggedFirstEncode_`,
primera codificación exitosa tras `EnsureCodecContextsOpen()`) vino de un frame mayormente estático de
escena de carga y no es representativo del contenido real — no debería leerse como un número típico
o esperado. La afirmación honesta es que el ratio de compresión real depende mucho de lo que haya en
pantalla y sigue siendo una medición abierta, no que el valor de entonces de
`kAssumedCompressionRatio` (`3.0`, ni ningún otro número concreto) sea lo que logra realmente el
gameplay.

*Actualización:* la línea periódica `compression check` de v0.4.0 dio la primera medición real con
gameplay (~11-15x, ver la nota de reajuste del §4.3), y desde v0.5.1 esos números forman parte de la
línea `perf check` (§4.5), medidos en la sesión de cada usuario. Sigue siendo un dato por sesión y
tipo de contenido, no una garantía.

### 4.5. v0.5.1: telemetría `perf check`

Para que un reporte de rendimiento se pueda comprobar en el log del usuario en vez de suponerlo,
`VideoDelayFilter` escribe una línea INFO cada 10 segundos de render (`kPerfLogIntervalNs`), durante
`Filling` y `Active`. Sustituye a la línea `compression check` de cada 300 frames e incluye sus
datos:

```
perf check (periodic, 10.0s): buf=1920x1080 (capped to OBS output 1920x1080, canvas 2560x1440)
fps=60.00 | render p50/p95/max=…ms readback=… encode=… decode=… upload=… | calls=…/s captures=…/s
uploads=…/s holds=N rawFallback=N | obs frames+N lagged+N skipped+N | ratio=…x (cycle …x)
ring=…MB/N slots growths=N
```

(una sola línea en el log; partida aquí.)

- **Geometría:** la resolución del buffer y el motivo — `capped to OBS output WxH, canvas WxH`,
  `not capped, source WxH, output WxH` o `cap off (buffer_output_cap=false), …` — con
  `, reduced for RAM` cuando entró en juego la regla 2 del §3.5; más los fps exactos.
- **Tiempos:** tiempo real por fase (`os_gettime_ns`) de la llamada completa a `Render()`, la lectura
  de staging, la codificación (o la copia en crudo), la decodificación y la subida de texturas;
  p50/p95 mediante `std::nth_element` sobre un array fijo de 4096 muestras por fase, máximo exacto.
  Sin asignaciones por frame ni por ventana.
- **Contadores:** llamadas a `Render()`, celdas capturadas, subidas, holds (§4.2), fallbacks a crudo
  (falló la codificación), ratio de compresión de esta ventana y del ciclo completo, memoria del ring
  (la reservada, es decir, comprometida; desde v0.5.1 la residente es menor, ver §4.3) y número de
  slots, crecimientos de slots (§4.3), y los incrementos de los contadores propios de OBS
  en la misma ventana — frames totales (`obs_get_total_frames`), frames perdidos por retraso de
  renderizado (`obs_get_lagged_frames`) y frames saltados por retraso de codificación
  (`video_output_get_skipped_frames`).
- **Ventanas:** una pausa de más de 1s entre llamadas a `Render()` (filtro oculto, OBS atascado)
  cierra antes la ventana actual (`resumed after a pause`), para no diluir sus tasas por segundo. La
  última ventana parcial se registra al desactivar el delay (`filter disabled`, desde la tarea
  `release_buffers`) y al destruirse el filtro con el delay activo, p. ej. al cerrar OBS
  (`filter destroyed`; seguro en el destructor porque OBS solo destruye una fuente después de
  desengancharla, y la línea solo lee miembros propios del filtro). Antes de v0.5.1 el filtro en la
  práctica nunca se destruía: `ObsFrontendBridge` perdía una referencia a él en cada `Enable()`
  (`obs_source_get_filter_by_name()` devuelve una referencia nueva); ahora cada búsqueda la guarda en
  un `OBSSourceAutoRelease`, y el destructor, que libobs ejecuta en su hilo de destrucción, entra en
  el contexto gráfico para liberar los objetos de GPU. Las ventanas de menos de 1s se descartan.

Cómo leer cada campo desde el lado del usuario: `docs/TROUBLESHOOTING-es.md`, "Encoding
overloaded".

## 5. Alcance de v0.2.0, tal y como se distribuye realmente

Incluido:

- Enable / Disable / Toggle, sin que el output de streaming se toque **jamás**.
- Escena en directo (obligatoria) + escena de carga opcional mostrada mientras se llena el buffer.
- Calidad mínima seleccionable (480p/720p/1080p); la duración pedida se acorta en vez de bajarla
  jamás.
- Estimación en vivo de ajuste del buffer en el dock, con un aviso no bloqueante si la duración pedida
  completa no cabe en el presupuesto de RAM detectado a la calidad elegida.
- Vídeo y audio retrasados juntos, sincronizados.
- Buffer de RAM liberado automáticamente al pulsar Disable.
- Estado visible con cuenta atrás en vivo mientras se llena: `Inactive` / `Filling (Ns restantes)` /
  `Active` / `Error`.
- 3 hotkeys nativas de OBS (Toggle obligatoria, Enable/Disable opcionales) — usables directamente
  desde Stream Deck con su propia acción "System: Hotkey", sin necesitar un plugin dedicado de Stream
  Deck.
- Persistencia de ajustes por perfil de OBS.
- Manejo de errores sin crashes (ninguna escena en directo elegida, la escena en directo resuelve a
  nada, etc. → estado `Error` con un mensaje claro en el dock, nunca un crash).

Explícitamente fuera de alcance para v0.2.0 (ver `docs/ROADMAP.md`):

- **Compresión** real de vídeo/audio — el ring buffer almacena NV12 sin comprimir hoy. Se investigó
  un pipeline real de codificación/decodificación (vendorizar FFmpeg tanto para codificar COMO para
  decodificar, ya que `obs_video_encoder_create` solo puede alimentar el propio pipeline de output de
  OBS y libobs no tiene ninguna API pública de decodificador) y se difirió explícitamente como trabajo
  futuro de varias sesiones. También se investigó y se descartó una vía con compute shaders de GPU —
  libobs no expone ninguna API de compute/dispatch en su API pública de gráficos, confirmado tras una
  revisión exhaustiva de `graphics.h`. (La compresión de vídeo en Windows/Linux es justo lo que añade
  v0.3.0 — ver §4; la compresión de audio sigue fuera de alcance.)
- Presets de delay guardados.
- Elegir una fuente individual en vez de una escena entera como "en directo".
- Un plugin dedicado de Trigglow para Stream Deck (la vía de hotkey nativa ya funciona hoy).
- Instaladores firmados (el instalador actual de Windows no está firmado — Early Access).
- macOS/Linux **probados en directo** (ambos ya compilan en verde en CI; ninguno se ha ejecutado en
  directo todavía).

## 6. Código heredado del modo reconexión

`delay-controller.*` y el cableado de hotkeys específico de reconexión del primer diseño abandonado en
§2 siguen presentes en este repositorio, sin usar — `plugin-main.cpp` ya no instancia `DelayController`
ni conecta sus hotkeys/dock. Se conservan en vez de borrarse por si algún día se quiere un modo
reconexión opcional de menor consumo de recursos (el modo buffer cambia RAM por cero reconexiones;
algunos usuarios con hardware muy limitado podrían preferir el compromiso antiguo) — no es una promesa
de que vaya a volver, simplemente no se ha tirado.

## 7. Riesgo conocido, todavía no confirmado como resuelto

Se observó en directo un crash real de "Device Remove/Reset" de la GPU al pulsar Disable, bajo el
diseño anterior de una textura de GPU persistente por cada frame almacenado. Nunca se determinó de
forma concluyente que este plugin fuera la causa raíz antes de que se hiciera la reescritura a
RAM/NV12 (§3.1) — la reescritura reduce plausiblemente el riesgo (un puñado de objetos de GPU pequeños
y fijos en vez de docenas de objetos grandes que escalan con la duración del delay), pero eso no es lo
mismo que una corrección confirmada. Trátalo como un área de riesgo real y probable en directo, no
como un problema resuelto, hasta que se vuelva a probar específicamente para ver si reaparece.

## 8. Arquitectura

```
plugin-main.cpp              → obs_module_load/unload, cableado de todos los componentes
buffer-mode-controller.*      → la máquina de estados descrita en §3.3 (Inactive/Filling/Active/Error)
obs-frontend-bridge.*         → la única capa que toca obs-frontend-api.h + el cableado de filtros/escenas
video-delay-filter.*          → §3.1 -- el ring buffer en RAM/NV12 y su pequeño pool fijo de objetos de GPU
                                (§4 -- compresión MJPEG de v0.3.0, solo Windows/Linux;
                                §4.5 -- telemetría perf check de v0.5.1)
buffer-fit.*                  → §3.5 -- aritmética pura de dimensionado del ring (techo de salida, suelo
                                de calidad, presupuesto de RAM, fps exactos), sin libobs/FFmpeg (v0.5.1)
encoder-check.*               → §3.5 -- decisión pura del WARN de "encoder por encima del buffer"
                                (qué encoders ven el lienzo principal), sin libobs (v0.5.1)
audio-delay-filter.*          → §3.2 -- ring de delay de audio por fuente hoja
hardware-info.*               → consulta de la RAM total del sistema (sustituye a la antigua consulta VRAM/DXGI)
scene-combo-box.*             → combo box del dock poblado desde obs_frontend_get_scene_names()
settings-ui.*                 → el dock de Qt (escena en directo/de carga, segundos, calidad, estimación
                                de ajuste, Enable/Disable, cuenta atrás de llenado en vivo)
hotkeys.*                     → registro de las 3 hotkeys nativas de OBS, conectadas a
                                Enable()/Disable()/Toggle() de BufferModeController
logging.*                     → envoltorio de logging con prefijo de componente
delay-controller.*            → §6 -- lógica heredada del modo reconexión, presente pero sin usar
tests/buffer_fit_tests.cpp    → §3.5 -- tests unitarios de buffer-fit.*, solo se compilan con
                                -DTRIGGLOW_BUILD_TESTS=ON (v0.5.1)
tests/encoder_check_tests.cpp → §3.5 -- tests unitarios de encoder-check.*, igual (v0.5.1)
```

Principio de diseño: **`BufferModeController` es el único dueño del estado**. Tanto el dock como las
hotkeys llaman a los mismos métodos públicos (`Enable()`, `Disable()`, `Toggle()`) — no hay una ruta de
código separada para "botón" vs. "hotkey", lo cual reduce a la mitad la superficie de bugs de
desincronización de estado.

## 9. Licencia y repositorio

Este proyecto está construido sobre la plantilla oficial `obsproject/obs-plugintemplate`, distribuida
bajo **GPL-2.0-or-later** (ver `LICENSE`). No es una elección de marca: **`libobs`, la librería contra
la que enlaza cualquier plugin de OBS, es en sí misma GPL-2.0**, y el consenso extendido en la
comunidad de OBS es que cualquier plugin que enlace contra ella queda vinculado por esa licencia. En
la práctica, esto significa que si distribuyes un binario compilado de este plugin, la GPL exige poner
a disposición de quien reciba ese binario el código fuente correspondiente — "gratis pero de código
cerrado" no es una combinación que permita esta licencia. (Esto no es asesoría legal — para tener
certeza sobre tu situación concreta, merece la pena una revisión legal real antes de una distribución
a gran escala.)

**Cómo gestiona esto este proyecto:** el código de Trigglow Dynamic Delay se publica en su **propio
repositorio separado**, dedicado únicamente a este plugin — **no** en el repositorio principal de
Trigglow (donde vive el resto del producto: backend, base de datos, frontend web), que permanece
privado. Publicar solo el plugin en su propio repositorio satisface la GPL sin exponer el resto del
producto. El botón "Source code / GitHub" en `/dynamic-delay` en la web apunta a este repositorio
dedicado, no al repositorio principal de Trigglow.
