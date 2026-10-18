*[English version](TROUBLESHOOTING.md)*

# Troubleshooting básico — Trigglow Dynamic Delay

## El dock no aparece en OBS
- `View → Docks` y comprueba que "Trigglow Dynamic Delay" esté marcado.
- Revisa el log de OBS (`Ayuda → Registros y Perfiles → Ver log actual`) buscando la línea
  `[obs-trigglow-dynamic-delay] loaded successfully`. Si no aparece, el plugin no se cargó — revisa
  que el binario esté en la carpeta de plugins correcta para tu sistema operativo (ver README) y que
  la arquitectura (64 bits) coincida con tu instalación de OBS.

## El botón "Enable" aparece en gris, sin poder pulsarlo
- `Enable` permanece deshabilitado hasta que elijas una **escena en directo** en el combo box del
  dock — el plugin se niega a armarse sin nada que almacenar en el buffer, en vez de dejarte pulsar y
  acabar en `Error`. Elige tu escena en directo en el desplegable; `Enable` se vuelve pulsable en
  cuanto hay una seleccionada.
- `Enable`/`Disable` también están ambos deshabilitados mientras ya hay una operación en curso (el
  dock lo muestra también poniendo en gris los combos de escena, el spinner de segundos y el combo de
  calidad durante `Filling`/`Active`). Esto es lo esperado: cambia escenas/segundos/calidad solo
  mientras está en `Inactive`.

## El dock muestra un estado "Error"
- La línea de estado del dock muestra `● Error` con una línea de detalle legible debajo explicando
  qué falló (por ejemplo, que la escena en directo elegida ya no corresponde a ninguna escena real de
  OBS, o que se eliminó una fuente que antes existía). Es un estado de fallo controlado y sin crash,
  no un cuelgue.
- Pulsa `Disable` para volver a `Inactive`, corrige lo que describa la línea de detalle
  (normalmente: vuelve a elegir la escena en directo, ya que el nombre de escena guardado puede haber
  dejado de existir) y pulsa `Enable` de nuevo.
- Si la línea de detalle no queda clara o parece incorrecta, revisa el log de OBS buscando líneas con
  el prefijo `[obs-trigglow-dynamic-delay]` alrededor del momento en que apareció `Error` — llevan más
  detalle del que cabe en el mensaje corto del dock.

## "Encoding overloaded" (sobrecarga al codificar) o frames perdidos en cuanto activo el delay

**Qué lo causa.** Con el delay activo, el plugin comprime (MJPEG) cada frame capturado y descomprime
el retrasado dentro del hilo gráfico de OBS — el mismo hilo que tiene que terminar a tiempo cada
frame de tu stream (16,7 ms por frame a 60 fps, 33,3 ms a 30 fps). Cuando ese trabajo tarda
demasiado, OBS se queda atrás y muestra "Encoding overloaded" / sobrecarga al codificar (frames
saltados por retraso de codificación) o retraso de renderizado (frames perdidos). Hasta la v0.5.0
el buffer se guardaba a la resolución del **lienzo**, así que con un lienzo 2560x1440 y una salida
1920x1080 cada frame costaba ahí 14,3/17,4 ms p50/p95 en nuestros benchmarks — la mayor parte de
un frame a 60 fps — aunque OBS tira esos píxeles de más al escalar a la salida.

**La v0.5.1 corrige el caso de lienzo mayor que la salida.** El buffer nunca se guarda por encima
de la resolución de salida de OBS (`Ajustes → Vídeo → Resolución de salida (escalada)`), y la
compresión usa varios hilos: el mismo montaje de lienzo 1440p / salida 1080p cuesta ahora ~5,2/5,9
ms por frame en el mismo benchmark. Lo primero, actualiza a la v0.5.1 (el dock muestra un aviso
cuando hay una versión nueva). Si bajaste el lienzo para esquivar el problema, puedes volver a subirlo.

**Si sigue pasando con la v0.5.1:**
- **Pon el lienzo igual que la salida si puedes** (`Ajustes → Vídeo`: Resolución base (lienzo) =
  Resolución de salida (escalada)). Un lienzo mayor ya no le cuesta nada extra al plugin, pero OBS
  sigue renderizando todas las escenas a la resolución del lienzo.
- **Cierra los proyectores, el multiview y las vistas previas extra que no necesites.** Desde la
  v0.5.1 cada frame retrasado se descomprime una sola vez aunque haya varias vistas abiertas, pero
  cada vista abierta sigue siendo trabajo de renderizado extra para OBS.
