# Especificación técnica — Trigglow Dynamic Delay for OBS

Estado: **MVP / v0.1.0 — Early Access**
Fecha: 2026-08-23
Nombre de máquina del plugin: `obs-trigglow-dynamic-delay`
Web: https://trigglow.virosms.com/dynamic-delay

---

## 1. Resumen del producto

Trigglow Dynamic Delay es un plugin nativo para OBS Studio (C++, CMake, plantilla oficial
[`obsproject/obs-plugintemplate`](https://github.com/obsproject/obs-plugintemplate)) que permite
activar, desactivar y alternar un delay (retraso) configurable del stream de salida directamente
desde OBS — con un botón, una hotkey nativa de OBS o un botón de Stream Deck mapeado a esa hotkey.
No requiere ninguna app externa, panel web ni proceso separado: todo vive dentro del proceso de OBS.

## 2. Validación técnica — qué es realmente posible hoy

Antes de prometer nada, verificamos el comportamiento real de la API pública de OBS leyendo el
código fuente de `libobs` (no solo la documentación, que en algunos puntos está incompleta), y
contrastándolo con el hilo oficial del foro de OBS
["how to change stream delay without restarting OBS while live"](https://obsproject.com/forum/threads/how-to-change-stream-delay-without-restarting-obs-while-live.194645/)
y la propuesta abierta
["Allow adding/removing Output Delay while live"](https://ideas.obsproject.com/posts/541/allow-adding-removing-output-delay-while-live)
(todavía sin resolver en OBS a fecha de este documento).

**Hallazgo clave (verificado en `libobs/obs-output.c`, función `hook_data_capture()`):**

```c
if (output->delay_sec) {
    output->active_delay_ns = (uint64_t)output->delay_sec * 1000000000ULL;
    ...
    os_atomic_set_bool(&output->delay_active, true);
}
```

`obs_output_set_delay(output, delay_sec, flags)` (en `libobs/obs-output-delay.c`) es un simple
*setter*: guarda `delay_sec` y `flags` en la estructura del output, pero **no altera el buffer que
ya está en marcha**. El valor de `delay_sec` solo se lee y se convierte en el retraso real
(`active_delay_ns`) **una vez, en el momento en que el output arranca** (`hook_data_capture`, que se
ejecuta al iniciar la captura de datos del stream). Esto confirma, a nivel de código y no solo de
rumor de foro, que:

> **OBS bloquea el valor de delay en el instante en que arranca el stream. Llamar a
> `obs_output_set_delay()` mientras el stream ya está activo no cambia el comportamiento del
> stream en curso — solo queda "armado" para la próxima vez que el output arranque.**

Esto **no es una limitación de la Frontend API de OBS ni de este plugin: es una limitación
arquitectónica de `libobs` en sí misma**, la misma que sufre la propia UI nativa de OBS (Configuración
→ Avanzado → Delay) y cualquier otro plugin que use la API pública. No existe ninguna función
oficial, documentada o interna, que permita re-bufferizar un output ya activo sin pasar por un
reinicio del mismo.

### 2.1. Qué SÍ es real y viable (base del MVP)

Usando exclusivamente API pública y estable de OBS:

- `obs_frontend_get_streaming_output()` — obtiene el output de streaming activo (con `obs_output_release()` obligatorio tras usarlo).
- `obs_output_set_delay(output, segundos, flags)` / `obs_output_get_delay()` / `obs_output_get_active_delay()` — leer/escribir el delay configurado y el delay realmente activo.
- `obs_frontend_streaming_start()` / `obs_frontend_streaming_stop()` / `obs_frontend_streaming_active()` — controlar el ciclo de vida del stream desde un plugin.
- `obs_frontend_add_event_callback()` con el enum `obs_frontend_event` (`OBS_FRONTEND_EVENT_STREAMING_STARTING/STARTED/STOPPING/STOPPED`) — para mantener sincronizado el estado del plugin con OBS.
- `obs_hotkey_register_frontend()` — registrar hotkeys nativas de OBS (aparecen en Configuración → Atajos de teclado, exactamente como cualquier hotkey nativa; esto es lo que hace posible mapear un botón de Stream Deck sin ninguna integración especial).
- `obs_frontend_add_dock_by_id()` — panel/dock nativo dentro de OBS con Qt.
- `obs_module_get_config_path()` + `obs_data_t` — persistencia de settings dentro de la carpeta de configuración de OBS (por perfil, sin bases de datos externas).

### 2.2. Estrategia MVP honesta: "activar/desactivar en vivo" vía reconexión controlada

Dado que no existe una forma de recalcular el buffer de un output ya activo, la única manera **real
y honesta** de que el delay cambie efectivamente mientras ya estás en directo es:

1. Aplicar el nuevo valor con `obs_output_set_delay()`.
2. Si el stream **ya está activo**, hacer un **reinicio controlado** del output
   (`obs_frontend_streaming_stop()` seguido de un reinicio automático vía
   `OBS_FRONTEND_EVENT_STREAMING_STOPPED` → `obs_frontend_streaming_start()`), para que el nuevo
   valor se "arme" y aplique en el siguiente arranque, que ocurre en segundos.
3. Si el stream **no está activo** (aún no has empezado a emitir), el cambio es instantáneo y sin
   ningún efecto secundario — es la ruta recomendada para "preparar" el delay antes de salir en
   directo.

**Esto implica una reconexión breve y visible en la plataforma de destino (Kick, Twitch, YouTube...)
cuando cambias el delay estando ya en directo.** No es un efecto secundario oculto: el plugin lo
muestra explícitamente en el diálogo de confirmación y en el indicador de estado, y se documenta sin
adornos en la web y en el README. Preferimos ser honestos con esta limitación real de OBS antes que
vender una función "instantánea sin cortes" que no existe hoy en la plataforma.

### 2.3. Qué queda fuera del MVP (candidato a v2/v3)

Existe un patrón alternativo, usado por plugins de terceros conocidos (p. ej. "Dynamic Delay" de
exeldro, mencionado en el hilo del foro citado arriba): implementar el delay como un **filtro de
vídeo/audio personalizado** que bufferiza fotogramas en memoria de forma independiente al output de
streaming, evitando así el reinicio. Es una vía real, pero con contrapartidas conocidas y
documentadas por la propia comunidad: alto consumo de RAM proporcional a los segundos de delay y
framerate, y dificultad real para mantener audio y vídeo sincronizados cuando se cambia el valor en
caliente. No es apta para un MVP fiable en directo — queda documentada como exploración de v2, no
como promesa de v1.

## 3. Alcance del MVP (v0.1.0)

Incluido:

- Un flujo principal: **Enable / Disable / Toggle Delay**, con reconexión controlada si hace falta.
- Estado visible: `Inactive` / `Active` / `Error`, con el valor de delay configurado y el delay
  realmente activo (`obs_output_get_active_delay()`) cuando difieren.
- Hotkeys nativas de OBS: Toggle (obligatoria), Enable y Disable (opcionales, registradas siempre
  pero libres de asignar).
- Panel (dock) mínimo en OBS con: estado, campo de segundos, checkbox "modo seguro" (ver más abajo),
  y los tres botones.
- Persistencia de settings en la carpeta de configuración de OBS (por perfil).
- Manejo de errores sin crashear: sin output de streaming disponible, output no soporta delay
  (`OBS_OUTPUT_ENCODED` no presente), o llamada fuera de un estado válido.
- Logs mínimos y útiles con prefijo `[trigglow-dynamic-delay]` en el log de OBS.

"Modo seguro / fallback": si al intentar aplicar el nuevo delay el output no vuelve a arrancar en un
tiempo razonable (p. ej. credenciales inválidas, red caída), el plugin **no reintenta agresivamente**
— pasa a estado `Error`, deja un log claro, y no dispara reinicios en bucle. El streamer conserva el
control manual total.

Explícitamente fuera del MVP (v0.2+, ver roadmap):

- Perfiles/presets de delay guardados.
- Delay distinto por escena.
- Integración vía WebSocket/API adicional para Stream Deck (v0.1.0 usa exclusivamente hotkeys
  nativas de OBS, que Stream Deck ya soporta de forma nativa vía su acción "System Hotkey" /
  "Hotkey").
- Filtro de delay sin reconexión (ver §2.3).
- Builds firmados / instaladores. v0.1.0 se distribuye sin firmar, ver README.

## 4. Flujo de usuario

1. Instalar el plugin (copiar binario a la carpeta de plugins de OBS).
2. Abrir OBS → aparece el dock "Trigglow Dynamic Delay" (View → Docks si no es visible).
3. Configurar segundos de delay (por defecto: 10s) y pulsar "Guardar / Aplicar" — si no estás en
   directo, el valor queda armado sin más.
4. Ir a Configuración → Atajos de teclado → buscar "Trigglow" → asignar una tecla a "Toggle Delay"
   (y opcionalmente Enable/Disable).
5. En Stream Deck: usar la acción nativa "System: Hotkey" (o equivalente) y mapearla a esa misma
   combinación de teclas. No hace falta ningún plugin de Stream Deck propio en el MVP.
6. En directo: pulsar el botón del dock, la hotkey, o el botón de Stream Deck. Si el stream está
   activo, OBS mostrará una reconexión breve (unos segundos) — el indicador pasa a `Active` cuando
   el nuevo delay ya está aplicado.

## 5. Arquitectura

```
plugin-main.cpp        → obs_module_load/unload, registro de módulo y wiring inicial
delay-controller.*      → estado (Inactive/Active/Error), lógica de aplicar/activar/desactivar delay
obs-frontend-bridge.*    → única capa que toca obs-frontend-api.h (streaming start/stop/active,
                          get_streaming_output, event callback) — aísla el resto del plugin de la API
hotkeys.*                → registro/gestión de las 3 hotkeys frontend y su enlace con delay-controller
settings-ui.*            → dock Qt mínimo: labels de estado, spin box de segundos, checkbox modo
                          seguro, 3 botones — solo lectura/escritura sobre delay-controller
logging.*                → wrapper fino sobre blog()/obs_log con prefijo de plugin
```

Principio de diseño: **`delay-controller` es el único dueño del estado** (máquina de estados simple:
`Inactive → Applying → Active`, con transición a `Error` desde cualquier estado). Tanto la UI del
dock como las hotkeys llaman a los mismos tres métodos públicos (`enable()`, `disable()`, `toggle()`)
— no hay dos caminos de código distintos para "botón" y "hotkey", lo que reduce a la mitad la
superficie de bugs de sincronización de estado.

## 6. Limitaciones conocidas (v0.1.0)

- Cambiar el delay estando en directo provoca una reconexión breve y visible en la plataforma de
  destino (ver §2.2). No es opcional en v0.1.0: es como funciona OBS.
- Solo afecta al **output de streaming**, no a la grabación local ni al Replay Buffer.
- Requiere que el output soporte encoders (`OBS_OUTPUT_ENCODED`); no aplicable a salidas RAW poco
  habituales.
- Sin integración directa de Stream Deck (plugin propio de Stream Deck) — v0.1.0 depende de que
  Stream Deck controle la hotkey nativa de OBS, que es un flujo 100% soportado y estable, pero
  requiere el paso manual de asignación descrito en la web/README.
- Sin perfiles ni distintos valores guardados — un único valor de segundos activo a la vez.
- El texto de la UI del dock está en español, escrito directamente en el código (no pasa por el
  sistema de locales `data/locale/*.ini` de OBS todavía). El plugin usa
  `OBS_MODULE_USE_DEFAULT_LOCALE` y trae `data/locale/en-US.ini` listo (vacío, como en la plantilla
  oficial) para cuando se quiera internacionalizar de verdad vía `obs_module_text()` — decisión
  consciente para no añadir una capa más al MVP.

## 7. Plan de evolución v2

Ver `docs/ROADMAP.md`.

## 8. Licencia y publicación del código

Este proyecto usa como base la plantilla oficial `obsproject/obs-plugintemplate`, que se distribuye
bajo **GPL-2.0-or-later** (ver `LICENSE`). Esto no es una elección de branding: **`libobs`, la
librería contra la que enlaza cualquier plugin de OBS, es en sí misma GPL-2.0**, y el consenso
extendido en la comunidad de OBS es que cualquier plugin que enlace contra ella queda alcanzado por
esa licencia. En la práctica esto significa que, si distribuyes un binario compilado de este plugin,
la GPL te obliga a poner el código fuente correspondiente a disposición de quien reciba ese binario
— "gratuito pero de código cerrado" no es una combinación compatible con esta licencia. (Esto no es
asesoría legal — si necesitas certeza sobre tu caso concreto, conviene una revisión legal real antes
de una distribución a gran escala.)

**Cómo se resuelve en este proyecto:** el código de Trigglow Dynamic Delay se publica en un
**repositorio propio y separado**, dedicado únicamente a este plugin — **no** en el repositorio
principal de Trigglow (donde vive el resto de la app: backend, base de datos, frontend de la web),
que permanece privado. Publicar solo el plugin en su propio repo cumple con la GPL sin exponer nada
del resto del producto. El botón "Código fuente / GitHub" de `/dynamic-delay` en la web apunta a ese
repositorio dedicado, no al monorepo de Trigglow.

Checklist al crear ese repositorio nuevo:
- Nombre sugerido: `obs-trigglow-dynamic-delay` (coincide con el nombre de máquina del plugin).
- Sube exactamente el contenido de este ZIP tal cual — es un proyecto autocontenido, no depende de
  nada del monorepo de Trigglow.
- Actualiza el enlace del botón en `DynamicDelayPage.tsx` (`apps/web/src/pages/DynamicDelayPage.tsx`
  del monorepo de Trigglow) con la URL real de este nuevo repositorio en cuanto exista.
- Mantén el `LICENSE` (GPL-2.0-or-later) tal cual, salvo que una revisión legal indique lo contrario.
