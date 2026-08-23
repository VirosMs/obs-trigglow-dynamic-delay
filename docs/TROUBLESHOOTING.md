# Troubleshooting básico — Trigglow Dynamic Delay

## El dock no aparece en OBS
- `View → Docks` y comprueba que "Trigglow Dynamic Delay" esté marcado.
- Revisa el log de OBS (`Ayuda → Registros y Perfiles → Ver log actual`) buscando la línea
  `[obs-trigglow-dynamic-delay] loaded successfully`. Si no aparece, el plugin no se cargó — revisa
  que el binario esté en la carpeta de plugins correcta para tu sistema operativo (ver README) y que
  la arquitectura (64 bits) coincida con tu instalación de OBS.

## El estado se queda en "Applying..." indefinidamente
- Con "Modo seguro" activado, el plugin pasa automáticamente a `Error` tras ~12 segundos si OBS no
  confirma la reconexión. Si se queda colgado más tiempo que eso, revisa tu conexión de red y el
  estado de streaming en la propia OBS (¿sigue OBS intentando reconectar por su cuenta?).
- Con "Modo seguro" desactivado, el plugin asume que la reconexión solo va lenta y no fuerza un
  `Error` — puede quedarse en `Applying` más tiempo si tu red es lenta. Actívalo si prefieres que el
  plugin se rinda con un mensaje claro en vez de esperar indefinidamente.

## Aparece "No hay ningún output de streaming disponible ahora mismo"
- Comprueba que tienes un servicio de streaming configurado en `Configuración → Emisión`.
- Este mensaje es esperado si nunca has configurado streaming en este perfil de OBS.

## La hotkey no hace nada
- Confirma que la asignaste en `Configuración → Atajos de teclado` (buscando "Trigglow") — el
  plugin registra las 3 hotkeys sin ninguna tecla asignada por defecto.
- Prueba primero el botón del dock para descartar que sea un problema de configuración del plugin en
  sí, y no de la hotkey/Stream Deck.

## El botón de Stream Deck no hace nada
- Verifica que la combinación configurada en Stream Deck coincide exactamente con la asignada en
  OBS.
- Ver `docs/STREAM_DECK.md` para el flujo completo paso a paso.

## El plugin no compila
- Revisa `docs/BUILD_VALIDATION.md` — ahí se documenta exactamente qué se verificó (sintaxis de la
  mayoría del código C++ contra las firmas reales de la API de OBS) y qué no (compilación real con
  Qt6 y el SDK completo de OBS, que requiere que TÚ hagas la primera build siguiendo el README).
- Un error de compilación en tu primera build es información valiosa — probablemente sea una
  diferencia entre la versión exacta de OBS/Qt que estás usando y la que se referenció al escribir
  este código. Abre un issue en el repositorio con el log de error completo.

## No sé si el problema es del plugin o de mi configuración de streaming
- Desactiva el plugin (bórralo temporalmente de la carpeta de plugins) y confirma que el streaming
  funciona bien sin él. Si el problema persiste sin el plugin, no es cosa del plugin.
