*[English version](SPEC.md)*

# Especificación técnica — Trigglow Dynamic Delay para OBS

Estado: **MVP / v0.2.0 — Early Access**
Última actualización: 2026-08-26 (reescrita desde cero — ver §0)
Nombre interno del plugin: `obs-trigglow-dynamic-delay`
Web: https://trigglow.virosms.com/dynamic-delay

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
`delay-controller.*`/`hotkeys.*` del modo reconexión sigue en este repositorio, inactivo — ver §5 — por
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
una huella de GPU fija y pequeña (ver §6).

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
un streamer que quiera una grabación local sin delay junto a un stream con delay no está cubierto por
v0.2.0 tal cual.

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

### 3.5. Dimensionado del buffer: primero el suelo de calidad, después la duración

El usuario elige dos cosas, ambas de libre elección, nunca restringidas:

- **Delay (segundos)** — de 1 a 60 en el spinner del dock (un límite de cordura de la interfaz; el
  límite real es la aritmética de RAM de abajo).
- **Calidad mínima** — 480p / 720p / 1080p. La altura capturada del vídeo retrasado nunca baja de
  esto, pase lo que pase.

`EnsureRingSized()` calcula un **presupuesto de RAM** a partir de la RAM total del sistema
(`GlobalMemoryStatusEx` en Windows; sin implementar en otras plataformas, donde se usa un valor de
respaldo conservador) — el 50% de la RAM total, acotado entre un suelo de 1GB y un techo de 24GB. Si
los segundos pedidos × el suelo de calidad elegido no caben en ese presupuesto, el plugin acorta la
**duración realmente almacenada** en vez de bajar nunca de la calidad elegida. El dock muestra una
estimación en vivo y no bloqueante de qué logrará realmente una combinación dada de (segundos,
calidad) *antes* de que el usuario pulse Enable — "aconsejar según el hardware, pero a su elección":
esta estimación nunca bloquea ni recorta las elecciones del usuario, solo avisa.

## 4. Alcance de v0.2.0, tal y como se distribuye realmente

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
  revisión exhaustiva de `graphics.h`.
- Presets de delay guardados.
- Elegir una fuente individual en vez de una escena entera como "en directo".
- Un plugin dedicado de Trigglow para Stream Deck (la vía de hotkey nativa ya funciona hoy).
- Instaladores firmados (el instalador actual de Windows no está firmado — Early Access).
- macOS/Linux **probados en directo** (ambos ya compilan en verde en CI; ninguno se ha ejecutado en
  directo todavía).

## 5. Código heredado del modo reconexión

`delay-controller.*` y el cableado de hotkeys específico de reconexión del primer diseño abandonado en
§2 siguen presentes en este repositorio, sin usar — `plugin-main.cpp` ya no instancia `DelayController`
ni conecta sus hotkeys/dock. Se conservan en vez de borrarse por si algún día se quiere un modo
reconexión opcional de menor consumo de recursos (el modo buffer cambia RAM por cero reconexiones;
algunos usuarios con hardware muy limitado podrían preferir el compromiso antiguo) — no es una promesa
de que vaya a volver, simplemente no se ha tirado.

## 6. Riesgo conocido, todavía no confirmado como resuelto

Se observó en directo un crash real de "Device Remove/Reset" de la GPU al pulsar Disable, bajo el
diseño anterior de una textura de GPU persistente por cada frame almacenado. Nunca se determinó de
forma concluyente que este plugin fuera la causa raíz antes de que se hiciera la reescritura a
RAM/NV12 (§3.1) — la reescritura reduce plausiblemente el riesgo (un puñado de objetos de GPU pequeños
y fijos en vez de docenas de objetos grandes que escalan con la duración del delay), pero eso no es lo
mismo que una corrección confirmada. Trátalo como un área de riesgo real y probable en directo, no
como un problema resuelto, hasta que se vuelva a probar específicamente para ver si reaparece.

## 7. Arquitectura

```
plugin-main.cpp              → obs_module_load/unload, cableado de todos los componentes
buffer-mode-controller.*      → la máquina de estados descrita en §3.3 (Inactive/Filling/Active/Error)
obs-frontend-bridge.*         → la única capa que toca obs-frontend-api.h + el cableado de filtros/escenas
video-delay-filter.*          → §3.1 -- el ring buffer en RAM/NV12 y su pequeño pool fijo de objetos de GPU
audio-delay-filter.*          → §3.2 -- ring de delay de audio por fuente hoja
hardware-info.*               → consulta de la RAM total del sistema (sustituye a la antigua consulta VRAM/DXGI)
scene-combo-box.*             → combo box del dock poblado desde obs_frontend_get_scene_names()
settings-ui.*                 → el dock de Qt (escena en directo/de carga, segundos, calidad, estimación
                                de ajuste, Enable/Disable, cuenta atrás de llenado en vivo)
hotkeys.*                     → registro de las 3 hotkeys nativas de OBS, conectadas a
                                Enable()/Disable()/Toggle() de BufferModeController
logging.*                     → envoltorio de logging con prefijo de componente
delay-controller.*            → §5 -- lógica heredada del modo reconexión, presente pero sin usar
```

Principio de diseño: **`BufferModeController` es el único dueño del estado**. Tanto el dock como las
hotkeys llaman a los mismos métodos públicos (`Enable()`, `Disable()`, `Toggle()`) — no hay una ruta de
código separada para "botón" vs. "hotkey", lo cual reduce a la mitad la superficie de bugs de
desincronización de estado.

## 8. Licencia y repositorio

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
