# Validación de compilación — qué se verificó y qué no (honesto, sin humo)

Este documento describía originalmente (2026-08-23) un bootstrap hecho en un sandbox en la nube sin
acceso real al SDK de OBS ni a Qt6 — solo verificación de sintaxis (`g++ -fsyntax-only`) contra
headers sintéticos, y `settings-ui.cpp` sin compilar ni una sola vez. Eso ya no describe el estado
real del proyecto. Desde entonces el plugin se ha compilado con éxito **muchas veces** en CI real
contra el SDK real de OBS y Qt6 real, y se ha probado en directo dentro de una instancia real de OBS
Studio. Esto se reescribe aquí para reflejar eso, con la misma honestidad de siempre: qué está
confirmado y qué todavía no.

## Lo que SÍ está confirmado, y cómo

1. **Compilación real en CI en las 3 plataformas.** `.github/workflows/push.yaml` y
   `build-project.yaml` compilan el plugin en Windows, macOS y Ubuntu contra el **SDK real de OBS**
   (`obs-studio` v31.1.1, ver `buildspec.json`) y **Qt6 real** (prebuilt de `obsproject/obs-deps`), no
   headers sintéticos ni un chequeo de solo-sintaxis. Esto incluye el linkado real contra
   `libobs`/`obs-frontend-api`/Qt6 — no solo comprobación de tipos. El estado de estos workflows es
   verde en las 3 plataformas.

2. **`src/settings-ui.cpp` (el dock de Qt) ya se ha compilado de verdad**, con el Qt6 real del SDK de
   OBS, como parte de esas mismas builds de CI — ya no es el único archivo del proyecto sin verificar
   que era en 2026-08-23. Los layouts, señales/slots, `QSpinBox`, `QComboBox`, etc. compilan y enlazan
   contra Qt6 real.

3. **Pruebas en directo dentro de una instancia real y en ejecución de OBS Studio 32.2.2 en Windows**,
   con captura de juego real, a lo largo de varias iteraciones del desarrollo del modo buffer (ver
   `docs/SPEC.md` para el diseño completo). Concretamente, el desarrollador ha confirmado en directo:
   - El llenado y vaciado del ring buffer (`Inactive → Filling → Active` y vuelta a `Inactive` al
     hacer Disable) con contenido de vídeo y audio real.
   - Habilitar/Deshabilitar (Enable/Disable) repetidamente sin crashes, incluyendo el ciclo completo
     de la migración de un diseño de una-textura-de-GPU-por-frame a RAM+NV12 (ver `docs/SPEC.md` §3.1).
   - Corrección de color confirmada visualmente tras la conversión RGBA↔NV12 (BT.601) — el vídeo
     retrasado se ve igual que el vídeo en directo, sin desplazamientos de color.
   - Sincronía de audio: el audio retrasado se mantiene en sync con el vídeo retrasado a lo largo del
     tiempo, no solo al empezar.
   - Uso de RAM confirmado en el Administrador de tareas de Windows, consistente con el presupuesto de
     RAM calculado por `EnsureRingSized()` (ver `docs/SPEC.md` §3.5).
   - La corrección de "liberar RAM al hacer Disable" (`docs/SPEC.md` §3.4): confirmado que el uso de
     RAM vuelve a bajar tras pulsar Disable, en vez de quedarse reservado indefinidamente.

## Lo que TODAVÍA no está confirmado (y por qué)

- **macOS y Linux no se han probado en directo** dentro de una instancia real de OBS — ambos compilan
  en verde en CI (SDK real, no sintético), pero nadie ha ejecutado el plugin todavía dentro de un OBS
  en ejecución en esas plataformas. Trátalos como "compila, comportamiento en directo sin confirmar"
  hasta que alguien lo pruebe.
- **El riesgo de crash "Device Remove/Reset" de la GPU no está confirmado como resuelto.** Se observó
  en directo bajo el diseño anterior (una textura de GPU por frame almacenado); la reescritura a
  RAM/NV12 reduce plausiblemente el riesgo (un puñado de objetos de GPU fijos y pequeños en vez de
  docenas que escalan con la duración del delay) pero esto no es lo mismo que una prueba de que ya no
  puede ocurrir. Ver `docs/SPEC.md` §6 para el detalle completo — sigue siendo un área de riesgo real
  hasta que se vuelva a probar específicamente buscando que reaparezca.
- **No existe una suite de pruebas automatizadas dedicada** (unit tests, integration tests, etc.) para
  este plugin. Toda la validación descrita arriba es CI de compilación/enlazado más pruebas manuales
  en directo por parte del desarrollador — no hay cobertura automatizada que se ejecute en cada commit
  más allá de "¿compila?".

## Recomendación

Trata este plugin como un **MVP con compilación real confirmada en las 3 plataformas y comportamiento
en directo confirmado en Windows**, no como un producto con cobertura de pruebas automatizada ni con
paridad de pruebas en directo entre plataformas. Si vas a usarlo en macOS o Linux, o si tu escenario
se acerca al patrón que causó el crash de GPU descrito arriba (delays largos, habilitar/deshabilitar
repetidamente durante una sesión larga), prueba primero fuera de una emisión real antes de confiar en
él para directo.
