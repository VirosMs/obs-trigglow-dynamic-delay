*[English version](README.md)*

# Trigglow Dynamic Delay for OBS

Plugin nativo de OBS Studio que retrasa el **vídeo y el audio de tu stream juntos**, un número
configurable de segundos, desde un botón, una hotkey nativa de OBS o un Stream Deck — **sin que el
output de streaming se toque nunca.** Sin reconexión, sin corte, en ningún momento, por ningún
motivo. Sin app externa, sin panel web, sin proceso aparte: todo vive dentro del propio proceso de
OBS. **Estado: MVP / v0.7.0 — Early Access.**

A partir de v0.3.0, el ring buffer de RAM se comprime de verdad con MJPEG en Windows y Linux
(FFmpeg vendorizado), con un fallback automático y seguro a almacenamiento sin comprimir si el codec
no está disponible — esto nunca baja la calidad mínima elegida. Los slots del ring también reservan
ahora RAM a un tamaño presupuestado en vez del techo bruto completo, creciendo solo cuando un frame
concreto necesita más espacio. Medido en una sesión real de 30s@1080p60: el uso de RAM del buffer
pasó de ~6.3GB a ~2.9GB. El ratio de compresión exacto depende mucho del contenido y todavía no se
ha medido contra gameplay real (macOS todavía no forma parte de esto — sigue funcionando exactamente
igual que antes, sin comprimir). Ver `docs/SPEC.md` para el detalle técnico completo.

La v0.3.1 arregla que los selectores de escena del dock aparecieran en blanco al abrir OBS por
primera vez o reinstalar el plugin (los ajustes guardados estaban bien, la interfaz simplemente
todavía no los restauraba — ver `docs/SPEC.md` §3.3), y le da al instalador de Windows branding real
de Trigglow en vez de los gráficos genéricos de Inno Setup.

La v0.3.2 arregla que el audio se notara claramente por detrás del vídeo en delays largos o de alta
calidad (30s+, 1080p): el vídeo acorta en silencio su propia duración real de buffer cuando el
presupuesto de RAM no da para el pedido completo, pero el audio seguía guardando siempre los
segundos completos — ambos se desincronizaban exactamente por esa diferencia. El audio ahora se
ajusta a lo que el vídeo realmente puede entregar (ver `docs/SPEC.md` §3.3).

La v0.3.3 añade una puerta de cuenta gratuita antes de Enable (ver `docs/ACCOUNT_GATE.md`) — sigue
siendo 100% gratis, solo un inicio de sesión único.

La v0.4.0 añade un botón **"Reportar un problema"** en el dock (crea un ticket de soporte real con
el log actual de OBS adjuntado automáticamente), un **chequeo de actualizaciones dentro del
plugin** contra las releases de GitHub de este repositorio, y **localización real** (el dock, los
diálogos, los nombres de filtro/hotkey y los mensajes de estado ahora siguen el idioma configurado
en OBS en vez de estar fijos en español). También reajusta la suposición de compresión del ring de
RAM usando la primera medición real de ratios de compresión sobre gameplay en vivo (~11-15x, no el
3x asumido de forma conservadora desde v0.3.0) — la memoria total medida del proceso de OBS a
30s/1080p60 bajó de ~2.8GB a ~2.1GB — y rediseña la disposición del dock. Ver `CHANGELOG-es.md`
para todo lo anterior.

La v0.5.0 añade un **overlay "Delay Ns"** opcional (con selector de cuatro esquinas), un solo botón
Activar/Desactivar y un dock responsivo, y corrige que el delay de vídeo se desviara del tiempo real
(ahora sigue el reloj igual que el audio), que se ignorara la escena en directo elegida al reutilizar
la escena envoltorio, y que el instalador de Windows no incluyera el idioma español. Ver
`CHANGELOG-es.md`.

