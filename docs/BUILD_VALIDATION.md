# Validación de compilación — qué se verificó y qué no (honesto, sin humo)

Este proyecto se generó en un entorno de sandbox en la nube sin acceso a `apt` (los espejos de
Ubuntu devuelven `403 Forbidden` por política de red del sandbox), por lo que **no fue posible
instalar `libobs-dev` / `qt6-base-dev` reales ni hacer un build+link completo contra OBS Studio**
dentro de esta sesión. Esto se documenta aquí sin adornos, tal y como se pidió: nada de "demo falsa".

## Lo que SÍ se verificó, y cómo

1. **Ground truth de la API, leída directamente del código fuente real**, no solo de la
   documentación (que en varios puntos está incompleta): se clonó
   [`obsproject/obs-studio`](https://github.com/obsproject/obs-studio) (sparse checkout de
   `libobs/obs-output.c`, `libobs/obs-output-delay.c`, `libobs/obs-output.h`, `libobs/obs-hotkey.h`,
   `libobs/util/base.h`, `frontend/api/obs-frontend-api.h`) y se clonó
   [`obsproject/obs-plugintemplate`](https://github.com/obsproject/obs-plugintemplate) completo como
   base real del proyecto (no una recreación de memoria). Esto es lo que permitió confirmar, a nivel
   de código, el hallazgo central del §2 de `SPEC.md` (que el delay se "bloquea" al arrancar el
   output) y las firmas exactas de cada función usada.

2. **Comprobación de sintaxis real con `g++ -std=c++17 -fsyntax-only`** contra un conjunto de headers
   sintéticos (`/tmp/synthetic-obs-headers` durante la generación; no se incluyen en este ZIP porque
   no son parte del proyecto, son un arnés de verificación) construidos copiando literalmente las
   firmas verificadas en el paso 1 — no inventadas. Archivos verificados así, con éxito
   (`exit: 0`, sin errores ni warnings):
   - `src/logging.cpp`
   - `src/obs-frontend-bridge.cpp` — **este paso encontró y corrigió un bug real**: el callback de
     `obs_frontend_add_event_callback` estaba declarado como `void(int, void*)` en vez de
     `void(enum obs_frontend_event, void*)`, lo cual no compila. Corregido antes de entregar el ZIP.
   - `src/delay-controller.cpp`
   - `src/hotkeys.cpp`
   - `src/plugin-main.cpp` (parcial: la lógica de ciclo de vida del módulo, hotkeys y
     persistencia de settings se verificó con stubs de `QString`/`QDir`/`QFileInfo`/`QWidget`
     mínimos; **el contenido interno de `settings-ui.cpp` NO se verificó**, ver más abajo)

## Lo que NO se verificó (y por qué)

- **`src/settings-ui.cpp`** (el dock de Qt: layouts, señales/slots, `QSpinBox`, `QCheckBox`, etc.)
  no se pudo compilar ni siquiera en modo sintaxis-únicamente en este sandbox, porque no hay un Qt6
  real disponible y no se pudo instalar (`apt-get install qt6-base-dev` falló por la política de red
  del sandbox, no por un problema del paquete). El código se escribió siguiendo los patrones
  estándar de Qt Widgets (los mismos usados por cientos de plugins de OBS reales), pero **su primera
  compilación real ocurrirá quien lo compile con el SDK oficial de OBS** — ver `README.md` para los
  pasos. Es el único archivo del proyecto sin verificación de sintaxis en esta sesión.
- **Ningún linkado real contra `libobs`/`obs-frontend-api`/Qt6** ocurrió — solo verificación de
  sintaxis y tipos contra las firmas reales. Esto NO sustituye una build real; es una red de
  seguridad adicional antes de que tú hagas la primera build de verdad.
- **No se probó dentro de una instancia real de OBS** — ni el registro del dock, ni las hotkeys, ni
  el comportamiento de reconexión al aplicar el delay en directo. Esa es la verificación que falta
  y que solo tú puedes hacer siguiendo la guía rápida del `README.md`.

## Recomendación

Trata este plugin como un **MVP listo para tu primera compilación real, no como un binario ya
probado**. La checklist final de esta entrega incluye exactamente los pasos para hacer esa primera
build y detectar cualquier detalle que solo un compilador con el SDK real de OBS pueda pillar (por
ejemplo, cualquier diferencia entre mis headers sintéticos y la versión exacta de OBS que uses).
