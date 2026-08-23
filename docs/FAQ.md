# FAQ — Trigglow Dynamic Delay

**¿Por qué se corta un momento el stream cuando cambio el delay en directo?**
Porque OBS bloquea el valor de delay en el instante en que arranca el stream, y no existe ninguna
API pública (ni siquiera interna) para recalcular el buffer de un output ya activo. La única forma
real de que el nuevo valor se aplique mientras ya estás en directo es reconectar el output. Es una
limitación de OBS en sí mismo, no de este plugin — detalle técnico completo, con el código fuente de
OBS citado, en `docs/SPEC.md` §2.

**¿Puedo evitar esa reconexión?**
No en v0.1.0. Si quieres cero cortes, actívalo/desactívalo/config úralo **antes** de salir en
directo — en ese caso el cambio es instantáneo y sin ningún efecto secundario.

**¿Funciona con Twitch/YouTube/Kick/cualquier plataforma?**
Sí — el plugin trabaja sobre el output de streaming de OBS, no sobre una plataforma concreta. El
comportamiento (incluida la reconexión) es el mismo independientemente de a dónde emitas.

**¿Necesito un panel web o una cuenta de Trigglow para usarlo?**
No. Todo el control vive dentro de OBS. La web de Trigglow es solo para descargarlo y consultar la
documentación.

**¿Necesito el plugin de Stream Deck de Trigglow?**
No — v0.1.0 usa la hotkey nativa de OBS, que Stream Deck puede disparar directamente con su propia
acción "Hotkey". Ver `docs/STREAM_DECK.md`.

**¿Afecta a la grabación local o al Replay Buffer?**
No, v0.1.0 solo afecta al output de streaming.

**¿Cuánto delay máximo soporta?**
El dock acepta hasta 1800 segundos (30 minutos) como límite de sensatez del MVP. OBS en sí no
impone ese límite exacto, pero delays muy largos consumen memoria en proporción al framerate y a los
segundos configurados.

**¿Qué pasa si no tengo el servicio de streaming configurado en OBS?**
El plugin lo detecta y pasa a estado `Error` con un mensaje explicando que no hay ningún output de
streaming disponible — no se cuelga ni crashea.

**¿Es un producto terminado?**
No — está marcado explícitamente como **MVP / Early Access**. Es funcional y pensado para un uso
real en directo, pero con un alcance deliberadamente acotado. Ver `docs/ROADMAP.md` para lo que
viene después.
