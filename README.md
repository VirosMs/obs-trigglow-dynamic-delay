# Trigglow Dynamic Delay for OBS

Plugin nativo de OBS Studio para activar, desactivar y alternar un delay configurable del stream
desde dentro de OBS — con un botón, una hotkey nativa o Stream Deck. Sin app externa, sin panel web
como forma principal de control. **Estado: MVP / v0.1.0 — Early Access.**

Antes de nada, lee `docs/SPEC.md` (especificación técnica completa, incluida la limitación real de
OBS que determina cómo funciona este plugin) y `docs/BUILD_VALIDATION.md` (qué se verificó realmente
y qué no, en esta primera entrega).

## Requisitos de compilación

Basado en la plantilla oficial [`obsproject/obs-plugintemplate`](https://github.com/obsproject/obs-plugintemplate),
sin modificar su sistema de build (`cmake/`, `CMakePresets.json`, `.github/`) salvo para activar
`ENABLE_FRONTEND_API` y `ENABLE_QT` y registrar los archivos fuente nuevos.

| Plataforma | Herramientas |
|---|---|
| Windows | Visual Studio 17 2022, CMake ≥ 3.30.5 |
| macOS | Xcode 16.0, CMake ≥ 3.30.5 |
| Ubuntu 24.04 | CMake ≥ 3.28.3, `ninja-build`, `pkg-config`, `build-essential` |

**Windows es la plataforma prioritaria para v0.1.0** (así lo pidió el producto); macOS/Linux están
soportados por el propio sistema de build de la plantilla, pero no se han probado en esta entrega.

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

## Instalar el plugin compilado en OBS

1. Localiza el binario generado (`.dll` en Windows dentro de `build_x64/RelWithDebInfo/`, `.plugin`
   en macOS, `.so` en Linux).
2. Cópialo a la carpeta de plugins de OBS:
   - Windows: `C:\Program Files\obs-studio\obs-plugins\64bit\`
   - macOS: `~/Library/Application Support/obs-studio/plugins/`
   - Linux: `~/.config/obs-studio/plugins/`
3. Reinicia OBS.

(Un instalador con un solo clic queda fuera del MVP — ver `docs/ROADMAP.md`.)

## Usar el plugin

1. Abre OBS. Si el dock **"Trigglow Dynamic Delay"** no aparece, ve a `View → Docks` y actívalo.
2. Configura los segundos de delay (por defecto 10s) y decide si quieres "Modo seguro" activado
   (recomendado: sí).
3. Ve a `Configuración → Atajos de teclado`, busca "Trigglow" y asigna una tecla a **Toggle Dynamic
   Delay** (y opcionalmente a Enable/Disable por separado).
4. Ya puedes usar el botón del dock, la hotkey, o (ver más abajo) Stream Deck.

**Importante:** si ya estás en directo cuando activas/desactivas/cambias el delay, OBS reconectará
brevemente el stream para aplicar el nuevo valor — es una limitación real de OBS, no un bug del
plugin. Detalle completo en `docs/SPEC.md` §2.

## Mapear un botón de Stream Deck

No hace falta ningún plugin de Stream Deck propio en v0.1.0:

1. En el software de Stream Deck, añade la acción **"System" → "Hotkey"** (el nombre exacto puede
   variar ligeramente según tu versión de Stream Deck).
2. Configúrala para que envíe la combinación de teclas que le asignaste a "Toggle Dynamic Delay" en
   el paso anterior.
3. Pon el foco en la ventana de OBS al pulsar el botón (Stream Deck envía la pulsación de teclado al
   sistema; OBS necesita tener el foco, o al menos estar corriendo y escuchando el atajo global,
   según cómo tengas configurado OBS/tu SO).

Guía más detallada, con capturas de ejemplo y solución de problemas: `docs/STREAM_DECK.md`.

## Estructura del proyecto

```
src/
  plugin-main.cpp          → obs_module_load/unload, wiring de todos los componentes
  delay-controller.{hpp,cpp} → máquina de estados (Inactive/Applying/Active/Error), lógica principal
  obs-frontend-bridge.{hpp,cpp} → única capa que toca obs-frontend-api.h
  settings-ui.{hpp,cpp}    → dock Qt mínimo
  hotkeys.{hpp,cpp}        → registro de las 3 hotkeys nativas de OBS
  logging.{hpp,cpp}        → wrapper de logging con prefijo de componente
docs/
  SPEC.md                  → especificación técnica completa (empieza por aquí)
  BUILD_VALIDATION.md       → qué se verificó de verdad en esta entrega, y qué no
  ROADMAP.md                → v0.2, v0.3 y qué queda fuera de alcance
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
del producto. Detalle completo de esta decisión en `docs/SPEC.md` §8.