- **Salida 1440p (no solo lienzo):** a 30 fps debería funcionar; a 60 fps todavía puede sobrecargar
  OBS, porque la compresión sigue ejecutándose dentro del hilo gráfico. Sacarla de ese hilo, con
  1440p60 como meta, es el plan para la siguiente versión (v0.6.0, ver `docs/ROADMAP-es.md`);
  todavía no tiene fecha. Mientras tanto, usa salida 1080p para 60 fps, o 30 fps con salida 1440p.
- **Descarta al propio codificador:** si "Encoding overloaded" también aparece con el delay
  desactivado, el plugin no es la causa — revisa el preset y los ajustes de tu codificador.
- Si sigues atascado: usa el botón **Reportar un problema** del dock después de tener el delay
  Activo al menos 30 segundos en la misma sesión de OBS. Adjunta automáticamente el log actual de
  OBS, incluidas las líneas `perf check` de abajo.

**Cómo leer la línea `perf check` (para soporte).** Con el delay activo (Filling o Active), el
plugin escribe una línea en el log de OBS cada 10 segundos, y una última al desactivar el delay o
cerrar OBS. Ejemplo (valores ilustrativos — los tuyos serán distintos):

```
[obs-trigglow-dynamic-delay] [video-delay-filter] perf check (periodic, 10.0s): buf=1920x1080 (capped to OBS output 1920x1080, canvas 2560x1440) fps=60.00 | render p50/p95/max=5.20/5.90/8.40ms readback=0.35/0.52/1.10 encode=2.10/2.60/4.30 decode=1.70/2.00/3.10 upload=0.40/0.55/1.20 | calls=60.0/s captures=60.0/s uploads=60.0/s holds=0 rawFallback=0 | obs frames+600 lagged+0 skipped+0 | ratio=12.4x (cycle 12.1x) ring=1077MB/1816 slots growths=0
```

| Campo | Qué indica |
|---|---|
| `perf check (motivo, Ns)` | Por qué se escribió la línea (`periodic`, `resumed after a pause`, `filter disabled`, `filter destroyed`) y cuántos segundos cubre. |
| `buf=WxH (…)` | Resolución a la que se guarda el buffer, y por qué: `capped to OBS output …` (el límite de la v0.5.1 está activo), `not capped …` (normalmente: el lienzo ya es igual o menor que la salida), `cap off (buffer_output_cap=false) …` (límite desactivado a mano, ver abajo). `, reduced for RAM` significa que además tuvo que encogerse para que el delay quepa en el presupuesto de RAM. |
| `render p50/p95/max` | Tiempo total que el plugin pasa por llamada en el hilo gráfico de OBS, en ms. Compáralo con el presupuesto por frame: 16,7 ms a 60 fps, 33,3 ms a 30 fps. Un p95 muy por debajo del presupuesto significa que no es el plugin lo que sobrecarga OBS. |
| `readback` / `encode` / `decode` / `upload` | Lo mismo, por fase: copia GPU→CPU, compresión, descompresión y subida del frame retrasado a la GPU. |
| `calls`, `captures`, `uploads` (por segundo) | Llamadas de render del plugin (una por vista abierta: Programa, vista previa, proyectores…), frames capturados (debería acercarse a tus fps) y frames retrasados subidos. |
| `holds` | Veces que se repitió el último frame porque el que tocaba no se podía mostrar. Alguno suelto no importa; un contador que crece sin parar significa que la captura se está quedando atrás. |
| `rawFallback` | Frames guardados sin comprimir porque falló la compresión. Debería ser 0. |
| `obs frames+ lagged+ skipped+` | Contadores propios de OBS en la misma ventana: frames renderizados, frames perdidos por retraso de renderizado y frames saltados por retraso de codificación. `lagged`/`skipped` distintos de cero con un p95 de `render` cerca del presupuesto apuntan al plugin; con un `render` bajo, busca en otra parte (juego, codificador, carga de la GPU). |
| `ratio`, `ring`, `growths` | Ratio de compresión (de esta ventana y del ciclo de delay completo), RAM reservada para el buffer y su número de slots, y cuántos slots tuvieron que crecer por encima de su tamaño presupuestado. Desde la v0.5.1 el buffer no se rellena con ceros, así que la memoria que el Administrador de tareas le atribuye (la residente) suele quedar bastante por debajo de `ring`: solo lo que los frames han escrito de verdad. |

