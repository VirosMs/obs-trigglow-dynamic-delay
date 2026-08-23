# Guía rápida: Trigglow Dynamic Delay + Stream Deck

Trigglow Dynamic Delay **no necesita un plugin de Stream Deck propio** en v0.1.0. Usa las hotkeys
nativas que este plugin registra en OBS, y Stream Deck ya sabe disparar cualquier hotkey de OBS.

## Paso 1 — Asignar la hotkey en OBS

1. `Configuración → Atajos de teclado`.
2. Busca "Trigglow" en el buscador de la parte superior.
3. Verás 3 entradas:
   - **Trigglow: Toggle Dynamic Delay** (la que casi todo el mundo quiere mapear)
   - **Trigglow: Enable Dynamic Delay**
   - **Trigglow: Disable Dynamic Delay**
4. Haz clic en el campo junto a la que quieras usar y pulsa la combinación de teclas que prefieras
   (ej. `Ctrl+Alt+D`). Evita combinaciones que ya uses para otra cosa en el sistema.

## Paso 2 — Mapear el botón en Stream Deck

1. Abre el software de Stream Deck (Elgato).
2. En el panel de acciones, busca la categoría **System** y arrastra la acción **Hotkey** (a veces
   aparece como "System: Hotkey" o "Enviar pulsación de teclado") a un botón.
3. Configura esa acción con la misma combinación de teclas que asignaste en el Paso 1.
4. (Opcional) Cambia el icono y el título del botón a algo reconocible, como "Delay ON/OFF".

## Paso 3 — Probarlo

1. Con OBS abierto (no hace falta estar en directo para la primera prueba), pulsa el botón del
   Stream Deck.
2. Deberías ver el estado del dock "Trigglow Dynamic Delay" cambiar entre `Inactive` y `Active`.
3. Repite ya en directo para comprobar la reconexión breve — ver `docs/SPEC.md` §2 para entender por
   qué ocurre.

## Notas y limitaciones de este flujo

- Si tu Stream Deck está configurado para enviar la pulsación al sistema en vez de a una ventana
  concreta, asegúrate de que OBS puede recibir atajos globales (por defecto, OBS registra sus
  hotkeys a nivel de sistema operativo en Windows/macOS, así que normalmente funciona incluso con
  OBS en segundo plano — pero puede variar según tu configuración de accesibilidad/permisos del SO).
- Si usas la misma combinación de teclas para otra cosa en tu sistema, puede haber conflictos.
  Prueba con una combinación poco habitual.
- Un plugin de Stream Deck propio de Trigglow (con estado ON/OFF reflejado en el color del propio
  botón) está en el roadmap de v0.3 — ver `docs/ROADMAP.md`. No es necesario para usar el MVP hoy.
