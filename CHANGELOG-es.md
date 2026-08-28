*[English version](CHANGELOG.md)*

# Changelog — Trigglow Dynamic Delay for OBS

## v0.3.2 — 2026-08-27 (Early Access)

**Arreglado:**
- El audio podía notarse claramente por detrás del vídeo en delays largos
  y/o de alta calidad (30s+, 1080p) — reportado en directo como "el audio
  sale después de la acción del vídeo". Causa raíz: cuando el presupuesto
  de RAM no da para el delay completo pedido a la calidad elegida,
  `VideoDelayFilter` acorta en silencio su propia duración real de buffer
  (así ha sido siempre, desde antes incluso de que existiera la
  compresión — es por diseño, la calidad siempre gana a la duración), pero
  `AudioDelayFilter` siempre guarda exactamente los segundos completos
  pedidos (PCM barato, sin presupuesto de RAM aplicado) — así que cada vez
  que el vídeo tenía que acortarse, el audio seguía retrasando más tiempo
  del que el vídeo realmente aplicaba, y la brecha crecía cuanto más largo
  o de más calidad era el pedido. Arreglado haciendo que
  `BufferModeController` le pregunte al filtro de vídeo cuánto puede
  realmente guardar (una nueva consulta `get_effective_delay_seconds`) en
  cuanto el buffer se llena de verdad, y recortando el delay del audio para
  que coincida siempre que el vídeo haya tenido que acortarse. Efecto
  secundario puntual: si esta corrección se dispara, el ring de audio se
  reinicia y queda en silencio brevemente (bastante menos de un segundo)
  justo cuando el buffer pasa a `Active`, mientras se vuelve a llenar hasta
  la duración corregida — un buen cambio a cambio de no quedar
  desincronizado el resto de la sesión.

## v0.3.1 — 2026-08-27 (Early Access)

**Arreglado:**
- Los selectores de escena en directo/de carga del dock aparecían vacíos
  cada vez que se abría OBS por primera vez (o se reinstalaba el plugin),
  aunque los nombres guardados se cargaban correctamente por detrás —
  dando la sensación de que el plugin "olvidaba" la configuración y
  obligando a reelegirla cada vez. Causa raíz: `obs_module_load()` (y por
  tanto la primera vez que el dock rellena esos desplegables) corre antes
  de que OBS termine de cargar la colección de escenas, así que el primer
  intento de listar escenas siempre volvía vacío. El dock ahora también
  vuelve a rellenar ambos desplegables en cuanto OBS termina de cargar,
  reseleccionando lo que estuviera guardado.

**Cambiado:**
- El instalador de Windows ahora usa branding real de Trigglow — icono
  propio del `.exe`/barra de tareas y banner/logo del asistente, generados
  a partir de la misma marca usada en trigglow.virosms.com — en vez de los
  gráficos genéricos por defecto de Inno Setup.

## v0.3.0 — 2026-08-27 (Early Access)

Compresión real del buffer. El ring buffer ahora codifica cada frame con MJPEG (todo-intra, acorde
a los resets abruptos y las lecturas por índice del ring) antes de guardarlo, y lo decodifica de
vuelta al reproducirlo — reduciendo el uso real de RAM por encima del ya logrado con NV12 en
v0.2.0, sin bajar nunca la calidad mínima que elijas.

**Incluido:**
- FFmpeg (avcodec/avutil) vendorizado y enlazado en Windows + Linux; cada slot del ring se
  codifica en MJPEG al capturarlo y se decodifica al reproducirlo.
- Fallback automático y seguro a NV12 sin comprimir (idéntico a v0.2.0) siempre que el codec no
  pueda abrirse o un frame concreto falle al codificar/decodificar — la compresión nunca es un
  requisito imprescindible para que el buffer funcione.
- Los slots del ring ahora reservan RAM según un tamaño presupuestado (comprimido) en vez del
  techo crudo completo, creciendo un slot concreto solo cuando un frame realmente necesita más
  sitio. Medido en vivo a 30s@1080p: **~2,9GB en total con el buffer activo, frente a los ~6,3GB**
  de antes de esta versión — el contenido real de gameplay comprime algo peor que el ratio
  asumido, así que esto varía según lo que se vea en pantalla.
- macOS todavía no forma parte de esta versión — no hay ningún build estático de FFmpeg de
  confianza equivalente disponible ahí hoy (ver `docs/ROADMAP.md`); el plugin sigue funcionando
  exactamente igual que en v0.2.0 en macOS, sin comprimir.

