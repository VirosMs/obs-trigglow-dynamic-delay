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
  el prefijo `[trigglow-dynamic-delay]` alrededor del momento en que apareció `Error` — llevan más
  detalle del que cabe en el mensaje corto del dock.

## El vídeo retrasado se ve con menos calidad de la esperada
- Esto es el comportamiento normal del dimensionado del buffer, no un bug: `EnsureRingSized()` calcula
  un presupuesto de RAM a partir de la RAM total de tu máquina (el 50%, acotado entre 1GB y 24GB) y
  nunca deja que el vídeo almacenado baje de la **calidad mínima** que elegiste (480p/720p/1080p) —
  en su lugar, si los segundos pedidos × esa calidad no caben en el presupuesto, acorta en silencio la
  **duración realmente almacenada**. Así que si la imagen en sí se ve borrosa, es muy poco probable
  que sea el plugin bajando la calidad; revisa mejor la resolución de tu lienzo/output de OBS y los
  ajustes del codificador.
- Para saber si tu combinación de segundos + calidad pedida realmente cabe en tu hardware, mira la
  línea de estimación de ajuste en vivo del dock *antes* de pulsar Enable — avisa (sin bloquear)
  cuando la duración completa pedida no cabrá a la calidad elegida, y te dice qué se logrará
  realmente. Ver `docs/SPEC.md` §3.5 para la lógica completa de dimensionado.
- Si quieres garantizar la duración completa pedida, baja el suelo de calidad, baja los segundos
  pedidos, o libera RAM del sistema (cierra otras aplicaciones que consuman mucha RAM) antes de
  pulsar Enable.

## El buffer nunca parece llenarse / "Filling" nunca llega a "Active"
- Revisa el log de OBS buscando avisos con el prefijo `[trigglow-dynamic-delay]` alrededor del
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
  confirmada para el riesgo de GPU Device-Remove/Reset descrito en `docs/SPEC.md` §6, y cualquier
  suite de pruebas automatizadas dedicada).
- Un error de compilación en tu propia máquina es información valiosa — probablemente sea una
  diferencia entre la versión exacta de OBS/Qt que estás usando y las fijadas en `buildspec.json`.
  Abre un issue en el repositorio con el log de error completo.

## No sé si el problema es del plugin o de mi configuración de streaming
- Desactiva el plugin (bórralo temporalmente de la carpeta de plugins) y confirma que el streaming
  funciona bien sin él. Si el problema persiste sin el plugin, no es cosa del plugin — y esta es una
  prueba especialmente limpia para este plugin en concreto, ya que nunca toca el output de streaming
  en absoluto; si tienes problemas de streaming, no los causó él.