La v0.6.0 corrige que OBS mostrara **"Encoding overloaded" (sobrecarga al codificar)** en cuanto se
activaba el delay con un lienzo mayor que la salida (p. ej. lienzo 2560x1440, salida 1920x1080): el
buffer nunca se guarda por encima de la resolución de salida de OBS, y la compresión y la
descompresión se ejecutan en hilos propios del plugin en vez de en el hilo gráfico de OBS — a
1440p60 el trabajo del plugin ahí baja de ~14/17 ms a ~1,2–1,7/1,6–2,1 ms por frame (p50/p95,
benchmark fuera de OBS), así que **la salida 1440p a 60 fps está soportada**. Si tu equipo no llega,
el plugin baja la calidad del buffer paso a paso (nunca por debajo de tu calidad mínima, nunca
cortando el delay) y lo dice en el dock. Además usa menos RAM (~0,6–0,8 GB en vez de ~1,9 GB para
30 s a 1440p60), mantiene audio y vídeo exactamente con el mismo delay, apaga de verdad los filtros
de delay que quedaban encendidos al cerrar OBS estando Activo, y trae un dock, un diálogo de reporte
y un overlay del directo rediseñados y alineados con trigglow.com. Los objetos que gestiona el plugin
pasan de "(no tocar)" a "(auto)": actualiza cualquier acción de Stream Deck u obs-websocket que use
los nombres antiguos; el overlay del directo va firmado "By Trigglow". La calidad mínima llega
ahora hasta 1440p (2K) y 2160p (4K). (Se preparó un hotfix v0.5.1 que nunca se publicó; forma parte
de la v0.6.0.)
Ver `CHANGELOG-es.md` y `docs/TROUBLESHOOTING-es.md`.

La v0.6.0 también trae a **Trigi**, el asistente de soporte con IA de Trigglow, a "Reportar un
problema" (Windows, con sesión iniciada): pregunta por Dynamic Delay en el diálogo y convierte la
conversación en un ticket si no lo resuelve. El log de OBS nunca se envía a la IA, y los datos
técnicos de OBS son opcionales, con vista previa. **El inicio de sesión es nuevo y más seguro** (un
código que comparar en trigglow.com, un inicio de sesión propio del plugin cifrado para tu usuario
de Windows, que Cerrar sesión revoca), los reportes van a tu cuenta con el log de OBS redactado y
solo si quieres, y las conexiones de red del plugin se endurecen. **Actualizar es obligatorio: en
cuanto se publique la v0.6.0, la v0.5.x y anteriores ya no podrán iniciar sesión, y las sesiones
que ya tienen dejarán de funcionar tras un periodo de transición.** El inicio de sesión, Trigi y los
reportes desde OBS solo funcionan en Windows (también en la v0.7.0); macOS y Linux los tendrán con
el cliente HTTP previsto a continuación (`docs/ROADMAP-es.md`). Ver `docs/FAQ-es.md`.

La v0.7.0 es una versión de optimización: **en Windows el buffer del delay ocupa más o menos la
mitad de RAM.** Los frames que esperan el delay se guardan con el encoder de vídeo por hardware de
tu GPU (HEVC con NVENC, AMF o Quick Sync, o H.264 si la GPU no tiene HEVC) y también se decodifican
en la GPU, en vez de comprimirse uno a uno como imágenes MJPEG. A 1440p60 el buffer pasa de ~675 MB
a ~325 MB con 30 s, y de ~1,2 GB a ~520 MB con 60 s (medido fuera de OBS con una GPU AMD; NVENC de
NVIDIA aún no está medido). La imagen con delay se parece más a la original y el plugin usa menos
CPU. Para no provocar "Encoding overloaded" cuando el buffer comparte el motor de vídeo con el
encoder de OBS, baja su resolución si ese motor se satura y vuelve a MJPEG al momento si OBS empieza
a saltarse frames, sin tocar el delay. Sin encoder de hardware guarda en MJPEG como hasta ahora. Sin
controles ni avisos nuevos; un ajuste oculto, `buffer_codec`, fuerza MJPEG para soporte. Linux y
macOS no cambian. Ver `CHANGELOG-es.md` y la tabla de RAM de `docs/FAQ-es.md`.

Antes de nada, lee `docs/SPEC.md` (especificación técnica completa de cómo funciona realmente el
modo buffer, y por qué el enfoque obvio de "simplemente cambiar el delay nativo de OBS en directo"
se probó y se descartó) y `docs/BUILD_VALIDATION.md` (qué se verificó realmente y qué no, en esta
primera entrega).

## Requisitos de uso