**Limitaciones conocidas (no son bugs):**
- El ratio de compresión real todavía no se ha medido contra gameplay real prolongado — el único
  número registrado hasta ahora (muy alto) vino de una escena de carga mayormente estática y no es
  representativo. El uso real de RAM en la práctica puede ser mayor o menor que los ~2,9GB medidos
  aquí según el contenido.
- Un slot que en algún momento necesitó más sitio del presupuestado mantiene esa capacidad mayor
  durante el resto de la sesión (por diseño, para evitar una reasignación en cada frame) — el RAM
  solo puede crecer durante una sesión, nunca bajar, hasta que Disable() libera el buffer entero.

**Cambios en el repo:** `main` ahora es una rama protegida (pull request obligatorio, incluso para
el mantenedor) — el desarrollo ocurre en `develop`, integrado vía PR. Ver `README.md` si vas a
contribuir.

## v0.2.0 — 2026-08-26 (MVP / Early Access)

Primera versión que refleja el diseño realmente enviado. Plugin nativo de OBS Studio (C++,
plantilla oficial `obsproject/obs-plugintemplate`) que retrasa el vídeo y el audio de tu stream
juntos, sin tocar nunca el output de streaming — sin reconexión, sin corte, en ningún momento, por
ningún motivo.

Antes se publicó un `0.1.0` (2026-08-23) que describía un delay **basado en reconexión**,
totalmente abandonado durante el desarrollo a favor del modo buffer sin reconexión de más abajo —
ver `docs/SPEC.md` §0/§2 para el por qué. Los assets de esa release `0.1.0` son de antes de esta
reescritura por completo; `0.2.0` es la primera release que refleja de verdad lo que se distribuye.

**Incluido:**
- **Modo buffer** sin reconexión: dos filtros de OBS acumulan vídeo y audio en un anillo de RAM del
  sistema, y el plugin cambia el Programa de OBS entre la escena en directo, una escena de carga
  opcional y una escena wrapper delayed, internamente. El delay nativo del output de streaming
  nunca se toca.
- Enable / Disable / Toggle desde un dock nativo de OBS, con estado visible y cuenta atrás en vivo
  mientras se llena el buffer: `Inactive` / `Filling (Ns restantes)` / `Active` / `Error`.
- Vídeo y audio delayed juntos, en sincronía, usando los mismos segundos configurados.
- Calidad mínima seleccionable (480p/720p/1080p) para el vídeo delayed. Si el delay y la calidad
  pedidos no caben juntos en el presupuesto de RAM detectado en la máquina, el plugin acorta el
  tiempo real de buffer en vez de bajar nunca de la calidad elegida.
- Estimación de ajuste de buffer en vivo, no bloqueante, y presupuesto de RAM detectado, mostrados
  en el dock y actualizados según el usuario ajusta segundos o calidad — antes incluso de pulsar
  Enable.
- Buffer de RAM liberado automáticamente al pulsar Disable.
- 3 hotkeys nativas de OBS (Toggle, Enable, Disable) — utilizables directamente desde Stream Deck
  vía su propia acción "System: Hotkey", sin necesitar un plugin de Stream Deck propio.
- Persistencia de settings por perfil de OBS.
- Manejo de errores sin crashear (sin escena en directo elegida, escena en directo que no resuelve
  a nada, etc. → estado `Error` con un mensaje claro en el dock, nunca un crash).

**Limitaciones conocidas (no son bugs):**
- El ring buffer almacena frames NV12 sin comprimir — una compresión real de vídeo/audio (un
  pipeline de encode/decode basado en FFmpeg) se investigó y se aplazó explícitamente como trabajo
  futuro; ver `docs/SPEC.md` §5 (la compresión de vídeo se entregó en v0.3.0 más abajo — ver
  `docs/SPEC.md` §4 — la de audio sigue aplazada).
- Un diseño anterior con una textura de GPU persistente por frame almacenado produjo un crash real
  de "Device Remove/Reset" al pulsar Disable durante pruebas en directo. La reescritura actual a
  RAM/NV12 usa un pool pequeño y fijo de objetos GPU en su lugar y probablemente reduce ese riesgo,
  pero todavía no se ha vuelto a probar específicamente para confirmar que no reaparece — ver
  `docs/SPEC.md` §7.

**Fuera de alcance en v0.2.0:** compresión real del buffer, presets de delay guardados, elegir una
fuente individual en vez de una escena completa, un plugin de Stream Deck propio de Trigglow,
instaladores firmados, verificación en directo de macOS/Linux (ambos compilan en verde en CI pero
no se han probado en directo). Ver `docs/ROADMAP.md`.
