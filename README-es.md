*[English version](README.md)*

# Trigglow Dynamic Delay for OBS

Plugin nativo de OBS Studio que retrasa el **vídeo y el audio de tu stream juntos**, un número
configurable de segundos, desde un botón, una hotkey nativa de OBS o un Stream Deck — **sin que el
output de streaming se toque nunca.** Sin reconexión, sin corte, en ningún momento, por ningún
motivo. Sin app externa, sin panel web, sin proceso aparte: todo vive dentro del propio proceso de
OBS. **Estado: MVP / v0.3.1 — Early Access.**

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
de Trigglow en vez de los gráficos genéricos de Inno Setup. Ver `CHANGELOG-es.md`.

Antes de nada, lee `docs/SPEC.md` (especificación técnica completa de cómo funciona realmente el
modo buffer, y por qué el enfoque obvio de "simplemente cambiar el delay nativo de OBS en directo"
se probó y se descartó) y `docs/BUILD_VALIDATION.md` (qué se verificó realmente y qué no, en esta
primera entrega).

## Requisitos de uso

**OBS Studio 31.1.0 o superior.** El binario v0.3.1 se compila contra las fuentes de OBS 31.1.1
(ver `buildspec.json`) y se ha verificado que carga y funciona correctamente en OBS **32.2.2**
(el mecanismo de compatibilidad de plugins de OBS rechaza solo los plugins compilados contra una
versión *más nueva* que la que está corriendo; nunca se ha probado en versiones anteriores a 31.1.1).

## Requisitos de compilación

Basado en la plantilla oficial [`obsproject/obs-plugintemplate`](https://github.com/obsproject/obs-plugintemplate),
sin modificar su sistema de build (`cmake/`, `CMakePresets.json`, `.github/`) salvo para activar
`ENABLE_FRONTEND_API` y `ENABLE_QT` y registrar los archivos fuente nuevos.

| Plataforma | Herramientas |
|---|---|
| Windows | Visual Studio 17 2022, CMake ≥ 3.30.5 |
| macOS | Xcode 16.0, CMake ≥ 3.30.5 |
| Ubuntu 24.04 | CMake ≥ 3.28.3, `ninja-build`, `pkg-config`, `build-essential` |

**Windows es la plataforma prioritaria para v0.3.1** (así lo pidió el producto); macOS/Linux
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

**Windows: usa el instalador.** Descarga `obs-trigglow-dynamic-delay-0.3.1-windows-x64-setup.exe`
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
   **calidad mínima** (480p/720p/1080p) por debajo de la cual el vídeo delayed nunca debe caer. El
   dock muestra una estimación en vivo de si los segundos elegidos caben realmente en la RAM a esa
   calidad *antes* de pulsar Enable — si no caben, avisa de que el tiempo real de buffer se
   acortará en vez de bajar nunca la calidad, pero nunca bloquea tu elección.
3. Ve a `Configuración → Atajos de teclado`, busca "Trigglow" y asigna una tecla a **Toggle Dynamic
   Delay** (y opcionalmente a Enable/Disable por separado).
4. Pulsa Enable (desde el botón del dock, la hotkey o Stream Deck). El dock muestra `Filling` con
   una cuenta atrás en vivo mientras se llena el buffer hasta el delay pedido; en cuanto se llena,
   pasa automáticamente a `Active` y el Programa empieza a mostrar el vídeo y el audio delayed, en
   sincronía. **El output de streaming en sí no se toca en ningún momento**: los espectadores en
   Twitch/YouTube/Kick/donde sea nunca ven un corte ni una reconexión, ya pulses el botón antes o
   durante un directo.

Pulsar Disable libera el buffer de RAM al instante y vuelve a la escena que estaba antes de
activarlo.

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
   pasa por `Filling` (con la cuenta atrás en vivo del dock) hasta `Active` en cuanto el buffer se
   llena — no hay reconexión ni corte visible en el stream en ningún momento de esa transición.

Guía más detallada, con capturas de ejemplo y solución de problemas: `docs/STREAM_DECK.md`.

## Estructura del proyecto

```
src/
  plugin-main.cpp                 → obs_module_load/unload, wiring de todos los componentes
  buffer-mode-controller.{hpp,cpp} → la máquina de estados (Inactive/Filling/Active/Error) que
                                     orquesta el modo buffer — la única dueña del estado
  obs-frontend-bridge.{hpp,cpp}    → única capa que toca obs-frontend-api.h + el cableado de
                                     filtros/escenas
  video-delay-filter.{hpp,cpp}     → el ring buffer en RAM/NV12 del vídeo delayed + su pequeño pool
                                     fijo de objetos GPU
  audio-delay-filter.{hpp,cpp}     → ring de delay de audio por fuente hoja, sincronizado con el
                                     vídeo
  hardware-info.{hpp,cpp}          → consulta de RAM total del sistema, usada para dimensionar el
                                     presupuesto del buffer
  scene-combo-box.{hpp,cpp}        → combo box del dock poblado a partir de la lista de escenas de
                                     OBS
  settings-ui.{hpp,cpp}            → el dock Qt (escena en directo/de carga, segundos, calidad,
                                     estimación de ajuste, Enable/Disable, cuenta atrás de llenado)
  hotkeys.{hpp,cpp}                → registro de las 3 hotkeys nativas de OBS, conectadas a
                                     BufferModeController
  logging.{hpp,cpp}                → wrapper de logging con prefijo de componente
  delay-controller.{hpp,cpp}       → lógica heredada del modo reconexión del diseño original
                                     abandonado (ver `docs/SPEC.md` §6) — presente en el repo pero
                                     nunca instanciada por plugin-main.cpp; no se envía en ningún
                                     build actual
docs/
  SPEC.md                  → especificación técnica completa (empieza por aquí)
  INSTALL_GUIDE-es.md       → guía de instalación paso a paso con capturas (SmartScreen,
                             asistente del instalador, activar el panel en OBS)
  BUILD_VALIDATION.md       → qué se verificó de verdad en esta entrega, y qué no
  ROADMAP.md                → v0.3, v0.4 y qué queda fuera de alcance
  STREAM_DECK.md             → guía detallada de Stream Deck
  FAQ.md · TROUBLESHOOTING.md
CMakeLists.txt, CMakePresets.json, buildspec.json, cmake/  → sistema de build de la plantilla oficial
```

## Licencia y repositorio

GPL-2.0-or-later, heredada de `obsproject/obs-plugintemplate` (ver `LICENSE`) — no es solo una
formalidad: `libobs` es GPL-2.0, así que enlazar contra ella arrastra la misma licencia a este
plugin, lo que obliga a publicar el código fuente de cualquier binario distribuido.

Por eso este proyecto vive en **su propio repositorio, separado del monorepo principal de
Trigglow** (que permanece privado) — publicar solo el plugin aquí cumple la GPL sin exponer el resto
del producto. Detalle completo de esta decisión en `docs/SPEC.md` §9.

`main` es una rama protegida en GitHub: las fusiones requieren un pull request, incluso para el
propio dueño del repositorio (sin pushes directos, sin force-push, sin borrar la rama). El trabajo
del día a día ocurre en `develop`; los PRs van de `develop` a `main`.
