*[English version](FAQ.md)*

# FAQ — Trigglow Dynamic Delay

**¿Se corta o reconecta el stream al activar, desactivar o usar el delay?**
No — nunca, en ningún momento, por ningún motivo. Ese es precisamente el objetivo del plugin: no
llama en absoluto al ajuste de delay nativo del output de streaming de OBS. En su lugar, dos
filtros de OBS acumulan vídeo y audio en un anillo de RAM del sistema, y el plugin cambia el
Programa de OBS entre tu escena en directo, una escena de carga opcional y una escena "wrapper"
delayed, todo internamente — el output de streaming en sí nunca ve nada de esto. Detalle técnico
completo, incluyendo por qué se probó y se abandonó un diseño anterior basado en reconexión, en
`docs/SPEC.md` §2–§3.

**¿El audio también se retrasa, en sincronía con el vídeo?**
Sí. Vídeo y audio usan el mismo delay configurado en segundos, así que vuelven a estar en
sincronía — el audio usa un filtro aparte adjunto a cada fuente individual con audio dentro de tu
escena en directo (OBS no permite adjuntar un filtro de audio directamente a una escena), pero
ambos funcionan con el mismo valor de `delaySeconds`. Desde la v0.3.2, esto sigue siendo cierto
incluso si el presupuesto de RAM obliga al vídeo a guardar de verdad menos tiempo del pedido (ver la
siguiente pregunta) — el audio se ajusta automáticamente a lo que el vídeo realmente puede entregar,
en vez de quedarse por delante. Desde la v0.6.0 ambos usan exactamente el mismo delay en segundos
enteros también en ese caso (antes podían quedar hasta ~1 s separados), y el dock y el overlay del
directo muestran esa misma cifra. Ver `docs/SPEC-es.md` §3.2/§3.3.

**¿Cómo interactúan los ajustes "Delay (segundos)" y "Calidad mínima"?**
De forma independiente, por diseño — eliges ambos libremente. La calidad mínima (480p/720p/1080p)
marca hasta dónde puede bajar el plugin la resolución del buffer para ahorrar RAM: nunca baja de
ahí, y en su lugar acorta el tiempo real de buffer si los segundos pedidos no caben en el
presupuesto de RAM detectado en tu máquina a esa calidad. Eso sí, desde la v0.6.0 nunca eleva el
buffer por encima de la resolución de salida de OBS (Ajustes → Vídeo → Resolución de salida
(escalada)): con una salida de 720p el buffer es 720p aunque elijas 1080p, porque OBS no tiene nada
por encima de su salida que emitir. También es el suelo de la reducción automática de calidad cuando
tu equipo no llega (ver más abajo). El dock muestra una estimación en vivo, no bloqueante, de lo que
una combinación concreta va a lograr realmente (segundos y resolución) mientras ajustas cualquiera
de los dos, antes incluso de pulsar Enable. Ver `docs/SPEC.md` §3.5.

**¿Qué pasa si el delay + calidad que pido no caben en RAM?**
Recibes un aviso claro en el dock *antes* de activarlo — nunca falla en silencio ni crashea. El
plugin simplemente guarda menos segundos de los pedidos a la calidad elegida en vez de bajar nunca
de esa calidad; bajar los segundos o la calidad mínima te devuelve al tiempo completo pedido.

**¿Por qué no puedo cambiar el delay mientras está activo?**
Desde la v0.6.0 las escenas, los segundos de delay y la calidad mínima se bloquean mientras el delay
se prepara o está activo, con una línea que dice "Desactiva el delay para cambiarlo." Antes, los
segundos seguían editables y cada tecla redimensionaba el buffer en pleno directo. Desactiva el
delay, cámbialo y vuelve a activarlo; se llena de nuevo con los segundos nuevos.

**¿Cuánto delay máximo soporta?**
El spinner del dock acepta 1–60 segundos como rango de sensatez de la interfaz. El límite real es
lo que quepa en el presupuesto de RAM detectado en tu máquina (aproximadamente la mitad de la RAM
total del sistema, con un suelo de 1GB y un techo de 24GB) a la calidad elegida — si 60s a tu
calidad elegida no caben, la estimación en vivo del dock te lo dirá antes de activarlo, y el plugin
guardará tantos segundos como quepan de verdad en su lugar.

