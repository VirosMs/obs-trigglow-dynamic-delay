*[English version](SPEC.md)*

# Especificación técnica — Trigglow Dynamic Delay para OBS

Estado: **MVP / v0.7.0 — Early Access**
Última actualización: 2026-10-20 (v0.7.0: §3.5, §4–§4.8, §8; v0.6.0: §3.1–§3.8, §4–§4.7, §8;
reescrita desde cero el 2026-08-26 — ver §0). Se preparó un hotfix v0.5.1 que nunca se publicó; todo
lo que llevaba salió en la v0.6.0, así que este documento fecha esos cambios en la v0.6.0.
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
escena en directo elegida por el usuario. En cada llamada a `video_render` (desde la v0.6.0):

1. Como mucho una vez por celda de captura (un intervalo de reloj de `1e9 × fps_den / fps_num` ns,
   dos en el peldaño de degradación a mitad de fps, §4.7), renderiza la escena en directo en un único
   render target de captura RGBA reutilizable al tamaño actual del buffer — su proyección ortogonal
   sigue abarcando el tamaño real de la escena, así que la GPU rasteriza la escena reducida.
2. Convierte ese frame RGBA a **I420** — Y, más U y V a la mitad de ancho y de alto (promediados por
   bloques, BT.601, rango de estudio) — en tres pequeñas pasadas a pantalla completa sobre tres render
   targets R8, con un shader propio compilado una sola vez con `gs_effect_create()`. libobs no tiene
   ninguna API pública de compute/dispatch (comprobado directamente contra `graphics.h`), así que las
   pasadas normales de shader de píxeles son el único mecanismo público para esta conversión. (Hasta
   la v0.5.0 era NV12, dos planos, desentrelazado en la CPU antes de cada compresión y reentrelazado
   después de cada descompresión.)
3. Lanza una **lectura asíncrona de GPU a CPU** hacia uno de 3 slots rotatorios de staging (tres
   superficies R8 cada uno): `gs_stage_texture` ahora y `gs_stagesurface_map` dos capturas después,
   para que el hilo gráfico nunca se bloquee esperando una copia que acaba de empezar. Los planos
   mapeados se copian en un frame crudo de un pequeño pool y se entregan a los hilos de compresión
   (§4.6).
4. Reserva el siguiente slot de un **ring en RAM del sistema** (`FrameRing`, §4.6) para esa captura,
   marcado con su hora de captura; un hilo de compresión comprime el frame (§4) y guarda el paquete en
   la arena del ring.
5. Elige el frame más nuevo capturado hace al menos `delay` segundos, ya descomprimido por adelantado
   por el hilo de descompresión, lo sube a 3 pequeñas texturas de reproducción **solo cuando cambia**
   (el resto de vistas de la escena solo lo vuelven a dibujar) y lo dibuja con la técnica inversa del
   mismo shader (`DrawI420`).

Un delay de 0 s se salta todo esto desde la v0.6.0: el vídeo pasa sin retraso, sin ring y sin códec.
El mínimo del dock es 1 y un filtro que gestiona el dock ignora cualquier otro valor (§3.3), así que
esto solo se alcanza en un filtro que el dock todavía no gestiona (uno añadido a mano).

