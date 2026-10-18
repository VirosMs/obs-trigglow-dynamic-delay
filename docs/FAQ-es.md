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
en vez de quedarse por delante. Ver `docs/SPEC.md` §3.2/§3.3.

**¿Cómo interactúan los ajustes "Delay (segundos)" y "Calidad mínima"?**
De forma independiente, por diseño — eliges ambos libremente. La calidad mínima (480p/720p/1080p)
marca hasta dónde puede bajar el plugin la resolución del buffer para ahorrar RAM: nunca baja de
ahí, y en su lugar acorta el tiempo real de buffer si los segundos pedidos no caben en el
presupuesto de RAM detectado en tu máquina a esa calidad. Eso sí, desde la v0.5.1 nunca eleva el
buffer por encima de la resolución de salida de OBS (Ajustes → Vídeo → Resolución de salida
(escalada)): con una salida de 720p el buffer es 720p aunque elijas 1080p, porque OBS no tiene nada
por encima de su salida que emitir. El dock muestra una estimación en vivo, no bloqueante, de lo que
una combinación concreta va a lograr realmente (segundos y resolución) mientras ajustas cualquiera
de los dos, antes incluso de pulsar Enable. Ver `docs/SPEC.md` §3.5.

**¿Qué pasa si el delay + calidad que pido no caben en RAM?**
Recibes un aviso claro en el dock *antes* de activarlo — nunca falla en silencio ni crashea. El
plugin simplemente guarda menos segundos de los pedidos a la calidad elegida en vez de bajar nunca
de esa calidad; bajar los segundos o la calidad mínima te devuelve al tiempo completo pedido.

**¿Cuánto delay máximo soporta?**
El spinner del dock acepta 1–60 segundos como rango de sensatez de la interfaz. El límite real es
lo que quepa en el presupuesto de RAM detectado en tu máquina (aproximadamente la mitad de la RAM
total del sistema, con un suelo de 1GB y un techo de 24GB) a la calidad elegida — si 60s a tu
calidad elegida no caben, la estimación en vivo del dock te lo dirá antes de activarlo, y el plugin
guardará tantos segundos como quepan de verdad en su lugar.

**¿Ahora comprime el buffer? ¿Eso baja la calidad?**
A partir de v0.3.0, sí — en Windows y Linux, cada frame almacenado ahora se comprime con MJPEG en
RAM y se descomprime de vuelta al reproducirlo, con un fallback automático y seguro a almacenamiento
sin comprimir si el codec no está disponible por cualquier motivo. Sin pérdida de calidad: la
calidad mínima que elijas (480p/720p/1080p) funciona exactamente igual que antes, esto solo cambia
cómo se almacenan en memoria los frames del buffer. En una sesión de prueba real de 30s@1080p60, esto
redujo el uso de RAM del buffer aproximadamente a la mitad (~6.3GB → ~2.9GB). La reducción exacta
depende mucho de lo que haya en pantalla y todavía no se ha medido contra gameplay real sostenido —
ver `docs/SPEC.md` §4. macOS todavía no forma parte de esto y sigue funcionando exactamente igual
que antes, sin comprimir.

**OBS muestra "Encoding overloaded" (sobrecarga al codificar) en cuanto activo el delay. ¿Por qué?**
La compresión y descompresión de los frames del buffer se ejecuta dentro del hilo gráfico de OBS,
que tiene 16,7 ms por frame a 60 fps. Hasta la v0.5.0 el buffer se guardaba a la resolución del
lienzo, así que con un lienzo mayor que la salida (por ejemplo lienzo 2560x1440, salida 1920x1080)
ese trabajo se comía la mayor parte del frame y OBS se quedaba atrás. **La v0.5.1 corrige ese
caso**: el buffer nunca se guarda por encima de la resolución de salida de OBS y la compresión usa
varios hilos, así que un lienzo 1440p con salida 1080p cuesta más o menos lo mismo que un montaje
1080p normal. Actualiza a la v0.5.1; si bajaste el lienzo para esquivarlo, puedes volver a subirlo.
Si aun así sigue pasando, mira la sección "Encoding overloaded" de `docs/TROUBLESHOOTING-es.md`.

**¿Solo funciona a 1080p? ¿Y a 2K (1440p)?**
Funciona a cualquier resolución — el buffer sigue la resolución de salida de OBS. Para el
rendimiento lo que cuenta es la resolución y los fps de *salida*, no el lienzo:
- **Lienzo mayor que la salida** (p. ej. lienzo 1440p, salida 1080p): bien desde la v0.5.1, mismo
  coste que una salida 1080p.
- **Salida 1440p a 30 fps:** debería funcionar.
- **Salida 1440p a 60 fps:** todavía puede sobrecargar OBS en muchos equipos, porque la compresión
  sigue ejecutándose dentro del hilo gráfico de OBS. Sacarla de ese hilo, con 1440p60 como meta,
  es lo principal de la siguiente versión (v0.6.0, ver `docs/ROADMAP-es.md`); todavía no tiene
  fecha. Mientras tanto, usa salida 1080p para 60 fps, o 30 fps con salida 1440p.

**¿Afecta a la grabación local o al Replay Buffer?**
Indirectamente sí, y merece la pena entenderlo bien: el modo buffer funciona cambiando lo que
muestra el Programa de OBS, y tanto la grabación local como el Replay Buffer capturan ese mismo
output del Programa — así que si estás grabando (o tienes el Replay Buffer activo) mientras el
delay está `Active`, lo que se guarda es también el contenido delayed, igual que lo que ven los
espectadores. Ahora mismo no hay forma de retrasar el stream sin retrasar también la
grabación/Replay Buffer, ni al revés — esa separación por output queda explícitamente fuera de
alcance por ahora (ver `docs/ROADMAP.md`).

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
tu navegador, y el plugin sigue siendo 100% gratis) — ver `docs/ACCOUNT_GATE.md`.

**¿Necesito el plugin de Stream Deck de Trigglow?**
No — el plugin usa hotkeys nativas de OBS, que Stream Deck puede disparar directamente con su propia
acción "System: Hotkey". Ver `docs/STREAM_DECK.md`.

**¿Es un producto terminado?**
No — está marcado explícitamente como **MVP / Early Access**. Es funcional y pensado para un uso
real en directo, pero con un alcance deliberadamente acotado (ver `docs/ROADMAP.md` para lo que
viene después), y una zona de riesgo — un crash de GPU observado bajo un diseño anterior — que se
cree reducida pero todavía no reconfirmada específicamente (`docs/SPEC.md` §7).