**OBS Studio 31.1.0 o superior.** El binario v0.7.0 se compila contra las fuentes de OBS 31.1.1
(ver `buildspec.json`); se ha verificado con versiones anteriores que el plugin carga y funciona
correctamente en OBS **32.2.2** (el mecanismo de compatibilidad de plugins de OBS rechaza solo los
plugins compilados contra una versión *más nueva* que la que está corriendo; nunca se ha probado en
versiones anteriores a 31.1.1).

**Resolución y fps.** Cualquier lienzo: el buffer del delay sigue la resolución de **salida** de OBS
(`Ajustes → Vídeo → Resolución de salida (escalada)`) y nunca la supera, así que un lienzo mayor que
la salida no le cuesta nada extra al plugin. Las salidas 1080p y **1440p hasta 60 fps** están
soportadas desde la v0.6.0 (cifras en `CHANGELOG-es.md`, medidas fuera de OBS en una CPU de 8
núcleos/16 hilos). En un equipo que no llega, el buffer baja a menos resolución (nunca por debajo de
la calidad mínima que elijas) y, como último paso, a la mitad de fps de captura, en vez de
sobrecargar OBS o cortar el delay; mientras tanto el dock muestra un aviso ámbar.

**Memoria.** El presupuesto de RAM del buffer es la mitad de la RAM del sistema, entre 1 GB y 24 GB
(en macOS y Linux, donde todavía no se detecta la RAM, 2 GB fijos). Lo que ocupa de verdad depende
de lo que haya en pantalla y de cómo se guarde. Medido fuera de OBS con contenido tipo gameplay a
60 fps (solo el buffer; el audio suma unos 11 MB por fuente con sonido para 30 s):

| Salida | 30 s, HEVC por hardware (Windows, v0.7.0) | 30 s, MJPEG | 60 s, HEVC por hardware | 60 s, MJPEG |
|---|---|---|---|---|
| 1080p60 | ~190 MB | ~400 MB | ~290 MB | ~750 MB |
| 1440p60 | ~325 MB | ~640 MB | ~520 MB | ~1200 MB |
| 2160p60 | ~590 MB | ~1250 MB | ~920 MB | ~2300 MB |

Las cifras por hardware son de una GPU AMD (AMF); NVENC de NVIDIA aún no está medido. MJPEG es lo
que usan Linux, Windows sin encoder de hardware y el ajuste de soporte `buffer_codec`. Si los
segundos que pides no caben a tu calidad mínima, se guardan menos segundos (el dock te lo dice antes
de activarlo). La compresión necesita FFmpeg, que va incluido en Windows y Linux; en macOS el buffer
se guarda sin comprimir. Detalles en `docs/FAQ-es.md`.

**Una cuenta gratuita de trigglow.com, en Windows.** `Activar delay` necesita que inicies sesión
una vez desde el dock (en tu navegador; el plugin sigue siendo 100% gratis). Por ahora el inicio de
sesión solo funciona en Windows (v0.6.0 y v0.7.0): macOS y Linux todavía no tienen el cliente HTTP
del plugin, previsto a continuación (ver `docs/ROADMAP-es.md`). Ver `docs/ACCOUNT_GATE.md`.

## Uso de la red y privacidad

- **En cada arranque de OBS**: una comprobación de tu inicio de sesión con `api.trigglow.com` (más
  una renovación cuando toca) y una comprobación de la última release de este repositorio en
  `api.github.com`. Nada más se ejecuta en segundo plano.
- **Solo cuando lo usas**: iniciar y cerrar sesión, preguntar a Trigi y enviar un reporte, todo a
  `api.trigglow.com`. El plugin solo abre sus propias páginas fijas de www.trigglow.com y este
  repositorio en GitHub, en tu navegador.
- **Cómo**: solo HTTPS, sin seguir redirecciones, el inicio de sesión solo se envía a
  `api.trigglow.com`, se guarda cifrado para tu usuario de Windows y nunca se escribe en el log de
  OBS, igual que los códigos de inicio de sesión, los IDs y lo que escribes a Trigi.