Solo existe un **número pequeño y fijo de objetos de GPU**, sin importar cuántos segundos se estén
almacenando — un render target de captura, tres render targets de planos, 3 slots de staging de tres
superficies, 3 texturas de reproducción. Esto sustituyó a un diseño anterior (una textura de GPU
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
`delaySeconds`, así que ambos llegan sincronizados; cuando el presupuesto de RAM recorta el ring de
vídeo, ambos usan en su lugar el mismo delay efectivo en segundos enteros (§3.3).

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
el resto de la sesión. (Límite conocido hasta la v0.6.0: la consulta devolvía el tramo del ring
redondeado hacia abajo a segundos enteros, mientras que `Render()` limitaba el delay de vídeo al
tramo sin redondear, así que cuando el presupuesto de RAM recortaba el ring el vídeo y el audio
podían quedar hasta ~1s separados. La v0.6.0 lo cierra, ver más abajo. La consulta tampoco salta ya
al hilo gráfico.)

**v0.6.0: apagar los filtros de delay que dejó encendidos una sesión anterior.** Los filtros se
guardan encendidos en la colección de escenas cuando OBS se cierra con el delay Active, así que en
el siguiente arranque la escena en directo quedaba con delay permanente mientras el dock mostraba
`Inactive`. La v0.5.0 añadió `ObsFrontendBridge::DisableAllDelayFilters()` en
`FrontendEvent::FinishedLoading` (solo con el controlador en `Inactive`), pero enumeraba con
`obs_enum_sources()`, que nunca devuelve filtros (libobs 31.1.1: solo inputs no privados y grupos),
así que no apagaba nada. Ahora reúne todas las fuentes que pueden llevar uno de nuestros filtros —
inputs y grupos (`obs_enum_sources`), las escenas del lienzo principal (`obs_enum_scenes`) y las
fuentes de audio globales de los canales de salida (`obs_get_output_source`) —, recorre sus filtros
con `obs_source_enum_filters` y solo entonces apaga los que coinciden, fuera de todo lock de libobs
(`obs_source_set_enabled()` emite una señal cuyos handlers pueden tomar esos locks). Se ejecuta en
`FrontendEvent::FinishedLoading` y en `FrontendEvent::SceneCollectionChanged` (una colección a la que
se cambia puede traerlos igual que la que se carga al arrancar), en ambos casos solo con el
controlador en `Inactive`, justo después de la migración de nombres del §3.7.

**v0.6.0: cambiar de colección de escenas desactiva el delay.** La escena contenedora y los filtros
pertenecen a la colección en la que se crearon, así que con el delay en marcha un cambio dejaba el
dock diciendo "Delay activo" mientras la colección nueva funcionaba sin ningún delay. En
`OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING` (`FrontendEvent::SceneCollectionChanging`: cambiar,
nueva, eliminar), con la colección antigua todavía cargada,
`BufferModeController::StopForSceneCollectionChange()` detiene el delay como `Disable()` — filtros
apagados, el Programa de vuelta en la escena que había antes de Activar —, así que la colección
antigua se guarda así, y pasa a `Inactive` con `Status.CollectionChanged` ("El delay se desactivó al
cambiar de colección de escenas…") y un WARN. OBS 31 no envía `COLLECTION_CHANGING` al duplicar o
renombrar, solo `COLLECTION_CHANGED` con la copia ya cargada: entonces se hace la misma parada (la
primera, antes de la migración y la limpieza de arriba), que apaga los filtros de la copia, y solo
cambia el Programa si la copia se abrió en la escena contenedora, que si no mostraría la escena en
directo sin delay.

**v0.6.0: un único delay en segundos enteros para vídeo y audio (diseño A.4).**
`bufferfit::Compute()` devuelve `effectiveDelaySeconds`: el delay pedido cuando el ring lo guarda
entero, y si no `floor((frames − 1 − holgura) / fps)`, segundos enteros que conservan el cuarto de
segundo de holgura contra el jitter de captura. `Render()` retrasa el vídeo exactamente eso
(`effectiveDelaySeconds × 1e9`; solo si cabe menos de un segundo usa el tramo propio del ring), y el
audio recibe el mismo valor. `get_effective_delay_seconds` ya no salta al hilo gráfico:
`VideoDelayFilter::PlannedEffectiveDelaySeconds()` ejecuta el mismo `bufferfit::Compute()` en el hilo
que llama, con el delay y el suelo de calidad que envió el dock (o, en un filtro que el dock todavía
no gestiona, sus ajustes), la información de vídeo de OBS y el tamaño del objetivo — las mismas
entradas que usa `EnsureRingSized()`. Así `SyncAudioDelayToVideoEffective()` da al audio el valor
correcto desde `Enable()` y justo después de un ajuste que cambia el propio controlador, no un
`Render()` más tarde; un cambio de 30 → 40 s ya no deja el audio en 30 s. El resultado se guarda en
`BufferModeStatus::effectiveDelaySeconds`, la única copia, que el dock y el overlay muestran desde
`Filling`: con 60 s pedidos y 41 s que caben, el chip pone "41 / 60 s" en ámbar, el mensaje es
`Status.FillingTrimmed` y la ventana de llenado dura 41 s, todo lo que tiene que guardar el ring
recortado. El tramo en sí usa aritmética racional a los `fps_num/fps_den` exactos: multiplicar por
un intervalo de frame truncado podía redondear a 29s un tramo de 1800 frames a 60fps.

**v0.6.0: rellenar detrás de la escena de carga cuando el buffer se reinicia.** Un redimensionado
duro (§4.6) — el delay, la calidad mínima o los ajustes de vídeo de OBS cambiados mientras el ring
guarda frames, o el interruptor de emergencia cambiado — vacía el ring, así que la escena
contenedora no mostraría nada durante `delay` segundos. El filtro emite `buffer_restarted` en su
fuente; `ObsFrontendBridge` se conecta a esa señal al activar el filtro y la pasa al hilo de UI como
`FrontendEvent::BufferRestarted`; `BufferModeController` vuelve a poner la escena de carga en el
Programa (manteniendo la escena en directo renderizando), resincroniza el audio y vuelve a
`Filling`. `Enable()` y ese rellenado incrementan `BufferModeStatus::fillGeneration`, y el dock solo
rearma su temporizador de llenado cuando cambia, así que otras actualizaciones de estado durante el
llenado (activar o desactivar el overlay, cambiar su esquina) ya no reinician la cuenta atrás ni el
paso a la escena contenedora. Para un cambio que hace el propio controlador (`SetDelaySeconds()` /
`SetMinResolutionHeight()` en `Active`), predice el reinicio con `EstimateBufferFit()` y cambia el
Programa antes de enviar el ajuste. Sin escena de carga, el Programa se queda en la escena
contenedora en blanco, nunca en la escena en directo sin delay. Desde la v0.6.0 el dock bloquea las
escenas en directo y de carga, los segundos y la calidad mínima en `Filling`/`Active` (un control
deshabilitado también sale del orden de tabulación), y los filtros solo aplican los valores del dock
(abajo), así que en la práctica los reinicios vienen de resets de vídeo de OBS.

**v0.6.0: el dock es el único sitio donde se elige el delay.** Las propiedades de los propios
filtros "(auto)" permitían saltarse el plugin (90 s en el diálogo del filtro de vídeo dejaban el
vídeo a 90 s y el audio a 60; 0 s dejaba pasar la escena en directo mientras el dock decía "Delay
activo"). Ahora:
- Sus propiedades no tienen campos de delay ni de calidad, solo una nota (`Filter.VideoDelay.Managed`,
  `Filter.AudioDelay.Managed`) de que los gestiona el dock.
- `ObsFrontendBridge` envía el delay y el suelo de calidad del dock con los procs
  `set_managed_delay_seconds` y `set_managed_min_resolution_height` de cada filtro. A partir de ahí
  el filtro queda *gestionado*: `Update()` ignora un valor distinto en sus ajustes (el botón de
  valores por defecto de OBS, un diálogo antiguo, obs-websocket) con un WARN por cada envío, y
  `PlannedEffectiveDelaySeconds()` lee los valores gestionados. Un filtro todavía no gestionado
  (añadido a mano, o cargado con una colección antes del primer Activar) sigue aplicando sus ajustes.
- Con el delay en marcha, el puente guarda referencias débiles a los filtros que activó y escucha su
  señal `enable`: uno desactivado desde fuera del dock (su icono del ojo en el diálogo de Filtros,
  obs-websocket) se vuelve a activar al momento (como mucho se lo salta el frame que se está
  renderizando), con un WARN por activación. Las mismas referencias débiles sirven para la consulta
  de salud del dock una vez por segundo (sin recorrer la escena, sin llenar el log de WARN al
  renombrar la escena en directo), los ajustes, la consulta del delay efectivo y para apagar los
  filtros, así que Desactivar sigue funcionando tras renombrar la escena en directo.

Limitación conocida: borrar a mano un filtro de delay "(auto)" con el delay en marcha no se detecta
— la escena contenedora muestra entonces la escena en directo sin filtro, sin delay, hasta que el
usuario desactiva y vuelve a activar el delay desde el dock (`Enable()` vuelve a crear el filtro).

Limitación conocida, que ya existía antes de la v0.6.0: el plugin no vigila el Programa con el delay
en `Active`. Si el usuario (o una acción de Stream Deck u obs-websocket) cambia el Programa a la
propia escena en directo en vez de a la escena contenedora, la escena en directo se muestra y se
emite tal cual, sin delay, porque el filtro de vídeo está en la escena contenedora. El Programa tiene
que quedarse en la escena que eligió el plugin mientras el delay está en marcha; para cambiar de
escena hay que desactivar antes el delay.

### 3.4. Liberar el ring de RAM al hacer Disable

OBS se salta por completo el `video_render` de un filtro **deshabilitado** — `VideoDelayFilter::Render()`
no vuelve a ejecutarse una vez deshabilitado, así que nada reduciría/liberaría su ring de RAM por sí
solo. `VideoDelayFilter` registra un proc handler `"release_buffers"` en su propio `obs_source_t`
(`obs_source_get_proc_handler`); `ObsFrontendBridge::SetBufferFilterEnabled(false)` lo llama. Como esa
llamada puede producirse desde cualquier hilo (en la práctica, el hilo del clic de botón del dock de
Qt) mientras que el ring solo se toca desde el hilo de gráficos/vídeo, la liberación se encola en
`OBS_TASK_GRAPHICS` mediante `obs_queue_task(..., wait=true)` en vez de ejecutarse in situ — se
ejecuta estrictamente después de cualquier llamada a Render() todavía en curso, sin necesitar ningún
lock.

Desde la v0.6.0 esa tarea (`ReleaseBuffersTask`) registra la última ventana de telemetría del ciclo
(§4.5) y después llama a `StopCycle(true)`: el ring, la referencia que tiene cada trabajo en cola y el
almacenamiento del pool de frames crudos pasan al hilo liberador del pipeline de códec (§4.6), cada
hilo de códec cierra sus propios contextos, y `ReleaseSizedGpuObjects()` libera los render targets de
captura y de planos, las superficies de staging y las texturas de reproducción — ~100 MB de RAM
(medidos) y ~37 MB de VRAM (estimados) a 1440p que antes seguían ocupados con el delay desactivado.
Reabrirlos en el siguiente Enable cuesta del orden de un milisegundo. Cambio de contrato: `wait=true`
garantiza ahora que el ring está desenganchado del filtro, no que su memoria ya haya vuelto al
sistema — el hilo liberador la libera milisegundos después. El hilo gráfico nunca libera la arena
(cientos de MB, decenas de ms) ni espera a un hilo.

### 3.5. Dimensionado del buffer: techo de salida, después suelo de calidad, después duración

El usuario elige dos cosas, ambas de libre elección, nunca restringidas:

- **Delay (segundos)** — de 1 a 60 en el spinner del dock (un límite de cordura de la interfaz; el
  límite real es la aritmética de RAM de abajo).
- **Calidad mínima** — 480p / 720p / 1080p / 1440p (2K) / 2160p (4K). El suelo para la reducción por
  RAM de abajo: antes que guardar el vídeo retrasado por debajo de esta altura, el plugin guarda
  menos segundos. Eso sí, desde la v0.6.0 nunca eleva el buffer por encima de la resolución de
  salida de OBS (regla 1 de abajo): con una salida de 720p el buffer es 720p aunque el mínimo sea
  1080p, porque guardar más filas de las que OBS emite no aporta nada. También es el suelo de la
  degradación automática (§4.7). Con el suelo a la altura de la salida (p. ej. 1440p con una salida
  1440p), la reducción por RAM recorta segundos en vez de filas, y a la degradación solo le queda el
  peldaño de mitad de fps.

`EnsureRingSized()` calcula un **presupuesto de RAM** a partir de la RAM total del sistema
(`GlobalMemoryStatusEx` en Windows; sin implementar en otras plataformas, donde se usa un valor de
respaldo conservador) — el 50% de la RAM total, acotado entre un suelo de 1GB y un techo de 24GB.

Desde la v0.6.0 el dimensionado en sí es una función pura, `bufferfit::Compute()`
(`src/buffer-fit.hpp`/`.cpp`, sin libobs ni FFmpeg para poder probarla con tests unitarios), a la
que llaman con la misma entrada `EnsureRingSized()` (que redimensiona el ring de verdad) y
`VideoDelayFilter::EstimateBufferFit()` (la estimación del dock), así que lo que predice el dock es
lo que ocurre. Su entrada es el tamaño del objetivo, la geometría del lienzo
(`obs_video_info::base_*`) y de la salida (`output_*`), los `fps_num/fps_den` exactos, el delay, la
calidad mínima, el presupuesto de RAM y el ratio de compresión asumido (§4.3). Aplica tres reglas,
en orden:

1. **Techo de salida (v0.6.0).** El buffer nunca guarda más píxeles de los que emite la mezcla
   principal de OBS (Ajustes → Vídeo → Resolución de salida (escalada)). Lo que esté por encima lo
   descarta el propio reescalado de salida de OBS, pero le costaba CPU al plugin: la codificación y
   decodificación MJPEG (§4.2) escalan más o menos linealmente con los píxeles y se ejecutaban en el
   hilo gráfico de OBS hasta la v0.6.0. Un usuario con un lienzo 2560x1440 y una salida 1920x1080 a
   60fps veía "Encoding overloaded" en OBS con v0.5.0, que guardaba el buffer al tamaño del lienzo;
   en un benchmark fuera de OBS con el mismo FFmpeg y el mismo pipeline, un buffer 2560x1440 cuesta
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
   alturas se redondean con `llround` y se fuerzan pares (croma 4:2:0), con un mínimo de 2. Los
   anchos son el múltiplo de 8 más cercano que no supere la fuente (`AlignedWidth()`, mínimo 8), en
   todas las reglas y en todos los peldaños de degradación, así que cada fila de cada plano I420 ocupa
   un número entero de palabras de 4 bytes: libobs-opengl mapea una superficie de staging con
   `linesize = width` mientras que `glReadPixels()` rellena cada fila hasta 4 bytes, lo que torcía
   cualquier otro ancho en Linux y macOS.
3. **Duración al final.** Si ni siquiera el suelo cabe, el ring recibe menos frames: el plugin acorta
   la **duración realmente almacenada** en vez de bajar del suelo.

El número de frames usa el ritmo exacto: `ceil(delay × fps_num / fps_den)` más un cuarto de segundo
de holgura (el jitter de captura nunca debe dejar el slot más antiguo más joven que el delay) más un
slot, así que 30s a 60000/1001 necesitan 1815 slots en vez de un recuento truncado a 59fps. La celda
de captura es `1e9 × fps_den / fps_num` ns (16.683.333ns a 59,94fps); antes de la v0.6.0 era
`1e9 / 59` en ese caso, lo que perdía casi un frame real por segundo. Desde la v0.7.0 la holgura es
de al menos un GOP (medio segundo) cuando el ciclo puede guardar un códec inter-frame, y el ratio de
compresión supuesto sale de la clase de almacenamiento del ciclo: 5x para MJPEG, 12x cuando un
códec por hardware ya ha funcionado en la sesión (§4.8).

El dock muestra una estimación en vivo y no bloqueante de qué logrará realmente una combinación
dada de (segundos, calidad) — duración y resolución del buffer, techo incluido — *antes* de que el
usuario pulse Enable — "aconsejar según el hardware, pero a su elección": esta estimación nunca
bloquea ni recorta las elecciones del usuario, solo avisa. `EnsureRingSized()` registra
`buffering at WxH (capped to OBS output WxH, canvas WxH)` siempre que se aplica el techo, y un WARN
si el ring tiene que reconstruirse mientras guarda frames (ajustes de vídeo de OBS, delay o calidad
cambiados con el delay activo — el ring vuelve a llenarse desde vacío).

**Válvula de escape: `buffer_output_cap` (v0.6.0).** Un ajuste oculto del plugin:
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
cuándo está desactivado. El interruptor de emergencia `codec_workers` (§4.6) se lee y se envía igual.
Pasos para el usuario: `docs/TROUBLESHOOTING-es.md`.

**Encoders por encima del buffer (v0.6.0).** Con el techo activo, una salida cuyo encoder de vídeo
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

**Techo de los encoders (v0.7.0).** Guardar más líneas de las que manda cualquier encoder no aporta
nada, así que cuando todos los encoders activos del lienzo principal codifican por debajo de la
salida de OBS, el buffer se limita al mayor de ellos (`encodercheck::EncoderCeiling()`, puro,
`tests/encoder_check_tests.cpp`):
- E = los encoders de vídeo activos cuyo ámbito es el lienzo principal, exacto o por relación de
  aspecto (emisión, grabación, Replay Buffer, salidas de otros plugins y todas las pistas de
  Enhanced Broadcasting: `obs_output_get_video_encoder2()` para cada índice); los de otros lienzos
  nunca cuentan.
- **Sin techo** (se queda el límite de salida de la regla 1) si E está vacío, si hay una salida de
  vídeo sin encoder sobre la mezcla principal (cámara virtual, NDI, DeckLink: toman la salida tal
  cual), si un encoder reescalado por GPU tiene una relación de aspecto que no es ni la del lienzo
  ni la de la salida (podría ser el lienzo principal estirado), o con `buffer_output_cap`
  desactivado.
- Si no, `s = máx. sobre E de max(w / baseW, h / baseH)`, y el techo es el lienzo escalado por `s`
  (ancho múltiplo de 8, alto par) cuando `s` queda por debajo de la escala del límite de salida:
  solo puede bajar respecto a la salida, nunca subir. Un encoder a la resolución de salida o por
  encima mantiene el límite de salida (y el WARN de arriba).
- Es un **techo blando**: `bufferfit` y el ring siguen dimensionados para la salida (no cambian el
  límite de RAM, el número de slots ni el delay de A/V en segundos enteros), y el filtro reconstruye
  su escalera de degradación desde el techo hasta `min(suelo, techo)`. Así puede levantarse en
  cualquier momento sin reiniciar el ring; una grabación que arranca después por encima del techo
  recibe frames al tamaño nuevo al cabo de `delay` segundos.
- `BufferModeController` lo recalcula en `Enable()`, en cada inicio o parada de la emisión, la
  grabación, el Replay Buffer y la cámara virtual, y una vez por segundo mientras el delay está
  activo (las salidas que arrancan otros plugins no lanzan ningún evento del frontend), y lo envía
  con el proc `set_encoder_ceiling` del filtro. El log dice `encoder ceiling WxH: … (output WxH,
  canvas WxH)` o `no encoder ceiling: <motivo>`; el dock no muestra nada nuevo (su peldaño 0 es el
  techo). Con una salida 1440p emitida a 1080p guarda a 1080p: −34 % del buffer con MJPEG y −42 %
  con HEVC (estimación fuera de OBS).

**Tests (v0.6.0).** `tests/buffer_fit_tests.cpp` prueba `bufferfit::Compute()` sin OBS: lienzo =
salida, 1440p → 1080p, 4K → 1080p, salida mayor que el lienzo, relaciones de aspecto distintas, un
suelo por encima de la salida, reducción por RAM dentro de rango, un suelo activo, redondeo, anchos
múltiplos de 8, dimensiones impares, entradas degeneradas, un objetivo que no es el lienzo, delay
cero, 59,94fps, el delay efectivo en segundos enteros, el límite desactivado y barridos de
presupuesto/salida. `tests/encoder_check_tests.cpp` prueba la decisión del WARN de encoders
(`encodercheck`), con los buffers calculados por `bufferfit::Compute()` con y sin límite:
coincidencia de relación de aspecto, un encoder del lienzo sin escalar y escalado en CPU, encoders
reescalados en GPU por encima/igual/por debajo del buffer, pistas de Enhanced Broadcasting, lienzos
verticales (720x1280, 1080x1920) y un lienzo principal vertical, y un buffer al que el límite no
afecta (límite desactivado, salida = lienzo, limitado por RAM). `buffer_fit_tests` cubre además los
suelos 1440p y 2160p (una salida 1440p con un suelo 1440p se queda en 2560x1440 y recorta el delay a
los segundos enteros que caben). Otras siete suites cubren el resto del código sin OBS:
`ui_tokens_tests` (contraste WCAG en las paletas de OBS 31), `auto_names_tests` (§3.7),
`overlay_layout_tests` (§3.6), `frame_arena_tests`, `frame_ring_tests` y `degradation_policy_tests`
(§4.6, §4.7), y `pipeline_stress_tests` (§4.6). Desactivados por defecto, así que el empaquetado del
plugin no cambia. Dos formas de compilarlos (`tests/CMakeLists.txt`):

```
# Desde el build del propio plugin (mismos flags de compilación que el plugin):
cmake --preset <preset> -DTRIGGLOW_BUILD_TESTS=ON
cmake --build <dir> --config RelWithDebInfo
ctest --test-dir <dir> -C RelWithDebInfo --output-on-failure

# Por separado, sin descargar ninguna dependencia de OBS/Qt/FFmpeg:
cmake -S tests -B build_tests
cmake --build build_tests --config RelWithDebInfo
ctest --test-dir build_tests -C RelWithDebInfo --output-on-failure
```

El job `unit-tests` de CI (`.github/workflows/build-project.yaml`) ejecuta la forma por separado en
Ubuntu, Windows y macOS, y bloquea el job de release si fallan.

### 3.6. El overlay del directo (v0.6.0)

Mientras el delay está `Active` y "Mostrar overlay de delay en el directo" está marcado, la escena
contenedora muestra una insignia "DELAY 30 s" con el delay que de verdad se aplica
(`BufferModeStatus::effectiveDelaySeconds`). Desde la v0.6.0 es **una única imagen que pinta el
propio plugin** (`src/overlay-badge.*`, QPainter solo en el hilo de UI: placa tinta, regla de marca,
marca de Dynamic Delay, la Archivo empaquetada o, si no está, la fuente de interfaz del sistema, y
bajo la placa, fuera de ella y debajo de la marca, el texto pequeño `Overlay.Signature`,
"By Trigglow" en todos los idiomas, en la Geist Mono empaquetada a 11 px, mist al 75 % con un halo
oscuro muy suave y sin caja: 7,3 px en un directo a 720p), guardada como un PNG por cada valor de
segundos en la carpeta de configuración del plugin (`overlay/delay-badge-r3-<N>s.png`, 500x180, 2,5
veces el diseño de 200x72 -- la placa de 200x58 y debajo la línea de la firma sobre píxeles
transparentes -- para que siga nítida) y mostrada por una sola fuente de imagen, **Trigglow Delay
Badge (auto)**. El `r3` es `overlaylayout::kRenderVersion`, que sube cada vez que cambia el dibujo:
un PNG que dejó en esa carpeta una compilación anterior no se muestra nunca, y el primer dibujo de
la sesión lo borra. Sustituye al PNG de la insignia más una fuente de texto aparte en Segoe UI de la
v0.5.x (solo Windows, con desplazamientos mágicos para alinear las dos); su texto ya no se puede
editar, y la fuente de texto antigua se borra en cuanto se encuentra.

Colocación (`src/overlay-layout.*`, pura, `tests/overlay_layout_tests.cpp`): a un 2,2% del lado
corto del lienzo de la esquina elegida (24 px a 1080p), conservando la proporción del lado corto que
tiene a 1080p, así que se lee igual a 720p, 1440p y 4K y no se extiende por un lienzo vertical de
1080x1920. Toda la imagen respeta ese margen: en una esquina superior la placa queda en él, en una
inferior, la firma. Lo último que aplicó el plugin (esquina, tamaño del lienzo, posición, escala,
alto de la insignia) se guarda en los ajustes privados del elemento de escena: el plugin la vuelve a
colocar solo cuando cambia la esquina en el dock, o cuando cambia el tamaño del lienzo o el alto de
la insignia (`kDesignHeight`, 58 antes de la firma) y el elemento sigue donde lo puso el plugin. Se
conserva la posición o el tamaño que el usuario haya puesto en OBS. Si ya existe una fuente de
imagen con ese nombre, se reutiliza en vez de añadir copias "... 2".

### 3.7. Nombres de los objetos que gestiona el plugin (v0.6.0)

OBS los busca por su nombre, así que son constantes fijas en inglés y ASCII, nunca traducidas
(`src/auto-names.*`): la escena contenedora **Trigglow Delay Buffer (auto)**, el filtro de vídeo
**Trigglow Buffer Mode Delay (auto)**, un filtro de audio por fuente
`Trigglow Audio Delay (auto) - <fuente>`, y el overlay **Trigglow Delay Badge (auto)**. Hasta la
v0.5.x terminaban en "(no tocar)" / "(auto, no tocar)" en todos los idiomas.

`ObsFrontendBridge::MigrateLegacyAutoNames()` renombra las colecciones antiguas una sola vez, en su
sitio (se conservan posición, filtros y ajustes), en `FinishedLoading` y `SceneCollectionChanged` con
el controlador en `Inactive`. Toda búsqueda acepta también los nombres antiguos (`LookupNames()`),
así que una colección cuyo nombre nuevo ya estaba ocupado sigue funcionando con el antiguo. La escena
contenedora, con cualquiera de los dos nombres, nunca se ofrece como escena en directo ni de carga.
Consecuencias documentadas para el usuario (`CHANGELOG-es.md`, `docs/TROUBLESHOOTING-es.md`): las
acciones que se refieren a estos objetos por su nombre (Stream Deck, obs-websocket) hay que
actualizarlas, y volver a la v0.5.0 o anterior después del cambio de nombre crea un juego
"(no tocar)" junto al "(auto)" (la versión antigua desactiva los filtros "(auto)" al cargar, así que
se quedan inertes).

### 3.8. Cliente de cuenta, Trigi y soporte (v0.6.0)

Esta sección cubre solo la parte del plugin. Cómo responde trigglow.com (sus límites, el propio
asistente, cuánto tiempo guarda los datos) vive fuera de este repositorio; el plugin solo reacciona a
las respuestas y códigos de error documentados, y nunca muestra el texto de error del servidor.

**Un único camino de red.** `win-http.*` es el único código que abre conexiones: WinHTTP en Windows;
en macOS y Linux `Available()` es falso y toda llamada devuelve "no disponible" al instante (allí no
hay inicio de sesión, Trigi ni reportes hasta que tengan cliente HTTP, previsto a continuación).
Cada petición es HTTPS con TLS 1.2 o superior, nunca sigue una redirección (un 3xx es un error, así
que un token Bearer no puede rebotar a otro host), no envía cookies ni el inicio de sesión
automático de Windows, tiene timeouts por paso y un límite total (una ventana de recepción más larga
para las respuestas y los resúmenes de Trigi), pone un tope al tamaño de la respuesta, rechaza
valores de cabecera que pudieran inyectar líneas y no escribe nada en el log por sí mismo.
`api-client.*` va encima: peticiones solo a `api.trigglow.com`, un `User-Agent`
`TrigglowDynamicDelay/<X.Y.Z> (OBS plugin; <windows|macos|linux>)` con la versión real, cabeceras
JSON, el cuerpo `{ "error": "<código>" }` traducido a estados del plugin, `Retry-After` y la lista
fija de páginas que el plugin puede abrir (inicio de sesión, soporte, privacidad y la página del
plugin en www.trigglow.com, este repositorio en GitHub), comprobadas por prefijo. Las peticiones que
crean algo (una conversación, una pregunta, un resumen, un ticket, un adjunto) nunca se reintentan
solas. Un build de prueba local solo puede apuntar a otro host con la opción de CMake
`TRIGGLOW_ALLOW_API_HOST_OVERRIDE`, que CI rechaza.

**Inicio de sesión (vinculación v2; `auth-client.*`, `auth-manager.*`, `token-store.*`).**
1. El inicio devuelve un código de enlace (va en la URL de la página), un código corto de usuario que
   se muestra en el dock, un secreto de sondeo que se queda en memoria, la página que hay que abrir,
   una caducidad y un intervalo de sondeo. Cada valor se comprueba (formato, el prefijo de la página,
   caducidad e intervalo acotados) antes de abrir nada; si alguno no encaja, el inicio de sesión
   falla de forma limpia y no se prueba ningún flujo anterior.
2. La página se abre en el navegador del sistema y el dock muestra el código de usuario. El plugin
   sondea con el secreto en el cuerpo de la petición al intervalo indicado hasta la caducidad:
   pendiente, aprobado (el token se entrega una sola vez), denegado ("No he sido yo"), caducado; "más
   despacio" alarga el intervalo y los errores de red siguen sondeando hasta la caducidad. Cancelar
   olvida el enlace y el secreto en local.
3. El token es opaco (el plugin nunca lo interpreta) y solo sirve para las llamadas propias del
   plugin. En Windows se guarda con `CryptProtectData` para el usuario actual, sin interfaz, aparte de
   los ajustes del plugin, escrito en un fichero temporal y movido a su sitio. Se guarda el nombre
   visible; el email, no. Si no se puede descifrar, se borra y el plugin queda sin sesión.
4. En cada carga: una comprobación de la cuenta, y una renovación antes de cualquier otra llamada con
   sesión cuando toca (el token nuevo se guarda antes de usarlo). `401 token_rotated` vuelve a leer el
   token guardado o renueva, y reintenta una vez; cualquier otro 401 termina la sesión (se borra el
   almacenamiento; Trigi conserva el texto que se estaba escribiendo) salvo que para entonces haya
   guardado un token más nuevo; `403 suspended` o `account_inactive` conserva el token pero bloquea
   Enable; los errores de red, los 5xx o un servidor antiguo mantienen la sesión, y Enable funciona
   sin conexión. La caducidad local nunca bloquea Enable: decide el servidor cuando responde. Una
   renovación que necesita una llamada de "Reportar un problema" se hace en una tarea de
   `AuthManager` que solo cancela la descarga del plugin, y su token se guarda en cuanto el servidor
   responde: cerrar el diálogo a mitad no puede perder el token nuevo (el antiguo, usado otra vez,
   parecería robado al servidor, que entonces termina la sesión).
5. Cerrar sesión envía la revocación en segundo plano con un timeout corto y borra el almacenamiento
   local sea cual sea el resultado. Gana a todo lo que siga esperando al servidor (una renovación, el
   cambio de la v0.5.x, una espera de inicio de sesión): el almacén cuenta los cierres de sesión, un
   token conseguido a través de uno nunca se guarda, y además se revoca. Así un cierre de sesión
   nunca se deshace en el siguiente arranque.
6. Migración desde la v0.5.x: cuando solo existe el inicio de sesión antiguo en texto plano, se cambia
   una vez por un token del plugin; si sale bien, el fichero antiguo se sobrescribe con ceros y se
   borra; un 401 lo borra y termina la sesión; los errores de red, los 5xx, un servidor antiguo o un
   límite lo conservan para el siguiente arranque (Enable sigue funcionando mientras tanto, pero Trigi
   y los tickets con sesión no usan el inicio de sesión antiguo). Si ya hay un inicio de sesión
   cifrado (o uno ilegible), un fichero antiguo que haya quedado se sobrescribe y se borra al cargar.

Ni el dock ni el log muestran nunca el enlace, los códigos, el secreto ni el token.

**Trigi (`support-client.*`, `support-service.*`, `trigi-page.*`, `trigi-states.*`).**
- Solo se muestra en Windows, con sesión, cuando la comprobación de la cuenta dice que Trigi está
  disponible y la versión del plugin es al menos la mínima que indica (comparación numérica
  `X.Y.Z`). Si no, el diálogo se abre en el formulario de ticket; sin sesión ofrece "Inicia sesión
  para preguntar a Trigi".
- El aviso del primer uso (versión 1) hay que aceptarlo antes de la primera pregunta. La versión
  aceptada se guarda en local, y una versión mayor vuelve a mostrar el aviso.
- La conversación se crea de forma perezosa, con la primera pregunta; abrir el diálogo no hace
  ninguna llamada a Trigi. Su cuerpo de creación lleva exactamente el idioma, el cliente
  (`obs_dynamic_delay`, versión, SO), la versión del aviso y, solo si la casilla está marcada, los
  datos técnicos. El ID de la conversación solo vive en memoria.
- Como mucho hay una petición de Trigi en vuelo; un contador de generación descarta las respuestas de
  una conversación anterior o de un diálogo cerrado. Las llamadas van como trabajos en segundo
  plano (abajo) y vuelven al hilo de UI; nada se ejecuta en el hilo gráfico de OBS.
- Tres formas de respuesta: una respuesta (texto más una acción opcional), un rechazo o "no
  disponible". Una acción hacia un ticket de Dynamic Delay o hacia una persona se convierte en "Crear
  ticket con esta conversación"; un ticket de cualquier otra categoría, en "Abrir soporte en
  trigglow.com". Los eventos de valoración, "resuelta", "abandonada" (se envía en segundo plano al
  cerrar un diálogo con preguntas y sin ticket) y paso a una persona no esperan respuesta.
- Los errores se traducen en estados de la página (`trigi-states.*`, probado sin Qt): sin conexión y
  "ha tardado demasiado" con reintento manual, error del servidor, límite (Enviar desactivado durante
  la cuenta atrás de `Retry-After`), sesión terminada, conversación cerrada (una nueva con la
  siguiente pregunta), Trigi apagado (se abre el formulario con el texto), versión demasiado antigua
  (el formulario más "Descargar la actualización"), cuenta no activa. Una pregunta que no llegó se
  marca como "No enviado".
- **Datos técnicos v1** (`support-diagnostics.*`, `diagnostics-collector.*`): opcionales,
  desmarcados por defecto y recordados en local, con vista previa del JSON exacto. Solo números,
  booleanos y valores fijos (versiones, arquitectura, renderizador, fabricante de la GPU a partir del
  ID numérico del adaptador, RAM total, familia del codificador, estado y ajustes del delay,
  lienzo/salida, fps, contadores de retraso de OBS, salud del pipeline, hasta 5 códigos de problemas
  recientes), cada número acotado a su rango y con 2 decimales como mucho, 1 KB como máximo. El tipo
  no tiene ningún campo que pueda guardar un nombre, una ruta, una URL, una clave, un usuario, el
  nombre de la GPU, un ID de hardware ni una IP. La instantánea se toma en el hilo de UI sin esperar al
  hilo gráfico y se envía una vez, al crear la conversación. Un campo o código nuevo implica la
  versión 2, que solo se envía si el servidor dice que la acepta.
- **Render** (`chat-text.*`, sin Qt, con tests): quita los caracteres de control salvo el salto de
  línea, los de dirección bidi, los emojis y pictogramas y el UTF-8 inválido; recorta a 4.000
  caracteres; parte el texto en líneas (párrafo, viñeta `- `, paso `N. `) y segmentos (texto,
  `**negrita**`, `` `código` ``); los encabezados y las vallas de código se quedan como texto. La
  página construye el texto enriquecido solo con segmentos escapados, `<b>` y `<span class="code">`
  (`trigi-widgets.*`), nunca con formatos de texto automáticos ni Markdown, nunca con enlaces
  externos, y su documento nunca carga un recurso. Las URLs de una respuesta se quedan como texto.

**Tickets y el log de OBS (`report-bug-dialog.*`, `bug-report.*`, `log-redact.*`).**
- Con sesión: un ticket de la categoría `dynamic_delay` con asunto, mensaje (que termina en un pie
  visible y editable con las versiones del plugin, de OBS y del SO) y, tras una conversación, un
  enlace a ella. Solo se lee el ID del ticket, que ni se muestra ni se escribe en el log. Después, si
  la casilla está marcada, el log va como una sola parte `text/plain` llamada `obs-log.txt`; si eso
  falla, el ticket ya existe y el diálogo ofrece "Guardar log".
- Invitado: el diálogo pregunta primero si trigglow.com exige una comprobación humana a los
  invitados; si la exige (o se rechaza un ticket de invitado por ese motivo), no muestra el
  formulario de invitado sino "inicia sesión o usa el formulario web", sin reintentar. El adjunto de
  invitado envía su clave de acceso en una cabecera, nunca en la URL, y después el ticket se abre en
  el navegador.
- El log: solo el log actual del OBS en el que corre el plugin (que solo se localiza en Windows).
  Ninguna llamada de libobs ni del frontend da ese fichero, y el más reciente de una carpeta de logs
  puede ser de otro OBS (uno portable, una segunda instancia), así que al cargar el plugin escribe
  una línea con una marca aleatoria y después solo lee el fichero, entre las carpetas de logs de este
  OBS, que la contiene; si no aparece, no se adjunta nada. El log se lee y se redacta en memoria,
  línea a línea, con escáneres lineales en vez de `std::regex` (`src/log-redact.hpp` enumera las
  reglas): URLs de stream, cualquier otra URL (se conserva el host), nombres de usuario en rutas de
  Windows/macOS/Linux (también perfiles movidos, recursos de red y carpetas de OneDrive de empresa),
  enlaces de overlay, emails (también con acentos y con la '@' codificada), valores tras nombres con
  forma de clave (`client_secret`, `x_api_key`, claves JSON), cabeceras de credenciales, JWT,
  direcciones IPv4/IPv6, tokens largos, grupos con forma de clave de stream y base64, y además, en lo
  posible, nombres de dispositivos y títulos de ventanas entre comillas; las marcas de tiempo, las
  versiones y los atajos se mantienen. Si pasa de 4,5 MB después de redactar, se quedan los primeros
  256 KB, una marca y el final. La casilla "Adjuntar el registro (log) de OBS" empieza desmarcada y
  solo se puede marcar después de ver la vista previa (las primeras y las últimas 200 líneas), que
  avisa de que pueden quedar nombres de escenas, fuentes, dispositivos de audio y ventanas. Ya no se
  escribe ninguna copia, y las que dejaron versiones anteriores en `%TEMP%\Trigglow` se borran al
  cargar.

**Trabajos en segundo plano y descarga del plugin (`background-tasks.*`, `cancel-token.*`,
`ui-tasks.*`).** Cada trabajo de red (la comprobación de la sesión, la renovación y el cambio desde
la v0.5.x, la espera del inicio de sesión, Trigi, los tickets y sus adjuntos, la lectura del log
para un ticket o una vista previa, la comprobación de actualizaciones) va en su propio hilo con un
token de cancelación, dentro de un grupo cuyo dueño es `AuthManager`, un diálogo de soporte o el
dock. WinHTTP funciona en modo asíncrono, así que cancelar un trabajo corta su llamada al momento
(una respuesta de Trigi puede tardar hasta 90 s), y un timeout es un estado propio ("Trigi ha
tardado demasiado") en vez de "sin conexión". Los resultados llegan al hilo de UI por una única cola
que vacía el plugin; destruir un dueño cancela sus trabajos y descarta sus resultados, también los
que ya estaban en cola. `obs_module_unload()` lo cancela todo, rechaza trabajos nuevos y espera como
mucho 2 s; en Windows cada hilo de trabajo guarda una referencia a la DLL y termina con
`FreeLibraryAndExitThread()`, así que ningún código del plugin se ejecuta después de que OBS lo
descargue. Medido contra un sustituto local que retenía una respuesta de Trigi 90 s: la descarga
tardó como mucho 7 ms. Revocar el token al cerrar sesión y avisar a trigglow.com de que una
conversación con Trigi se abandonó no tienen dueño: solo la descarga los cancela. Lo prueba
`background_tasks_tests`.

**Qué llega al log de OBS.** `[auth-manager]` signed in, signed out, session ended, token renewed,
migrated legacy login; `[trigi]` session started, message sent (longitud, datos técnicos sí o no),
reply (código HTTP, longitud, acción), refused, unavailable, rate limited, errores HTTP por código;
`[support]` ticket created (con o sin el log). Nunca el token, el enlace, los códigos ni el secreto,
IDs de conversaciones o de tickets, la clave de acceso de un ticket, el texto de las preguntas o las
respuestas, los datos técnicos, un email o un nombre, ni rutas absolutas.

## 4. v0.3.0: compresión real MJPEG del ring de RAM

La aritmética del presupuesto de RAM del §3.5 anterior sigue asumiendo un ring sin comprimir para el
mecanismo central del §3 — eso era exacto para v0.2.0, pero a partir de v0.3.0 cada frame del buffer
se comprime además con MJPEG al capturarlo y se descomprime al reproducirlo, en las plataformas donde
eso está disponible. Esta sección describe ese pipeline; desde la v0.6.0 se ejecuta en hilos propios
(§4.6) sobre un ring de cabeceras de slot y una arena por bloques, en vez de dentro de `Render()`
sobre un buffer por slot.

**Alcance de plataforma:** solo donde está definido `TRIGGLOW_HAVE_FFMPEG` — Windows y Linux, vía un
build de FFmpeg vendorizado, de release fijada, de BtbN LGPL-shared (`avcodec`/`avutil`/`swresample`;
nunca "latest", para mantener los builds reproducibles). Todavía no existe un build estático de
FFmpeg de confianza equivalente para macOS, así que macOS no forma parte de esto: guarda los frames
sin comprimir (I420 en crudo desde la v0.6.0, `MakeRawCodec()`). En cualquier sitio donde el códec no
esté disponible (macOS hoy, o un build sin FFmpeg enlazado), todos los caminos de código de abajo
caen automáticamente al almacenamiento sin comprimir — la compresión es una adición sobre el
mecanismo existente, nunca un requisito nuevo para que funcione.

**Desde la v0.7.0** el build de Windows guarda el buffer con un códec inter-frame por hardware
(HEVC, o H.264 en una GPU sin HEVC, con NVENC, AMF o Quick Sync, decodificado con D3D11VA) donde
funcione, y con MJPEG si no: §4.8. MJPEG sigue siendo el códec de Linux, de Windows sin un encoder
por hardware utilizable, de los momentos en que se prepara uno y de cada vuelta a MJPEG; §4.1–§4.7
lo describen, junto con el pipeline que comparten los dos códecs.

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

*v0.7.0:* MJPEG suponía entre el 80 y el 85 % de la RAM del plugin a 1440p60, y un encoder HEVC por
hardware guarda los mismos frames con entre un 67 y un 74 % menos de bytes y mejor imagen, así que
el ring aprendió a manejar GOP (§4.8): cada paquete lleva su códec, si es keyframe, y su cadena y su
seq; un P-frame solo se decodifica justo después de su referencia; el ring guarda un GOP de holgura
para que el keyframe del frame que toca siga siempre ahí; y un decoder que arranca, se reanuda o
salta se pone al día desde ese keyframe sin mostrar nada. "Cualquier slot, en cualquier momento"
pasó a ser "cualquier keyframe, en cualquier momento", y MJPEG, con todos sus paquetes keyframe,
sigue funcionando exactamente como se describe aquí.

### 4.2. Flujo de codificación/decodificación

El códec está detrás de una pequeña interfaz: hasta la v0.6.0 `VideoCodec`
(`src/codec-pipeline.hpp`), y desde la v0.7.0 un `storage::Encoder` y un `storage::Decoder`
separados (`src/storage-codec.hpp`, §4.8). Cada instancia MJPEG la abre, usa y cierra un solo hilo.
En las plataformas con `TRIGGLOW_HAVE_FFMPEG` es MJPEG (`src/mjpeg-codec.*`,
`MjpegEncoder`/`MjpegDecoder` desde la v0.7.0); en el resto, el códec sin comprimir guarda los
frames tal cual.

- **Codificar:** los planos I420 leídos de vuelta de la GPU (§3.1, paso 3) se copian en un `AVFrame`
  reutilizado (`AV_PIX_FMT_YUV420P`, etiquetado `AVCOL_RANGE_MPEG` — el rango de estudio que escribe
  el shader; el codificador MJPEG de FFmpeg solo acepta esa etiqueta con `strict_std_compliance =
  FF_COMPLIANCE_UNOFFICIAL`, y son solo metadatos: un comentario de 14 bytes, planos decodificados
  idénticos) y se pasan por un `AVCodecContext` MJPEG con calidad fija vía `AV_CODEC_FLAG_QSCALE`
  (`qmin` = `qmax` = `kMjpegQuality = 5` — un punto dulce de MJPEG comúnmente citado, visualmente
  cercano a sin pérdidas mientras sigue comprimiendo de forma significativa; no es un ajuste del
  dock). El codificador se reabre de forma perezosa cuando cambian el tamaño del frame o el número de
  hilos de slice (del orden de 1 ms, en el hilo que comprime). El hilo de compresión copia el paquete
  en la arena del ring y publica el slot (§4.6).
- **Decodificar:** el hilo de descompresión fija (pin) el slot, copia su paquete, lo suelta y lo
  decodifica fuera de todo lock. El frame decodificado es el propio `AVFrame` del decodificador, que se
  sube plano a plano con su propio stride (sin copia ni reentrelazado) y vuelve al pool de buffers de
  FFmpeg al soltarlo.
- **Fallback:** si el codificador no se puede abrir a ese tamaño o un frame falla al comprimirse, el
  frame se guarda en crudo (I420 compacto) en la arena; los frames en crudo se cuentan
  (`rawFallback`) y se muestran sin decodificar. La compresión, por tanto, nunca es un requisito duro
  para que el ring funcione — solo una optimización de RAM añadida encima.

**Dónde se ejecuta esto y cuánto cuesta (v0.6.0).** Hasta la v0.5.0 tanto la codificación como la
decodificación se ejecutaban de forma síncrona dentro de `Render()`, en el hilo gráfico de OBS — el
hilo que tiene que terminar cada frame en 1/fps (16,7ms a 60fps) — y a la resolución del lienzo:
14,3/17,4ms p50/p95 por frame a 1440p, 8,6/10,4ms a 1080p (benchmark fuera de OBS, mismo FFmpeg).
Desde la v0.6.0 se ejecutan en los hilos de códec (§4.6), y el hilo gráfico solo conserva las pasadas
de conversión, la copia de la lectura y la subida: ~1,2–1,7/1,6–2,1ms p50/p95 por frame a 1440p60 en
el benchmark de hilos fuera de OBS (Ryzen 7 7700X), con compresión ~8,8/10,4ms y descompresión
~6,2/7,5ms en sus propios hilos.

- **Hilos de slice, de forma explícita.** Un contexto de compresión puede usar 1 o 2 hilos de slice
  desde la v0.7.0 (1, 2 o 4 antes; cada hilo de slice tiene sus propios buffers de codificación,
  ~15MB a 1440p y ~33MB a 2160p) (`thread_type = FF_THREAD_SLICE`, `thread_count = k`); cuántos lo
  decide la política de degradación (§4.7), y en modo síncrono (§4.6) empieza en
  `hardware_concurrency() / 4` acotado a [1, 2] (`CodecPipeline::kMaxSlicesPerContext = 2`), porque
  ahí reducen el tiempo real de la compresión en el hilo gráfico (compresión a 1080p de 5,1 a 1,8ms
  p50 con 4 slices en la v0.6.0, unos 3ms con 2, el mismo CPU total, paquetes ~3,5% más grandes).
  `thread_type` hay que fijarlo de forma explícita: con solo `thread_count > 1`, FFmpeg elige
  threading por frames para MJPEG (`QSCALE` se salta su excepción de un solo hilo),
  `receive_packet()` devuelve entonces `EAGAIN` y todos los frames se guardarían en crudo. Si
  `active_thread_type` sigue indicando threading por frames, el codificador se reabre con un solo
  hilo (WARN). El decodificador se queda en `thread_count = 1` (el decodificador MJPEG de n9.0 no
  tiene threading). La línea de log `MJPEG encoder open` indica tamaño, qscale, tablas de Huffman,
  hilos de slice pedidos y concedidos, `active_thread_type` y las CPU lógicas.
- **Tablas de Huffman estándar.** El codificador se abre con `huffman=default` (`av_opt_set()` sobre
  sus opciones privadas) en vez de las tablas "óptimas" que FFmpeg usa por defecto, que necesitan una
  segunda pasada por frame. Con más de un slice FFmpeg ya usa las tablas estándar, así que esto importa
  para los contextos de un solo slice con los que empieza el pipeline de hilos: un 10-17% menos de
  tiempo de compresión (p50-p95, 1440p, n9.0.2 vendorizado) a cambio de paquetes un 3-3,5% más
  grandes. La codificación Huffman no pierde información, así que los frames decodificados son
  idénticos, y cada paquete sigue llevando sus tablas (DHT). La línea de log de apertura dice
  `huffman=default`, o por qué no se pudo fijar.
- **Cada frame se decodifica y se sube como mucho una vez.** `Render()` se ejecuta una vez por vista
  de la escena (Programa, vista previa, proyectores, multiview). El hilo de descompresión decodifica
  cada frame una vez, y `Render()` solo sube un frame decodificado si no está ya en las texturas de
  reproducción; el resto de vistas solo las vuelven a dibujar. Si el frame que toca no está listo
  (descartado, todavía comprimiéndose o no descomprimido a tiempo), se vuelve a dibujar el último
  frame subido en este ciclo (un "hold") en vez de nada; el siguiente frame se sigue eligiendo por
  antigüedad, así que un hold es un tirón de un frame, nunca una deriva del delay. Mientras el ring se
  está llenando no se dibuja nada. La caché de reproducción se invalida al final de cada ciclo y en
  cada redimensionado, así que un hold nunca puede mostrar un frame de un ciclo anterior ni una
  textura destruida.

### 4.3. Memoria: slots presupuestados (v0.3.0–v0.5.0) y después una arena por bloques (v0.6.0)

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

**Arena por bloques (v0.6.0).** Los buffers por slot tenían dos costes más. Se rellenaban con ceros
al dimensionarlos (`assign(budgetedBytes, 0)`), lo que tocaba todas las páginas del ring de golpe en
el hilo gráfico: ~1,9GB residentes a 1440p60 y un parón de ~381ms (unos 23 frames perdidos) en el
primer `Render()` tras `Enable()`. Y liberar el ring suponía un `VirtualFree` por slot (1816 a
1440p60, 87ms medidos, 141ms en el log de un usuario), también en el hilo gráfico. Desde la v0.6.0 los
datos viven en una `FrameArena` (`src/frame-arena.*`): los paquetes se asignan uno detrás de otro en
bloques, sin rellenar con ceros, y un paquete nunca cruza dos bloques. Cada bloque cuenta sus
asignaciones vivas; cuando el ring ha sobrescrito todos los frames que tenía un bloque, el bloque se
recicla entero. Como el ring libera los frames más o menos en el orden en que los escribió, la
memoria comprometida y la residente se quedan cerca de la ventana real de paquetes más uno o dos
bloques: ~576-768MB para 30s a 1440p60 en la simulación fuera de OBS (de contenido normal a pesado),
frente a 1923MB residentes antes. Las asignaciones y liberaciones pueden venir de cualquier hilo y en
cualquier orden (los hilos de compresión asignan, el ring libera al reutilizar un slot). `Free()`
nunca llama al sistema: los bloques vacíos van a una lista de reserva, y `Trim()`, la única llamada
que devuelve memoria (las reservas por encima de dos), se ejecuta en el hilo liberador (§4.6).
Dimensionado (`ArenaConfigFor()`): bloques de alrededor de 1/16 de los bytes esperados del ring,
entre 4 y 32MiB y nunca menos de dos frames en crudo (sitio para el fallback a crudo); un límite duro
en el presupuesto de RAM (o en lo que se dimensionó el ring, si es más), por encima del cual una
asignación falla y el frame se descarta (`drops arena`) en vez de que la memoria crezca sin control.
`kAssumedCompressionRatio` ahora solo decide cuántos segundos caben en el presupuesto y el tamaño
esperado de la arena; ya no se reserva nada por slot.

**Bloques más pequeños (v0.7.0).** `FrameArena::SizedFor()` usa ahora bloques de 1/64 de los bytes
esperados del ring en MiB enteros, entre 4 y 8MiB, y guarda como mucho uno de repuesto
(`maxSpareBlocks = 1`); desaparece el mínimo de "dos frames en crudo por bloque", porque un frame en
crudo mayor que un bloque (2160p, el fallback a crudo) ya va por la vía de tamaño excesivo. La
memoria comprometida de más sobre los paquetes vivos baja de 35–71MB a unos dos bloques (la cola
libre del bloque actual, la cabeza ya liberada del más antiguo y el de repuesto). Sin FFmpeg todos
los paquetes son frames en crudo, así que ahí los bloques guardan un número entero de ellos. Los
bytes esperados siguen la clase de almacenamiento del ciclo (§4.8): `slots × crudo / 5` para MJPEG y
`/ 12` para el códec por hardware.

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
gameplay (~11-15x, ver la nota de reajuste del §4.3), y desde la v0.6.0 el ratio forma parte de la
línea `pipeline check` (§4.5), medido en la sesión de cada usuario. Sigue siendo un dato por sesión y
tipo de contenido, no una garantía.

### 4.5. v0.6.0: telemetría `perf check` y `pipeline check`

Para que un reporte de rendimiento se pueda comprobar en el log del usuario en vez de suponerlo,
`VideoDelayFilter` escribe dos líneas INFO cada 10 segundos de render (`kPerfLogIntervalNs`), durante
`Filling` y `Active`. Sustituyen a la línea `compression check` de cada 300 frames:

```
perf check (periodic, 10.0s): buf=1920x1080 (capped to OBS output 1920x1080, canvas 2560x1440)
fps=60.00 | render p50/p95/max=…ms own=… readback=… upload=… | calls=…/s captures=…/s uploads=…/s
holds=N (late decode N, dropped N, encoding N) | obs frames+N lagged+N skipped+N

pipeline check (periodic): mode=workers codec=mjpeg rung=0/4 capture=1920x1080@60.00fps enc=1x1 L=3
S=3 | enc0 p50/p95/max=…ms busy=…% enc1=… busy=…% dec=… busy=…% | drops reserve=N backpressure=N
arena=N capture=N rawFallback=N encFail=N decFail=N staleEnc=N decJumps=N staleDec=N | queue enc
max=N avg=… dec ahead avg=… min=N | ratio=…x arena=…MB committed (live …MB, peak …MB, N blocks of
…MB, limit …MB) ring=N slots (ready N, dropped N, in flight N, pinned N)
```

(dos líneas en el log; partidas aquí. `codec=` desde la v0.7.0, seguidas de una tercera línea,
`storage codec check`, §4.8.)

- **`perf check` — el hilo gráfico.** La resolución del buffer a calidad completa y el motivo —
  `capped to OBS output WxH, canvas WxH`, `not capped, source WxH, output WxH` o
  `cap off (buffer_output_cap=false), …` — con `, reduced for RAM` cuando entró en juego la regla 2
  del §3.5; los fps exactos; tiempo real (`os_gettime_ns`) de la llamada completa a `Render()`
  (`render`), de la parte propia del plugin (`own`: todo menos renderizar la escena en directo en la
  textura de captura, que OBS renderizaría de todos modos — lo que juzga la política de degradación,
  §4.7), de la lectura de staging con su copia y de la subida de texturas; llamadas a `Render()`,
  capturas y subidas por segundo; holds (§4.2) separados por el estado del slot que tocaba (Ready pero
  no decodificado a tiempo, Dropped, todavía Encoding); y los incrementos de los contadores propios de
  OBS en la misma ventana — frames totales (`obs_get_total_frames`), frames perdidos por retraso de
  renderizado (`obs_get_lagged_frames`) y frames saltados por retraso de codificación
  (`video_output_get_skipped_frames`).
- **`pipeline check` — los hilos y la memoria.** Modo (`workers`, o `sync` con el interruptor de
  emergencia, §4.6), el códec que guarda ahora los frames (v0.7.0: `mjpeg`, el nombre de FFmpeg del
  encoder por hardware, como `hevc_nvenc`, o `raw` sin FFmpeg), peldaño de degradación y el último,
  el tamaño y el ritmo que se capturan ahora, contextos de compresión × hilos de slice,
  descompresión anticipada (`L`) y slots de staging (`S`); p50/p95/max por frame y proporción
  ocupada de cada contexto de compresión y del descompresor; descartes (sin slot libre, compresión
  atrasada, arena en su límite, lectura fallida), fallbacks a crudo, fallos de códec, compresiones
  obsoletas por un slot reutilizado, saltos del descompresor, frames decodificados descartados por
  obsoletos tras una pausa del render; profundidad de la cola de compresión y frames decodificados
  por adelantado; ratio de compresión; memoria comprometida, viva y pico de la arena, bloques,
  tamaño de bloque y límite; y los estados de los slots del ring.
- **Muestras:** p50/p95 mediante `std::nth_element` sobre un array fijo de 4096 muestras por serie,
  máximo exacto. Sin asignaciones por frame ni por ventana. Las tasas de frames descartados y
  repetidos de la última ventana alimentan también el aviso de degradación del dock (§4.7).
- **Ventanas:** una pausa de más de 1s entre llamadas a `Render()` (filtro oculto, OBS atascado)
  cierra antes la ventana actual (`resumed after a pause`), para no diluir sus tasas por segundo. La
  última ventana parcial se registra al desactivar el delay (`filter disabled`, desde la tarea
  `release_buffers`), al ponerlo a 0s (`delay set to 0`) y al destruirse el filtro con el delay
  activo, p. ej. al cerrar OBS (`filter destroyed`; seguro en el destructor porque OBS solo destruye
  una fuente después de desengancharla, y las líneas solo leen miembros propios del filtro). Antes de
  la v0.6.0 el filtro en la práctica nunca se destruía: `ObsFrontendBridge` perdía una referencia a él
  en cada `Enable()` (`obs_source_get_filter_by_name()` devuelve una referencia nueva); ahora cada
  búsqueda la guarda en un `OBSSourceAutoRelease`, y el destructor, que libobs ejecuta en su hilo de
  destrucción, espera a que terminen los hilos de códec y después entra en el contexto gráfico para
  liberar los objetos de GPU. Las ventanas de menos de 1s se descartan.
- **Otras líneas:** `delay pipeline: …` al empezar cada ciclo (modo, tamaño del buffer, slots, delay,
  la clase de almacenamiento con su ratio supuesto y su margen de GOP desde la v0.7.0, contextos de
  compresión × slices, descompresión anticipada, slots de staging, la escalera de degradación hasta
  su último peldaño, tamaño de bloque y límite de la arena); un WARN por cada cambio de peldaño
  (§4.7); `settings loaded` y `enabled` con `output_cap=on|off codec_workers=on|off` y, desde la
  v0.7.0, `buffer_codec=auto|mjpeg` (`enabled` también da la `storage_class` del ciclo). Las líneas
  propias del códec de almacenamiento están en §4.8.

Cómo leer cada campo desde el lado del usuario: `docs/TROUBLESHOOTING-es.md`, "Cómo leer las líneas
del log".

### 4.6. v0.6.0: hilos de códec, ring y arena

Invariantes del diseño: el delay de vídeo es exacto por reloj de pared, los frames se eligen por
antigüedad de captura, y vídeo y audio comparten un único delay en segundos enteros (§3.3); "nunca
corta": en `Active` el ring solo se vacía por un cambio real de geometría; todo lo que es `gs_*` se
queda en el hilo gráfico (los hilos de códec nunca tocan la GPU); y cada `AVCodecContext` lo usa un
solo hilo, que también lo abre y lo cierra (desde la v0.7.0 uno por hardware se abre en el hilo de
preparación y se entrega, siempre a un solo hilo a la vez: invariante I8, §4.8).
`src/codec-pipeline.*`, `src/frame-ring.*` y `src/frame-arena.*` son C++ puro (std::thread, sin OBS,
con el códec inyectado), así que los tests los manejan sin OBS ni FFmpeg.

**Hilos.**

| Hilo | Hace | Nunca hace |
|---|---|---|
| Hilo gráfico de OBS (`Render()`) | Renderiza y convierte la captura, recoge el slot de staging que se lanzó dos capturas antes en un frame crudo de un pool (`kMaxEncodeContexts + 2` frames), reserva el slot del ring, envía el trabajo de compresión; elige el frame decodificado que toca, lo sube si es nuevo y lo dibuja; ejecuta la política de degradación cada 250ms | Comprimir, descomprimir, (des)entrelazar, liberar el ring, esperar a un hilo |
| Hilos de compresión (1-2, `trigglow-enc-N`) | Sacan trabajos de una cola FIFO acotada, comprimen fuera de todo lock con su propio contexto de códec, copian el paquete en la arena y publican el slot | Tocar la GPU, compartir un contexto de códec |
| Hilo de descompresión (1, `trigglow-dec`) | Decodifica en orden de captura hasta `L = 3` frames por delante de lo que hay en pantalla, a partir del objetivo de reproducción que publica el hilo gráfico (`ahora − delay`, predecible por el reloj); salta directamente al frame que toca cuando se queda atrás; en los últimos 250ms de la ventana de llenado se le apunta a los frames más antiguos (`PrimeDecode()`), para que el primer frame retrasado esté listo cuando toca; guarda `L + 2` frames decodificados | Decodificar frames que nunca se van a mostrar |
| Liberador (1, `trigglow-reap`) | Libera lo que nunca debe liberarse en el hilo gráfico ni frenar una compresión: rings desenganchados y sus arenas (600-770MB a 1440p, decenas de ms), el pool de frames crudos tras Disable, frames decodificados descartados al desenganchar; ejecuta el `Trim()` de la arena; desde la v0.7.0 también cierra los encoders y decoders por hardware retirados | Cualquier otra cosa |
| Preparación del códec (1, `trigglow-codec-setup`, v0.7.0, se crea al primer uso) | Abre y sondea un encoder por hardware al tamaño de frame que pide el hilo de compresión 0, y lo entrega con un decoder ya calentado (§4.8) | Codificar o decodificar un frame capturado; retrasar una captura |

**Ring y estados de slot.** `FrameRing` es un número fijo de cabeceras de slot más la arena. Cada
slot está `Free`, `Reserved`, `Encoding`, `Ready` o `Dropped`, y quien es dueño de un estado es el
único que toca los datos del slot en él: el hilo gráfico reserva (`Free/Ready/Dropped → Reserved`, en
orden de captura, con una generación nueva y única entre rings), un hilo de compresión lo toma y lo
publica (`Reserved → Encoding → Ready`) solo si la generación sigue coincidiendo, y el descompresor
fija un slot `Ready` para copiar su paquete sin el lock. `captureNs` se asigna al reservar, así que las
marcas de tiempo nunca decrecen del slot más antiguo al más nuevo termine en el orden que termine la
compresión, y la búsqueda binaria de la reproducción sigue siendo válida con slots
`Reserved/Encoding/Dropped` por medio; para mostrar se elige el slot `Ready` más nuevo en el objetivo
o antes, mirando hacia atrás unos pocos slots no listos (una repetición de un frame anterior, nunca
uno del futuro). Un mutex protege solo las cabeceras y los índices, y se mantiene para trabajo
O(1)/O(log n); las copias, compresiones, descompresiones y liberaciones ocurren fuera de él, y nunca
se anida con los locks del pipeline ni de la arena.

**Contrapresión.** Sin frame crudo libre (la compresión va atrasada): el hilo gráfico descarta la
captura **más nueva** — no se desperdicia nada ya hecho, la cola nunca crece, y la reproducción por
antigüedad simplemente repite el frame anterior. Sin slot libre (un hilo de compresión o el
descompresor un ring entero por detrás), o con la arena en su límite: igual, el frame se descarta.
Todo se cuenta (`drops …`) y, si se mantiene, es tarea de la política de degradación (§4.7). Tras una
pausa del render (más de 4 intervalos de captura desde la llamada de reproducción anterior), los
frames decodificados que se quedaron muy por detrás del que toca se descartan sin mostrarse
(`staleDec`) y se mantiene el frame en pantalla hasta que el descompresor, que salta al frame que
toca, lo tiene listo — en vez de enseñar durante un momento frames cientos de ms más viejos que el
delay.

**Ciclo de vida.** El primer `Render()` tras Enable dimensiona el ring (`EnsureRingSized()`),
construye la escalera de degradación y engancha un `FrameRing` nuevo al pipeline; los hilos se crean
de forma perezosa la primera vez y viven lo que el filtro, esperando en variables de condición (sin
coste en reposo). Disable: §3.4. Un **redimensionado duro** — nuevo tamaño de buffer, número de slots,
intervalo de frame, delay efectivo o valor del interruptor de emergencia con frames en el ring —
termina el ciclo (el ring viejo al liberador) y empieza uno nuevo, con un WARN y la señal
`buffer_restarted` (§3.3). Un **cambio suave** — un peldaño de degradación o el paralelismo de
compresión — conserva el mismo ring y el mismo número de slots, así que el tramo y el audio no se
tocan: los slots de staging y los frames crudos llevan su propio tamaño, cada compresor reabre su
contexto de forma perezosa, el decodificador MJPEG se adapta al tamaño de cada frame y las texturas de
reproducción se recrean cuando cambia el tamaño del frame subido. Destrucción (hilo de destrucción de
OBS, nunca el hilo gráfico): parar y esperar a los hilos (cada uno cierra su códec; la espera dura
como mucho una compresión o descompresión, o la liberación de un ring) y después liberar los objetos
de GPU dentro de un contexto gráfico.

**Interruptor de emergencia: `codec_workers` (oculto).** `"codec_workers": false` en
`buffer-mode-settings.json` pone el pipeline en modo síncrono: `SubmitEncode()` y
`AcquireForDisplay()` ejecutan el códec en línea en el hilo gráfico, como hasta la v0.5.0 pero con
slices, sobre el mismo ring y la misma arena; el liberador sigue liberando la memoria fuera del hilo
gráfico (y si no puede arrancar, la memoria se libera en línea). La degradación sigue activa, y añade
hilos de slice antes de bajar de peldaño. Mismas reglas que `buffer_output_cap` (§3.5): solo se lee
al cargar OBS con `ReadHiddenSwitch()`, solo un `false` literal de JSON lo desactiva (cualquier otro
tipo lo deja activado con un WARN y desaparece del archivo al salir), solo se vuelve a escribir
cuando es `false`, se envía al filtro en cada `Enable()`
(`ObsFrontendBridge::SetBufferFilterCodecWorkers()`), sin propiedad en el filtro ni control en el
dock. Un valor distinto del del pipeline en marcha crea un pipeline nuevo en el siguiente ciclo (los
hilos del anterior se esperan en el destructor, nunca en el hilo gráfico). Si no se puede arrancar un
hilo de códec, el pipeline pasa solo a modo síncrono. Aparece como `codec_workers=on|off` en las
líneas `settings loaded` y `enabled`, y como `mode=` en `pipeline check`. Desde la v0.7.0 el modo
síncrono guarda siempre MJPEG: el códec por hardware necesita los hilos asíncronos, así que
`codec_workers=false` manda sobre `buffer_codec` (la línea `enabled` lo dice).

**Medido** (benchmark de hilos `bench-workers` fuera de OBS, Ryzen 7 7700X, FFmpeg n9.0.2
vendorizado, contenido sintético tipo gameplay, 20s por ejecución, un contexto de compresión y un
slice, p50/p95 en ms):

| Caso | Total en el hilo gráfico | Compresión | Descompresión | Ocupación compresión / descompresión | Núcleos de CPU en total |
|---|---|---|---|---|---|
| 1440p60 normal | 1,2–1,7 / 1,6–2,1 | 8,8–9,6 / 10,4–11,2 | 6,2–6,5 / 7,5–7,6 | 53–57% / 37–39% | ~1,0 |
| 1440p60 pesado | 1,3 / 1,6 | 10,0–10,3 / 12,5–12,7 | 6,7–6,8 / 8,1–8,3 | 61–62% / 41–42% | ~1,1 |
| 1080p60 normal | 0,6 / 0,9 | 5,1 / 6,2 | 3,6 / 4,3 | 30% / 22% | ~0,5 |
| 1440p60, modo síncrono, 4 slices | 10,6 / 12,2 | — | — | — | ~1,0 |

Las cifras del hilo gráfico son una aproximación (copias a memoria normal); la lectura más la subida
reales en D3D11 midieron 0,47/0,58ms a 1440p en una RX 7800 XT. Todavía no se ha medido en directo
dentro de OBS, ni en GPU NVIDIA.

**Tests.** `frame_arena_tests` (asignación secuencial, liberaciones desordenadas, reciclado de
bloques, el límite, datos de tamaño excesivo, `Trim()`, un régimen FIFO estable, una ejecución con
varios hilos comprobando el contenido), `frame_ring_tests` (máquina de estados, generaciones, fallos
de reserva, selección por antigüedad con slots descartados por medio, descompresión anticipada,
liberación de datos) y `pipeline_stress_tests`: un hilo gráfico simulado a 250fps (con el retraso de
staging de 2 frames) que en momentos al azar desengancha y vuelve a enganchar (Disable,
redimensionado duro), cambia el paralelismo, el tamaño del frame (cambio suave de peldaño) y el ritmo
de captura, con un códec falso que marca cada frame con su hora de captura. Comprobaciones duras: un
frame mostrado nunca es más joven que el objetivo de reproducción, las horas de captura mostradas
solo crecen, el contenido de cada frame coincide con su marca de tiempo, sin bloqueos (watchdog), sin
fugas en la arena, todos los rings se liberan y ninguno en el hilo gráfico simulado. También cubre una
pausa del render de 300ms y un descompresor 2,5 capturas lento.

### 4.7. v0.6.0: degradación automática

`src/degradation-policy.*` (pura, `tests/degradation_policy_tests.cpp`) decide, solo a partir de
números, cuánto paralelismo tienen los compresores y en qué **peldaño de calidad** se captura el
buffer, para que un equipo que no da más baje la calidad de la imagen retrasada en vez de retrasar a
OBS o cortar el delay.

**Los dos mandos.**
- *Paralelismo* (contextos de compresión × hilos de slice): con hilos de códec `1×1 → 2×1 → 2×2`
  desde la v0.7.0 (`→ 2×4` antes); en modo síncrono `1 → 2` slices en el único contexto (`→ 4`
  antes). El paso de partida se predice según la resolución: el primero cuyo p95 de compresión
  previsto (`1,1 × (0,3 + 2,3 ms por megapíxel)`, dividido entre 1,6 por cada duplicación de slices)
  no pase del 70% de lo que tienen los contextos por frame — 1440p60 empieza en 1×1 y 4K60 en 2×1;
  el modo síncrono empieza en `hardware_concurrency() / 4` slices, acotado a [1, 2]. El códec por
  hardware (§4.8) tiene un único paso, `1×1`: un solo contexto mantiene el bitstream en orden de
  captura, y los slices no le aportan nada. `SetParallelism()` cambia los pasos cuando cambia el
  códec en uso y conserva el peldaño.
- *Peldaño de calidad*: el peldaño 0 es el buffer dimensionado (§3.5); después pasos de ×0,83 en
  escala hasta la calidad mínima del usuario en píxeles del buffer (el mismo suelo que usó
  `bufferfit`; el propio suelo es el último peldaño de resolución, nunca por debajo); y después ese
  suelo capturado a la mitad de fps (una celda de captura de cada dos), solo cuando la salida va a
  48fps o más. Los anchos son múltiplos de 8 y las alturas pares. Cada peldaño es un cambio suave
  (§4.6): mismo ring, mismo número de slots, así que el delay nunca se mueve; en el peldaño a mitad de
  fps el tramo en tiempo no cambia y la reproducción por antigüedad muestra cada frame dos veces.

**Evaluación.** Cada 250ms, sobre ventanas de los últimos 2s (0,5s para la regla inmediata), y solo
2s después del cambio anterior (enfriamiento). Cada métrica necesita al menos 20 muestras (10 para la
regla inmediata) o se ignora.

| Disparador (bajada, un paso) | Condición | Acción |
|---|---|---|
| Hilo gráfico | Trabajo propio del plugin (`own`, §4.5): p95 > 40% del intervalo de frame, o p95 > 25% mientras OBS perdió frames en la ventana, o p50 de los últimos 0,5s > 50% (comprobado 0,5s después de un cambio) | Síncrono: primero más slices, después un peldaño más bajo. Con hilos: un peldaño más bajo (lo que queda en el hilo gráfico escala con los píxeles) |
| Compresión (con hilos) | p95 > 80% de lo que tienen los contextos por frame (`p95 / (intervalo de captura × contextos)`), o > 2% de capturas descartadas | Primero más paralelismo, un peldaño más bajo cuando está al máximo |
| Descompresión (con hilos) | p95 > 80% del intervalo de captura, o > 5% de los frames que tocan no decodificados a tiempo | Un peldaño más bajo (el decodificador MJPEG no tiene threading) |
| Subida (un paso) | La carga prevista un paso más arriba (p95 medido × proporción de píxeles, o × 1,6 por cada división a la mitad de los slices, × 1,1 de margen) se mantiene ≤ 25% del intervalo de frame en el hilo gráfico y ≤ 50% en los hilos de códec, durante 30s seguidos | Primero la calidad (`higher rung`), después se devuelve el paralelismo hasta el paso de partida |

El descompresor va `delay` segundos por detrás de la captura, así que justo después de un cambio de
peldaño todavía decodifica frames del peldaño anterior: cada frame va etiquetado con el peldaño en que
se capturó, y las métricas de descompresión solo cuentan cuando vienen del peldaño actual — ningún
segundo paso por una carga a la que el primero todavía no ha llegado en el descompresor, y ninguna
subida con las cifras del peldaño anterior. En el último peldaño con la carga todavía demasiado alta
no se hace nada más (sin oscilaciones, sin cortes); las capturas se siguen descartando y contando.

**Con el códec de almacenamiento (v0.7.0).** La etiqueta de descompresión es `(peldaño, clase de
códec)`, así que las cifras del decoder por hardware y las de MJPEG nunca se mezclan tras un cambio
de códec. Las repeticiones que provoca el decoder a propósito (ponerse al día desde un keyframe,
esperar al siguiente, §4.8) y los paquetes que el encoder nunca llegó a guardar no cuentan como
descompresión lenta. La latencia del encoder nunca cuenta como carga de compresión (invariante I6,
§4.8): solo cuenta el tiempo propio del hilo en sus llamadas. La política del códec de
almacenamiento puede forzar una bajada de peldaño al momento (`ForceLower()`, causa `Encode`, así
que con el aviso que ya existía en el dock) y vetar la subida durante 5 minutos (`Sample::blockUp`).
`Rebase()` mueve la escalera bajo el techo de los encoders (§3.5) en un peldaño dado, conservando el
paralelismo.

**Aviso.** Cada cambio de peldaño es un WARN (`delay buffer lower rung (encode, load 85%): now WxH at
F fps capture, rung N of M (…) -- last 2s: … The delay itself is unchanged.`); los cambios de
paralelismo son INFO. El filtro guarda un `PipelineHealth` (`src/pipeline-health.hpp`: peldaño,
peldaño máximo, tamaño actual y completo, fps de captura y de salida, contextos y slices de
compresión, causa de la última bajada, frames descartados y repetidos por minuto en la última ventana
de telemetría, un contador de cambios) bajo un pequeño mutex; su proc `get_pipeline_state` solo lo
copia, sin saltar nunca al hilo gráfico, así que un render con retraso no puede bloquear la interfaz.
El dock lo consulta una vez por segundo en `Filling`/`Active` (con la referencia débil al filtro que
guarda el puente, §3.3) y, mientras el peldaño está por encima de 0, muestra "Calidad del buffer
reducida" en la tarjeta de estado: qué se redujo (la resolución frente al tamaño completo, los fps de
captura o ambos), la causa (el hilo gráfico de OBS, comprimir o descomprimir los fotogramas) y que el
delay sigue intacto, más los fotogramas repetidos o descartados por minuto cuando hay alguno. La
línea de estimación de RAM bajo los campos (peldaño 0, "En la RAM solo caben 41 s de 60 s…") se
oculta mientras tanto, porque el aviso ya dice lo que guarda el buffer. Los lectores de pantalla lo
oyen una vez: el nombre accesible de la tarjeta solo cambia con el estado y cuando el aviso aparece o
se va, lo que se redujo va en su descripción, y con Qt 6.8+ un `QAccessibleAnnouncementEvent` lee el
aviso cuando aparece o cambia su causa, y `Dock.Health.Restored` cuando se va con el delay todavía en
marcha (con Qt anterior cambia el nombre de la tarjeta).

### 4.8. v0.7.0: el códec de almacenamiento — almacenamiento inter-frame por hardware en Windows

**Por qué.** Medido fuera de OBS a 1440p60 con 30 s, entre el 80 y el 85 % de la RAM del plugin era
la arena, es decir, los propios paquetes MJPEG; el resto (contextos de códec, reservas de frames,
holgura de bloques) era poco. Solo con menos bytes por frame se podía reducir a la mitad. MJPEG no
puede dárselos sin empeorar la imagen (cada escalón de qscale cuesta más o menos un punto de VMAF
tras la codificación del stream), mientras que un encoder HEVC por hardware a QP constante 20 con un
keyframe por segundo guardaba los mismos frames con entre un 67 y un 74 % menos de bytes, con mejor
imagen y una fracción de la CPU. Ningún encoder por software que no sea GPL es lo bastante rápido a
1440p60 junto a un juego, así que sin encoder por hardware el buffer se queda en MJPEG.

**Piezas.**

```
G (hilo gráfico)          captura -> lectura I420 -> frame en crudo, slot del ring reservado
                              |
trigglow-enc-0                v
  encoder por hardware (asíncrono, un contexto) <-- adoptado entre frames -- trigglow-codec-setup
  o MJPEG (puente mientras se prepara, alternativa)                          (ordena, abre, sondea)
                              |
ring + arena                  v
  paquetes HEVC/H.264 y MJPEG, cada uno con su códec, keyframe, cadena y seq
                              |
trigglow-dec                  v
  D3D11VA -> NV12 -> I420 (un decoder por software solo en emergencias)
                              |
G                             v
  reproducción por antigüedad; cada 250 ms la política del códec baja un peldaño, evacúa o cede
```

- **Interfaz** (`src/storage-codec.hpp`, C++17 puro, sin OBS ni FFmpeg). `storage::Encoder` es
  asíncrono: `Send(frame, seq, forceKey)` copia el frame (`Ok`, `Busy` o `Failed`), y
  `Receive(packet)` entrega los paquetes listos en el orden de `Send()`, cada uno con su `seq` y si
  es keyframe; `Drain()` lo vacía antes de cerrarlo e `InFlight()` cuenta los frames que aún no han
  salido. `storage::Decoder::Decode(packet, wantImage, …)` decodifica un paquete guardado
  (`wantImage = false`: puesta al día, solo referencias) y `Flush()` gestiona una discontinuidad.
  Los frames decodificados son siempre I420, así que la reproducción nunca sabe qué códec había
  detrás. Implementaciones: sin comprimir, MJPEG (`src/mjpeg-codec.*`, todos los paquetes keyframe)
  y la de hardware (`src/hw-storage-codec.*`, `TRIGGLOW_HAVE_HW_STORAGE`, solo Windows).
  `ChainTracker` e `InFlightQueue` llevan las cuentas de un bitstream para cualquier códec.
- **Metadatos del ring** (`src/frame-ring.*`). `FrameRing::Payload` lleva el códec, `key`, `chain`
  (una sesión de bitstream: un encoder desde su primer keyframe, para un ring) y `seq` (consecutivo
  en el bitstream). `ringselect::Decodable()`: un keyframe siempre, un P-frame solo justo después de
  su referencia (misma cadena, seq siguiente). `PlanDecodeStart()` retrocede desde el frame que toca
  hasta el keyframe desde el que se puede decodificar (solo por los paquetes de su propio códec, así
  que un frame MJPEG en medio no corta la cadena); `ChooseDecodeStart()` se pone al día desde él o
  espera al siguiente keyframe, lo que muestre antes un frame nuevo. La máquina de estados de los
  slots, las generaciones y el `captureNs` al reservar no cambian.
- **Invariantes**, citadas por número en el código:

| # | Invariante | Cómo |
|---|---|---|
| I1 | Dentro de una cadena, el bitstream sigue el orden de captura | Un único contexto inter, paquetes emparejados en el orden de `Send()`; un paquete fuera de orden es un error grave |
| I2 | Nunca se decodifica un P-frame sin su referencia | `Decodable()`; si no, el decoder salta al siguiente keyframe |
| I3 | Las capturas descartadas antes de codificarse no consumen seq; tras perder un paquete ya codificado, el siguiente frame es un keyframe forzado y no se guarda ningún P-frame huérfano | `ChainTracker` |
| I4 | Con un códec inter-frame, el keyframe del frame que toca siempre está en el ring | Un GOP de holgura en `bufferfit::SlackFrames()` |
| I5 | La reproducción (re)arranca en un keyframe, manteniendo mientras tanto el frame en pantalla | `FrameRing::PlanDecodeStart()` |
| I6 | La latencia de un encoder nunca cuenta como carga | La degradación solo ve el tiempo propio del hilo; la latencia va a la política del códec |
| I7 | Ni un cambio de códec, ni de cadena, ni de resolución vacía el ring o mueve el delay | Los rings admiten varios códecs; los frames decodificados son siempre I420; MJPEG hace de puente |
| I8 | Cada encoder y decoder lo usa un solo hilo a la vez, nunca el hilo gráfico | Se abre en el hilo de preparación, se entrega bajo el mutex del pipeline y lo cierra su hilo o el liberador |
| I9 | La clase de almacenamiento que dimensiona el ring de un ciclo es fija durante el ciclo | Se escribe en el filtro en `Enable()`; el ring y el delay previsto leen el mismo valor |

**Encoders por hardware.** `RankCandidates()` (`src/storage-codec-select.*`, puro) ordena
`hevc_nvenc`, `hevc_amf`, `hevc_qsv`, `h264_nvenc`, `h264_amf`, `h264_qsv`, y solo deja los
fabricantes de GPU que informa DXGI (`QueryGpuAdapters()`), así que nunca se carga el runtime de
otro fabricante; primero van los motores en los que OBS no codifica (una iGPU junto a un stream en
la gráfica dedicada). NVENC solo es candidato mientras OBS tenga como mucho 3 encoders NVENC en
marcha (`kMaxObsNvencForBuffer`), así queda una sesión libre incluso con el antiguo límite de 5.
Quick Sync nunca es candidato en un OBS elevado (`TokenElevation`): su runtime lo carga un cargador
que busca DLL en rutas que controla un usuario sin privilegios. Los encoders que fallaron de forma
grave en esta sesión van a una lista negra. Ajustes:
- HEVC a QP constante 20 y H.264 a 18; un keyframe cada medio segundo (`KeyframeIntervalFrames()`:
  `ceil(fps / 2)`, 30 frames a 60 fps); sin B-frames; IDR forzados (`pict_type = I` más la opción
  del backend); parámetros en banda, y copiados delante de un keyframe que no los lleve
  (`src/annexb.*`); `pts = seq`, que se empareja con los frames del pipeline.
- NVENC: `p1`, `ll`, `constqp`, latencia cero, sin lookahead, sin multipass, sin AQ espacial, 4
  superficies. AMF: `lowlatency_high_quality`, `speed`, `cqp`, sin preanálisis, cabeceras en cada
  IDR. Quick Sync: `veryfast`, entrada NV12 (el croma se entrelaza en el hilo de compresión),
  `async_depth` 1.
- Frames de entrada en memoria del sistema desde una reserva de buffers (I420; NV12 para Quick
  Sync). NVENC y AMF se abren en un dispositivo D3D11 del adaptador de su fabricante, el de OBS si
  es de ese fabricante.

**Decoders.** D3D11VA en el adaptador de OBS (`obs_video_info.adapter`), de baja latencia y un hilo;
la imagen se descarga como NV12 en un frame reutilizado y su croma se separa a I420 en el hilo de
descompresión (en una puesta al día no se descarga nada). Nunca pasa a software en silencio: un
stream que no puede decodificar es un fallo. El decoder HEVC/H.264 por software de libavcodec (con
hilos de slice) es solo la vía de emergencia, para los frames por hardware que quedan en el ring
cuando falla el decoder por hardware o cuando el buffer abandona un motor que comparte con OBS para
decodificar (VCN de AMD e Intel decodifican en el mismo motor que codifica; NVDEC de NVIDIA es un
motor aparte). Cuesta unos 15ms de CPU por frame a 1440p y no llega a 2160p60; entonces el decoder
salta al siguiente keyframe y se mantiene el frame en pantalla.

**Preparación y adopción.** El hilo de compresión 0 le pide al hilo de preparación
(`trigglow-codec-setup`, que se crea al primer uso) un encoder al tamaño de frame que necesita, y
mientras tanto sigue guardando en MJPEG (el puente; nunca se descarta ni se retrasa una captura).
Para cada candidato, en más o menos un segundo: lo abre, le manda 8 frames sintéticos (un degradado
en movimiento con ruido suave) con keyframes forzados en el 0 y el 5, y comprueba 8 paquetes en
orden, keyframes en el 0 y el 5 con sus parámetros, un decode D3D11VA del 0 al 7 y, tras vaciar el
decoder, del 5 al 7, los tamaños, y un PSNR-Y por encima de 30dB (un detector de basura, no un
listón de calidad). Lo que pasa se entrega abierto, con su decoder ya calentado, y se adopta en una
frontera de frame en una cadena nueva que empieza en un keyframe. Un registro de la sesión recuerda
qué pasó (un tamaño nuevo solo necesita entonces una reapertura de 2 frames), qué falló y a qué
tamaño, y qué falló de forma grave. La preparación consulta un predicado de cancelación entre pasos
(Disable, cierre, un objetivo MJPEG, una cesión) y cierra en su propio hilo lo que abrió. Medido en
una RX 7800 XT (AMF): sondeo de 330–380ms a 1440p, reapertura de unos 100ms, todos los peldaños de
las escaleras de 2160p, 1440p y 1080p se abren y hacen el viaje de ida y vuelta, y en D3D11VA un
cambio de parámetros del stream cuesta 11ms (15ms a 2160p).

**Hilo de compresión con el códec por hardware.** Un único contexto (I1). Cada frame enviado queda
en cola "en vuelo" `{reserva, seq, hora de envío}` y se guarda cuando sale su paquete; con frames en
vuelo y sin trabajo, el hilo sondea cada 2ms. Un paquete fuera de orden, un error del encoder, o
500ms con frames en vuelo y ningún paquete mientras siguen llegando frames (un intento de `Send()`
en los últimos 250ms, para que una pausa del render no sea un atasco) es un error grave: se pierde
lo que estaba en vuelo y el encoder se cierra. Un paquete que no cabe en la arena, o cuyo slot se
reutilizó, rompe la cadena: el slot pasa a `Dropped`, el siguiente `Send()` fuerza un keyframe y los
P-frames no se guardan hasta entonces (I3). Un tamaño de frame nuevo (un peldaño, un reinicio del
vídeo de OBS) vacía el encoder anterior (como mucho 50ms), MJPEG hace de puente y se le pide a la
preparación el tamaño nuevo. El tiempo propio del hilo en `Send()`/`Receive()`/copia/publicación
alimenta la degradación; la latencia de `Send()` al paquete y los frames en vuelo alimentan la
política del códec (I6).

**Hilo de descompresión.** Un decoder por cada códec que hay en el ring; el de hardware llega ya
calentado. Al arrancar, tras una pausa o cuando se queda atrás sobre un P-frame, planifica desde el
keyframe (I5): se pone al día sin mostrar y después decodifica el frame que toca, o espera al
siguiente keyframe; la espera se publica para que el hilo gráfico cuente esas repeticiones aparte
(`holdsCatchUp`), nunca como descompresión lenta. Un cambio de tamaño de frame o de códec en una
frontera de cadena solo necesita vaciar el decoder, o el decoder del otro códec. Los decoders
ociosos se cierran a los 3 s.

**Política del códec de almacenamiento** (`src/storage-codec-policy.*`, pura,
`tests/storage_policy_tests.cpp`), evaluada en el hilo gráfico cada 250ms, antes de la política de
degradación:

| Estado | Qué significa |
|---|---|
| `Mjpeg` | Solo MJPEG: `buffer_codec` en `mjpeg`, modo síncrono, sin GPU o ningún encoder funciona |
| `Probing` | La preparación busca un encoder; MJPEG hace de puente |
| `Hw` | El encoder por hardware guarda los frames |
| `HwGuarded` | Su motor avisó: un peldaño menos y sin subir durante 5 minutos |
| `Yielded` | Una salida de OBS que arrancaba se quedó con la sesión NVENC |
| `Evacuated` | De vuelta en MJPEG hasta el siguiente `Enable()` (sin segundo intento en el mismo ciclo) |

- **Entradas.** `engineShared` (`ComputeEngineShare()`): el encoder del buffer es de la misma
  familia que un encoder de vídeo activo de OBS (`FamilyFromObsEncoderId()`: `*nvenc*`, `*amf*`,
  `*qsv*`, los de CPU), o su decode va en una GPU AMD o Intel cuyo motor codifica para OBS; lo
  desconocido cuenta como compartido. Los frames que OBS se salta por retraso de codificación: la
  mezcla principal (`video_output_get_skipped_frames()`, leída en el hilo gráfico) más las mezclas
  de los encoders de OBS (reescalado por GPU, pistas de Enhanced Broadcasting), leídas una vez por
  segundo en el hilo de la UI y enviadas con `report_encoder_lag`. Una salida de OBS que se para con
  `OBS_OUTPUT_ENCODE_ERROR` (se vigila la señal de parada de cada salida activa con encoder de
  vídeo) llega con `report_obs_encode_error`. Los errores graves del encoder o del decoder por
  hardware vienen del pipeline.
- **Reglas**, de mayor a menor prioridad: un error grave evacúa al momento (el fallo del propio
  encoder lo manda a la lista negra de la sesión). Con el motor compartido, un error de codificación
  de OBS, o al menos un frame que OBS se saltó en los últimos 2s, evacúa al momento, sin esperar
  nunca al enfriamiento (un error de codificación de OBS que llega mientras se sondea o se ha cedido
  se guarda hasta que el hardware vuelve a guardar frames). El aviso temprano, solo con el motor
  compartido, mantenido durante 1s con al menos 20 paquetes y 2s después de la transición anterior:
  latencia p95 por encima de 2,5 intervalos de captura (42ms a 60fps; AMF midió 20–21ms) o 3 frames
  en vuelo. Baja un peldaño (menos píxeles es lo que libera el motor: −31 % por peldaño de ×0,83) y
  veta la subida durante 5 minutos; si no queda peldaño por encima del suelo, evacúa. En un motor
  que OBS no comparte, el buffer no puede provocar el retraso de codificación de OBS, así que nada
  de esto aplica (la degradación sigue ocupándose de la carga de CPU y del hilo gráfico).
- **La evacuación** apunta el pipeline a MJPEG sin parar: el encoder por hardware se vacía (50ms) y
  se cierra en el liberador, los frames siguientes son keyframes MJPEG en una cadena nueva, el ring
  guarda los dos y la reproducción sigue sin hueco. Con el decode en el motor compartido (AMD,
  Intel) y el retraso de OBS como causa, los frames por hardware que quedan en el ring se
  decodifican por software. La RAM sube hasta el nivel de MJPEG durante los siguientes `delay`
  segundos, dentro del presupuesto. El WARN: `delay buffer codec <encoder> -> mjpeg: <motivo> …;
  buffer encoder latency p95 N ms -- the delay itself is unchanged, …`.
- **Sesiones NVENC.** Con `OBS_FRONTEND_EVENT_{STREAMING,RECORDING,REPLAY_BUFFER}_STARTING`,
  `BufferModeController` llama al proc `yield_hw_encoder` del filtro, que solo actúa cuando puede
  haber una sesión NVENC abierta o abriéndose; el hilo de compresión 0 (o la preparación) la cierra
  y guarda en MJPEG. La UI espera a eso como mucho 100ms, y solo cuando los encoders NVENC de OBS,
  el que arranca y el del buffer suman 4 o más. 3s después de que arranque la salida (10s después de
  la cesión como mucho) llama a `resume_hw_encoder`, se vuelven a contar los encoders de OBS y la
  política vuelve a `Probing`.

**Clase de almacenamiento y dimensionado.** `BufferModeController` fija la clase del ciclo en
`Enable()` y la escribe en el filtro (el proc `set_storage_class` y el ajuste `storage_class`, I9):

| Clase | Cuándo | Ratio supuesto | Holgura |
|---|---|---|---|
| `intra` | `buffer_codec` en `mjpeg`, `codec_workers` desactivado, sin GPU, Linux (macOS: ratio 1) | 5x | `ceil(fps / 4)` |
| `intra-gop` | El hardware puede tomar el relevo, pero ningún encoder ha pasado aún su sondeo en esta sesión (el primer ciclo) | 5x | `max(ceil(fps / 4), GOP)` |
| `inter` | Un encoder por hardware pasó su sondeo en esta sesión | 12x | `max(ceil(fps / 4), GOP)` |

`ApplyStorageClass()` fija los dos parámetros para `EnsureRingSized()` y para `EstimateBufferFit()`,
así que el ring, la estimación del dock y el delay del audio mantienen un único delay en segundos
enteros. El ratio solo importa cuando falta RAM (4K, delays largos, equipos de 8–16GB): 12x mete 2,4
veces los frames de 5x, y aguanta aunque el ciclo acabe en MJPEG (13–17x medido); con contenido peor
la arena descarta frames (repeticiones) en vez de crecer. El tamaño de bloque de la arena sigue los
mismos bytes esperados (§4.3).

**`buffer_codec`** (oculto, `buffer-mode-settings.json`). `plugin-main.cpp` lo lee al cargar con
`storage::ParseBufferCodec()`: si falta (o es null de JSON) vale `"auto"`; solo cuentan los textos
exactos en minúsculas `"auto"` y `"mjpeg"`; cualquier otra cosa (`"MJPEG"`, `"hevc"`, `false`, `0`)
se queda en `"auto"` con un WARN y desaparece del archivo al salir; solo se vuelve a escribir cuando
es `"mjpeg"`. El controlador lo envía al filtro en cada `Enable()` (`SetBufferFilterCodec()`), y
`codec_workers=false` manda sobre él. Sin propiedad en el filtro ni control en el dock.

**Telemetría.** En la preparación: `storage codec: auto -> hevc_amf (AMF, QP 20, GOP 30 = 0.50 s, no
B-frames, decode d3d11va on "<adaptador>") at WxH; probe N ms (open N ms on its adapter, 8/8
packets, IDR in-band ok, fresh-decoder start ok, psnr N dB); tried: …; engine shared with OBS:
yes|no|unknown; NVENC sessions by OBS: N`, o `storage codec: auto -> mjpeg at WxH (…)` con el
motivo. En cada ventana de telemetría, justo después de `pipeline check`:

```
storage codec check (periodic): codec=hevc_nvenc(qp20,gop30) dec=d3d11va | hwenc lat
p50/p95/max=…ms inflight max=N send p95=…ms | keys=N forcedIdr=N chainBreaks=N orphanP=N catchUps=N
(frames N, max Nms) noKeyWaits=N noKeySkips=N | bridge frames=N codecChanges=N hardErrors=N |
policy=Hw guard=off | obs encode-lag skipped main+N mixes+N
```

(una sola línea; una corta, `codec=mjpeg(q5,huff=default) dec=mjpeg policy=Mjpeg | obs encode-lag
…`, cuando todo el ciclo es MJPEG). `PipelineHealth` v2 añade el códec, el nombre del encoder, el
decode por hardware, el estado de la política y su contador de cambios, solo para el diagnóstico de
soporte; `get_pipeline_state` recibe el tamaño de la estructura de quien llama y nunca copia más
allá. El dock no cambia.

**Plataformas y licencias.** Solo Windows: el FFmpeg vendorizado de BtbN (LGPL 3.0, sin
`--enable-gpl` ni `--enable-nonfree`, comprobado en CI) ya trae los encoders NVENC, AMF y Quick Sync
y D3D11VA en `avcodec-63.dll`/`avutil-61.dll`, así que no se distribuye ninguna DLL nueva; los
runtimes de los encoders vienen con el driver de la GPU; `data/licenses/LICENSE-ffmpeg.txt` viaja
con las DLL. Linux sigue con MJPEG sobre el libavcodec del sistema (los encoders VAAPI necesitan
frames de hardware, y Linux no se ha probado en directo); macOS guarda I420 en crudo como antes. Los
códecs por hardware los licencia el fabricante de la GPU; el decoder HEVC/H.264 por software ya
venía en el avcodec que se distribuye (es una nota, no asesoría legal).

**Medido** (benchmark de almacenamiento compilado con las fuentes del plugin, fuera de OBS: Ryzen 7
7700X, RX 7800 XT con AMF, contenido sintético tipo gameplay a 60fps, working set privado del
pipeline del buffer sin audio; build v0.7.0, `auto` frente a `mjpeg`):

| Salida | Delay | MJPEG | HEVC por hardware | Menos | PSNR-Y MJPEG / HEVC | CPU del códec MJPEG / HEVC |
|---|---|---|---|---|---|---|
| 1080p60 | 30s | ~395MB | ~190MB | −52 % | 40,4–40,5 / 45,3–45,4 dB | 0,45 / 0,18–0,24 núcleos |
| 1080p60 | 60s | ~750MB | ~285MB | −62 % | 40,4 / 45,1–45,3 dB | 0,48 / 0,19–0,21 núcleos |
| 1440p60 | 30s | ~635MB | ~325MB | −49 % | 40,1–40,3 / 44,7–45,1 dB | 0,78–0,88 / 0,20–0,32 núcleos |
| 1440p60 | 60s | ~1200MB | ~520MB | −57 % | 40,1–40,3 / 44,8–45,0 dB | 0,58–0,82 / 0,22 núcleos |
| 2160p60 | 30s | ~1250–1275MB | ~590MB | −53 % | 40,9 / 45,2–45,3 dB | 1,6–2,0 / 0,60–0,66 núcleos |
| 2160p60 | 60s | ~2315MB | ~880–920MB | −60 a −62 % | 40,8 / 45,2 dB | 2,0 / 0,39–0,63 núcleos |

- La v0.6.0 ocupaba ~661MB (1440p60, 30s; ~685MB con sus objetos del lado de la GPU) y ~1232MB (60s)
  con MJPEG.
- La latencia del encoder por hardware es de más o menos un frame (p50 de 19–24ms, p95 de 20–26ms en
  la mayoría de las pruebas, 39ms en una a 2160p60), con 1 frame en vuelo. Su sondeo y su adopción
  no cuestan ninguna captura.
- Junto a un proxy de stream 1440p60 codificado en tiempo real con `h264_amf` en la misma GPU
  (14 Mbps CBR, preset de calidad, 2 B-frames), los dos mantuvieron 60fps: el stream a 1,01x tiempo
  real, y la codificación y la decodificación del buffer en ~30 % del motor de vídeo.
- Una pausa de render de 500ms: 1–2 repeticiones en los 2s siguientes, el primer frame nuevo a los
  33–42ms, una puesta al día de 11 frames (22–25ms). Cambiando MJPEG ↔ hardware cada 10s durante un
  minuto: 5 cambios de códec, 14 frames de puente, 0 errores graves.
- Tras Disable, el proceso volvió a menos de 13–25MB por encima de su base, sin ningún encoder por
  hardware abierto.
- **Sin medir**: NVENC y NVDEC de NVIDIA (los bytes por frame, y con ellos la RAM, pueden variar),
  Quick Sync de Intel, la VRAM que ocupa el códec por hardware, y nada dentro de un OBS real.

**Tests.** `storage_codec_tests` (frames en vuelo de 0 a 16, `ChainTracker`, parámetros Annex B con
start codes de 3 y 4 bytes y paquetes truncados, separación NV12, `ParseBufferCodec()`,
`RankCandidates()` con y sin Quick Sync, `FamilyFromObsEncoderId()`, motor compartido, una
simulación de un solo hilo con pérdidas aleatorias), `storage_policy_tests` (prioridades, la regla
del motor compartido, enfriamiento, veto, cesiones, muestras mínimas, un tamaño en el que no abre
ningún encoder, una hora en el límite de los umbrales con como mucho una transición),
`pipeline_gop_stress_tests` y `adversarial_gop_tests` (el pipeline real con un códec inter-frame
falso, `tests/fake-gop-codec.hpp`, cuyo decoder marca cualquier P-frame decodificado sin su
referencia: latencia de 0–3 frames, GOP de 2–8, pérdidas, cambios de códec y de tamaño, pausas de
render, preparaciones de 0–200ms que a veces fallan, evacuaciones, cesiones, encoders y decoders
rotos, un decode por software lento; nunca una imagen corrupta, cadenas que empiezan en un keyframe,
un hilo por objeto de códec y nunca el hilo gráfico, la latencia nunca contada como carga), y
`hw_storage_codec_tests` (Windows, builds locales con el FFmpeg del plugin en `.deps`: el FFmpeg
real con fallos inyectados, aperturas fallidas, una preparación cancelada, un decoder que no llega a
abrirse). El job de ThreadSanitizer también ejecuta `pipeline_gop_stress_tests`.

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
plugin-main.cpp              → obs_module_load/unload, cableado de todos los componentes, archivo de
                                ajustes (buffer_output_cap / codec_workers / buffer_codec ocultos,
                                copia .invalid)
buffer-mode-controller.*      → la máquina de estados descrita en §3.3 (Inactive/Filling/Active/Error)
obs-frontend-bridge.*         → la única capa que toca obs-frontend-api.h + el cableado de filtros/escenas
video-delay-filter.*          → §3.1 -- captura, conversión I420, lectura, reproducción y el pequeño
                                pool fijo de objetos de GPU; dueño del ring, del pipeline de códec y
                                de las políticas de degradación y del códec de almacenamiento (§4.5
                                -- telemetría perf check / pipeline check; §4.8 -- storage codec
                                check)
codec-pipeline.*              → §4.6, §4.8 -- hilos de compresión/descompresión, el liberador
                                (v0.6.0) y el hilo de preparación del códec de almacenamiento
                                (v0.7.0)
storage-codec.hpp             → §4.8 -- interfaz del códec de almacenamiento, códec sin comprimir,
                                contabilidad de cadenas, invariantes I1-I9 (v0.7.0)
hw-storage-codec.*            → §4.8 -- encoders HEVC/H.264 por hardware (NVENC, AMF, Quick Sync),
                                decoders D3D11VA y por software de emergencia, el sondeo (Windows,
                                v0.7.0)
storage-codec-select.*        → §4.8 -- lectura de buffer_codec, familias de encoders de OBS, orden
                                de candidatos, motor compartido; puro (v0.7.0)
storage-codec-policy.*        → §4.8 -- la política del códec de almacenamiento; pura (v0.7.0)
annexb.*                      → §4.8 -- parámetros Annex B en banda; puro (v0.7.0)
mjpeg-codec.*                 → §4.2 -- MJPEG sobre el FFmpeg vendorizado, una instancia por hilo
                                (solo Windows/Linux)
frame-ring.*                  → §4.6, §4.8 -- ring de cabeceras de slot con máquina de estados
                                (v0.6.0), códec/keyframe/cadena/seq por paquete y planes de arranque
                                del decode (v0.7.0)
frame-arena.*                 → §4.3 -- arena por bloques para los datos de los frames (v0.6.0)
degradation-policy.*          → §4.7 -- degradación automática, pura (v0.6.0)
pipeline-health.hpp           → §4.7 -- lo que lee el dock sobre el pipeline (v0.6.0)
perf-stats.*                  → §4.5 -- series de percentiles de tamaño fijo y ventanas móviles
buffer-fit.*                  → §3.5 -- aritmética pura de dimensionado del ring (techo de salida, suelo
                                de calidad, presupuesto de RAM, fps exactos, delay efectivo en segundos
                                enteros), sin libobs/FFmpeg
encoder-check.*               → §3.5 -- decisión pura del WARN de "encoder por encima del buffer"
                                (qué encoders ven el lienzo principal) y el techo de los encoders
                                (v0.7.0), sin libobs
audio-delay-filter.*          → §3.2 -- ring de delay de audio por fuente hoja
overlay-badge.*               → §3.6 -- pinta el overlay del directo como un único PNG (QPainter, hilo de UI)
overlay-layout.*              → §3.6 -- colocación pura del overlay en el lienzo
auto-names.*                  → §3.7 -- nombres de los objetos gestionados y su migración
hardware-info.*               → consulta de la RAM total del sistema; los adaptadores de GPU
                                (fabricante, nombre y VRAM de DXGI) para el códec de almacenamiento
                                (v0.7.0)
win-http.* / api-client.*     → §3.8 -- el único camino HTTPS endurecido y el cliente de
                                api.trigglow.com (Windows; "no disponible" en macOS/Linux hasta que
                                tengan cliente HTTP)
json.*                        → §3.8 -- lector/escritor JSON estricto y acotado
auth-manager.* / auth-client.* → §3.8 -- inicio de sesión (vinculación v2), renovación, cierre de
                                sesión, cambio desde la v0.5.x, reglas de 401/403; la puerta de
                                cuenta (docs/ACCOUNT_GATE.md)
token-store.*                 → §3.8 -- el inicio de sesión del plugin, cifrado con CryptProtectData
support-client.* / support-service.* → §3.8 -- llamadas de Trigi y de tickets; hilos, una petición
                                de Trigi a la vez, elecciones locales, líneas de log
support-diagnostics.* / diagnostics-collector.* → §3.8 -- los datos técnicos v1 opcionales
chat-text.*                   → §3.8 -- saneado y parser puros de las respuestas de Trigi
trigi-page.* / trigi-widgets.* / trigi-states.* → §3.8 -- la página de Trigi, su vista segura y
                                sus decisiones de estado
report-bug-dialog.*           → §3.8 -- "Reportar un problema": Trigi, tickets con cuenta y de
                                invitado
bug-report.* / log-redact.*   → §3.8 -- el log actual de OBS, leído y redactado en memoria
update-checker.*              → comprobación de la última release de GitHub; solo abre las
                                releases de este repo
background-tasks.* / cancel-token.* / ui-tasks.* → §3.8 -- trabajos en segundo plano cancelables,
                                sus dueños y el cierre acotado al descargar el plugin
scene-combo-box.*             → combo box del dock poblado desde obs_frontend_get_scene_names()
settings-ui.*                 → el dock de Qt (escena en directo/de carga, segundos, calidad, estimación
                                de ajuste, Enable/Disable, cuenta atrás de llenado en vivo, campos
                                bloqueados, aviso de degradación)
ui-tokens.* / ui-theme.*      → colores derivados de la paleta de OBS (WCAG AA), hoja de estilos,
                                fuentes empaquetadas e iconos SVG recoloreados
hotkeys.*                     → registro de las 3 hotkeys nativas de OBS, conectadas a
                                Enable()/Disable()/Toggle() de BufferModeController
logging.*                     → envoltorio de logging con prefijo de componente
delay-controller.*            → §6 -- lógica heredada del modo reconexión, presente pero sin usar
tests/                        → 23 suites, solo se compilan con -DTRIGGLOW_BUILD_TESTS=ON o por
                                separado (cmake -S tests): buffer_fit, encoder_check, ui_tokens,
                                auto_names, overlay_layout, frame_arena, frame_ring,
                                degradation_policy, pipeline_stress, storage_codec, storage_policy,
                                pipeline_gop_stress, adversarial_gop, background_tasks, json,
                                log_redact, chat_text, support_diagnostics, api_client,
                                token_store, auth_client, support_client, trigi_states (las del
                                cliente, con un sustituto de red guionizado); más hw_storage_codec
                                en Windows cuando el FFmpeg del plugin está en .deps
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