**¿Comprime el buffer? ¿Eso baja la calidad?**
Sí, desde la v0.3.0 — en Windows y Linux, cada frame almacenado se comprime con MJPEG en RAM y se
descomprime de vuelta al reproducirlo, con un fallback automático y seguro a almacenamiento sin
comprimir si el códec no está disponible por cualquier motivo. La compresión es visualmente casi sin
pérdidas, y la calidad mínima que elijas (480p/720p/1080p) funciona exactamente igual que antes:
esto solo cambia cómo se almacenan en memoria los frames del buffer. El gameplay real comprime unas
11–15x (medido en directo desde la v0.4.0), aunque depende de lo que haya en pantalla. Desde la
v0.6.0 la compresión se ejecuta en hilos propios del plugin, no en el hilo gráfico de OBS, y la
memoria sigue lo que de verdad pesan los frames: unos 0,6–0,8 GB para 30 s a 1440p60 en nuestra
simulación fuera de OBS. Ver `docs/SPEC-es.md` §4. macOS todavía no forma parte de esto y guarda el
buffer sin comprimir.

**OBS muestra "Encoding overloaded" (sobrecarga al codificar) en cuanto activo el delay. ¿Por qué?**
Hasta la v0.5.0, la compresión y descompresión de los frames del buffer se ejecutaba dentro del hilo
gráfico de OBS, que tiene 16,7 ms por frame a 60 fps, y el buffer se guardaba a la resolución del
lienzo: con un lienzo mayor que la salida (por ejemplo lienzo 2560x1440, salida 1920x1080) ese
trabajo se comía la mayor parte del frame y OBS se quedaba atrás. **La v0.6.0 lo corrige**: el buffer
nunca se guarda por encima de la resolución de salida de OBS, y la compresión se ejecuta en hilos
propios del plugin, así que lo que queda en el hilo gráfico de OBS es uno o dos milisegundos por
frame. Actualiza a la v0.6.0; si bajaste el lienzo para esquivarlo, puedes volver a subirlo. Si aun
así sigue pasando, mira la sección "Encoding overloaded" de `docs/TROUBLESHOOTING-es.md`.

**¿Solo funciona a 1080p? ¿Y a 2K (1440p)?**
Funciona a cualquier resolución — el buffer sigue la resolución de salida de OBS. Para el
rendimiento lo que cuenta es la resolución y los fps de *salida*, no el lienzo:
- **Lienzo mayor que la salida** (p. ej. lienzo 1440p, salida 1080p): bien desde la v0.6.0, mismo
  coste que una salida 1080p.
- **Salida 1440p a 30 o 60 fps:** soportada desde la v0.6.0. En nuestro benchmark fuera de OBS a
  1440p60 (una CPU de 8 núcleos/16 hilos) el trabajo del plugin en el hilo gráfico de OBS es de
  ~1,2–1,7/1,6–2,1 ms p50/p95 por frame, y la compresión y la descompresión ocupan en total menos de
  un núcleo de CPU en sus propios hilos.
- **Si tu equipo no llega** con tus ajustes, el plugin baja la resolución del buffer (nunca por
  debajo de tu calidad mínima) y, como último paso, sus fps de captura, en vez de dejar que OBS se
  quede atrás o de cortar el delay. El dock te lo dice con un aviso ámbar (siguiente pregunta).

**¿Qué significa el aviso ámbar "Calidad del buffer reducida" del dock?**
Tu equipo no daba para el buffer del delay a calidad completa, así que el plugin lo aligeró: primero
añade hilos de compresión (eso no muestra aviso), después guarda el buffer a menos resolución, nunca
por debajo de tu calidad mínima, y como último paso captura un frame de cada dos (solo con una salida
a 48 fps o más). El aviso dice qué se redujo y por qué. Los segundos de delay no cambian nunca y el
directo no se corta; los frames repetidos, si los hay, se ven como un tirón breve. Cuando baja la
carga, vuelve a subir solo tras 30 s con margen de sobra, y el aviso desaparece. Qué hacer, si hace
falta algo: `docs/TROUBLESHOOTING-es.md`.

**¿Afecta a la grabación local o al Replay Buffer?**
Indirectamente sí, y merece la pena entenderlo bien: el modo buffer funciona cambiando lo que
muestra el Programa de OBS, y tanto la grabación local como el Replay Buffer capturan ese mismo
output del Programa — así que si estás grabando (o tienes el Replay Buffer activo) mientras el
delay está `Active`, lo que se guarda es también el contenido delayed, igual que lo que ven los
espectadores. Ahora mismo no hay forma de retrasar el stream sin retrasar también la
grabación/Replay Buffer, ni al revés — esa separación por output queda explícitamente fuera de
alcance por ahora (ver `docs/ROADMAP.md`).

