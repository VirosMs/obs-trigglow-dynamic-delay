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
ambos funcionan con el mismo valor de `delaySeconds`. Ver `docs/SPEC.md` §3.2.

**¿Cómo interactúan los ajustes "Delay (segundos)" y "Calidad mínima"?**
De forma independiente, por diseño — eliges ambos libremente, y el plugin nunca baja en silencio tu
calidad mínima elegida (480p/720p/1080p). Lo que sí puede hacer es acortar el tiempo real de buffer
si los segundos pedidos no caben en el presupuesto de RAM detectado en tu máquina a esa calidad. El
dock muestra una estimación en vivo, no bloqueante, de lo que una combinación concreta va a lograr
realmente mientras ajustas cualquiera de los dos, antes incluso de pulsar Enable. Ver `docs/SPEC.md`
§3.5.

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
No. Todo el control vive dentro de OBS. La web de Trigglow es solo para descargarlo y consultar la
documentación.

**¿Necesito el plugin de Stream Deck de Trigglow?**
No — v0.1.0 usa hotkeys nativas de OBS, que Stream Deck puede disparar directamente con su propia
acción "System: Hotkey". Ver `docs/STREAM_DECK.md`.

**¿Es un producto terminado?**
No — está marcado explícitamente como **MVP / Early Access**. Es funcional y pensado para un uso
real en directo, pero con un alcance deliberadamente acotado (ver `docs/ROADMAP.md` para lo que
viene después), y una zona de riesgo — un crash de GPU observado bajo un diseño anterior — que se
cree reducida pero todavía no reconfirmada específicamente (`docs/SPEC.md` §6).