La línea `settings loaded` (al arrancar OBS) y la línea `enabled` (cada vez que pulsas Enable)
terminan en `output_cap=on` u `output_cap=off`, para que soporte sepa qué dimensionado se usaba.

**Desactivar el límite de salida (`buffer_output_cap`).** Solo para quien graba (o envía una pista
de Enhanced Broadcasting) por encima de la resolución de salida y prefiere esa calidad para el
tramo retrasado a costa de CPU. Recupera el dimensionado de la v0.5.0, incluido el riesgo de
sobrecarga al codificar descrito arriba, y como hay que guardar más píxeles por segundo, la misma
RAM da para menos: si falta RAM, el buffer se reduce o el delay acaba siendo más corto de lo pedido.
Cuando marcaría la diferencia, el log avisa con una línea como `video encoder "…" (output "…") is
2560x1440, above the delay buffer's 1920x1080 …: its delayed video is upscaled`, que además da el
tamaño que tendría el buffer sin el límite (y, si no cupiera, cuántos segundos guardaría).

1. **Cierra OBS del todo.** El ajuste solo se lee al arrancar OBS, y OBS reescribe este archivo al
   salir, así que un cambio hecho con OBS abierto se pierde.
2. Abre `%APPDATA%\obs-studio\plugin_config\obs-trigglow-dynamic-delay\buffer-mode-settings.json`
   con un editor de texto (OBS portable: `config\obs-studio\plugin_config\obs-trigglow-dynamic-delay\`
   dentro de la carpeta de OBS; macOS/Linux: la misma carpeta
   `plugin_config/obs-trigglow-dynamic-delay/` dentro del directorio de configuración de OBS). Si el
   archivo todavía no existe, abre y cierra OBS una vez con el plugin instalado.
3. Añade `"buffer_output_cap": false` como una clave más, dejando todo lo demás como está (ojo con
   la coma de la línea anterior). Tiene que ser un `false` literal, sin comillas: `"false"` (texto),
   `0` o cualquier otra cosa deja el límite activado y el log explica por qué
   (`"buffer_output_cap" … is not a JSON boolean`).

   ```json
   {
       "delay_seconds": 30,
       "overlay_corner": 0,
       "buffer_output_cap": false
   }
   ```

   (Tu archivo tiene más claves y tus propios valores; solo la última línea es nueva.)
4. Arranca OBS. El log muestra `output_cap=off` en la línea `settings loaded`, y otra vez al pulsar
   Enable; la línea `perf check` dice `cap off (buffer_output_cap=false)`.

Si el archivo queda con un JSON inválido (basta con una coma o unas comillas de menos), el plugin no
lo sobrescribe: al arrancar lo aparta como `buffer-mode-settings.json.<fecha-hora>.invalid` en la
misma carpeta, usa los valores por defecto (también para las escenas en directo y de carga, el delay
y el overlay, hasta que los vuelvas a elegir) y escribe un WARN con las dos rutas
(`settings file … is not valid JSON -- moved it to …`). Corrige la copia, cierra OBS, renómbrala de
vuelta a `buffer-mode-settings.json` y vuelve a arrancar OBS.

Para deshacerlo: con OBS cerrado, borra la clave (o ponla a `true`) y vuelve a arrancar OBS.

## El vídeo retrasado se ve con menos calidad de la esperada
- Esto es el comportamiento normal del dimensionado del buffer, no un bug: `EnsureRingSized()` calcula
  un presupuesto de RAM a partir de la RAM total de tu máquina (el 50%, acotado entre 1GB y 24GB) y
  nunca deja que el vídeo almacenado baje de la **calidad mínima** que elegiste (480p/720p/1080p) —
  en su lugar, si los segundos pedidos × esa calidad no caben en el presupuesto, acorta en silencio la
  **duración realmente almacenada**. La única excepción, desde la v0.5.1: el buffer nunca se guarda
  por encima de la resolución de salida de OBS, así que con una salida de 720p es 720p aunque el
  mínimo sea 1080p — OBS no emite nada por encima de su salida de todos modos. Así que si la imagen
  en sí se ve borrosa, es muy poco probable que sea el plugin bajando la calidad; revisa mejor la
  resolución de tu lienzo/output de OBS y los ajustes del codificador.
- Si **grabas** a una resolución mayor que la salida (p. ej. grabación reescalada al lienzo), desde
  la v0.5.1 el tramo retrasado de esa grabación se reescala hacia arriba desde el buffer del tamaño
  de la salida; el log avisa de ello. Mira `buffer_output_cap` en la sección de arriba si prefieres
  el comportamiento anterior.
- Para saber si tu combinación de segundos + calidad pedida realmente cabe en tu hardware, mira la
  línea de estimación de ajuste en vivo del dock *antes* de pulsar Enable — avisa (sin bloquear)
  cuando la duración completa pedida no cabrá a la calidad elegida, y te dice qué se logrará
  realmente. Ver `docs/SPEC.md` §3.5 para la lógica completa de dimensionado.
- Si quieres garantizar la duración completa pedida, baja el suelo de calidad, baja los segundos
  pedidos, o libera RAM del sistema (cierra otras aplicaciones que consuman mucha RAM) antes de
  pulsar Enable.

## El buffer nunca parece llenarse / "Filling" nunca llega a "Active"
- Revisa el log de OBS buscando avisos con el prefijo `[obs-trigglow-dynamic-delay]` alrededor del
  momento en que pulsaste Enable. Dos causas conocidas que registran un aviso específico en vez de
  simplemente no hacer nada en silencio:
  - `Render(): obs_filter_get_target() returned null` — el target del filtro de la escena
    contenedora desapareció (por ejemplo, la escena en directo se borró o se renombró por debajo del
    plugin después de pulsar Enable).
  - `Render(): target's base size is 0x0, skipping until it's real` — la escena en directo todavía no
    ha producido un tamaño de frame real (esto es normal en el primer render o los dos primeros justo
    después de Enable y debería resolverse solo; si persiste, comprueba que la escena en directo
    tenga de verdad fuentes visibles y activas dentro).
- Si no aparece ninguno de los dos avisos y la cuenta atrás simplemente parece lenta, eso es lo
  esperado: la ventana de llenado dura exactamente lo mismo que los `delaySeconds` pedidos — un delay
  de 60 segundos tarda 60 segundos reales en llenarse antes de pasar a `Active`.

## El estado se queda colgado en "Filling" mucho más tiempo que los segundos pedidos
- Esto es un síntoma, no un modo documentado — el plugin no tiene ningún estado de reconexión ni de
  "Applying" que esperar. Si `Filling` de verdad nunca llega a `Active`, sigue la sección "el buffer
  nunca parece llenarse" de arriba y revisa el log de OBS buscando avisos.

## La hotkey no hace nada
- Confirma que la asignaste en `Configuración → Atajos de teclado` (buscando "Trigglow") — el plugin
  registra 3 hotkeys (`Trigglow: Toggle Dynamic Delay`, `Trigglow: Enable Dynamic Delay`,
  `Trigglow: Disable Dynamic Delay`) sin ninguna tecla asignada por defecto.
- Prueba primero el botón del dock para descartar que sea un problema de configuración del plugin en
  sí (por ejemplo, que no haya ninguna escena en directo elegida, lo cual también bloquea que la
  hotkey de Enable/Toggle haga algo), y no de la asignación de la hotkey/Stream Deck.

## El botón de Stream Deck no hace nada
- Verifica que la combinación configurada en Stream Deck coincide exactamente con la asignada en
  OBS.
- Ver `docs/STREAM_DECK.md` para el flujo completo paso a paso.

## El plugin no compila
- Revisa `docs/BUILD_VALIDATION.md` — ahí se documenta qué se ha verificado (builds reales en CI
  contra el SDK real de OBS y Qt6 real en Windows/macOS/Ubuntu, además de pruebas en directo dentro de
  una instancia real de OBS en Windows) y qué no (pruebas en directo en macOS/Linux, una corrección
  confirmada para el riesgo de GPU Device-Remove/Reset descrito en `docs/SPEC.md` §7, y cualquier
  suite amplia de pruebas automatizadas — la v0.5.1 solo añade tests unitarios del cálculo de
  dimensionado del buffer, que se compilan con `-DTRIGGLOW_BUILD_TESTS=ON`).
- Un error de compilación en tu propia máquina es información valiosa — probablemente sea una
  diferencia entre la versión exacta de OBS/Qt que estás usando y las fijadas en `buildspec.json`.
  Abre un issue en el repositorio con el log de error completo.

## No sé si el problema es del plugin o de mi configuración de streaming
- Desactiva el plugin (bórralo temporalmente de la carpeta de plugins) y confirma que el streaming
  funciona bien sin él. Si el problema persiste sin el plugin, no es cosa del plugin — y esta es una
  prueba especialmente limpia para este plugin en concreto, ya que nunca toca el output de streaming
  en absoluto; si tienes problemas de streaming, no los causó él.