**La escena y los filtros del plugin tienen nombres nuevos. ¿Tengo que hacer algo?**
Desde la v0.6.0 terminan en "(auto)" en vez de "(no tocar)": **Trigglow Delay Buffer (auto)**,
**Trigglow Buffer Mode Delay (auto)**, `Trigglow Audio Delay (auto) - <fuente>` y **Trigglow Delay
Badge (auto)**. Tus colecciones de escenas se renombran solas. Solo tienes que actualizar las
acciones de Stream Deck, obs-websocket u otro control remoto que usen esos objetos **por su
nombre**; los atajos del plugin no se ven afectados. Si después vuelves a la v0.5.0 o anterior, esa
versión crea su propio juego "(no tocar)" junto al "(auto)": borra el que no uses.

**¿Qué provoca el estado `Error`?**
Cualquier cosa que de otro modo dejaría al plugin en una condición rota pero silenciosa: no haber
elegido ninguna escena en directo, o que la escena en directo elegida ya no resuelva a nada (por
ejemplo, si se borró). El dock muestra un mensaje claro explicando qué falla en vez de colgarse o
crashear.

**¿Funciona con Twitch/YouTube/Kick/cualquier plataforma?**
Sí — el modo buffer trabaja enteramente sobre el propio output de Programa de OBS, nunca sobre la
conexión de streaming a una plataforma concreta, así que el comportamiento es idéntico
independientemente de a dónde emitas.

**¿Necesito un panel web o una cuenta de Trigglow para usarlo?**
Panel web no: todo el control vive dentro de OBS. Desde la v0.3.3 sí necesitas iniciar sesión una
vez con una cuenta gratuita de trigglow.com para que Enable funcione (el inicio de sesión se hace en
tu navegador, y el plugin sigue siendo 100% gratis) — ver `docs/ACCOUNT_GATE.md`. **En la v0.6.0, el
inicio de sesión (y con él Activar delay y Trigi) solo funciona en Windows.** macOS y Linux todavía
no tienen el cliente HTTP del plugin; está previsto para la v0.6.1.

