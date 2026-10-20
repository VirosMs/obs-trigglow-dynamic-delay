*[English version](INSTALL_GUIDE.md)*

# Guía de instalación (paso a paso, con capturas)

Esto explica el instalador de Windows de principio a fin, incluido el aviso de SmartScreen de
Microsoft Edge que verás porque el instalador todavía no está firmado (Early Access — ver
`docs/ROADMAP-es.md`). Las capturas de abajo son de una instalación real en una versión anterior,
antes de que v0.3.1 añadiera el icono/banner de Trigglow que ves hoy en el instalador — los pasos y
la disposición son iguales, solo el estilo genérico y el número de versión de la esquina están
desactualizados.

Desde la v0.7.1 no hay página de carpeta y el propio instalador te pide cerrar OBS (paso 4). Linux
y macOS: ver sus secciones más abajo.

## Compatibilidad

| Plugin | OBS mínimo | Probado hasta | Win | macOS | Linux |
|---|---|---|---|---|---|
| 0.7.1 | 31.1.0 | 32.2.2 | Instalador, `.zip` (ubicación de OBS 33) | `.pkg` | `.deb`, `.rpm`, `.pkg.tar.zst`, `.tar.xz` |
| 0.7.0 | 31.1.0 | 32.2.2 | Instalador, `.zip` ("Legacy" en OBS 33) | `.pkg` | `.deb`, solo Ubuntu 24.04 con OBS 32.2 |

"Probado hasta" es el OBS más nuevo en el que se ha usado el plugin en directo (en Windows). OBS 33
está en release candidate: la v0.7.1 ya se instala donde busca los plugins, pero aún no se ha
probado en directo con él. En un OBS más antiguo que el mínimo el plugin no se carga, y explica por
qué en el log de OBS. Linux y macOS están más abajo.

## 1. Descarga

