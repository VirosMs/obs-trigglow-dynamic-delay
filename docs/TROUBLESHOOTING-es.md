*[English version](TROUBLESHOOTING.md)*

# Troubleshooting básico — Trigglow Dynamic Delay

## El dock no aparece en OBS
- `View → Docks` y comprueba que "Trigglow Dynamic Delay" esté marcado.
- Revisa el log de OBS (`Ayuda → Registros y Perfiles → Ver log actual`) buscando la línea
  `[obs-trigglow-dynamic-delay] loaded successfully`. Si no aparece, el plugin no se cargó — revisa
  que el binario esté en la carpeta de plugins correcta para tu sistema operativo (ver README) y que
  la arquitectura (64 bits) coincida con tu instalación de OBS.

## El botón "Activar delay" aparece en gris, sin poder pulsarlo
- `Activar delay` permanece deshabilitado hasta que inicies sesión con tu cuenta gratuita de
  Trigglow y elijas una **escena en directo** en el dock. Mientras está deshabilitado, una lista
  debajo indica cuál de las dos cosas falta. La escena auxiliar del propio plugin ("Trigglow Delay
  Buffer (auto)") nunca se ofrece como escena en directo, y una escena guardada que ya no existe
  cuenta como no elegida.
- Mientras el delay se prepara o está activo, `Activar delay` se sustituye por `Desactivar delay`:
  solo hay un botón a la vez.

## No puedo cambiar los segundos de delay, la calidad ni las escenas
- **Es a propósito.** Mientras el delay se prepara (`Preparando delay`) o está activo (`Delay
  activo`), los selectores de escena en directo y de carga, los segundos de delay y la calidad
  mínima están bloqueados: muestran un candado en lugar de sus flechas, y la línea de debajo dice
  "Desactiva el delay para cambiarlo." Antes de la v0.6.0 los segundos seguían editables con el
  delay activo y cada tecla redimensionaba el buffer en pleno directo.
- Pulsa `Desactivar delay`, cambia lo que necesites y vuelve a pulsar `Activar delay`. El buffer se
  vuelve a llenar con los segundos nuevos (con la escena de carga en el Programa, si elegiste una).
- **El dock es el único sitio donde se elige el delay.** Los filtros del propio plugin ("Trigglow
  Buffer Mode Delay (auto)" y los "Trigglow Audio Delay (auto) - …") ya no tienen campos de delay ni
  de calidad en sus propiedades, solo una nota de que los gestiona el dock. Un valor que se les
  escriba desde cualquier otro sitio (el botón de valores por defecto de OBS, obs-websocket) se ignora, con
  un WARN en el log. Con el delay en marcha, desactivar uno de ellos desde fuera del dock (su icono
  del ojo en el diálogo de Filtros, obs-websocket) lo vuelve a activar al momento, también con un
  WARN: desactiva el delay desde el dock.

## El dock muestra un error
- La tarjeta de estado se pone en rojo, con un icono de alerta, el titular "No se pudo activar" y
  una línea de detalle que explica qué falló (por ejemplo: inicia sesión primero, elige una escena en
  directo, o no se pudo crear la escena auxiliar). Es un estado de fallo controlado y sin crash, no
  un cuelgue.
- Corrige lo que describa la línea de detalle (normalmente: vuelve a elegir la escena en directo, ya
  que el nombre de escena guardado puede haber dejado de existir) y pulsa `Activar delay` de nuevo.
- Si la línea de detalle no queda clara o parece incorrecta, revisa el log de OBS buscando líneas con
  el prefijo `[obs-trigglow-dynamic-delay]` alrededor del momento en que apareció el error — llevan
  más detalle del que cabe en el mensaje corto del dock.

## Iniciar sesión desde el dock
- **En la v0.6.0 el inicio de sesión solo funciona en Windows.** En macOS y Linux el plugin todavía
  no tiene cliente HTTP (previsto para la v0.6.1), así que allí no están disponibles el inicio de
  sesión ni, con él, `Activar delay`, Trigi y los reportes desde OBS.
- **Cómo va.** Pulsa **Iniciar sesión**: el navegador abre trigglow.com (inicia sesión o crea ahí una
  cuenta gratuita) y el dock muestra "La página debe mostrar el código KD7F-29QM." (tu código será
  otro). Comprueba que la página muestra el mismo código y pulsa **Confirmar**; el dock lo recoge solo
  unos segundos después.
- **No se abrió el navegador** ("No se pudo abrir el navegador. Copia el enlace y ábrelo tú."): pulsa
  **Copiar enlace** y pégalo en un navegador de este equipo.
- **Los códigos no coinciden, o no lo has iniciado tú**: pulsa **No he sido yo** en la página. No se
  conecta nada, y el dock dice "Inicio de sesión cancelado desde la web." Si sí querías iniciar
  sesión, vuelve a empezar desde el dock.
- **"Esta solicitud viene de una red distinta a la tuya"** en la página: la solicitud de OBS y tu
  navegador parecen venir de redes distintas. Es normal con una VPN o un proxy en solo uno de los
  dos, o con la página abierta en el móvil o desde otra conexión. Si acabas de pulsar **Iniciar
  sesión** tú y los códigos coinciden, puedes confirmar; si no, pulsa **No he sido yo**.
- **"El enlace de inicio de sesión caducó. Vuelve a intentarlo."**: la solicitud no se confirmó a
  tiempo o ya se había usado. Vuelve a pulsar **Iniciar sesión** para obtener un código nuevo.
- **"No se pudo conectar con trigglow.com. Revisa tu conexión e inténtalo de nuevo."**: OBS no llega
  a trigglow.com (sin conexión, cortafuegos, proxy). El plugin solo habla con api.trigglow.com por
  HTTPS; deja pasar a OBS.
- **"Demasiados intentos. Espera un momento y vuelve a intentarlo."**: espera un minuto y vuelve a
  intentarlo.
- **"No se pudo iniciar sesión. Vuelve a intentarlo."**: una respuesta inesperada. Vuelve a
  intentarlo en un momento; si sigue pasando, envía un reporte desde
  [trigglow.com/support](https://www.trigglow.com/support).
- **"Esta versión del plugin ya no puede iniciar sesión. Actualiza el plugin."**: instala la última
  versión (ver "El plugin dice que lo actualice" más abajo).
- **"Tu sesión ha terminado. Vuelve a iniciar sesión para usar el delay."**: el inicio de sesión del
  plugin se revocó o caducó, por ejemplo porque desconectaste el plugin en trigglow.com, cambiaste o
  restableciste allí la contraseña, o porque llevabas mucho tiempo sin abrir OBS. Vuelve a iniciar
  sesión desde el dock.
- **"Tu cuenta de Trigglow no está activa."**: la cuenta está suspendida o no está activa, y `Activar
  delay` queda bloqueado. Pulsa **Abrir trigglow.com** para ver el motivo, o escribe a soporte desde
  la web.

## Dónde se guarda el inicio de sesión y cómo cerrar sesión o desconectar el plugin
- En Windows el plugin guarda su propio inicio de sesión, cifrado con la protección de datos de
  Windows para tu usuario de Windows y aparte de los ajustes del plugin. Otro usuario de Windows, o una
  copia de los ficheros en otro equipo, no puede usarlo: si no se puede descifrar, el plugin lo
  descarta y te pide iniciar sesión. Tu email no se guarda, y el inicio de sesión nunca se adjunta a
  un reporte.
- **Cerrar sesión** en el dock lo revoca en trigglow.com y lo borra de este equipo (la copia local se
  borra aunque no haya conexión).
- **Desde trigglow.com**, por ejemplo si ya no tienes ese equipo: Ajustes → Sesiones activas → Apps
  conectadas → **Desconectar** junto a "Trigglow Dynamic Delay (OBS)". El plugin te pedirá iniciar
  sesión de nuevo la próxima vez que lo compruebe.
- **Si vienes de la v0.5.x**: el inicio de sesión antiguo, guardado en texto plano, se cambia por el
  nuevo en el primer arranque de la 0.6.0, y el fichero antiguo se sobrescribe y se borra. No hay que
  borrar nada a mano.

## El plugin dice que lo actualice
- "Esta versión del plugin ya no puede iniciar sesión. Actualiza el plugin." (dock), "Actualiza el
  plugin para usar Trigi" (Reportar un problema) y "Actualización necesaria" (página de inicio de
  sesión de trigglow.com) significan que tu versión es demasiado antigua. Desde la v0.6.0 el inicio de
  sesión de la v0.5.x y anteriores está retirado, así que esas versiones ya no pueden usar el delay.
- Descarga la última versión (**Descargar la actualización** en el diálogo, el aviso de
  actualización del dock o la
  [última release](https://github.com/VirosMs/obs-trigglow-dynamic-delay/releases/latest)) e
  instálala con OBS cerrado. Tus ajustes se conservan, y el inicio de sesión de la v0.5.x pasa solo;
  si el dock te pide iniciar sesión, hazlo una vez.

## Preguntar a Trigi (Reportar un problema)
- **Trigi no aparece y el diálogo se abre en el formulario de ticket.** Trigi necesita Windows, una
  cuenta con la sesión iniciada y una versión del plugin que admita. Sin sesión, el diálogo ofrece
  **Inicia sesión para preguntar a Trigi** y **Enviar un reporte sin Trigi**; en macOS y Linux, usa el
  formulario web. Si Trigi está apagado de momento, el formulario dice "Trigi no está disponible
  ahora. Tu mensaje va directo al equipo de soporte." y tu mensaje llega a una persona.
- **Qué significa cada mensaje:**
  - "No hay conexión con trigglow.com. Revisa tu conexión y vuelve a intentarlo.": revisa la conexión
    (cortafuegos, proxy, VPN) y pulsa **Reintentar**. Nada se reintenta solo.
  - "Trigi ha tardado demasiado en responder. Vuelve a intentarlo.": vuelve a intentarlo; si sigue
    pasando, crea un ticket.
  - "trigglow.com no responde ahora mismo. Vuelve a intentarlo en unos minutos.": un problema
    temporal por nuestra parte.
  - "Has hecho muchas preguntas seguidas. Vuelve a intentarlo en unos minutos.": **Enviar** queda
    desactivado con una cuenta atrás, y al terminar aparece "Ya puedes volver a preguntar a Trigi."
    Mientras tanto, **Crear ticket** funciona.
  - "Trigi no puede responder ahora. Crea un ticket y el equipo te ayudará.": Trigi no está
    disponible de momento; el ticket llega al equipo.
  - "Solo puedo ayudarte con Trigglow y Dynamic Delay.": pregunta por Dynamic Delay o escribe al
    equipo.
  - "Tu sesión de Trigglow ha terminado. Vuelve a iniciar sesión para seguir preguntando a Trigi…":
    vuelve a iniciar sesión (se completa desde el dock); lo que escribiste se conserva.
  - "Esta conversación se ha cerrado. Reintenta para empezar una nueva.": tu siguiente pregunta
    empieza una conversación nueva.
  - "Actualiza el plugin para usar Trigi.": ver "El plugin dice que lo actualice" más arriba; puedes
    seguir enviando un reporte desde el mismo diálogo.
  - **No enviado** bajo una pregunta: no llegó a trigglow.com. **Reintentar** la vuelve a enviar.
- **Trigi puede equivocarse.** Revisa lo que te diga antes de cambiar tu configuración. Si una
  respuesta no te ayuda, pulsa **No** en "¿Te ha servido?" y crea un ticket con la conversación.
- **Los enlaces de las respuestas de Trigi no se pueden pulsar**, a propósito. Los botones del propio
  diálogo abren las páginas correctas (soporte en trigglow.com, la actualización en GitHub).

## Enviar un reporte
- **Con la sesión iniciada (Windows)**: el ticket se crea desde tu cuenta, así que solo escribes qué
  ha pasado (el asunto es opcional). Para enviar al equipo el log de OBS, abre **Ver qué se envía**,
  revísalo y después marca **Adjuntar el registro (log) de OBS** (empieza desmarcada y no se puede
  marcar antes de haberlo visto): las rutas con tu usuario de Windows, URLs, claves, emails e IPs van
  ocultas, pero pueden quedar nombres de escenas, fuentes, dispositivos de audio y ventanas.
  "Ticket enviado" ofrece **Ver ticket en trigglow.com**; las respuestas llegan por email y a tu panel
  de soporte.
- **"No encontramos el log de OBS. El ticket se enviará sin él."**: el plugin solo adjunta el log del
  OBS en el que se ejecuta, y no lo ha encontrado entre los logs de ese OBS (por ejemplo, si borraste
  los ficheros de log con OBS abierto). Envía el reporte sin él, o reinicia OBS y vuelve a
  intentarlo.
- **"El ticket se creó, pero no se pudo adjuntar el log."**: el ticket existe. Pulsa **Guardar log
  redactado…** y añade ese fichero al ticket en trigglow.com.
- **Sin sesión**: el formulario de invitado pide un nombre y un email para responderte. Si el diálogo
  dice "Para enviar el reporte desde OBS, inicia sesión. También puedes usar el formulario web.",
  trigglow.com está pidiendo a los invitados una comprobación humana (captcha), que OBS no puede
  mostrar: inicia sesión, o pulsa **Abrir soporte web** y envíalo desde el navegador. **Guardar log
  redactado…** guarda el log redactado para que lo adjuntes allí (revísalo antes: pueden quedar
  nombres de escenas, fuentes, dispositivos de audio y ventanas).
- **"Has enviado muchos reportes seguidos. Vuelve a intentarlo en unos minutos."**: espera y vuelve a
  enviarlo.
- **"Tu sesión de Trigglow ha caducado. Vuelve a conectar desde el dock."**: vuelve a iniciar sesión
  y envíalo.
- **macOS y Linux**: en la v0.6.0 el formulario aparece desactivado. Usa
  [trigglow.com/support](https://www.trigglow.com/support) y adjunta tú el log de OBS (revísalo
  antes: ahí el plugin no puede ocultar tus datos personales).

## "Encoding overloaded" (sobrecarga al codificar) o frames perdidos en cuanto activo el delay

**Qué lo causaba antes de la v0.6.0.** Con el delay activo, el plugin comprime (MJPEG) cada frame
capturado y descomprime el retrasado. Hasta la v0.5.0 eso se ejecutaba dentro del hilo gráfico de
OBS — el mismo hilo que tiene que terminar a tiempo cada frame de tu stream (16,7 ms por frame a
60 fps, 33,3 ms a 30 fps) — y el buffer se guardaba a la resolución del **lienzo**. Con un lienzo
2560x1440 y una salida 1920x1080 eso costaba 14,3/17,4 ms p50/p95 por frame en nuestros benchmarks,
la mayor parte de un frame a 60 fps, aunque OBS tira esos píxeles de más al escalar a la salida.
Cuando ese trabajo tarda demasiado, OBS se queda atrás y muestra "Encoding overloaded" / sobrecarga
al codificar (frames saltados por retraso de codificación) o retraso de renderizado (frames
perdidos).

**La v0.6.0 lo corrige.** Lo primero, actualiza (el dock muestra un aviso cuando hay una versión
nueva).
- El buffer nunca se guarda por encima de la resolución de salida de OBS (`Ajustes → Vídeo →
  Resolución de salida (escalada)`). Si bajaste el lienzo para esquivar el problema, puedes volver a
  subirlo.
- La compresión y la descompresión se ejecutan en hilos propios del plugin, no en el hilo gráfico de
  OBS. Lo que el plugin sigue haciendo ahí (leer el frame de vuelta de la GPU y subir el retrasado)
  cuesta ~1,2–1,7/1,6–2,1 ms p50/p95 por frame a 1440p60 en nuestro benchmark fuera de OBS, en vez de
  ~14/17 ms.
- **La salida 1440p a 60 fps está soportada.** Si tu equipo no llega, el plugin baja la calidad del
  buffer paso a paso en vez de dejar que OBS se quede atrás, y te lo dice en el dock (sección
  siguiente). El delay en sí no cambia nunca.

**Si sigue pasando con la v0.6.0:**
- **Descarta al propio codificador:** si "Encoding overloaded" también aparece con el delay
  desactivado, el plugin no es la causa — revisa el preset y los ajustes de tu codificador.
- **Mira el dock mientras ocurre.** Un aviso ámbar "Calidad del buffer reducida" significa que el
  plugin ya está aligerando su propio trabajo (ver la sección siguiente).
- **Pon el lienzo igual que la salida si puedes** (`Ajustes → Vídeo`: Resolución base (lienzo) =
  Resolución de salida (escalada)). Un lienzo mayor ya no le cuesta nada extra al plugin, pero OBS
  sigue renderizando todas las escenas a la resolución del lienzo.
- **Cierra los proyectores, el multiview y las vistas previas extra que no necesites.** Cada frame
  retrasado se descomprime una sola vez aunque haya varias vistas abiertas, pero cada vista abierta
  sigue siendo trabajo de renderizado extra para OBS.
- Si sigues atascado: usa el botón **Reportar un problema** del dock después de tener el delay
  activo al menos 30 segundos en la misma sesión de OBS, y adjunta el log actual de OBS (revísalo con
  **Ver qué se envía** y después marca **Adjuntar el registro (log) de OBS**): incluye las líneas
  `perf check` y `pipeline check` que se explican más abajo.

## El dock dice "Calidad del buffer reducida" (aviso ámbar)

**Qué significa.** Tu equipo no daba para el buffer del delay a calidad completa, así que el plugin
lo aligeró en vez de dejar que OBS perdiera frames. El aviso dice qué redujo y por qué, por ejemplo
"El buffer guarda a 1592×896 en vez de 1920×1080 porque comprimir los fotogramas consume mucha CPU.
Tu delay de 30 s sigue intacto." Las causas que puede indicar son que el hilo gráfico de OBS va muy
cargado, comprimir los fotogramas o descomprimirlos. Si en los últimos 10 segundos se repitió o se
descartó algún fotograma, una segunda línea dice cuántos por minuto; en el directo se ven como un
fotograma repetido, nunca como un cambio del delay.

**Qué hace el plugin, por orden:**
1. Añade hilos de compresión (hasta dos contextos de compresión con hasta cuatro hilos cada uno).
   Esto no muestra aviso: la calidad no se toca.
2. Guarda el buffer a menos resolución, en pasos de un 17% aproximadamente, **nunca por debajo de la
   calidad mínima** que elegiste en el dock.
3. Como último paso, captura un frame de cada dos (30 fps en un stream de 60 fps), solo cuando tu
   salida va a 48 fps o más.

Nunca acorta el delay ni vacía el buffer por esto. Cuando la carga baja y el siguiente peldaño hacia
arriba lleva 30 segundos seguidos con margen de sobra, vuelve a subir de uno en uno, y el aviso
desaparece en cuanto el buffer vuelve a la calidad completa.

**Qué puedes hacer:**
- Nada, si la imagen retrasada te parece bien: está funcionando como se diseñó.
- Cierra lo que compite por la CPU (pestañas del navegador con vídeo, codificaciones en segundo
  plano, una segunda grabación) o baja la carga de CPU del juego.
- Baja la resolución o los fps de salida en `Ajustes → Vídeo` si el aviso aparece todo el rato con
  tus ajustes actuales.
- Una calidad mínima más alta mantiene el buffer más nítido, pero deja al plugin menos pasos antes
  de tener que capturar a la mitad de fps.
- Si crees que el aviso no tiene sentido, envía un reporte con **Reportar un problema** con el delay
  activo al menos 30 segundos: cada cambio de peldaño es una línea WARN en el log, con su causa y las
  cifras que lo provocaron.

## Cómo leer las líneas del log (para soporte)

Con el delay en marcha (Filling o Active), el plugin escribe dos líneas en el log de OBS cada 10
segundos, y una última pareja al desactivar el delay, ponerlo a 0 s o cerrar OBS. Ejemplo (valores
ilustrativos — los tuyos serán distintos; cada una es una sola línea en el log):

```
[obs-trigglow-dynamic-delay] [video-delay-filter] perf check (periodic, 10.0s): buf=1920x1080 (capped to OBS output 1920x1080, canvas 2560x1440) fps=60.00 | render p50/p95/max=2.10/2.90/4.80ms own=0.70/1.00/1.90 readback=0.25/0.40/0.90 upload=0.15/0.22/0.60 | calls=60.0/s captures=60.0/s uploads=60.0/s holds=0 (late decode 0, dropped 0, encoding 0) | obs frames+600 lagged+0 skipped+0
[obs-trigglow-dynamic-delay] [video-delay-filter] pipeline check (periodic): mode=workers rung=0/4 capture=1920x1080@60.00fps enc=1x1 L=3 S=3 | enc0 p50/p95/max=5.10/6.20/9.80ms busy=31% enc1=0.00/0.00/0.00 busy=0% dec=3.60/4.30/6.90 busy=22% | drops reserve=0 backpressure=0 arena=0 capture=0 rawFallback=0 encFail=0 decFail=0 staleEnc=0 decJumps=0 staleDec=0 | queue enc max=1 avg=0.05 dec ahead avg=2.90 min=2 | ratio=16.5x arena=192MB committed (live 110MB, peak 192MB, 6 blocks of 32MB, limit 16384MB) ring=1816 slots (ready 1810, dropped 0, in flight 2, pinned 0)
```

**`perf check`** (lo que cuesta el plugin en el hilo gráfico de OBS):

| Campo | Qué indica |
|---|---|
| `perf check (motivo, Ns)` | Por qué se escribió la línea (`periodic`, `resumed after a pause`, `filter disabled`, `filter destroyed`, `delay set to 0`) y cuántos segundos cubre. |
| `buf=WxH (…)` | Resolución del buffer a calidad completa, y por qué: `capped to OBS output …` (el límite de salida está activo), `not capped …` (normalmente: el lienzo ya es igual o menor que la salida), `cap off (buffer_output_cap=false) …` (límite desactivado a mano, ver abajo). `, reduced for RAM` significa que además tuvo que encogerse para que el delay quepa en el presupuesto de RAM. Lo que se captura ahora mismo, tras una posible degradación, es `capture=` en `pipeline check`. |
| `render p50/p95/max` | Tiempo total de cada llamada del plugin en el hilo gráfico, en ms, **incluido el render de tu escena en directo**, que OBS haría de todos modos. |
| `own` | La parte propia del plugin: conversión, lectura y copia, subida (y el códec en modo síncrono). Es lo que juzga la degradación. Compáralo con el presupuesto por frame: 16,7 ms a 60 fps, 33,3 ms a 30 fps. |
| `readback` / `upload` | Copia GPU→CPU de cada captura y subida del frame retrasado a la GPU. |
| `calls`, `captures`, `uploads` (por segundo) | Llamadas de render del plugin (una por vista abierta: Programa, vista previa, proyectores…), frames capturados (cerca de tus fps, o la mitad en el último paso de degradación) y frames retrasados subidos. |
| `holds (late decode, dropped, encoding)` | Veces que se repitió el último frame porque el que tocaba no estaba listo, separadas por motivo: no se descomprimió a tiempo, se descartó al capturar o todavía se estaba comprimiendo. Alguno suelto no importa; un contador que crece sin parar es lo que hace entrar a la degradación. |
| `obs frames+ lagged+ skipped+` | Contadores propios de OBS en la misma ventana: frames renderizados, frames perdidos por retraso de renderizado y frames saltados por retraso de codificación. `lagged`/`skipped` distintos de cero con un `own` alto apuntan al plugin; con un `own` bajo, busca en otra parte (juego, codificador, carga de la GPU). |

**`pipeline check`** (los hilos de compresión, la memoria del buffer y la degradación):

| Campo | Qué indica |
|---|---|
| `mode` | `workers` (normal) o `sync` (`codec_workers` desactivado, ver abajo: el códec va en el hilo gráfico). |
| `rung=N/M` | Paso de degradación: `0` es la calidad completa, `M` el último paso. Cualquier valor por encima de 0 es de lo que habla el aviso ámbar del dock. |
| `capture=WxH@fps` | Lo que se está capturando ahora mismo. |
| `enc=CxS`, `L`, `S` | Contextos de compresión × hilos por contexto, frames descomprimidos por adelantado (`L`) y slots de staging (`S`). |
| `enc0`, `enc1`, `dec` | p50/p95/max en ms por frame de cada hilo de compresión y del de descompresión, y lo ocupado que estuvo cada uno en la ventana. |
| `drops reserve/backpressure/arena/capture` | Capturas que nunca llegaron al buffer: no había slot libre, la compresión iba atrasada (se descarta la captura más nueva), la memoria del buffer llegó a su límite o falló una lectura de la GPU. Cada una se ve como un frame repetido, nunca como un desplazamiento del delay. |
| `rawFallback`, `encFail`, `decFail` | Frames guardados sin comprimir porque falló la compresión, y fallos de compresión/descompresión. Deberían ser 0. |
| `staleEnc`, `decJumps`, `staleDec` | Compresiones descartadas porque el slot se reutilizó, veces que el descompresor saltó hacia delante para ponerse al día, y frames descomprimidos por adelantado que eran demasiado antiguos para mostrarlos tras una pausa del render. |
| `queue enc`, `dec ahead` | Profundidad de la cola de compresión (máxima, media) y frames listos por delante de lo que hay en pantalla (media, mínimo). |
| `ratio`, `arena`, `ring` | Ratio de compresión, memoria del buffer (comprometida, viva, pico, bloques y su tamaño, límite) y estados de los slots. `committed` se acerca a lo que el Administrador de tareas atribuye al buffer. |

Otras líneas que conviene conocer: `settings loaded` (al arrancar OBS) y `enabled` (cada vez que
pulsas Activar) terminan en `output_cap=on|off` y `codec_workers=on|off`; `delay pipeline: …` (cada
vez que arranca el buffer) da la configuración elegida y la escalera de degradación; un WARN que
empieza por `delay buffer lower rung` o `delay buffer higher rung` marca cada paso de degradación,
con su causa y las cifras de los últimos 2 segundos.

**Líneas de inicio de sesión, Trigi y reportes** (desde la v0.6.0): dicen qué ha pasado y, si hay un
error, el código HTTP: `[auth-manager] signed in`, `signed out`, `session ended`, `token renewed`,
`migrated legacy login`; `[trigi] session started (available=true)`, `[trigi] message sent (len=…,
diagnostics=on|off)`, `[trigi] reply (HTTP 200, len=…, action=…)`, `[trigi] rate limited`; `[support]
ticket created (attachment=yes|no)`. Nunca contienen el enlace ni los códigos del inicio de sesión,
tokens, IDs de tickets o de conversaciones, lo que preguntaste, las respuestas de Trigi ni los datos
técnicos.

## Ajustes ocultos para soporte: `buffer_output_cap` y `codec_workers`

Dos interruptores sin control en el dock a propósito. Úsalos solo si te lo pide soporte, o si sabes
exactamente por qué necesitas uno.

**`buffer_output_cap`: desactivar el límite de salida.** Solo para quien graba (o envía una pista de
Enhanced Broadcasting) por encima de la resolución de salida y prefiere esa calidad para el tramo
retrasado a costa de CPU y RAM. Recupera el dimensionado de la v0.5.0 (resolución del lienzo,
reducida solo por RAM): más píxeles que comprimir por frame, y como hay que guardar más por segundo,
la misma RAM da para menos, así que si falta RAM el buffer se reduce o el delay acaba siendo más
corto de lo pedido. Cuando marcaría la diferencia, el log avisa con una línea como `video encoder
"…" (output "…") is 2560x1440, above the delay buffer's 1920x1080 …: its delayed video is
upscaled`, que además da el tamaño que tendría el buffer sin el límite (y, si no cupiera, cuántos
segundos guardaría).

**`codec_workers`: volver a ejecutar el códec en el hilo gráfico de OBS (interruptor de
emergencia).** Apaga los hilos de compresión: la compresión y la descompresión vuelven al hilo
gráfico de OBS, como antes de la v0.6.0 (con el compresor por slices). La memoria se sigue liberando
fuera de ese hilo, y la degradación sigue funcionando (primero añade hilos de compresión). Sirve
para averiguar si un problema viene de los hilos de compresión — por ejemplo una imagen retrasada
congelada o desordenada, o un crash en un hilo llamado `trigglow-enc`, `trigglow-dec` o
`trigglow-reap`. Devuelve el coste al hilo gráfico, así que a 1440p60 cuenta con ver "Encoding
overloaded" o el aviso ámbar mucho antes.

**Cómo poner cualquiera de los dos:**
1. **Cierra OBS del todo.** Los ajustes solo se leen al arrancar OBS, y OBS reescribe este archivo
   al salir, así que un cambio hecho con OBS abierto se pierde.
2. Abre `%APPDATA%\obs-studio\plugin_config\obs-trigglow-dynamic-delay\buffer-mode-settings.json`
   con un editor de texto (OBS portable: `config\obs-studio\plugin_config\obs-trigglow-dynamic-delay\`
   dentro de la carpeta de OBS; macOS/Linux: la misma carpeta
   `plugin_config/obs-trigglow-dynamic-delay/` dentro del directorio de configuración de OBS). Si el
   archivo todavía no existe, abre y cierra OBS una vez con el plugin instalado.
3. Añade `"buffer_output_cap": false` y/o `"codec_workers": false` como una clave más, dejando todo
   lo demás como está (ojo con la coma de la línea anterior). Tiene que ser un `false` literal, sin
   comillas: `"false"` (texto), `0` o cualquier otra cosa deja el ajuste activado, el log explica
   por qué (`"codec_workers" … is not a JSON boolean`) y ese valor desaparece del archivo al cerrar
   OBS.

   ```json
   {
       "delay_seconds": 30,
       "overlay_corner": 0,
       "codec_workers": false
   }
   ```

   (Tu archivo tiene más claves y tus propios valores; solo la última línea es nueva.)
4. Arranca OBS. La línea `settings loaded` termina en `output_cap=off` y/o `codec_workers=off`, y
   otra vez la línea `enabled` al pulsar Activar. Con el límite desactivado, `perf check` dice
   `cap off (buffer_output_cap=false)`; con los hilos de compresión apagados, `pipeline check` dice
   `mode=sync`.

Para deshacerlo: con OBS cerrado, borra la clave (o ponla a `true`) y vuelve a arrancar OBS.

**Si el archivo queda con un JSON inválido** (basta con una coma o unas comillas de menos), el
plugin no lo sobrescribe: al arrancar lo aparta como `buffer-mode-settings.json.<fecha-hora>.invalid`
en la misma carpeta, usa los valores por defecto (también para las escenas en directo y de carga, el
delay y el overlay, hasta que los vuelvas a elegir) y escribe un WARN con las dos rutas
(`settings file … is not valid JSON -- moved it to …`). Corrige la copia, cierra OBS, renómbrala de
vuelta a `buffer-mode-settings.json` y vuelve a arrancar OBS.

## El vídeo retrasado se ve con menos calidad de la esperada
- **Mira primero el dock.** Un aviso ámbar "Calidad del buffer reducida" significa que el plugin
  está guardando el buffer a menos resolución o menos fps por ahora porque tu equipo no llega (ver
  arriba). Vuelve solo a la calidad completa cuando baja la carga.
- Si no, es el dimensionado del buffer, no un bug: `EnsureRingSized()` calcula un presupuesto de RAM
  a partir de la RAM total de tu máquina (el 50%, acotado entre 1GB y 24GB) y nunca deja que el vídeo
  almacenado baje de la **calidad mínima** que elegiste (480p/720p/1080p) — en su lugar, si los
  segundos pedidos × esa calidad no caben en el presupuesto, acorta la **duración realmente
  almacenada**. Y el buffer nunca se guarda por encima de la resolución de salida de OBS, así que con
  una salida de 720p es 720p aunque el mínimo sea 1080p — OBS no emite nada por encima de su salida
  de todos modos.
- Con un lienzo mayor que la salida, el frame retrasado se vuelve a dibujar a tamaño de lienzo y OBS
  lo reduce otra vez, lo que puede verse algo más blando que la imagen en directo. Si no necesitas el
  lienzo grande, pon lienzo = salida.
- Si **grabas** a una resolución mayor que la salida (p. ej. grabación reescalada al lienzo), el
  tramo retrasado de esa grabación se reescala hacia arriba desde el buffer del tamaño de la salida;
  el log avisa de ello. Mira `buffer_output_cap` más arriba si prefieres el comportamiento anterior.
- Para saber si tu combinación de segundos + calidad pedida realmente cabe en tu RAM, mira la línea
  de estimación de RAM bajo los campos de delay y calidad del dock *antes* de pulsar Activar — "En la
  RAM caben los 30 s completos a 1920×1080", o "En la RAM solo caben 41 s de 60 s a …" (sin
  bloquear). Con el delay en marcha da los mismos segundos enteros que el chip de estado, y se oculta
  mientras se muestra el aviso ámbar "Calidad del buffer reducida", que entonces dice lo que de
  verdad guarda el buffer. Ver `docs/SPEC-es.md` §3.5 para la lógica completa de dimensionado.
- Si quieres garantizar la duración completa pedida, baja el suelo de calidad, baja los segundos
  pedidos, o libera RAM del sistema (cierra otras aplicaciones que consuman mucha RAM) antes de
  pulsar Activar.

## Tras actualizar, una acción de Stream Deck u obs-websocket ya no funciona
- La v0.6.0 renombró los objetos que gestiona el plugin: "(no tocar)" / "(auto, no tocar)" pasaron
  a "(auto)". La escena es ahora **Trigglow Delay Buffer (auto)**, los filtros **Trigglow Buffer
  Mode Delay (auto)** y `Trigglow Audio Delay (auto) - <fuente>`, y el overlay **Trigglow Delay Badge
  (auto)**. Tus colecciones se renombraron solas la primera vez que se cargaron.
- Cualquier acción que se refiera a ellos **por su nombre** (cambiar a la escena auxiliar, activar o
  desactivar un filtro de delay o la fuente del overlay) hay que apuntarla a los nombres nuevos. Las
  acciones que usan los atajos del plugin ("Trigglow: Activar/desactivar Dynamic Delay"…) no se ven
  afectadas.

## Volví a una versión anterior y ahora hay dos escenas auxiliares
- La v0.5.0 y anteriores no conocen los nombres "(auto)", así que crean su propia escena auxiliar,
  sus filtros de delay y su overlay "(no tocar)" junto a los renombrados (los filtros "(auto)" se
  desactivan al cargar la colección, así que se quedan inertes).
- Borra el juego que no uses. Si más adelante vuelves a la v0.6.0, se usa el juego "(auto)", y un
  juego "(no tocar)" sobrante no se renombra mientras su nombre nuevo esté ocupado: bórralo.

## El delay se desactivó al cambiar de colección de escenas
- **Es a propósito.** La escena auxiliar y los filtros de delay pertenecen a la colección en la que
  estabas, así que la nueva funcionaría sin ningún delay mientras el dock seguía diciendo "Delay
  activo". Desde la v0.6.0 el plugin desactiva el delay antes de que OBS deje la colección antigua
  (al cambiar, crear o eliminar una colección), la guarda con sus filtros apagados y su escena de
  vuelta en el Programa, y el dock dice: "El delay se desactivó al cambiar de colección de escenas.
  Vuelve a activarlo cuando quieras."
- Duplicar o renombrar una colección funciona igual, con una diferencia: OBS 31 no avisa de eso por
  adelantado, así que el plugin detiene el delay justo después de que se cargue la copia, apaga los
  filtros de la copia y sale de la escena auxiliar si la copia se abrió en ella.
- Pulsa `Activar delay` de nuevo en la colección nueva cuando quieras (elige antes su escena en
  directo si es otra).

## Borré uno de los filtros del plugin con el delay en marcha (problema conocido)
- Borrar a mano un filtro de delay "(auto)" con el delay en marcha deja salir el vídeo en directo
  sin delay: la escena auxiliar muestra entonces tu escena en directo sin filtro. Sigue así hasta que
  pulses `Desactivar delay` y `Activar delay` otra vez en el dock, que vuelve a crear el filtro (antes
  el buffer se vuelve a llenar).
- No borres ni renombres la escena ni los filtros del plugin; para parar el delay, usa el dock.

## Cambié el Programa a mi escena en directo y el directo salió sin delay (problema conocido)
- Con el delay en marcha, el Programa tiene que quedarse en la escena que eligió el plugin: la escena
  de carga mientras se llena, y después su escena auxiliar "Trigglow Delay Buffer (auto)", que
  contiene tu escena en directo con el filtro de delay. Si haces clic en tu escena en directo en la
  lista de escenas de OBS (o una acción de Stream Deck u obs-websocket cambia a ella), el Programa
  muestra la escena en directo tal cual y el directo la emite **sin delay**. Ya pasaba antes de la
  v0.6.0.
- Deja el Programa en la escena que eligió el plugin mientras el delay está en marcha. Para cambiar
  de escena, desactiva antes el delay desde el dock, cambia y vuelve a activarlo (el buffer se vuelve
  a llenar).

## El buffer nunca parece llenarse / `Preparando delay` nunca llega a `Delay activo`
- Revisa el log de OBS buscando avisos con el prefijo `[obs-trigglow-dynamic-delay]` alrededor del
  momento en que pulsaste Activar. Dos causas conocidas que registran un aviso específico en vez de
  simplemente no hacer nada en silencio:
  - `Render(): obs_filter_get_target() returned null` — el target del filtro de la escena
    contenedora desapareció (por ejemplo, la escena en directo se borró o se renombró por debajo del
    plugin después de pulsar Activar).
  - `Render(): target's base size is 0x0, skipping until it's real` — la escena en directo todavía no
    ha producido un tamaño de frame real (esto es normal en el primer render o los dos primeros justo
    después de Activar y debería resolverse solo; si persiste, comprueba que la escena en directo
    tenga de verdad fuentes visibles y activas dentro).
- Si no aparece ninguno de los dos avisos y la cuenta atrás simplemente parece lenta, eso es lo
  esperado: la ventana de llenado dura exactamente lo mismo que el delay pedido — un delay de 60
  segundos tarda 60 segundos reales en llenarse antes de pasar a `Delay activo`.
- Si el dock vuelve a `Preparando delay` con el delay ya activo, el buffer tuvo que reiniciarse (un
  cambio en los ajustes de vídeo de OBS). El log dice `the video buffer restarted empty`; el Programa
  muestra la escena de carga hasta que se rellena.

## El estado se queda en `Preparando delay` mucho más tiempo que los segundos pedidos
- Esto es un síntoma, no un modo documentado — el plugin no tiene ningún estado de reconexión ni de
  "Applying" que esperar. Si de verdad nunca llega a `Delay activo`, sigue la sección de arriba y
  revisa el log de OBS buscando avisos.

## La hotkey no hace nada
- Confirma que la asignaste en `Ajustes → Atajos de teclado` (buscando "Trigglow") — el plugin
  registra 3 atajos (`Trigglow: Activar/desactivar Dynamic Delay`, `Trigglow: Activar Dynamic
  Delay` y `Trigglow: Desactivar Dynamic Delay`; con OBS en inglés, `Trigglow: Toggle Dynamic
  Delay`, `Trigglow: Enable Dynamic Delay` y `Trigglow: Disable Dynamic Delay`) sin ninguna tecla
  asignada por defecto.
- Prueba primero el botón del dock para descartar que sea un problema de configuración del plugin en
  sí (por ejemplo, que no haya ninguna escena en directo elegida, lo cual también bloquea que la
  hotkey de Activar o Activar/desactivar haga algo), y no de la asignación de la hotkey/Stream Deck.

## El botón de Stream Deck no hace nada
- Verifica que la combinación configurada en Stream Deck coincide exactamente con la asignada en
  OBS.
- Si el botón cambia de escena o activa filtros por su nombre en vez de usar el atajo, mira "Tras
  actualizar, una acción de Stream Deck u obs-websocket ya no funciona" más arriba.
- Ver `docs/STREAM_DECK.md` para el flujo completo paso a paso.

## El plugin no compila
- Revisa `docs/BUILD_VALIDATION.md` — ahí se documenta qué se ha verificado (builds reales en CI
  contra el SDK real de OBS y Qt6 real en Windows/macOS/Ubuntu, además de pruebas en directo dentro de
  una instancia real de OBS en Windows) y qué no (pruebas en directo en macOS/Linux, y una corrección
  confirmada para el riesgo de GPU Device-Remove/Reset descrito en `docs/SPEC-es.md` §7). Los tests
  unitarios (9 suites, sin OBS) se compilan con `-DTRIGGLOW_BUILD_TESTS=ON` o por separado con
  `cmake -S tests -B build_tests`, y CI los ejecuta en todas las plataformas.
- Un error de compilación en tu propia máquina es información valiosa — probablemente sea una
  diferencia entre la versión exacta de OBS/Qt que estás usando y las fijadas en `buildspec.json`.
  Abre un issue en el repositorio con el log de error completo.

## No sé si el problema es del plugin o de mi configuración de streaming
- Desactiva el plugin (bórralo temporalmente de la carpeta de plugins) y confirma que el streaming
  funciona bien sin él. Si el problema persiste sin el plugin, no es cosa del plugin — y esta es una
  prueba especialmente limpia para este plugin en concreto, ya que nunca toca el output de streaming
  en absoluto; si tienes problemas de streaming, no los causó él.
