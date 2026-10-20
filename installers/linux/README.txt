Trigglow Dynamic Delay for OBS Studio -- Linux, per-user install
================================================================

Requires OBS Studio @OBS_MIN_VERSION@ or newer, installed from your distro or the
OBS PPA (not the Flatpak), on x86_64, with Qt 6.4 or newer.

Install or update (as your own user, not root):

    tar -xJf obs-trigglow-dynamic-delay-*-x86_64-linux-gnu.tar.xz
    cd obs-trigglow-dynamic-delay-*-x86_64-linux-gnu
    ./install.sh

It copies the plugin to ~/.local/share/obs-studio/plugins/ (OBS 33 and later)
and links it from ~/.config/obs-studio/plugins/ (OBS 31 and 32). Then start
OBS: the "Trigglow Dynamic Delay" dock is under View > Docks.

Remove it:  ./install.sh --uninstall
Your settings (~/.config/obs-studio/plugin_config/) are never touched.

On Debian, Ubuntu, Fedora or Arch, the .deb, .rpm or .pkg.tar.zst package
installs it for all users instead. Use one or the other, not both.

Guide: https://www.trigglow.com/dynamic-delay


Instalación por usuario en Linux
--------------------------------

Necesita OBS Studio @OBS_MIN_VERSION@ o posterior, instalado desde tu distribución o
el PPA de OBS (no el Flatpak), en x86_64, con Qt 6.4 o posterior.

Instalar o actualizar (con tu usuario, no como root):

    tar -xJf obs-trigglow-dynamic-delay-*-x86_64-linux-gnu.tar.xz
    cd obs-trigglow-dynamic-delay-*-x86_64-linux-gnu
    ./install.sh

Copia el plugin en ~/.local/share/obs-studio/plugins/ (OBS 33 y posteriores)
y lo enlaza desde ~/.config/obs-studio/plugins/ (OBS 31 y 32). Después abre
OBS: el panel "Trigglow Dynamic Delay" está en Ver > Paneles (Docks).

Quitarlo:  ./install.sh --uninstall
Tus ajustes (~/.config/obs-studio/plugin_config/) no se tocan nunca.

En Debian, Ubuntu, Fedora o Arch, el paquete .deb, .rpm o .pkg.tar.zst lo
instala para todos los usuarios. Usa uno u otro, no los dos.