Consigue `obs-trigglow-dynamic-delay-0.7.1-windows-x64-setup.exe` desde la
[última release](https://github.com/VirosMs/obs-trigglow-dynamic-delay/releases/latest). ¿Prefieres
instalarlo a mano? El `.zip` de esa misma release solo trae los archivos del plugin — ver
"Instalar el plugin" en `README-es.md`.

## 2. Windows SmartScreen: por qué aparece y cómo continuar

Como el instalador no está firmado con un certificado de firma de código, Microsoft Defender
SmartScreen no puede confirmar que sea seguro y avisa antes de dejarlo ejecutar. Es normal para
software Early Access sin firmar, no una señal de que algo esté mal — pero solo deberías continuar
con instaladores de un origen en el que confíes de verdad (en este caso, la página de Releases de
GitHub enlazada arriba).

Nada más terminar la descarga, Edge muestra este aviso. Haz clic en **"Mantener de todos modos"**:

![Aviso de SmartScreen justo tras la descarga](images/smartscreen-1-warning.png)

Si cerraste ese aviso, o vuelves a él más tarde desde la lista de Descargas, haz clic en el menú
**"..."** junto al archivo:

![Lista de Descargas, menú "..."](images/smartscreen-2-menu.png)

...y elige **"Conservar"**:

![Menú "...", opción Conservar](images/smartscreen-3-keep.png)

¿Usas otro navegador? El aviso y los nombres exactos de los botones serán distintos (Chrome,
Firefox), pero la decisión de fondo es la misma: confirmar que confías en el archivo antes de
abrirlo.

## 3. Cierra OBS antes de instalar

**Instálalo antes de salir en directo, no durante el stream.** El instalador necesita sobrescribir
archivos que OBS tiene abiertos mientras está en marcha — si OBS está abierto al empezar la
instalación, te pide cerrarlo (ver el paso 4 de abajo). Planea la instalación para antes de una
sesión, no a mitad de una emisión.

## 4. Ejecuta el instalador

Es un asistente estándar de siguiente-siguiente-siguiente:

1. Elige un idioma:

   ![Selector de idioma](images/installer-1-language.png)

2. Pantalla de bienvenida — confirma la versión que se va a instalar (esta captura muestra la
   0.2.0 y el estilo genérico de Inno Setup; desde v0.3.1 verás el icono y el banner de Trigglow en
   su lugar, además de la versión actual, pero con la misma disposición y botones):

   ![Pantalla de bienvenida, versión 0.2.0](images/installer-2-welcome.png)

3. Desde la v0.7.1 no hay carpeta que elegir: el plugin va siempre a la carpeta de plugins de OBS,
   `C:\ProgramData\obs-studio\plugins\obs-trigglow-dynamic-delay` (ver "Dónde quedan los
   archivos en Windows" más abajo). El instalador encuentra OBS por la clave del registro que
   escribe el instalador oficial de OBS y comprueba que sea OBS 31.1.0 o posterior; con un OBS más
   antiguo se detiene y te pide actualizar OBS antes.

4. Si OBS sigue abierto, el instalador te pide cerrarlo (Archivo → Salir) y pulsar **Reintentar**.

5. Instalando:

   ![Instalando / reiniciando aplicaciones](images/installer-5-installing.png)

6. Listo — opcionalmente, lanza OBS Studio al momento (se abre con tu usuario, no como administrador):

   ![Instalación completada](images/installer-6-finish.png)

## 5. Activa el panel en OBS

El panel no siempre aparece solo la primera vez. En la barra de menú de OBS, abre
**Paneles (View → Docks)**:

![Barra de menú de OBS, Paneles](images/obs-panel-1-menu.png)

Haz clic en **"Restablecer paneles"**, o simplemente asegúrate de que **"Trigglow Dynamic Delay"**
esté marcado en ese mismo menú:

![Menú de paneles con Trigglow Dynamic Delay listado](images/obs-panel-2-reset-panels.png)

El panel aparece mostrando `● Inactive` — ya puedes elegir una escena en directo y empezar. Ver
"Using the plugin" en `README-es.md` para qué configurar a continuación.

![Panel de Trigglow Dynamic Delay, estado Inactive](images/obs-panel-3-dock.png)

## 6. Inicia sesión una vez

`Activar delay` necesita una cuenta gratuita de trigglow.com, con la sesión iniciada una vez desde
el dock (por ahora solo en Windows; macOS y Linux tendrán inicio de sesión con el cliente HTTP
previsto a continuación, ver `docs/ROADMAP-es.md`):

1. Pulsa **Iniciar sesión** en el dock. El navegador abre trigglow.com; inicia sesión ahí o crea una
   cuenta gratuita.
2. El dock muestra un código ("La página debe mostrar el código KD7F-29QM."; el tuyo será otro).
   Comprueba que la página muestra el mismo código y pulsa **Confirmar**. Si los códigos no
   coinciden, o no lo has iniciado tú, pulsa **No he sido yo**.
3. Unos segundos después el dock dice "Conectado como …". No hace falta volver a iniciar sesión en
   el siguiente arranque.

Si no se abre el navegador, pulsa **Copiar enlace** y pégalo en tu navegador. Más detalles en la
sección "Iniciar sesión desde el dock" de `docs/TROUBLESHOOTING-es.md`.

## Linux

Un único build x86_64 del plugin funciona en todas estas; elige el paquete de tu familia de
distribuciones (los mismos formatos que la app de escritorio de Trigglow). Todos necesitan OBS
Studio 31.1.0 o posterior y Qt 6.4 o posterior, y los instalan, o se niegan a instalarse, a través
de tu gestor de paquetes.

| Distribución | Paquete | OBS Studio |
|---|---|---|
| Ubuntu 24.04 LTS | `.deb` | del [PPA de OBS](https://launchpad.net/~obsproject/+archive/ubuntu/obs-studio): el de Ubuntu es la 30.0, demasiado antiguo |
| Ubuntu 26.04 LTS, 26.10 | `.deb` | el de Ubuntu (32.1, 32.2) o el del PPA |
| Debian 13 | `.deb` | el de Debian es la 30.2, por debajo del mínimo: el `.deb` no se instala hasta que haya un OBS 31.1 o posterior |
| Linux Mint, Pop!_OS y otras basadas en Ubuntu | `.deb` | como su Ubuntu base |
| Fedora | `.rpm` | el de Fedora |
| openSUSE | `.rpm` | el de Packman |
| Arch, Manjaro, EndeavourOS | `.pkg.tar.zst` | el de la distribución |
| Cualquier otra (x86_64, glibc 2.39 o posterior) | `.tar.xz` | el tuyo, 31.1.0 o posterior |

El OBS de Flatpak no está soportado: ejecuta los plugins dentro de su propio runtime y los guarda en
otro sitio.

Instalación, con OBS cerrado:

```bash
sudo apt install ./obs-trigglow-dynamic-delay-0.7.1-x86_64-linux-gnu.deb   # Debian, Ubuntu
sudo dnf install ./obs-trigglow-dynamic-delay-0.7.1-x86_64-linux-gnu.rpm   # Fedora, openSUSE (zypper install)
sudo pacman -U obs-trigglow-dynamic-delay-0.7.1-x86_64-linux-gnu.pkg.tar.zst  # Arch
```

Los paquetes dejan el plugin en la ubicación del sistema de OBS 33,
`<libdir>/obs-modules/plugins/obs-trigglow-dynamic-delay.so`, con sus datos en
`/usr/share/obs/obs-modules/plugins/obs-trigglow-dynamic-delay/`, y enlaces simbólicos en las
ubicaciones legacy `<libdir>/obs-plugins/` y `/usr/share/obs/obs-plugins/` que leen OBS 31 y 32
(`<libdir>`: `/usr/lib/x86_64-linux-gnu`, `/usr/lib64` o `/usr/lib`). Se quita con tu gestor de
paquetes (`sudo apt remove obs-trigglow-dynamic-delay`, `dnf remove`, `pacman -R`).

**Instalación por usuario (`.tar.xz`, sin root):** descomprímelo y ejecuta `./install.sh` con tu
usuario. Copia el plugin en `~/.local/share/obs-studio/plugins/obs-trigglow-dynamic-delay/` (OBS 33
y posteriores) y lo enlaza desde `~/.config/obs-studio/plugins/obs-trigglow-dynamic-delay/bin/64bit/`
(OBS 31 y 32); `./install.sh --uninstall` quita las dos cosas. Usa un paquete o el `.tar.xz`, no
los dos.

Tus ajustes y tu inicio de sesión (`~/.config/obs-studio/plugin_config/obs-trigglow-dynamic-delay/`)
no se tocan nunca al instalar, actualizar ni quitar el plugin. El inicio de sesión todavía no está
disponible en Linux (ver el paso 6).

## macOS

Instala el `.pkg` de la misma release. Se instala en `~/Library/Application Support/obs-studio/plugins/`, que OBS 33 sigue usando.

## Dónde quedan los archivos en Windows

Desde la v0.7.1 el instalador deja el plugin en la carpeta de plugins de OBS para todos los
usuarios, `C:\ProgramData\obs-studio\plugins\obs-trigglow-dynamic-delay\`, nunca en la carpeta
del propio OBS:

- `obs-trigglow-dynamic-delay.dll` y sus DLL de FFmpeg, que cargan OBS 33 y posteriores;
- `bin\64bit\`, las mismas DLL, para OBS 31 y 32 (OBS 33 se las salta como duplicado legacy);
- `data\`, compartida por las dos.

Solo los administradores pueden cambiar esa carpeta, así que ningún otro usuario del equipo puede
dejar archivos junto al plugin. El `.zip` trae la misma carpeta `obs-trigglow-dynamic-delay`: para
instalarlo a mano, descomprímelo en `C:\ProgramData\obs-studio\plugins\`.

## Actualizar desde una versión anterior

**Desde la v0.7.0 o anterior en Windows**, esas versiones se instalaban dentro de la carpeta del
propio OBS (`C:\Program Files\obs-studio\obs-plugins\64bit\`), que OBS 33 marca como "Legacy" y
OBS 34 no cargará. El instalador de la v0.7.1 la traslada: ejecuta en silencio el desinstalador de
la versión anterior (solo después de comprobar que su carpeta es una carpeta de OBS con nuestros
archivos y que el desinstalador es el de esa instalación) y luego borra lo nuestro que quede allí
(nuestra DLL, nuestra carpeta de datos, y las DLL de FFmpeg solo si son exactamente las que trae el
instalador). El resto de archivos de OBS y tu `plugin_config` (ajustes, inicio de sesión) no se
tocan. Si antes habías descomprimido el `.zip` en `C:\ProgramData\obs-studio\plugins\`, el
instalador simplemente lo actualiza ahí.

**En Linux**, instala el paquete nuevo encima del anterior. El `.deb` de la v0.7.0 se sustituye sin
más; si habías copiado el plugin a mano en `~/.config/obs-studio/plugins/`, borra esa copia y usa
un paquete o el `install.sh` del `.tar.xz`.


Instala la versión nueva encima de la anterior, igual que antes (con OBS cerrado): tus escenas,
ajustes y atajos se conservan. **Desde la v0.5.x o anterior, actualizar es obligatorio**: el inicio
de sesión de esas versiones se apaga cuando se publica la v0.6.0, y las sesiones que ya tienen dejan
de funcionar tras un periodo de transición. En el primer arranque tu inicio de sesión pasa
solo al formato nuevo; si el dock te pide iniciar sesión, hazlo una vez (paso 6). Desde la v0.6.0
los objetos que gestiona el plugin terminan en "(auto)" en vez de "(no tocar)": ver
`CHANGELOG-es.md` si una acción de Stream Deck u obs-websocket los usa por su nombre.