- **Trigi** recibe lo que escribes y, solo si lo marcas, un pequeño conjunto de cifras y estados
  técnicos de OBS (nunca nombres, rutas, URLs ni claves), que ves antes de enviarlo. **El log de
  OBS nunca se envía a la IA**; solo se adjunta a un ticket si marcas su casilla, cosa que puedes
  hacer después de verlo en **Ver qué se envía**. Tu usuario de Windows, URLs, claves, emails e IPs
  van ocultos, pero pueden seguir apareciendo nombres de escenas, fuentes, dispositivos de audio y
  ventanas. Las conversaciones se guardan hasta 12 meses y puedes borrarlas antes en trigglow.com.
  Detalles en `docs/FAQ-es.md` y en la
  [Política de Privacidad](https://www.trigglow.com/legal/privacy).

## Requisitos de compilación

Basado en la plantilla oficial [`obsproject/obs-plugintemplate`](https://github.com/obsproject/obs-plugintemplate),
sin modificar su sistema de build (`cmake/`, `CMakePresets.json`, `.github/`) salvo para activar
`ENABLE_FRONTEND_API` y `ENABLE_QT` y registrar los archivos fuente nuevos.

| Plataforma | Herramientas |
|---|---|
| Windows | Visual Studio 17 2022, CMake ≥ 3.30.5 |
| macOS | Xcode 16.0, CMake ≥ 3.30.5 |
| Ubuntu 24.04 | CMake ≥ 3.28.3, `ninja-build`, `pkg-config`, `build-essential` |

**Windows es la plataforma prioritaria para v0.7.0** (así lo pidió el producto); macOS/Linux
compilan en verde en CI con el propio sistema de build de la plantilla, pero no se han probado en
directo en esta entrega.

## Compilar (Windows)

```powershell
git clone <tu-repo>/obs-trigglow-dynamic-delay.git
cd obs-trigglow-dynamic-delay
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

La primera configuración descargará automáticamente las fuentes de OBS y las dependencias
pre-compiladas (`obs-deps`, Qt6) que declara `buildspec.json` — no necesitas instalar OBS ni Qt tú
mismo por separado. Requiere conexión a internet la primera vez.

## Compilar (macOS / Linux)

```bash
cmake --preset macos      # o: ubuntu-x86_64
cmake --build --preset macos --config RelWithDebInfo
```

## Instalar el plugin

**Windows: usa el instalador.** Descarga `obs-trigglow-dynamic-delay-0.7.0-windows-x64-setup.exe`
desde la [última release](https://github.com/VirosMs/obs-trigglow-dynamic-delay/releases/latest) y
ejecútalo — detecta tu instalación de OBS automáticamente y copia los archivos por ti. Todavía no
está firmado (Early Access), así que Windows SmartScreen avisará antes de dejarlo ejecutar. **Guía
completa con capturas de cada paso, incluido el aviso de SmartScreen: `docs/INSTALL_GUIDE-es.md`.**
Instálalo antes de salir en directo, no a mitad de un stream — necesita OBS cerrado para
sobrescribir sus archivos.

**Instalación manual (cualquier plataforma, o si prefieres no ejecutar un instalador):**

1. Localiza el binario generado (`.dll` en Windows dentro de `build_x64/RelWithDebInfo/`, `.plugin`
   en macOS, `.so` en Linux) — o extrae el `.zip` de esa misma release.
2. Cópialo a la carpeta de plugins de OBS:
   - Windows: `C:\Program Files\obs-studio\obs-plugins\64bit\`
   - macOS: `~/Library/Application Support/obs-studio/plugins/`
   - Linux: `~/.config/obs-studio/plugins/`
3. Reinicia OBS.

macOS/Linux todavía no tienen instalador de un clic — la instalación manual es el único camino ahí
por ahora (ver `docs/ROADMAP-es.md`).

## Usar el plugin

1. Abre OBS. Si el dock **"Trigglow Dynamic Delay"** no aparece, ve a `View → Docks` y actívalo.
2. En el dock, elige tu **escena en directo** (obligatoria — la escena con tu contenido real) y,
   opcionalmente, una **escena de carga** para mostrar mientras se llena el buffer en vez de
   quedarte en la escena en directo sin delay. Configura el **Delay (segundos)** (1–60) y la
   **calidad mínima** (480p, 720p, 1080p, 1440p (2K) o 2160p (4K)) por debajo de la cual el vídeo
   delayed no baja para ahorrar RAM (tampoco se guarda nunca por encima de la resolución de salida
   de OBS: con una salida de 720p el buffer es 720p aunque el mínimo sea 1080p). El dock muestra una
   estimación en vivo de si los segundos elegidos caben realmente en la RAM a esa calidad *antes* de
   pulsar Enable — si no caben, avisa de que el tiempo real de buffer se acortará en vez de bajar
   más la calidad, pero nunca bloquea tu elección. Estos campos se bloquean mientras el delay se
   prepara o está activo ("Desactiva el delay para cambiarlo.").
3. Ve a `Ajustes → Atajos de teclado`, busca "Trigglow" y asigna una tecla a **Trigglow:
   Activar/desactivar Dynamic Delay** (y opcionalmente a "Activar" y "Desactivar" por separado; con
   OBS en inglés se llaman "Trigglow: Toggle/Enable/Disable Dynamic Delay").
4. Pulsa Activar delay (desde el botón del dock, la hotkey o Stream Deck). El dock muestra
   **Preparando delay** con una barra de progreso y una cuenta atrás en vivo mientras se llena el
   buffer hasta el delay pedido; en cuanto se llena, pasa automáticamente a **Delay activo** y el
   Programa empieza a mostrar el vídeo y el audio delayed, en sincronía. **El output de streaming en sí no se toca en ningún momento**: los espectadores en
   Twitch/YouTube/Kick/donde sea nunca ven un corte ni una reconexión, ya pulses el botón antes o
   durante un directo.

Pulsar Desactivar delay libera el buffer de RAM al instante y vuelve a la escena que estaba antes de
activarlo.

El plugin gestiona por su cuenta unos pocos objetos de tu colección de escenas: la escena **Trigglow
Delay Buffer (auto)**, los filtros **Trigglow Buffer Mode Delay (auto)** y `Trigglow Audio Delay
(auto) - <fuente>`, y la imagen del overlay **Trigglow Delay Badge (auto)**. El overlay lo puedes
mover o escalar en OBS y se queda donde lo pongas; los demás no los renombres ni los borres.

## Mapear un botón de Stream Deck

No hace falta ningún plugin de Stream Deck propio:

1. En el software de Stream Deck, añade la acción **"System" → "Hotkey"** (el nombre exacto puede
   variar ligeramente según tu versión de Stream Deck).
2. Configúrala para que envíe la combinación de teclas que le asignaste a "Toggle Dynamic Delay" en
   el paso anterior.
3. Pon el foco en la ventana de OBS al pulsar el botón (Stream Deck envía la pulsación de teclado al
   sistema; OBS necesita tener el foco, o al menos estar corriendo y escuchando el atajo global,
   según cómo tengas configurado OBS/tu SO).
4. Pulsarlo estando ya en directo es igual de seguro que pulsarlo antes de salir en directo: OBS
   pasa por **Preparando delay** (con la cuenta atrás en vivo del dock) hasta **Delay activo** en
   cuanto el buffer se llena — no hay reconexión ni corte visible en el stream en ningún momento de
   esa transición.

Guía más detallada, con capturas de ejemplo y solución de problemas: `docs/STREAM_DECK.md`.

## Estructura del proyecto

```
src/
  plugin-main.cpp                 → obs_module_load/unload, wiring de todos los componentes
  buffer-mode-controller.{hpp,cpp} → la máquina de estados (Inactive/Filling/Active/Error) que
                                     orquesta el modo buffer — la única dueña del estado
  obs-frontend-bridge.{hpp,cpp}    → única capa que toca obs-frontend-api.h + el cableado de
                                     filtros/escenas
  video-delay-filter.{hpp,cpp}     → el filtro de delay de vídeo: captura, conversión I420 en la
                                     GPU, lectura, reproducción, su pequeño pool fijo de objetos GPU
                                     y la telemetría perf check / pipeline check
  codec-pipeline.{hpp,cpp}         → hilos de compresión/descompresión, el hilo de preparación del
                                     códec de almacenamiento y el hilo liberador, fuera del hilo
                                     gráfico de OBS, con test de estrés en tests/
  storage-codec.hpp                → la interfaz del códec de almacenamiento (encoder asíncrono,
                                     decoder), el códec sin comprimir, utilidades NV12 y la
                                     contabilidad de cadenas de los códecs inter-frame
  hw-storage-codec.{hpp,cpp}       → v0.7.0, solo Windows: códec de almacenamiento HEVC/H.264 por
                                     hardware (NVENC, AMF, Quick Sync) con decode D3D11VA, y su
                                     sondeo
  storage-codec-select.{hpp,cpp}   → puro: el ajuste oculto buffer_codec, las familias de encoders
                                     de OBS y el orden de los encoders de hardware, con tests
  storage-codec-policy.{hpp,cpp}   → puro: cuándo funciona el códec por hardware y cuándo el buffer
                                     vuelve a MJPEG (protección frente a "Encoding overloaded"),
                                     con tests
  annexb.{hpp,cpp}                 → recorrido Annex B puro: parámetros en banda, con tests
  mjpeg-codec.{hpp,cpp}            → MJPEG sobre el FFmpeg vendorizado, una instancia por hilo
  frame-ring.{hpp,cpp}             → ring de cabeceras de slot con máquina de estados y, por
                                     paquete, códec, key, cadena y seq, con tests
  frame-arena.{hpp,cpp}            → arena por bloques para los frames comprimidos, con tests
  degradation-policy.{hpp,cpp}     → degradación automática pura (hilos, peldaños de resolución,
                                     mitad de fps), con tests
  pipeline-health.hpp              → lo que lee el dock sobre la degradación del buffer
  perf-stats.{hpp,cpp}             → series de percentiles de tamaño fijo para la telemetría
  buffer-fit.{hpp,cpp}             → aritmética pura de dimensionado del buffer (límite de
                                     resolución de salida, suelo de calidad, presupuesto de RAM),
                                     con tests unitarios en tests/
  encoder-check.{hpp,cpp}          → decisiones puras sobre los encoders de OBS: cuáles ven el
                                     lienzo principal, el aviso de "encoder por encima del buffer"
                                     en el log y el techo de los encoders sobre el buffer (v0.7.0),
                                     con tests
  audio-delay-filter.{hpp,cpp}     → ring de delay de audio por fuente hoja, sincronizado con el
                                     vídeo
  hardware-info.{hpp,cpp}          → consulta de RAM total del sistema, usada para dimensionar el
                                     presupuesto del buffer, y las GPU presentes (fabricante, para
                                     el códec de almacenamiento por hardware)
  scene-combo-box.{hpp,cpp}        → combo box del dock poblado a partir de la lista de escenas de
                                     OBS
  settings-ui.{hpp,cpp}            → el dock Qt (escena en directo/de carga, segundos, calidad,
                                     estimación de ajuste, Enable/Disable, cuenta atrás de llenado,
                                     aviso de degradación, aviso de actualización)
  ui-tokens.{hpp,cpp}              → tokens de color puros derivados de la paleta de OBS, con el
                                     contrato WCAG AA probado en tests/
  ui-theme.{hpp,cpp}               → la hoja de estilo única de la UI, las fuentes de marca
                                     empaquetadas y los iconos SVG recoloreados (src/resources/),
                                     compartidos por el dock y el diálogo
  overlay-badge.{hpp,cpp}          → pinta el overlay "DELAY 30 s" del directo como un único PNG
                                     (QPainter)
  overlay-layout.{hpp,cpp}         → colocación pura de ese overlay en el lienzo, con tests
  auto-names.{hpp,cpp}             → nombres de la escena, filtros y overlay que gestiona el plugin
                                     y su migración desde los nombres "(no tocar)" de v0.5.x, con
                                     tests
  report-bug-dialog.{hpp,cpp}      → diálogo "Reportar un problema": primero Trigi si está
                                     disponible, después el formulario de ticket (con cuenta o
                                     de invitado), con el log de OBS redactado solo si el usuario
                                     lo marca después de verlo
  trigi-page.{hpp,cpp}             → la página de Trigi en ese diálogo: aviso, datos técnicos,
                                     conversación, estados y el paso a un ticket
  trigi-widgets.{hpp,cpp}          → la vista de la conversación (nunca carga nada) y sus burbujas
  trigi-states.{hpp,cpp}           → decisiones puras de estado, botón y cuenta atrás, con tests
  chat-text.{hpp,cpp}              → saneado puro y mini parser de marcado de las respuestas de
                                     Trigi (texto escapado, negrita, código y listas), con tests
  support-client.{hpp,cpp}         → llamadas de Trigi y de tickets, respuestas y errores, con tests
  support-service.{hpp,cpp}        → la parte Qt del soporte: hilos en segundo plano, una petición
                                     de Trigi a la vez, elecciones locales, líneas de log
  support-diagnostics.{hpp,cpp}    → los datos técnicos opcionales de OBS (solo números, booleanos
                                     y valores fijos, en menos de 1 KB), con tests
  diagnostics-collector.{hpp,cpp}  → rellena esa instantánea desde OBS y el plugin
  bug-report.{hpp,cpp}             → localiza el log actual de este OBS (solo Windows por ahora:
                                     el que lleva una marca que el plugin escribe al cargar), lo
                                     lee y lo redacta en memoria, y borra las copias antiguas de
                                     %TEMP%
  log-redact.{hpp,cpp}             → redacción pura, línea a línea, del log de OBS, con tests
  update-checker.{hpp,cpp}         → compara la última release de GitHub de este repositorio contra
                                     la versión en ejecución
  background-tasks.{hpp,cpp}       → trabajos en segundo plano cancelables (inicio de sesión, Trigi,
  cancel-token.{hpp,cpp}             tickets, comprobación de actualizaciones) con un cierre acotado
  ui-tasks.{hpp,cpp}                 cuando OBS descarga el plugin, con tests; ui-tasks los une al
                                     bucle de eventos de Qt
  auth-manager.{hpp,cpp}           → la puerta de login de cuenta gratuita (ver `docs/ACCOUNT_GATE.md`),
                                     en hilos en segundo plano
  auth-client.{hpp,cpp}            → protocolo de inicio de sesión: vinculación v2, renovación,
                                     cierre de sesión, el cambio único desde la v0.5.x y las
                                     reglas de 401/403, con tests
  token-store.{hpp,cpp}            → el inicio de sesión del plugin, cifrado con la API de
                                     protección de datos de Windows, con tests
  api-client.{hpp,cpp}             → peticiones solo a api.trigglow.com, errores, Retry-After y
                                     las páginas fijas que puede abrir el plugin, con tests
  json.{hpp,cpp}                   → lector/escritor JSON estricto y acotado, con tests
  win-http.{hpp,cpp}               → el único camino WinHTTP endurecido (sin redirecciones, TLS
                                     1.2+, timeouts, tope de tamaño) para toda llamada de red;
                                     macOS/Linux responden "no disponible" hasta tener cliente HTTP
  i18n.hpp                         → helper Str() (obs_module_text()) — todo el texto visible de
                                     este plugin pasa por aquí; ver data/locale/*.ini
  hotkeys.{hpp,cpp}                → registro de las 3 hotkeys nativas de OBS, conectadas a
                                     BufferModeController
  logging.{hpp,cpp}                → wrapper de logging con prefijo de componente
  delay-controller.{hpp,cpp}       → lógica heredada del modo reconexión del diseño original
                                     abandonado (ver `docs/SPEC.md` §6) — presente en el repo pero
                                     nunca instanciada por plugin-main.cpp; no se envía en ningún
                                     build actual
data/
  locale/en-US.ini, es-ES.ini      → las cadenas traducidas reales del plugin (ver src/i18n.hpp)
  fonts/                           → Archivo y Geist Mono empaquetadas + sus licencias OFL
  licenses/                        → avisos de los componentes de terceros (iconos Lucide, DLL de FFmpeg)
tests/
  buffer_fit_tests.cpp             → tests unitarios de buffer-fit (desactivados por defecto:
                                     -DTRIGGLOW_BUILD_TESTS=ON, después ctest)
  encoder_check_tests.cpp          → tests unitarios de encoder-check (igual)
  ui_tokens_tests.cpp · auto_names_tests.cpp · overlay_layout_tests.cpp ·
  frame_arena_tests.cpp · frame_ring_tests.cpp · degradation_policy_tests.cpp ·
  storage_codec_tests.cpp · storage_policy_tests.cpp → igual, para esos módulos
  pipeline_stress_tests.cpp        → test de estrés de concurrencia de ring + arena + pipeline de
                                     códec
  pipeline_gop_stress_tests.cpp · adversarial_gop_tests.cpp → lo mismo con un códec inter-frame
                                     falso (fake-gop-codec.hpp): cadenas, pérdidas, evacuaciones,
                                     cesiones, pausas de render
  hw_storage_codec_tests.cpp       → el códec de almacenamiento por hardware sobre el FFmpeg real,
                                     con fallos inyectados (Windows, builds locales con el FFmpeg
                                     del plugin)
  json_tests.cpp · log_redact_tests.cpp · chat_text_tests.cpp · support_diagnostics_tests.cpp ·
  api_client_tests.cpp · token_store_tests.cpp · auth_client_tests.cpp ·
  support_client_tests.cpp · trigi_states_tests.cpp → cliente de cuenta, Trigi y tickets, con un
                                     sustituto de red guionizado (fake-transport.hpp)
  background_tasks_tests.cpp       → cancelación y cierre acotado de los trabajos en segundo plano
                                     (23 suites en total, 24 con hw_storage_codec_tests)
tools/fonts/                       → cómo se generan los subconjuntos de las fuentes empaquetadas
docs/
  SPEC.md                  → especificación técnica completa (empieza por aquí)
  INSTALL_GUIDE-es.md       → guía de instalación paso a paso con capturas (SmartScreen,
                             asistente del instalador, activar el panel en OBS)
  BUILD_VALIDATION.md       → qué se verificó de verdad en esta entrega, y qué no
  ROADMAP.md                → qué trajo cada versión, qué viene después y qué queda fuera de alcance
  STREAM_DECK.md             → guía detallada de Stream Deck
  FAQ.md · TROUBLESHOOTING.md
CMakeLists.txt, CMakePresets.json, buildspec.json, cmake/  → sistema de build de la plantilla oficial
```

## Licencia y repositorio

GPL-2.0-or-later, heredada de `obsproject/obs-plugintemplate` (ver `LICENSE`) — no es solo una
formalidad: `libobs` es GPL-2.0, así que enlazar contra ella arrastra la misma licencia a este
plugin, lo que obliga a publicar el código fuente de cualquier binario distribuido.

Por eso este proyecto vive en **su propio repositorio, separado del repositorio principal de
Trigglow** (que permanece privado) — publicar solo el plugin aquí cumple la GPL sin exponer el resto
del producto. Detalle completo de esta decisión en `docs/SPEC.md` §9.

`main` es una rama protegida en GitHub: las fusiones requieren un pull request, incluso para el
propio dueño del repositorio (sin pushes directos, sin force-push, sin borrar la rama). El trabajo
del día a día ocurre en `develop`; los PRs van de `develop` a `main`.

### Recursos de terceros

El plugin incluye algunos componentes de terceros, cada uno con su licencia (todas compatibles con
distribuirlos junto a un binario GPL-2.0):

| Recurso | Dónde | Licencia |
|---|---|---|
| **Archivo** (SemiBold, Bold), subconjunto | `data/fonts/Archivo-*.ttf` | SIL Open Font License 1.1, `data/fonts/OFL-Archivo.txt` |
| **Geist Mono** (Medium), subconjunto | `data/fonts/GeistMono-Medium.ttf` | SIL Open Font License 1.1, `data/fonts/OFL-GeistMono.txt` |
| Iconos de la interfaz (trazados de Lucide, como en la web de Trigglow) | compilados desde `src/resources/icons/` | ISC (partes MIT, Feather), `data/licenses/LICENSE-lucide.txt` |
| Bibliotecas de **FFmpeg** (avcodec, avutil, swresample), build LGPL de BtbN fijada en `buildspec.json`, solo Windows | DLL junto a la del plugin (`bin/64bit` en el paquete), sin modificar | LGPL 3.0 o posterior, con componentes MIT/BSD/Apache (libvpl, AMF, ffnvcodec); `data/licenses/LICENSE-ffmpeg.txt`, con la etiqueta de la build y su código fuente |

Las fuentes son instancias estáticas, en subconjunto, de las fuentes variables originales de
[google/fonts](https://github.com/google/fonts) (`ofl/archivo`, `ofl/geistmono`); `tools/fonts/`
explica cómo se generan. La OFL exige que su texto acompañe a las fuentes, así que cada `OFL-*.txt`
va junto a su fuente en el paquete y en el instalador. Las fuentes solo las usan el dock, el diálogo
y el overlay del plugin (la fuente de la interfaz de OBS nunca cambia); sin ellas, todo el texto usa
la fuente de OBS.