**¿Cómo funciona el inicio de sesión y qué guarda el plugin?**
El inicio de sesión siempre se hace en trigglow.com, en tu navegador: el plugin nunca te pide la
contraseña. Pulsa **Iniciar sesión** en el dock; muestra un código ("La página debe mostrar el
código KD7F-29QM."), y la página de trigglow.com muestra el mismo código junto con la app, su
versión y cuándo se pidió. Confirma solo si los códigos coinciden y lo has iniciado tú; **No he sido
yo** lo cancela. A partir de ahí el plugin guarda su propio inicio de sesión, que solo sirve para lo
que hace el plugin (comprobar tu cuenta, Trigi y tus propios tickets de Dynamic Delay), cifrado con
la protección de datos de Windows para tu usuario de Windows. Tu email no se guarda. **Cerrar
sesión** en el dock también lo desconecta en trigglow.com, y también puedes desconectarlo desde la
web: Ajustes → Sesiones activas → Apps conectadas.

**Tengo la v0.5.x o anterior. ¿Tengo que actualizar?**
Sí. El inicio de sesión de la v0.5.x y anteriores se retira con la v0.6.0: esas versiones ya no
pueden iniciar sesión en Trigglow, y las sesiones que ya tienen dejan de funcionar tras un periodo
de transición, así que el delay deja de funcionar en ellas. Instala la v0.6.0 encima de tu versión,
con OBS cerrado. En el primer arranque tu inicio de sesión pasa solo al formato nuevo; si el dock te
pide iniciar sesión, hazlo una vez. Tus escenas y ajustes se conservan.

**El dock o "Reportar un problema" me dice que actualice el plugin. ¿Qué hago?**
"Esta versión del plugin ya no puede iniciar sesión. Actualiza el plugin." en el dock, "Actualiza el
plugin para usar Trigi" en "Reportar un problema" o "Actualización necesaria" en la página de inicio
de sesión de trigglow.com significan lo mismo: tu versión es demasiado antigua para lo que intentas
hacer. Descarga la última versión (**Descargar la actualización** en el diálogo, el aviso de
actualización del dock o la
[última release](https://github.com/VirosMs/obs-trigglow-dynamic-delay/releases/latest)) e
instálala con OBS cerrado; tus ajustes y tu inicio de sesión se conservan. Mientras tanto puedes
seguir enviando un reporte desde el formulario del diálogo o desde
[trigglow.com/support](https://www.trigglow.com/support).

**¿Qué es Trigi?**
El asistente de soporte con IA de Trigglow, el mismo que en trigglow.com, ahora dentro del plugin:
abre **Reportar un problema** en el dock y pregúntale por Dynamic Delay (cómo configurarlo, qué dice
el dock, rendimiento, inicio de sesión). Responde en el propio diálogo. Es una IA y puede
equivocarse, así que revisa lo que te diga antes de cambiar tu configuración. Si no lo resuelve,
**Crear ticket con esta conversación** se la envía al equipo con el contexto, y siempre puedes
saltarte a Trigi y escribir directamente al equipo. Trigi está disponible en Windows con la sesión
iniciada; si no, el diálogo se abre en el formulario de ticket. La primera vez, un aviso breve
("Antes de empezar con Trigi") explica todo esto, y no se envía nada hasta que lo aceptas y mandas tu
primera pregunta.

**¿Qué ve Trigi y qué no?**
Ve lo que escribes y, solo si marcas **Incluir datos técnicos de OBS**, un pequeño conjunto de
cifras y estados que se envían una vez al empezar la conversación: versiones de OBS y de Windows,
renderizador, fabricante de la GPU, RAM total, familia del codificador, el estado, los segundos y la
calidad mínima del delay, tamaño del lienzo y de la salida, fps, los frames retrasados y saltados de
OBS, la salud del buffer y los códigos de problemas recientes del plugin. **Ver qué se envía**
muestra los datos exactos antes de enviarlos. La casilla está desmarcada hasta que la marques. Trigi
nunca recibe tu log de OBS (ni completo, ni en partes, ni resumido), y los datos técnicos nunca
incluyen nombres de escenas, fuentes ni filtros, rutas, URLs, claves ni datos de tu emisión, tu
nombre de usuario o tu email, el nombre de tu GPU, IDs de hardware ni IPs. Trigglow y un proveedor
de IA indicado en la [Política de Privacidad](https://www.trigglow.com/legal/privacy) procesan tus
mensajes y los datos técnicos. No escribas contraseñas, claves de stream, URLs de overlay ni datos
de pago. El log de OBS solo va con un ticket si marcas su casilla, cosa que puedes hacer después de
verlo en **Ver qué se envía**. Las rutas con tu usuario de Windows, URLs, claves, emails e IPs van
ocultas; aun así, pueden seguir apareciendo nombres de escenas, fuentes, dispositivos de audio y
ventanas, así que revísalo ahí antes.

**¿Cuánto tiempo se guardan mis conversaciones con Trigi y cómo las borro?**
Las conversaciones con Trigi y los datos técnicos que elijas enviar se guardan hasta 12 meses y
después se borran. El historial que ves en OBS solo está en memoria y desaparece al cerrar el
diálogo (**Borrar conversación** solo limpia esa vista). Para borrarlas antes, en trigglow.com ve a
Ajustes → Privacidad y datos → Conversaciones con Trigi → **Eliminar mis conversaciones con Trigi**,
o pídeselo a soporte. Una conversación que convertiste en ticket se queda en el hilo de ese ticket,
como el resto del ticket.

**¿Qué conexiones de red hace el plugin?**
En cada arranque de OBS, una comprobación de tu inicio de sesión con api.trigglow.com (más una
renovación cuando toca) y una comprobación de la última release de este repositorio en
api.github.com. Todo lo demás solo ocurre cuando lo usas: iniciar sesión, cerrar sesión, preguntar a
Trigi y enviar un reporte. Todo por HTTPS, el inicio de sesión solo se envía a api.trigglow.com, y
el plugin solo abre sus propias páginas fijas de www.trigglow.com y GitHub.

**¿Necesito el plugin de Stream Deck de Trigglow?**
No — el plugin usa hotkeys nativas de OBS, que Stream Deck puede disparar directamente con su propia
acción "System: Hotkey". Ver `docs/STREAM_DECK.md`.

**¿Es un producto terminado?**
No — está marcado explícitamente como **MVP / Early Access**. Es funcional y pensado para un uso
real en directo, pero con un alcance deliberadamente acotado (ver `docs/ROADMAP.md` para lo que
viene después), y una zona de riesgo — un crash de GPU observado bajo un diseño anterior — que se
cree reducida pero todavía no reconfirmada específicamente (`docs/SPEC.md` §7).
